/*
 * ProcessLog capture: a private ETW system-logger session with the kernel PROCESS and IMAGE_LOAD flags.
 *
 * Why a session of our own: System Informer's kernel informer (KSI process create/exit messages) is not exported
 * to plugins, and ExtendedTools' private kernel session only carries the disk, file and network flags and gives
 * other plugins no access to its events. A system-logger session (Windows 8+) is independent of the NT Kernel
 * Logger and of ExtendedTools, so neither is disturbed.
 *
 * Process Start/End (Process_V4_TypeGroup1) give PID, parent, session, user SID, command line and exit status,
 * straight from the kernel, so a process that lived 1 ms is still complete. The first image load of a new PID
 * gives the full image path. Events arrive up to ~1 s late, so nothing is ever read from the live process.
 *
 * The log is a UTF-8, tab-separated, append-only file (one line per start and per exit), written as events
 * arrive so it survives System Informer being killed, rotated to <file>.1 at a size cap.
 */

#include "processlog.h"
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#pragma comment(lib, "advapi32.lib")

#define PL_SESSION_NAME L"SiProcessLogSession"
#define PL_HASH_SIZE 1024
#define PL_ORPHAN_COUNT 64
#define PL_SID_CACHE_COUNT 32

// {3D6FA8D0-FE05-11D0-9DDA-00C04FD7BA7C} kernel Process events
static const GUID PlProcessGuid = { 0x3d6fa8d0, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
// {2CB15D1D-5FC1-11D2-ABE1-00A0C911F518} kernel Image events
static const GUID PlImageLoadGuid = { 0x2cb15d1d, 0x5fc1, 0x11d2, { 0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18 } };
// Our own session GUID (system logger mode needs a unique one).
static const GUID PlSessionGuid = { 0x7c4f3a52, 0x1e9b, 0x4d8a, { 0x9b, 0x6e, 0x5a, 0x2f, 0x0c, 0x3d, 0x81, 0xe4 } };

SRWLOCK PlLock = SRWLOCK_INIT;
PLOG_ENTRY *PlEntries = NULL;
ULONG PlEntryCount = 0;
volatile LONG PlVersion = 0;
ULONG64 PlChangeStamp = 0;
ULONG PlTrimCount = 0;
WCHAR PlLogFilePath[MAX_PATH] = { 0 };

static ULONG PlEntryCapacity = 0;
static ULONG64 PlNextSeq = 1;
static PLOG_ENTRY PlHash[PL_HASH_SIZE];
static ULONG PlMaxEntries = 50000;
static ULONG64 PlMaxFileBytes = 32ULL * 1024 * 1024;

static WCHAR PlStatusFilePath[MAX_PATH];
static HANDLE PlFile = INVALID_HANDLE_VALUE;
static ULONG64 PlFileSize = 0;

static HANDLE PlThread = NULL;
static HANDLE PlExitEvent = NULL;
static volatile BOOLEAN PlExiting = FALSE;
static volatile BOOLEAN PlActive = FALSE;
static volatile ULONG PlSessionStatus = ERROR_NOT_READY;
static volatile ULONG64 PlStartCount = 0;
static volatile ULONG64 PlEndCount = 0;
static volatile ULONG64 PlImageCount = 0;

typedef struct _PL_ORPHAN_IMAGE
{
    ULONG ProcessId;
    LONG64 Time;
    PWSTR Path;
} PL_ORPHAN_IMAGE;

static PL_ORPHAN_IMAGE PlOrphans[PL_ORPHAN_COUNT];
static ULONG PlOrphanNext = 0;

typedef struct _PL_SID_NAME
{
    UCHAR Sid[SECURITY_MAX_SID_SIZE];
    ULONG Length;
    PWSTR Name;
} PL_SID_NAME;

static PL_SID_NAME PlSidCache[PL_SID_CACHE_COUNT];
static ULONG PlSidCacheCount = 0;
static ULONG PlSidCacheNext = 0;

// ---- small helpers -----------------------------------------------------------------------------------------

PWSTR PlDup(
    _In_opt_ PCWSTR Text
    )
{
    SIZE_T length;
    PWSTR copy;

    if (!Text || !Text[0])
        return NULL;

    length = wcslen(Text);
    copy = malloc((length + 1) * sizeof(WCHAR));

    if (copy)
        memcpy(copy, Text, (length + 1) * sizeof(WCHAR));

    return copy;
}

VOID PlBufAppendN(
    _Inout_ PPL_BUF Buf,
    _In_reads_(Count) PCWSTR Text,
    _In_ SIZE_T Count
    )
{
    if (Buf->Length + Count + 1 > Buf->Capacity)
    {
        SIZE_T capacity = max(Buf->Capacity * 2, Buf->Length + Count + 256);
        PWSTR data = realloc(Buf->Data, capacity * sizeof(WCHAR));

        if (!data)
            return;

        Buf->Data = data;
        Buf->Capacity = capacity;
    }

    memcpy(Buf->Data + Buf->Length, Text, Count * sizeof(WCHAR));
    Buf->Length += Count;
    Buf->Data[Buf->Length] = UNICODE_NULL;
}

VOID PlBufAppend(
    _Inout_ PPL_BUF Buf,
    _In_opt_ PCWSTR Text
    )
{
    if (Text)
        PlBufAppendN(Buf, Text, wcslen(Text));
}

VOID PlBufAppendF(
    _Inout_ PPL_BUF Buf,
    _In_z_ _Printf_format_string_ PCWSTR Format,
    ...
    )
{
    WCHAR buffer[512];
    va_list args;
    int count;

    va_start(args, Format);
    count = _vsnwprintf_s(buffer, RTL_NUMBER_OF(buffer), _TRUNCATE, Format, args);
    va_end(args);

    if (count < 0)
        count = (int)wcslen(buffer);

    PlBufAppendN(Buf, buffer, (SIZE_T)count);
}

VOID PlBufFree(
    _Inout_ PPL_BUF Buf
    )
{
    free(Buf->Data);
    Buf->Data = NULL;
    Buf->Length = 0;
    Buf->Capacity = 0;
}

static VOID PlpTake(
    _Inout_ PWSTR *Target,
    _Inout_ PWSTR *Source
    )
{
    if (*Source)
    {
        free(*Target);
        *Target = *Source;
        *Source = NULL;
    }
}

static VOID PlpTakeIfEmpty(
    _Inout_ PWSTR *Target,
    _Inout_ PWSTR *Source
    )
{
    if (!*Target && *Source)
    {
        *Target = *Source;
        *Source = NULL;
    }
}

static LONG64 PlpNow(
    VOID
    )
{
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return ((LONG64)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

// \Device\HarddiskVolume3\x.exe -> C:\x.exe (System Informer's own conversion).
static PWSTR PlpToWin32Path(
    _In_opt_ PCWSTR NtPath
    )
{
    PPH_STRING ntName;
    PPH_STRING dosName;
    PWSTR result;

    if (!NtPath || !NtPath[0])
        return NULL;

    ntName = PhCreateString((PWSTR)NtPath);
    dosName = PhGetFileName(ntName);
    result = PlDup(dosName ? dosName->Buffer : NtPath);

    if (dosName)
        PhDereferenceObject(dosName);
    PhDereferenceObject(ntName);

    return result;
}

static PWSTR PlpSidToName(
    _In_reads_bytes_(Length) PUCHAR Sid,
    _In_ ULONG Length
    )
{
    UCHAR sidBuffer[SECURITY_MAX_SID_SIZE];
    PPH_STRING name;
    PWSTR result = NULL;
    ULONG i;

    if (Length == 0 || Length > sizeof(sidBuffer))
        return NULL;

    for (i = 0; i < PlSidCacheCount; i++)
    {
        if (PlSidCache[i].Length == Length && memcmp(PlSidCache[i].Sid, Sid, Length) == 0)
            return PlDup(PlSidCache[i].Name);
    }

    memcpy(sidBuffer, Sid, Length);

    if (!IsValidSid((PSID)sidBuffer) || GetLengthSid((PSID)sidBuffer) != Length)
        return NULL;

    if (name = PhGetSidFullName((PSID)sidBuffer, TRUE, NULL))
    {
        result = PlDup(name->Buffer);
        PhDereferenceObject(name);
    }

    if (!result)
    {
        PWSTR sidString;

        if (ConvertSidToStringSidW((PSID)sidBuffer, &sidString))
        {
            result = PlDup(sidString);
            LocalFree(sidString);
        }
    }

    if (result)
    {
        ULONG slot = PlSidCacheCount < PL_SID_CACHE_COUNT ? PlSidCacheCount++ : (PlSidCacheNext++ % PL_SID_CACHE_COUNT);

        free(PlSidCache[slot].Name);
        memcpy(PlSidCache[slot].Sid, Sid, Length);
        PlSidCache[slot].Length = Length;
        PlSidCache[slot].Name = PlDup(result);
    }

    return result;
}

// ---- entries -----------------------------------------------------------------------------------------------

static VOID PlpFreeEntry(
    _In_ PLOG_ENTRY Entry
    )
{
    free(Entry->Name);
    free(Entry->ParentName);
    free(Entry->CreatorName);
    free(Entry->ImagePath);
    free(Entry->CommandLine);
    free(Entry->User);
    free(Entry);
}

static PLOG_ENTRY PlpNewEntry(
    VOID
    )
{
    return calloc(1, sizeof(LOG_ENTRY));
}

static ULONG PlpHashIndex(
    _In_ ULONG ProcessId
    )
{
    return (ProcessId >> 2) & (PL_HASH_SIZE - 1);
}

static PLOG_ENTRY PlpHashFind(
    _In_ ULONG ProcessId
    )
{
    PLOG_ENTRY entry;

    for (entry = PlHash[PlpHashIndex(ProcessId)]; entry; entry = entry->HashNext)
    {
        if (entry->ProcessId == ProcessId)
            return entry;
    }

    return NULL;
}

static VOID PlpHashInsert(
    _In_ PLOG_ENTRY Entry
    )
{
    ULONG index = PlpHashIndex(Entry->ProcessId);

    Entry->HashNext = PlHash[index];
    PlHash[index] = Entry;
}

static VOID PlpHashRemove(
    _In_ PLOG_ENTRY Entry
    )
{
    PLOG_ENTRY *link = &PlHash[PlpHashIndex(Entry->ProcessId)];

    while (*link)
    {
        if (*link == Entry)
        {
            *link = Entry->HashNext;
            Entry->HashNext = NULL;
            return;
        }

        link = &(*link)->HashNext;
    }
}

static BOOLEAN PlpInHash(
    _In_ PLOG_ENTRY Entry
    )
{
    PLOG_ENTRY entry;

    for (entry = PlHash[PlpHashIndex(Entry->ProcessId)]; entry; entry = entry->HashNext)
    {
        if (entry == Entry)
            return TRUE;
    }

    return FALSE;
}

// Drops the oldest finished entries once the log holds more than PlMaxEntries.
static VOID PlpTrim(
    VOID
    )
{
    ULONG target;
    ULONG removed = 0;
    ULONG i;
    ULONG j = 0;

    if (PlEntryCount <= PlMaxEntries + 1000)
        return;

    target = PlEntryCount - PlMaxEntries;

    for (i = 0; i < PlEntryCount; i++)
    {
        PLOG_ENTRY entry = PlEntries[i];

        if (removed < target && !(entry->Flags & LE_RUNNING) && !PlpInHash(entry))
        {
            PlpFreeEntry(entry);
            removed++;
        }
        else
        {
            PlEntries[j++] = entry;
        }
    }

    PlEntryCount = j;

    if (removed)
        PlTrimCount++;
}

static BOOLEAN PlLoading;

// Adds the entry to the log. On FALSE (out of memory) the entry is not in the log and still owned by the caller.
static BOOLEAN PlpAppend(
    _In_ PLOG_ENTRY Entry
    )
{
    if (PlEntryCount == PlEntryCapacity)
    {
        ULONG capacity = PlEntryCapacity ? PlEntryCapacity * 2 : 4096;
        PLOG_ENTRY *entries = realloc(PlEntries, capacity * sizeof(PLOG_ENTRY));

        if (!entries)
            return FALSE;

        PlEntries = entries;
        PlEntryCapacity = capacity;
    }

    Entry->Seq = PlNextSeq++;
    Entry->ChangeStamp = ++PlChangeStamp;
    Entry->Flags |= LE_VISIBLE;
    PlEntries[PlEntryCount++] = Entry;

    if (!PlLoading) // loaded entries are chained in PlLoadHash: trim after loading
        PlpTrim();

    return TRUE;
}

PLOG_ENTRY PlFindEntryBySeq(
    _In_ ULONG64 Seq
    )
{
    LONG low = 0;
    LONG high = (LONG)PlEntryCount - 1;

    while (low <= high)
    {
        LONG mid = (low + high) / 2;
        ULONG64 seq = PlEntries[mid]->Seq;

        if (seq == Seq)
            return PlEntries[mid];
        if (seq < Seq)
            low = mid + 1;
        else
            high = mid - 1;
    }

    return NULL;
}

// Name of the process with this PID at time At (handles PID reuse).
static PCWSTR PlpLookupName(
    _In_ ULONG ProcessId,
    _In_ LONG64 At
    )
{
    PLOG_ENTRY entry;
    LONG i;
    ULONG scanned = 0;

    if (ProcessId == 0)
        return NULL;

    entry = PlpHashFind(ProcessId);

    if (entry && (entry->StartTime == 0 || entry->StartTime <= At + PL_TICKS_PER_SECOND))
        return entry->Name;

    for (i = (LONG)PlEntryCount - 1; i >= 0 && scanned < 4096; i--, scanned++)
    {
        entry = PlEntries[i];

        if (entry->ProcessId == ProcessId &&
            (entry->StartTime == 0 || entry->StartTime <= At + PL_TICKS_PER_SECOND) &&
            (entry->ExitTime == 0 || entry->ExitTime + 5 * PL_TICKS_PER_SECOND >= At))
        {
            return entry->Name;
        }
    }

    return NULL;
}

// ---- log file ----------------------------------------------------------------------------------------------

static VOID PlpAppendEscaped(
    _Inout_ PPL_BUF Buf,
    _In_opt_ PCWSTR Text
    )
{
    PCWSTR start;

    PlBufAppendN(Buf, L"\t", 1);

    if (!Text)
        return;

    for (start = Text; *Text; Text++)
    {
        PCWSTR escape = NULL;

        switch (*Text)
        {
        case L'\\': escape = L"\\\\"; break;
        case L'\t': escape = L"\\t"; break;
        case L'\n': escape = L"\\n"; break;
        case L'\r': escape = L"\\r"; break;
        }

        if (escape)
        {
            PlBufAppendN(Buf, start, Text - start);
            PlBufAppendN(Buf, escape, 2);
            start = Text + 1;
        }
    }

    PlBufAppendN(Buf, start, Text - start);
}

static VOID PlpOpenLogFile(
    VOID
    )
{
    LARGE_INTEGER size;

    PlFile = CreateFileW(PlLogFilePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (PlFile == INVALID_HANDLE_VALUE)
        return;

    PlFileSize = GetFileSizeEx(PlFile, &size) ? (ULONG64)size.QuadPart : 0;

    if (PlFileSize == 0)
    {
        static const CHAR header[] = "#ProcessLog 1\tkind\tpid\tkey\tstart\tppid\tparent\tcreator\tcreatorName\tsession\tname\timage\tcommandLine\tuser\tflags\texit\texitCode\n";
        DWORD written;

        if (WriteFile(PlFile, header, sizeof(header) - 1, &written, NULL))
            PlFileSize += written;
    }
}

static VOID PlpRotateLogFile(
    VOID
    )
{
    WCHAR oldPath[MAX_PATH + 4];

    CloseHandle(PlFile);
    PlFile = INVALID_HANDLE_VALUE;
    _snwprintf_s(oldPath, RTL_NUMBER_OF(oldPath), _TRUNCATE, L"%s.1", PlLogFilePath);
    MoveFileExW(PlLogFilePath, oldPath, MOVEFILE_REPLACE_EXISTING);
    PlpOpenLogFile();
}

// Kind 'S' = start seen (process running), 'E' = complete record. Both carry every field.
static VOID PlpWriteRecord(
    _In_ PLOG_ENTRY Entry,
    _In_ WCHAR Kind
    )
{
    PL_BUF line = { 0 };
    PSTR utf8;
    int bytes;

    // Every change the window shows ends in a record write.
    Entry->ChangeStamp = ++PlChangeStamp;

    if (PlFile == INVALID_HANDLE_VALUE)
        return;

    PlBufAppendF(&line, L"%c\t%lu\t%llx\t%lld\t%lu", Kind, Entry->ProcessId, Entry->Key, Entry->StartTime, Entry->ParentId);
    PlpAppendEscaped(&line, Entry->ParentName);
    PlBufAppendF(&line, L"\t%lu", Entry->CreatorId);
    PlpAppendEscaped(&line, Entry->CreatorName);
    PlBufAppendF(&line, L"\t%lu", Entry->SessionId);
    PlpAppendEscaped(&line, Entry->Name);
    PlpAppendEscaped(&line, Entry->ImagePath);
    PlpAppendEscaped(&line, Entry->CommandLine);
    PlpAppendEscaped(&line, Entry->User);
    PlBufAppendF(&line, L"\t%lx\t%lld\t%ld\n", Entry->Flags & LE_SAVED_MASK, Entry->ExitTime, Entry->ExitCode);

    if (!line.Data)
        return;

    bytes = WideCharToMultiByte(CP_UTF8, 0, line.Data, (int)line.Length, NULL, 0, NULL, NULL);

    if (bytes > 0 && (utf8 = malloc(bytes)))
    {
        DWORD written;

        WideCharToMultiByte(CP_UTF8, 0, line.Data, (int)line.Length, utf8, bytes, NULL, NULL);

        if (WriteFile(PlFile, utf8, bytes, &written, NULL))
            PlFileSize += written;

        free(utf8);
    }

    PlBufFree(&line);

    if (PlFileSize > PlMaxFileBytes)
        PlpRotateLogFile();
}

static VOID PlpUnescapeInPlace(
    _Inout_ PWSTR Text
    )
{
    PWSTR out = Text;

    for (; *Text; Text++)
    {
        if (*Text == L'\\' && Text[1])
        {
            Text++;

            switch (*Text)
            {
            case L't': *out++ = L'\t'; break;
            case L'n': *out++ = L'\n'; break;
            case L'r': *out++ = L'\r'; break;
            default: *out++ = *Text; break;
            }
        }
        else
        {
            *out++ = *Text;
        }
    }

    *out = UNICODE_NULL;
}

// While loading, every loaded entry is chained here by PID (through HashNext, newest first), so an 'E' line finds
// its 'S' line however many processes started in between. Cleared before live capture uses HashNext.
static PLOG_ENTRY PlLoadHash[PL_HASH_SIZE];
static ULONG PlLoadedLines = 0;
static ULONG PlLoadMs = 0;
static ULONG PlLoadExitUnknown = 0;

// Merges one loaded record: an 'E' record completes the matching earlier 'S' record.
static VOID PlpMergeLoaded(
    _Inout_ PLOG_ENTRY Record,
    _In_ WCHAR Kind
    )
{
    PLOG_ENTRY entry;
    ULONG index = PlpHashIndex(Record->ProcessId);

    PlLoadedLines++;

    for (entry = PlLoadHash[index]; entry; entry = entry->HashNext)
    {
        BOOLEAN sameKey;
        BOOLEAN sameStart;

        if (entry->ProcessId != Record->ProcessId)
            continue;

        sameKey = Record->Key && entry->Key == Record->Key;
        sameStart = Record->StartTime && entry->StartTime == Record->StartTime;

        if (!(sameKey || sameStart))
            continue;
        if (Record->Key && entry->Key && entry->Key != Record->Key)
            continue;
        if (Record->StartTime && entry->StartTime && entry->StartTime != Record->StartTime)
            continue;

        if (Kind == L'E' && (Record->Flags & LE_EXIT_UNKNOWN) && !Record->ExitTime && entry->ExitTime)
        {
            // never let an "exit not seen" line replace a real exit
        }
        else if (Kind == L'E')
        {
            PlpTake(&entry->Name, &Record->Name);
            PlpTake(&entry->ParentName, &Record->ParentName);
            PlpTake(&entry->CreatorName, &Record->CreatorName);
            PlpTake(&entry->ImagePath, &Record->ImagePath);
            PlpTake(&entry->CommandLine, &Record->CommandLine);
            PlpTake(&entry->User, &Record->User);
            entry->Key = Record->Key ? Record->Key : entry->Key;
            entry->StartTime = Record->StartTime ? Record->StartTime : entry->StartTime;
            entry->ExitTime = Record->ExitTime;
            entry->ExitCode = Record->ExitCode;
            entry->ParentId = Record->ParentId;
            entry->CreatorId = Record->CreatorId;
            entry->SessionId = Record->SessionId;
            entry->Flags = (Record->Flags & LE_SAVED_MASK) | LE_VISIBLE;
        }
        else
        {
            PlpTakeIfEmpty(&entry->ImagePath, &Record->ImagePath);
            PlpTakeIfEmpty(&entry->CommandLine, &Record->CommandLine);
            PlpTakeIfEmpty(&entry->User, &Record->User);
            PlpTakeIfEmpty(&entry->ParentName, &Record->ParentName);
        }

        PlpFreeEntry(Record);
        return;
    }

    if (!PlpAppend(Record))
    {
        PlpFreeEntry(Record);
        return;
    }

    Record->HashNext = PlLoadHash[index];
    PlLoadHash[index] = Record;
}

static VOID PlpParseLine(
    _In_reads_(Length) PCSTR Line,
    _In_ int Length
    )
{
    PWSTR fields[16];
    ULONG count = 0;
    PWSTR text;
    PWSTR cursor;
    PLOG_ENTRY record;
    int chars;
    WCHAR kind;

    if (Length <= 0 || Line[0] == '#')
        return;

    chars = MultiByteToWideChar(CP_UTF8, 0, Line, Length, NULL, 0);

    if (chars <= 0 || !(text = malloc((chars + 1) * sizeof(WCHAR))))
        return;

    MultiByteToWideChar(CP_UTF8, 0, Line, Length, text, chars);
    text[chars] = UNICODE_NULL;

    for (cursor = text, fields[count++] = text; *cursor && count < RTL_NUMBER_OF(fields); cursor++)
    {
        if (*cursor == L'\t')
        {
            *cursor = UNICODE_NULL;
            fields[count++] = cursor + 1;
        }
    }

    kind = fields[0][0];

    if (count < 16 || (kind != L'S' && kind != L'E') || !(record = PlpNewEntry()))
    {
        free(text);
        return;
    }

    for (ULONG i = 0; i < count; i++)
        PlpUnescapeInPlace(fields[i]);

    record->ProcessId = wcstoul(fields[1], NULL, 10);
    record->Key = _wcstoui64(fields[2], NULL, 16);
    record->StartTime = _wcstoi64(fields[3], NULL, 10);
    record->ParentId = wcstoul(fields[4], NULL, 10);
    record->ParentName = PlDup(fields[5]);
    record->CreatorId = wcstoul(fields[6], NULL, 10);
    record->CreatorName = PlDup(fields[7]);
    record->SessionId = wcstoul(fields[8], NULL, 10);
    record->Name = PlDup(fields[9]);
    record->ImagePath = PlDup(fields[10]);
    record->CommandLine = PlDup(fields[11]);
    record->User = PlDup(fields[12]);
    record->Flags = wcstoul(fields[13], NULL, 16) & LE_SAVED_MASK;
    record->ExitTime = _wcstoi64(fields[14], NULL, 10);
    record->ExitCode = wcstol(fields[15], NULL, 10);
    free(text);

    PlpMergeLoaded(record, kind);
}

static VOID PlpLoadFile(
    _In_ PCWSTR Path
    )
{
    HANDLE file;
    LARGE_INTEGER size;
    PCHAR data;
    DWORD read;
    PCHAR line;
    PCHAR end;

    file = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
        return;

    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 256LL * 1024 * 1024 ||
        !(data = malloc((SIZE_T)size.QuadPart)))
    {
        CloseHandle(file);
        return;
    }

    if (ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL))
    {
        for (line = data, end = data + read; line < end; )
        {
            PCHAR next = memchr(line, '\n', end - line);
            PCHAR lineEnd = next ? next : end;

            if (next)  // an unterminated last line may be half written: skip it
                PlpParseLine(line, (int)(lineEnd - line));

            line = lineEnd + 1;
        }
    }

    free(data);
    CloseHandle(file);
}

// ---- event parsing -----------------------------------------------------------------------------------------

typedef struct _PL_PROCESS_EVENT
{
    ULONG64 Key;
    ULONG ProcessId;
    ULONG ParentId;
    ULONG SessionId;
    LONG ExitStatus;
    PWSTR Name;
    PWSTR CommandLine;
    PWSTR User;
} PL_PROCESS_EVENT, *PPL_PROCESS_EVENT;

static PUCHAR PlpReadWide(
    _In_ PUCHAR Data,
    _In_ PUCHAR End,
    _Out_ PWSTR *Text
    )
{
    PUCHAR cursor = Data;
    SIZE_T bytes;

    *Text = NULL;

    while (cursor + 1 < End && (cursor[0] || cursor[1]))
        cursor += 2;

    bytes = cursor - Data;

    if (bytes && (*Text = malloc(bytes + sizeof(WCHAR))))
    {
        memcpy(*Text, Data, bytes); // payload strings are not aligned
        (*Text)[bytes / sizeof(WCHAR)] = UNICODE_NULL;
    }

    return cursor + 1 < End ? cursor + 2 : End;
}

static PUCHAR PlpReadAnsi(
    _In_ PUCHAR Data,
    _In_ PUCHAR End,
    _Out_ PWSTR *Text
    )
{
    PUCHAR cursor = Data;
    int chars;

    *Text = NULL;

    while (cursor < End && *cursor)
        cursor++;

    if (cursor > Data)
    {
        chars = MultiByteToWideChar(CP_ACP, 0, (PCSTR)Data, (int)(cursor - Data), NULL, 0);

        if (chars > 0 && (*Text = malloc((chars + 1) * sizeof(WCHAR))))
        {
            MultiByteToWideChar(CP_ACP, 0, (PCSTR)Data, (int)(cursor - Data), *Text, chars);
            (*Text)[chars] = UNICODE_NULL;
        }
    }

    return cursor < End ? cursor + 1 : End;
}

static ULONG PlpPointerSize(
    _In_ PEVENT_RECORD Record
    )
{
    return (Record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
}

static ULONG64 PlpReadPointer(
    _In_ PUCHAR Data,
    _In_ ULONG PointerSize
    )
{
    ULONG64 value = 0;
    memcpy(&value, Data, PointerSize);
    return value;
}

// Process_V3/V4_TypeGroup1: UniqueProcessKey, ProcessId, ParentId, SessionId, ExitStatus, DirectoryTableBase,
// [V4: Flags], UserSID (TOKEN_USER + SID), ImageFileName (ANSI), CommandLine (UTF-16), ...
static BOOLEAN PlpParseProcessEvent(
    _In_ PEVENT_RECORD Record,
    _Out_ PPL_PROCESS_EVENT Event
    )
{
    PUCHAR data = Record->UserData;
    PUCHAR end = data + Record->UserDataLength;
    ULONG ptr = PlpPointerSize(Record);
    UCHAR version = Record->EventHeader.EventDescriptor.Version;

    memset(Event, 0, sizeof(PL_PROCESS_EVENT));

    if (version < 3 || !data)
        return FALSE;
    if ((SIZE_T)(end - data) < ptr + 16 + ptr + (version >= 4 ? 4 : 0) + 4)
        return FALSE;

    Event->Key = PlpReadPointer(data, ptr);
    data += ptr;
    memcpy(&Event->ProcessId, data, 4);
    memcpy(&Event->ParentId, data + 4, 4);
    memcpy(&Event->SessionId, data + 8, 4);
    memcpy(&Event->ExitStatus, data + 12, 4);
    data += 16;
    data += ptr; // DirectoryTableBase

    if (version >= 4)
        data += 4; // Flags

    // UserSID: 0 (4 bytes) when absent, else TOKEN_USER (2 pointers) followed by the SID.
    if (PlpReadPointer(data, 4) == 0)
    {
        data += 4;
    }
    else
    {
        PUCHAR sid = data + 2 * ptr;
        ULONG sidLength;

        if (sid + 8 > end)
            return FALSE;

        sidLength = 8 + 4 * (ULONG)sid[1];

        if (sid + sidLength > end)
            return FALSE;

        Event->User = PlpSidToName(sid, sidLength);
        data = sid + sidLength;
    }

    data = PlpReadAnsi(data, end, &Event->Name);
    PlpReadWide(data, end, &Event->CommandLine);

    return TRUE;
}

static VOID PlpFreeProcessEvent(
    _In_ PPL_PROCESS_EVENT Event
    )
{
    free(Event->Name);
    free(Event->CommandLine);
    free(Event->User);
}

// ---- event handling (PlLock held exclusive) ----------------------------------------------------------------

static VOID PlpFillFromEvent(
    _Inout_ PLOG_ENTRY Entry,
    _Inout_ PPL_PROCESS_EVENT Event,
    _In_ LONG64 At,
    _In_ ULONG CreatorId
    )
{
    Entry->ParentId = Event->ParentId;
    Entry->SessionId = Event->SessionId;
    PlpTakeIfEmpty(&Entry->Name, &Event->Name);
    PlpTakeIfEmpty(&Entry->CommandLine, &Event->CommandLine);
    PlpTakeIfEmpty(&Entry->User, &Event->User);

    if (!Entry->ParentName)
        Entry->ParentName = PlDup(PlpLookupName(Event->ParentId, At));

    if (CreatorId && CreatorId != ULONG_MAX && CreatorId != Event->ParentId && CreatorId != Event->ProcessId)
    {
        Entry->CreatorId = CreatorId;

        if (!Entry->CreatorName)
            Entry->CreatorName = PlDup(PlpLookupName(CreatorId, At));
    }
}

// Events come from per-CPU buffers and are not delivered in order: a reused PID's Start can arrive before the
// End of the process that had the PID before. That older process is detached from the PID table but stays
// "running" in the log, where its End finds it by PID and kernel key.
static VOID PlpDetach(
    _In_ PLOG_ENTRY Entry
    )
{
    PlpHashRemove(Entry);

    if (!(Entry->Flags & LE_VISIBLE) && !PlpAppend(Entry)) // already running when logging started
        PlpFreeEntry(Entry);
}

static BOOLEAN PlpKeysMatch(
    _In_ ULONG64 EntryKey,
    _In_ ULONG64 EventKey
    )
{
    return !EntryKey || !EventKey || EntryKey == EventKey;
}

static VOID PlpHandleStart(
    _Inout_ PPL_PROCESS_EVENT Event,
    _In_ LONG64 Time,
    _In_ ULONG HeaderProcessId
    )
{
    PLOG_ENTRY entry;
    LONG i;
    ULONG scanned = 0;

    if (entry = PlpHashFind(Event->ProcessId))
        PlpDetach(entry);

    // The End event can be delivered before the Start event (different CPU buffers).
    for (i = (LONG)PlEntryCount - 1; i >= 0 && scanned < 64; i--, scanned++)
    {
        entry = PlEntries[i];

        if (entry->ProcessId == Event->ProcessId && entry->Key == Event->Key && (entry->Flags & LE_START_UNKNOWN))
        {
            entry->StartTime = Time;
            entry->Flags &= ~LE_START_UNKNOWN;
            free(entry->ParentName);
            entry->ParentName = NULL;
            PlpFillFromEvent(entry, Event, Time, HeaderProcessId);
            PlpWriteRecord(entry, L'E');
            return;
        }
    }

    if (!(entry = PlpNewEntry()))
        return;

    entry->ProcessId = Event->ProcessId;
    entry->Key = Event->Key;
    entry->StartTime = Time;
    entry->Flags = LE_RUNNING;
    PlpFillFromEvent(entry, Event, Time, HeaderProcessId);

    for (i = 0; i < PL_ORPHAN_COUNT; i++)
    {
        if (PlOrphans[i].Path && PlOrphans[i].ProcessId == entry->ProcessId &&
            _abs64(PlOrphans[i].Time - Time) < 5 * PL_TICKS_PER_SECOND)
        {
            entry->ImagePath = PlOrphans[i].Path;
            PlOrphans[i].Path = NULL;
            break;
        }
    }

    if (!PlpAppend(entry))
    {
        PlpFreeEntry(entry);
        return;
    }

    PlpHashInsert(entry);

    // The 'S' line is written once the image path is known (normally a few ms later, see PlpOnImageEvent).
    if (entry->ImagePath)
        PlpWriteRecord(entry, L'S');
}

static VOID PlpHandleEnd(
    _Inout_ PPL_PROCESS_EVENT Event,
    _In_ LONG64 Time
    )
{
    PLOG_ENTRY entry = PlpHashFind(Event->ProcessId);
    LONG i;
    ULONG scanned = 0;

    if (entry && !PlpKeysMatch(entry->Key, Event->Key))
    {
        // The PID already belongs to a newer process: the one ending was detached (see PlpDetach).
        entry = NULL;

        for (i = (LONG)PlEntryCount - 1; i >= 0 && scanned < 8192; i--, scanned++)
        {
            PLOG_ENTRY candidate = PlEntries[i];

            if (candidate->ProcessId == Event->ProcessId && (candidate->Flags & LE_RUNNING) &&
                PlpKeysMatch(candidate->Key, Event->Key) && !PlpInHash(candidate))
            {
                entry = candidate;
                break;
            }
        }
    }
    else if (!entry)
    {
        for (i = (LONG)PlEntryCount - 1; i >= 0 && scanned < 8192; i--, scanned++)
        {
            PLOG_ENTRY candidate = PlEntries[i];

            if (candidate->ProcessId == Event->ProcessId && (candidate->Flags & LE_RUNNING) &&
                PlpKeysMatch(candidate->Key, Event->Key))
            {
                entry = candidate;
                break;
            }
        }
    }

    if (entry)
    {
        PlpHashRemove(entry);
    }
    else
    {
        if (!(entry = PlpNewEntry()))
            return;

        entry->ProcessId = Event->ProcessId;
        entry->Flags = LE_START_UNKNOWN;
    }

    if (Event->Key)
        entry->Key = Event->Key;

    entry->ExitTime = Time;
    entry->ExitCode = Event->ExitStatus;
    entry->Flags &= ~LE_RUNNING;
    PlpFillFromEvent(entry, Event, entry->StartTime ? entry->StartTime : Time, 0);

    if (!(entry->Flags & LE_VISIBLE))
    {
        if (!entry->StartTime)
            entry->Flags |= LE_START_UNKNOWN;

        if (!PlpAppend(entry))
        {
            PlpFreeEntry(entry);
            return;
        }
    }

    PlpWriteRecord(entry, L'E');
}

// Rundown of processes that were already running when the session started: fill what enumeration lacks.
static VOID PlpHandleRundown(
    _Inout_ PPL_PROCESS_EVENT Event
    )
{
    PLOG_ENTRY entry = PlpHashFind(Event->ProcessId);

    if (!entry)
    {
        if (!(entry = PlpNewEntry()))
            return;

        entry->ProcessId = Event->ProcessId;
        entry->Flags = LE_RUNNING | LE_PREEXISTING;
        PlpHashInsert(entry);
    }

    if (!entry->Key)
        entry->Key = Event->Key;

    PlpFillFromEvent(entry, Event, entry->StartTime ? entry->StartTime : PlpNow(), 0);
}

static VOID PlpOnProcessEvent(
    _In_ PEVENT_RECORD Record
    )
{
    UCHAR opcode = Record->EventHeader.EventDescriptor.Opcode;
    PL_PROCESS_EVENT event;
    LONG64 time = Record->EventHeader.TimeStamp.QuadPart;

    if (opcode != EVENT_TRACE_TYPE_START && opcode != EVENT_TRACE_TYPE_END && opcode != EVENT_TRACE_TYPE_DC_START)
        return;

    if (!PlpParseProcessEvent(Record, &event))
    {
        PlpFreeProcessEvent(&event);
        return;
    }

    AcquireSRWLockExclusive(&PlLock);

    switch (opcode)
    {
    case EVENT_TRACE_TYPE_START:
        PlStartCount++;
        PlpHandleStart(&event, time, Record->EventHeader.ProcessId);
        break;
    case EVENT_TRACE_TYPE_END:
        PlEndCount++;
        PlpHandleEnd(&event, time);
        break;
    case EVENT_TRACE_TYPE_DC_START:
        PlpHandleRundown(&event);
        break;
    }

    ReleaseSRWLockExclusive(&PlLock);
    InterlockedIncrement(&PlVersion);
    PlpFreeProcessEvent(&event);
}

static BOOLEAN PlpWantsImage(
    _In_opt_ PLOG_ENTRY Entry,
    _In_ LONG64 Time
    )
{
    if (!Entry)
        return TRUE; // not seen yet: keep it for the Start event

    return !Entry->ImagePath && !(Entry->Flags & LE_PREEXISTING) && Entry->StartTime &&
        Time - Entry->StartTime < 10 * PL_TICKS_PER_SECOND;
}

// Image_Load (V2/V3): ImageBase, ImageSize, ProcessId, ImageChecksum, TimeDateStamp, 4 bytes, DefaultBase,
// Reserved1-4, FileName (UTF-16). The first load for a new process is its own image.
static VOID PlpOnImageEvent(
    _In_ PEVENT_RECORD Record
    )
{
    PUCHAR data = Record->UserData;
    PUCHAR end = data + Record->UserDataLength;
    ULONG ptr = PlpPointerSize(Record);
    LONG64 time = Record->EventHeader.TimeStamp.QuadPart;
    ULONG processId;
    PLOG_ENTRY entry;
    BOOLEAN wanted;
    PWSTR ntPath;
    PWSTR path;

    if (Record->EventHeader.EventDescriptor.Opcode != 10 || !data || (SIZE_T)(end - data) < 3 * ptr + 32)
        return;

    PlImageCount++;
    memcpy(&processId, data + 2 * ptr, 4);

    AcquireSRWLockShared(&PlLock);
    wanted = PlpWantsImage(PlpHashFind(processId), time);
    ReleaseSRWLockShared(&PlLock);

    if (!wanted)
        return;

    PlpReadWide(data + 3 * ptr + 32, end, &ntPath);
    path = PlpToWin32Path(ntPath);
    free(ntPath);

    if (!path)
        return;

    AcquireSRWLockExclusive(&PlLock);

    entry = PlpHashFind(processId);

    if (entry)
    {
        if (PlpWantsImage(entry, time))
        {
            entry->ImagePath = path;
            path = NULL;
            PlpWriteRecord(entry, L'S');
            InterlockedIncrement(&PlVersion);
        }
    }
    else
    {
        LONG i;
        ULONG scanned = 0;

        // Its End may have been delivered first (out of order): attach to the finished entry.
        for (i = (LONG)PlEntryCount - 1; i >= 0 && scanned < 256; i--, scanned++)
        {
            PLOG_ENTRY candidate = PlEntries[i];

            if (candidate->ProcessId == processId && !candidate->ImagePath && candidate->ExitTime &&
                !(candidate->Flags & LE_PREEXISTING) && _abs64(candidate->ExitTime - time) < 10 * PL_TICKS_PER_SECOND)
            {
                candidate->ImagePath = path;
                path = NULL;
                PlpWriteRecord(candidate, L'E');
                InterlockedIncrement(&PlVersion);
                break;
            }
        }
    }

    if (path && !entry)
    {
        PL_ORPHAN_IMAGE *orphan = &PlOrphans[PlOrphanNext++ % PL_ORPHAN_COUNT];

        free(orphan->Path);
        orphan->ProcessId = processId;
        orphan->Time = time;
        orphan->Path = path;
        path = NULL;
    }

    ReleaseSRWLockExclusive(&PlLock);
    free(path);
}

static VOID WINAPI PlpEventRecordCallback(
    _In_ PEVENT_RECORD Record
    )
{
    if (IsEqualGUID(&Record->EventHeader.ProviderId, &PlProcessGuid))
        PlpOnProcessEvent(Record);
    else if (IsEqualGUID(&Record->EventHeader.ProviderId, &PlImageLoadGuid))
        PlpOnImageEvent(Record);
}

// ---- session -----------------------------------------------------------------------------------------------

typedef struct _PL_TRACE_PROPERTIES
{
    EVENT_TRACE_PROPERTIES Properties;
    WCHAR LoggerName[64];
} PL_TRACE_PROPERTIES;

static VOID PlpInitProperties(
    _Out_ PL_TRACE_PROPERTIES *Buffer
    )
{
    memset(Buffer, 0, sizeof(PL_TRACE_PROPERTIES));
    Buffer->Properties.Wnode.BufferSize = sizeof(PL_TRACE_PROPERTIES);
    Buffer->Properties.Wnode.Guid = PlSessionGuid;
    Buffer->Properties.Wnode.ClientContext = 1; // QPC timestamps (converted to FILETIME for us)
    Buffer->Properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    Buffer->Properties.BufferSize = 64; // KB
    Buffer->Properties.MinimumBuffers = 4;
    Buffer->Properties.MaximumBuffers = 64;
    Buffer->Properties.FlushTimer = 1;
    Buffer->Properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    Buffer->Properties.EnableFlags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_IMAGE_LOAD | EVENT_TRACE_FLAG_NO_SYSCONFIG;
    Buffer->Properties.LoggerNameOffset = FIELD_OFFSET(PL_TRACE_PROPERTIES, LoggerName);
}

static VOID PlpStopSession(
    VOID
    )
{
    PL_TRACE_PROPERTIES properties;

    PlpInitProperties(&properties);
    ControlTraceW(0, PL_SESSION_NAME, &properties.Properties, EVENT_TRACE_CONTROL_STOP);
}

static ULONG PlpStartSession(
    VOID
    )
{
    PL_TRACE_PROPERTIES properties;
    TRACEHANDLE sessionHandle = 0;

    // A session left behind by a System Informer that was killed (install.ps1 does that): replace it.
    PlpStopSession();
    PlpInitProperties(&properties);
    return StartTraceW(&sessionHandle, PL_SESSION_NAME, &properties.Properties);
}

VOID PlGetStats(
    _Out_ PPL_STATS Stats
    )
{
    PL_TRACE_PROPERTIES properties;

    memset(Stats, 0, sizeof(PL_STATS));
    Stats->SessionStatus = PlSessionStatus;
    Stats->Active = PlActive;
    Stats->Starts = PlStartCount;
    Stats->Ends = PlEndCount;
    Stats->Images = PlImageCount;
    Stats->Entries = PlEntryCount;

    if (PlActive)
    {
        PlpInitProperties(&properties);

        if (ControlTraceW(0, PL_SESSION_NAME, &properties.Properties, EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS)
        {
            Stats->EventsLost = properties.Properties.EventsLost;
            Stats->BuffersLost = properties.Properties.RealTimeBuffersLost;
        }
    }
}

VOID PlWriteStatusFile(
    VOID
    )
{
    PL_STATS stats;
    WCHAR now[64];
    PL_BUF text = { 0 };
    HANDLE file;

    if (!PlStatusFilePath[0])
        return;

    PlGetStats(&stats);
    PlFormatTime(PlpNow(), now, RTL_NUMBER_OF(now));
    PlBufAppendF(&text, L"ProcessLog status at %s\r\n", now);
    PlBufAppendF(&text, L"session %s: StartTrace=%lu active=%u\r\n", PL_SESSION_NAME, stats.SessionStatus, stats.Active);
    PlBufAppendF(&text, L"events: starts=%llu ends=%llu imageLoads=%llu lost=%lu realTimeBuffersLost=%lu\r\n",
        stats.Starts, stats.Ends, stats.Images, stats.EventsLost, stats.BuffersLost);
    PlBufAppendF(&text, L"entries in memory: %lu\r\n", stats.Entries);
    PlBufAppendF(&text, L"history load: %lu lines in %lu ms, %lu marked exit not seen\r\n", PlLoadedLines, PlLoadMs, PlLoadExitUnknown);

    file = CreateFileW(PlStatusFilePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);

    if (file != INVALID_HANDLE_VALUE)
    {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, text.Data, (int)text.Length, NULL, 0, NULL, NULL);
        PSTR utf8;

        if (bytes > 0 && (utf8 = malloc(bytes)))
        {
            DWORD written;

            WideCharToMultiByte(CP_UTF8, 0, text.Data, (int)text.Length, utf8, bytes, NULL, NULL);
            WriteFile(file, utf8, bytes, &written, NULL);
            free(utf8);
        }

        CloseHandle(file);
    }

    PlBufFree(&text);
}

// ---- startup -----------------------------------------------------------------------------------------------

typedef struct _PL_LIVE_PROCESS
{
    ULONG ProcessId;
    ULONG ParentId;
    ULONG SessionId;
    LONG64 CreateTime;
    PWSTR Name;
} PL_LIVE_PROCESS;

static VOID PlpInitialize(
    VOID
    )
{
    PWSTR appData;
    WCHAR oldPath[MAX_PATH + 4];
    PVOID processes;
    PSYSTEM_PROCESS_INFORMATION process;
    PL_LIVE_PROCESS *live = NULL;
    ULONG liveCount = 0;
    ULONG liveCapacity = 0;
    ULONG i;

    if (PlLogFilePath[0] == UNICODE_NULL && (appData = _wgetenv(L"APPDATA")))
    {
        _snwprintf_s(PlLogFilePath, RTL_NUMBER_OF(PlLogFilePath), _TRUNCATE, L"%s\\SystemInformer\\ProcessLog.tsv", appData);
        _snwprintf_s(PlStatusFilePath, RTL_NUMBER_OF(PlStatusFilePath), _TRUNCATE, L"%s\\SystemInformer\\ProcessLog.status.txt", appData);
    }

    // Processes alive right now (to tell which logged processes are still running, and to know the ones that
    // started before logging).
    if (NT_SUCCESS(PhEnumProcesses(&processes)))
    {
        for (process = PH_FIRST_PROCESS(processes); process; process = PH_NEXT_PROCESS(process))
        {
            if (!process->UniqueProcessId)
                continue;

            if (liveCount == liveCapacity)
            {
                PL_LIVE_PROCESS *grown = realloc(live, (liveCapacity = liveCapacity ? liveCapacity * 2 : 512) * sizeof(PL_LIVE_PROCESS));

                if (!grown)
                    break;

                live = grown;
            }

            live[liveCount].ProcessId = HandleToUlong(process->UniqueProcessId);
            live[liveCount].ParentId = HandleToUlong(process->InheritedFromUniqueProcessId);
            live[liveCount].SessionId = process->SessionId;
            live[liveCount].CreateTime = process->CreateTime.QuadPart;
            live[liveCount].Name = NULL;

            if (process->ImageName.Buffer && process->ImageName.Length)
            {
                live[liveCount].Name = malloc(process->ImageName.Length + sizeof(WCHAR));

                if (live[liveCount].Name)
                {
                    memcpy(live[liveCount].Name, process->ImageName.Buffer, process->ImageName.Length);
                    live[liveCount].Name[process->ImageName.Length / sizeof(WCHAR)] = UNICODE_NULL;
                }
            }

            liveCount++;
        }

        PhFree(processes);
    }

    AcquireSRWLockExclusive(&PlLock);

    if (PlLogFilePath[0])
    {
        _snwprintf_s(oldPath, RTL_NUMBER_OF(oldPath), _TRUNCATE, L"%s.1", PlLogFilePath);
        ULONG64 loadStart = GetTickCount64();

        PlLoading = TRUE;
        PlpLoadFile(oldPath);
        PlpLoadFile(PlLogFilePath);
        PlLoading = FALSE;

        for (i = 0; i < PlEntryCount; i++)
            PlEntries[i]->HashNext = NULL;
        memset(PlLoadHash, 0, sizeof(PlLoadHash));

        PlLoadMs = (ULONG)(GetTickCount64() - loadStart);
        PlpOpenLogFile();
    }

    // Logged processes without an exit: still running (same PID and create time) or exit missed.
    for (i = 0; i < PlEntryCount; i++)
    {
        PLOG_ENTRY entry = PlEntries[i];
        BOOLEAN alive = FALSE;

        if (!(entry->Flags & LE_RUNNING))
            continue;

        for (ULONG j = 0; j < liveCount; j++)
        {
            if (live[j].ProcessId == entry->ProcessId && entry->StartTime &&
                _abs64(live[j].CreateTime - entry->StartTime) < 2 * PL_TICKS_PER_SECOND && !PlpHashFind(entry->ProcessId))
            {
                alive = TRUE;
                break;
            }
        }

        if (alive)
        {
            PlpHashInsert(entry);
        }
        else
        {
            entry->Flags &= ~LE_RUNNING;
            entry->Flags |= LE_EXIT_UNKNOWN;
            PlLoadExitUnknown++;
            PlpWriteRecord(entry, L'E');
        }
    }

    PlpTrim();

    // Everything else that is running now: kept out of the log until it exits.
    for (i = 0; i < liveCount; i++)
    {
        PLOG_ENTRY entry;

        if (PlpHashFind(live[i].ProcessId) || !(entry = PlpNewEntry()))
            continue;

        entry->ProcessId = live[i].ProcessId;
        entry->ParentId = live[i].ParentId;
        entry->SessionId = live[i].SessionId;
        entry->StartTime = live[i].CreateTime;
        entry->Name = live[i].Name;
        live[i].Name = NULL;
        entry->Flags = LE_RUNNING | LE_PREEXISTING;
        PlpHashInsert(entry);
    }

    ReleaseSRWLockExclusive(&PlLock);

    for (i = 0; i < liveCount; i++)
        free(live[i].Name);
    free(live);

    // Full image paths of the already running processes (outside the lock: may be slow).
    {
        ULONG count;
        ULONG *ids = NULL;

        AcquireSRWLockShared(&PlLock);
        count = 0;
        for (i = 0; i < PL_HASH_SIZE; i++)
            for (PLOG_ENTRY entry = PlHash[i]; entry; entry = entry->HashNext)
                count++;
        if (count && (ids = malloc(count * sizeof(ULONG))))
        {
            count = 0;
            for (i = 0; i < PL_HASH_SIZE; i++)
                for (PLOG_ENTRY entry = PlHash[i]; entry; entry = entry->HashNext)
                    if (!entry->ImagePath)
                        ids[count++] = entry->ProcessId;
        }
        ReleaseSRWLockShared(&PlLock);

        for (i = 0; ids && i < count; i++)
        {
            PPH_STRING fileName = NULL;
            PWSTR path = NULL;
            PLOG_ENTRY entry;

            if (NT_SUCCESS(PhGetProcessImageFileNameById(UlongToHandle(ids[i]), &fileName, NULL)) && fileName)
            {
                path = PlpToWin32Path(fileName->Buffer);
                PhDereferenceObject(fileName);
            }

            if (!path)
                continue;

            AcquireSRWLockExclusive(&PlLock);
            if ((entry = PlpHashFind(ids[i])) && !entry->ImagePath)
            {
                entry->ImagePath = path;
                path = NULL;
            }
            ReleaseSRWLockExclusive(&PlLock);
            free(path);
        }

        free(ids);
    }

    InterlockedIncrement(&PlVersion);
}

static DWORD WINAPI PlpCaptureThread(
    _In_ PVOID Parameter
    )
{
    UNREFERENCED_PARAMETER(Parameter);

    PlpInitialize();

    while (!PlExiting)
    {
        PlSessionStatus = PlpStartSession();

        if (PlSessionStatus == ERROR_SUCCESS && !PlExiting)
        {
            EVENT_TRACE_LOGFILEW logFile;
            TRACEHANDLE traceHandle;

            memset(&logFile, 0, sizeof(logFile));
            logFile.LoggerName = PL_SESSION_NAME;
            logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
            logFile.EventRecordCallback = PlpEventRecordCallback;

            traceHandle = OpenTraceW(&logFile);

            if (traceHandle != INVALID_PROCESSTRACE_HANDLE)
            {
                PlActive = TRUE;
                PlWriteStatusFile();
                ProcessTrace(&traceHandle, 1, NULL, NULL);
                PlActive = FALSE;
                CloseTrace(traceHandle);
            }
            else
            {
                PlSessionStatus = GetLastError();
            }
        }

        PlWriteStatusFile();

        if (PlExiting)
            break;

        // Session failed or was stopped from outside: try again shortly.
        WaitForSingleObject(PlExitEvent, 5000);
    }

    return 0;
}

VOID PlStartCapture(
    VOID
    )
{
    ULONG value;

    value = PhGetIntegerSetting(SETTING_NAME_MAX_FILE_MB);
    if (value >= 1 && value <= 1024)
        PlMaxFileBytes = (ULONG64)value * 1024 * 1024;

    value = PhGetIntegerSetting(SETTING_NAME_MAX_ENTRIES);
    if (value >= 1000 && value <= 1000000)
        PlMaxEntries = value;

    PlExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    PlThread = CreateThread(NULL, 0, PlpCaptureThread, NULL, 0, NULL);
}

VOID PlStopCapture(
    VOID
    )
{
    PlExiting = TRUE;

    if (PlExitEvent)
        SetEvent(PlExitEvent);

    PlpStopSession();

    if (PlThread)
    {
        WaitForSingleObject(PlThread, 3000);
        CloseHandle(PlThread);
        PlThread = NULL;
    }

    AcquireSRWLockExclusive(&PlLock);

    if (PlFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(PlFile);
        PlFile = INVALID_HANDLE_VALUE;
    }

    ReleaseSRWLockExclusive(&PlLock);
}
