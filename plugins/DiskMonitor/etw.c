/*
 * Per-app, per-disk I/O from ETW: a private system-logger session (Windows 8+, up to 8 can run side by side, so it
 * does not clash with ExtendedTools' own "SiKernelTraceSession") with only EVENT_TRACE_FLAG_DISK_IO enabled.
 * DiskIo Read/Write events carry the disk number, size and the issuing thread; the thread is mapped to its process
 * (cached). Bytes are summed per (disk, process) and handed to the provider thread once per refresh.
 * Needs administrator (System Informer normally runs elevated).
 */

#include "diskmon.h"
#include <evntrace.h>
#include <evntcons.h>

#define SESSION_NAME L"SiDiskMonitorSession"
#define TABLE_SIZE 4096     // (disk, pid) buckets per refresh
#define TID_CACHE 8192

// {3D6FA8D4-FE05-11D0-9DDA-00C04FD7BA7C}
static CONST GUID DiskIoProviderGuid = { 0x3d6fa8d4, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
// Session GUID for this plugin's logger.
static CONST GUID SessionGuid = { 0x6b1c7a52, 0x3f0e, 0x4d6a, { 0x9e, 0x51, 0x2a, 0x7c, 0x48, 0x15, 0xd2, 0x90 } };

typedef struct _DISKIO_READWRITE
{
    ULONG DiskNumber;
    ULONG IrpFlags;
    ULONG Size;
    ULONG Reserved;
    ULONG64 ByteOffset;
    PVOID FileObject;
    PVOID Irp;
    ULONG64 HighResResponseTime;
    ULONG IssuingThreadId;
} DISKIO_READWRITE, *PDISKIO_READWRITE;

typedef struct _BUCKET
{
    ULONG Disk;             // ULONG_MAX = empty
    ULONG ProcessId;
    ULONG64 ReadBytes;
    ULONG64 WriteBytes;
} BUCKET;

static SRWLOCK TableLock = SRWLOCK_INIT;
static BUCKET Table[TABLE_SIZE];
static ULONG TableUsed;

static struct { ULONG ThreadId; ULONG ProcessId; } TidCache[TID_CACHE];

static TRACEHANDLE SessionHandle;
static TRACEHANDLE ConsumerHandle = INVALID_PROCESSTRACE_HANDLE;
static HANDLE ConsumerThread;
static volatile LONG Exiting;
static ULONG StartStatus = ERROR_NOT_READY;

static ULONG ThreadToProcess(_In_ ULONG ThreadId)
{
    ULONG slot = (ThreadId / 4) % TID_CACHE;
    ULONG processId = 0;
    HANDLE thread;

    if (ThreadId == 0 || ThreadId == ULONG_MAX)
        return 4; // System

    if (TidCache[slot].ThreadId == ThreadId)
        return TidCache[slot].ProcessId;

    if (thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ThreadId))
    {
        processId = GetProcessIdOfThread(thread);
        CloseHandle(thread);
    }

    if (!processId)
        processId = 4; // thread already gone or protected: charge to System

    TidCache[slot].ThreadId = ThreadId;
    TidCache[slot].ProcessId = processId;
    return processId;
}

static VOID AddBytes(_In_ ULONG Disk, _In_ ULONG ProcessId, _In_ BOOLEAN Write, _In_ ULONG Bytes)
{
    ULONG hash = (Disk * 2654435761u) ^ (ProcessId * 40503u);

    AcquireSRWLockExclusive(&TableLock);

    for (ULONG probe = 0; probe < 32; probe++)
    {
        BUCKET* bucket = &Table[(hash + probe) % TABLE_SIZE];

        if (bucket->Disk == ULONG_MAX)
        {
            bucket->Disk = Disk;
            bucket->ProcessId = ProcessId;
            TableUsed++;
        }

        if (bucket->Disk == Disk && bucket->ProcessId == ProcessId)
        {
            if (Write)
                bucket->WriteBytes += Bytes;
            else
                bucket->ReadBytes += Bytes;
            break;
        }
    }

    ReleaseSRWLockExclusive(&TableLock);
}

static VOID WINAPI EventCallback(_In_ PEVENT_RECORD Record)
{
    UCHAR opcode = Record->EventHeader.EventDescriptor.Opcode;
    PDISKIO_READWRITE data;

    if (Exiting || !IsEqualGUID(&Record->EventHeader.ProviderId, &DiskIoProviderGuid))
        return;
    if (opcode != EVENT_TRACE_TYPE_IO_READ && opcode != EVENT_TRACE_TYPE_IO_WRITE)
        return;
    if (Record->UserDataLength < FIELD_OFFSET(DISKIO_READWRITE, IssuingThreadId) + sizeof(ULONG))
        return;

    data = Record->UserData;
    AddBytes(data->DiskNumber, ThreadToProcess(data->IssuingThreadId), opcode == EVENT_TRACE_TYPE_IO_WRITE, data->Size);
}

