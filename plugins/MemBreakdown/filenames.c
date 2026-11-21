/*
 * MemBreakdown file names: maps the kernel file keys found in PFN data to paths, using a short private ETW
 * system-logger session with the kernel's disk-file-io flag. When such a session stops, the kernel writes a
 * FileIo "rundown" event (key + NT path) for the files it knows. Plain Win32/ETW, shared with tools\memprobe.
 */

#include <phnt_windows.h>
#include <phnt.h>
#include <evntrace.h>
#include <evntcons.h>
#include "filenames.h"

static CONST GUID MbFileIoGuid = { 0x90cbdc39, 0x4a3e, 0x11d1, { 0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3 } };
#define MB_SESSION_NAME L"MemBreakdown File Names"

typedef struct _MB_COLLECT
{
    PMB_FILE_NAME Names;
    ULONG Count;
    ULONG Capacity;
    ULONG Events;
} MB_COLLECT, *PMB_COLLECT;

static MB_COLLECT MbpCollect;

static VOID WINAPI MbpEventCallback(_In_ PEVENT_RECORD Record)
{
    UCHAR opcode = Record->EventHeader.EventDescriptor.Opcode;
    PUCHAR data = Record->UserData;
    ULONG length = Record->UserDataLength;
    ULONG pointerSize = (Record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
    ULONG_PTR key;
    PCWSTR name;
    SIZE_T chars;
    PWSTR copy;

    if (!IsEqualGUID(&Record->EventHeader.ProviderId, &MbFileIoGuid))
        return;

    MbpCollect.Events++;

    // FileIo_Name: Name (0), FileCreate (32), FileRundown (36). Payload: FileObject (key), FileName.
    if ((opcode != 0 && opcode != 32 && opcode != 36) || length <= pointerSize + sizeof(WCHAR) || pointerSize != sizeof(ULONG_PTR))
        return;

    key = *(ULONG_PTR*)data;
    name = (PCWSTR)(data + pointerSize);
    chars = 0;

    while (chars < (length - pointerSize) / sizeof(WCHAR) && name[chars])
        chars++;

    if (!key || !chars)
        return;

    if (MbpCollect.Count == MbpCollect.Capacity)
    {
        ULONG capacity = MbpCollect.Capacity ? MbpCollect.Capacity * 2 : 4096;
        PMB_FILE_NAME names = RtlAllocateHeap(RtlProcessHeap(), 0, capacity * sizeof(MB_FILE_NAME));

        if (!names)
            return;

        if (MbpCollect.Names)
        {
            RtlCopyMemory(names, MbpCollect.Names, MbpCollect.Count * sizeof(MB_FILE_NAME));
            RtlFreeHeap(RtlProcessHeap(), 0, MbpCollect.Names);
        }

        MbpCollect.Names = names;
        MbpCollect.Capacity = capacity;
    }

    if (!(copy = RtlAllocateHeap(RtlProcessHeap(), 0, (chars + 1) * sizeof(WCHAR))))
        return;

    RtlCopyMemory(copy, name, chars * sizeof(WCHAR));
    copy[chars] = UNICODE_NULL;
    MbpCollect.Names[MbpCollect.Count].Key = key;
    MbpCollect.Names[MbpCollect.Count].Name = copy;
    MbpCollect.Count++;
}

static DWORD WINAPI MbpProcessThread(_In_ PVOID Parameter)
{
    TRACEHANDLE consumer = *(PTRACEHANDLE)Parameter;

    ProcessTrace(&consumer, 1, NULL, NULL);
    return 0;
}

static int __cdecl MbpNameCompare(_In_ const void* A, _In_ const void* B)
{
    ULONG_PTR a = ((const MB_FILE_NAME*)A)->Key;
    ULONG_PTR b = ((const MB_FILE_NAME*)B)->Key;

    return a < b ? -1 : a > b;
}

ULONG MbCollectFileNames(
    _Out_ PMB_FILE_NAMES Names,
    _In_ ULONG RunMs
    )
{
    struct { EVENT_TRACE_PROPERTIES Properties; WCHAR Name[128]; } buffer;
    PEVENT_TRACE_PROPERTIES properties = &buffer.Properties;
    TRACEHANDLE session = 0;
    TRACEHANDLE consumer;
    EVENT_TRACE_LOGFILEW logFile;
    HANDLE thread;
    ULONG error;

    RtlZeroMemory(Names, sizeof(MB_FILE_NAMES));
    RtlZeroMemory(&MbpCollect, sizeof(MbpCollect));

#define MB_INIT_PROPERTIES() \
    RtlZeroMemory(&buffer, sizeof(buffer)); \
    properties->Wnode.BufferSize = sizeof(buffer); \
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID; \
    properties->Wnode.ClientContext = 1; \
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES)

    // A session left over from a crash would block StartTrace: stop ours (only ours, by its own name).
    MB_INIT_PROPERTIES();
    ControlTraceW(0, MB_SESSION_NAME, properties, EVENT_TRACE_CONTROL_STOP);

    MB_INIT_PROPERTIES();
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    properties->EnableFlags = EVENT_TRACE_FLAG_DISK_FILE_IO;
    properties->BufferSize = 256;
    properties->MinimumBuffers = 32;
    properties->MaximumBuffers = 256;
    properties->FlushTimer = 1;

    if ((error = StartTraceW(&session, MB_SESSION_NAME, properties)) != ERROR_SUCCESS)
        return error;

    RtlZeroMemory(&logFile, sizeof(logFile));
    logFile.LoggerName = MB_SESSION_NAME;
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = MbpEventCallback;
    consumer = OpenTraceW(&logFile);

    if (consumer == INVALID_PROCESSTRACE_HANDLE)
    {
        error = GetLastError();
        MB_INIT_PROPERTIES();
        ControlTraceW(session, NULL, properties, EVENT_TRACE_CONTROL_STOP);
        return error;
    }

    thread = CreateThread(NULL, 0, MbpProcessThread, &consumer, 0, NULL);
    Sleep(RunMs);

    MB_INIT_PROPERTIES();
    error = ControlTraceW(session, NULL, properties, EVENT_TRACE_CONTROL_STOP);

    if (thread)
    {
        if (WaitForSingleObject(thread, 15000) == WAIT_TIMEOUT)
            CloseTrace(consumer);
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }

    CloseTrace(consumer);

    qsort(MbpCollect.Names, MbpCollect.Count, sizeof(MB_FILE_NAME), MbpNameCompare);
    Names->Names = MbpCollect.Names;
    Names->Count = MbpCollect.Count;
    Names->Events = MbpCollect.Events;
    RtlZeroMemory(&MbpCollect, sizeof(MbpCollect));

    return error;
}

PCWSTR MbFindFileName(
    _In_ PMB_FILE_NAMES Names,
    _In_ ULONG_PTR Key
    )
{
    MB_FILE_NAME key;
    PMB_FILE_NAME found;

    if (!Names->Count)
        return NULL;

    key.Key = Key;
    found = bsearch(&key, Names->Names, Names->Count, sizeof(MB_FILE_NAME), MbpNameCompare);

    // Several events can carry the same key: any of them names the file.
    return found ? found->Name : NULL;
}

VOID MbFreeFileNames(
    _Inout_ PMB_FILE_NAMES Names
    )
{
    for (ULONG i = 0; i < Names->Count; i++)
        RtlFreeHeap(RtlProcessHeap(), 0, Names->Names[i].Name);

    if (Names->Names)
        RtlFreeHeap(RtlProcessHeap(), 0, Names->Names);

    RtlZeroMemory(Names, sizeof(MB_FILE_NAMES));
}