static ULONG WINAPI BufferCallback(_In_ PEVENT_TRACE_LOGFILEW Buffer)
{
    return !Exiting;
}

static DWORD WINAPI ConsumerThreadStart(_In_ PVOID Parameter)
{
    ProcessTrace(&ConsumerHandle, 1, NULL, NULL);
    return 0;
}

static PEVENT_TRACE_PROPERTIES MakeProperties(_Out_writes_bytes_(Size) PUCHAR Buffer, _In_ ULONG Size)
{
    PEVENT_TRACE_PROPERTIES properties = (PEVENT_TRACE_PROPERTIES)Buffer;

    memset(Buffer, 0, Size);
    properties->Wnode.BufferSize = Size;
    properties->Wnode.Guid = SessionGuid;
    properties->Wnode.ClientContext = 1;            // QPC timestamps
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->MinimumBuffers = 4;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    properties->FlushTimer = 1;
    properties->EnableFlags = EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_NO_SYSCONFIG;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return properties;
}

VOID DiskEtwStart(VOID)
{
    UCHAR buffer[sizeof(EVENT_TRACE_PROPERTIES) + sizeof(SESSION_NAME) + 64];
    PEVENT_TRACE_PROPERTIES properties;
    EVENT_TRACE_LOGFILEW logFile;

    for (ULONG i = 0; i < TABLE_SIZE; i++)
        Table[i].Disk = ULONG_MAX;

    // A session left running by an earlier System Informer (crash, kill) is stopped and replaced.
    properties = MakeProperties(buffer, sizeof(buffer));
    ControlTraceW(0, SESSION_NAME, properties, EVENT_TRACE_CONTROL_STOP);

    properties = MakeProperties(buffer, sizeof(buffer));
    StartStatus = StartTraceW(&SessionHandle, SESSION_NAME, properties);

    if (StartStatus != ERROR_SUCCESS)
    {
        SessionHandle = 0;
        return;
    }

    memset(&logFile, 0, sizeof(logFile));
    logFile.LoggerName = SESSION_NAME;
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logFile.EventRecordCallback = EventCallback;
    logFile.BufferCallback = BufferCallback;

    ConsumerHandle = OpenTraceW(&logFile);

    if (ConsumerHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        StartStatus = GetLastError();
        DiskEtwStop();
        return;
    }

    ConsumerThread = CreateThread(NULL, 0, ConsumerThreadStart, NULL, 0, NULL);
}

VOID DiskEtwStop(VOID)
{
    UCHAR buffer[sizeof(EVENT_TRACE_PROPERTIES) + sizeof(SESSION_NAME) + 64];

    InterlockedExchange(&Exiting, TRUE);

    if (SessionHandle)
    {
        ControlTraceW(SessionHandle, NULL, MakeProperties(buffer, sizeof(buffer)), EVENT_TRACE_CONTROL_STOP);
        SessionHandle = 0;
    }

    if (ConsumerHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(ConsumerHandle);
        ConsumerHandle = INVALID_PROCESSTRACE_HANDLE;
    }

    if (ConsumerThread)
    {
        WaitForSingleObject(ConsumerThread, 2000);
        CloseHandle(ConsumerThread);
        ConsumerThread = NULL;
    }
}

ULONG DiskEtwStatus(VOID)
{
    return StartStatus;
}

// Hands every (disk, pid, read, write) collected since the last call to Callback, then clears the table.
VOID DiskEtwDrain(
    _In_ VOID (*Callback)(_In_ ULONG Disk, _In_ ULONG ProcessId, _In_ ULONG64 ReadBytes, _In_ ULONG64 WriteBytes, _In_opt_ PVOID Context),
    _In_opt_ PVOID Context
    )
{
    static BUCKET copy[TABLE_SIZE];
    ULONG count = 0;

    AcquireSRWLockExclusive(&TableLock);

    for (ULONG i = 0; i < TABLE_SIZE; i++)
    {
        if (Table[i].Disk != ULONG_MAX)
        {
            copy[count++] = Table[i];
            Table[i].Disk = ULONG_MAX;
            Table[i].ReadBytes = Table[i].WriteBytes = 0;
        }
    }

    TableUsed = 0;
    ReleaseSRWLockExclusive(&TableLock);

    for (ULONG i = 0; i < count; i++)
        Callback(copy[i].Disk, copy[i].ProcessId, copy[i].ReadBytes, copy[i].WriteBytes, Context);
}
