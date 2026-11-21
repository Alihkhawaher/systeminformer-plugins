/*
 * MemBreakdown - System Informer plugin that answers "where is my memory going?".
 *
 * A "RAM breakdown" section in System Information:
 *  - graphs (every tick): process private working sets, other in-use memory, standby (file cache), modified, free;
 *  - Breakdown tab: every physical page counted by use and list from a Superfetch PFN scan (like RAMMap's Use
 *    Counts), plus hardware-reserved, so the rows add up to the installed RAM;
 *  - Processes tab: top processes by private working set, with commit; double-click opens the process;
 *  - Pool tags tab: top kernel pool tags, described from System Informer's PoolTag.txt;
 *  - Files tab: files holding the most cached pages (names found through open handles).
 *
 * The PFN scan (memcore.c) runs on its own thread, only while this section is open, every
 * MemBreakdown.ScanIntervalSeconds (default 30; 0 = only when the section is opened).
 */

#include <phdk.h>
#include <settings.h>
#include <hndlinfo.h>
#include "memcore.h"
#include "resource.h"

#define PLUGIN_NAME L"MemBreakdown"
#define SETTING_NAME_SCAN_INTERVAL (PLUGIN_NAME L".ScanIntervalSeconds")
#define GRAPH_PADDING 3
#define METRIC_COUNT 5
#define MAX_ROWS 128
#define MAX_COLUMNS 6
#define TAB_COUNT 4

typedef enum _METRIC
{
    MetricPrivate,
    MetricOther,
    MetricStandby,
    MetricModified,
    MetricFree
} METRIC;

typedef enum _TAB
{
    TabBreakdown,
    TabProcesses,
    TabPool,
    TabFiles
} TAB;

typedef struct _POOLTAG_ENTRY
{
    ULONG Tag;
    PPH_STRING Driver;
    PPH_STRING Description;
} POOLTAG_ENTRY, *PPOOLTAG_ENTRY;

typedef struct _LIST_ROW
{
    WCHAR Name[400];
    WCHAR Cols[MAX_COLUMNS - 1][64];
    LPARAM Param;
} LIST_ROW, *PLIST_ROW;

// ---- state -----------------------------------------------------------------

static PPH_PLUGIN PluginInstance;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION SystemInformationInitializingCallbackRegistration;

// fast tier, written on the provider thread
static MB_FAST Fast;
static ULONG64 PrivateWsBytes;
static ULONG ProcessCount;
static ULONG64 Values[METRIC_COUNT];        // latest sample, bytes
static PH_CIRCULAR_BUFFER_FLOAT History[METRIC_COUNT];
static PH_CIRCULAR_BUFFER_FLOAT InUseHistory; // bytes, for the sidebar graph
static BOOLEAN Sampled;

// PFN scan, written on the scan thread
static PH_QUEUED_LOCK ScanLock = PH_QUEUED_LOCK_INIT;
static PMB_PFN_SCAN LastScan;
static PPH_STRING FileNames[MB_MAX_FILES];
static ULONG ScanSequence;
static HANDLE ScanEvent;
static volatile BOOLEAN ScanRequested;
static volatile BOOLEAN ScanRunning;
static ULONG ScanIntervalSeconds = 30;
static ULONG64 LastScanTick;

// pool tag descriptions (UI thread)
static PPOOLTAG_ENTRY PoolDb;
static ULONG PoolDbCount;
static BOOLEAN PoolDbLoaded;

// sysinfo window (UI thread)
static PPH_SYSINFO_SECTION Section;
static HWND Dialog;
static LONG DialogDpi;
static PH_LAYOUT_MANAGER LayoutManager;
static RECT GraphMargin;
static RECT GraphMarginScaled;
static HWND GraphHandle[METRIC_COUNT];
static PH_GRAPH_STATE GraphState[METRIC_COUNT];
static HWND Panel;
static HWND StatusLabel;
static HWND Tabs;
static HWND Lists[TAB_COUNT];
static ULONG CurrentTab;
static ULONG ShownFilesSequence = ULONG_MAX;
static LIST_ROW Rows[MAX_ROWS];

static CONST ULONG MetricLabelIds[METRIC_COUNT] = { IDC_LABEL0, IDC_LABEL1, IDC_LABEL2, IDC_LABEL3, IDC_LABEL4 };
static CONST PCWSTR MetricNames[METRIC_COUNT] =
{
    L"Process private working sets",
    L"Other in use (kernel, drivers, mapped files)",
    L"Standby (file cache)",
    L"Modified (waiting to be written)",
    L"Free and zeroed"
};

// ---- helpers ---------------------------------------------------------------

static PWSTR FormatSize(_In_ ULONG64 Bytes, _Out_writes_(Count) PWSTR Buffer, _In_ SIZE_T Count)
{
    PH_FORMAT format[1];

    PhInitFormatSize(&format[0], Bytes);
    if (!PhFormatToBuffer(format, 1, Buffer, Count * sizeof(WCHAR), NULL))
        Buffer[0] = UNICODE_NULL;

    return Buffer;
}

static PWSTR FormatPercent(_In_ ULONG64 Part, _In_ ULONG64 Whole, _Out_writes_(Count) PWSTR Buffer, _In_ SIZE_T Count)
{
    PH_FORMAT format[2];

    PhInitFormatF(&format[0], Whole ? (FLOAT)((DOUBLE)Part * 100.0 / (DOUBLE)Whole) : 0.0f, 1);
    PhInitFormatC(&format[1], L'%');
    if (!PhFormatToBuffer(format, 2, Buffer, Count * sizeof(WCHAR), NULL))
        Buffer[0] = UNICODE_NULL;

    return Buffer;
}

static VOID CopyText(_Out_writes_(Count) PWSTR Destination, _In_ SIZE_T Count, _In_opt_ PCWSTR Source)
{
    wcsncpy_s(Destination, Count, Source ? Source : L"", _TRUNCATE);
}

static ULONG64 PagesToBytes(_In_ ULONG64 Pages)
{
    return Pages * PAGE_SIZE;
}

static ULONG64 StandbyPages(_In_ PSYSTEM_MEMORY_LIST_INFORMATION Lists)
{
    ULONG64 total = 0;

    for (ULONG p = 0; p < 8; p++)
        total += Lists->PageCountByPriority[p];

    return total;
}

// Replaces the rows of a report list view in place (keeps selection and scroll position).
static VOID FillList(_In_ HWND List, _In_ ULONG Count, _In_ ULONG Columns)
{
    LONG rows;

    SendMessage(List, WM_SETREDRAW, FALSE, 0);
    rows = ListView_GetItemCount(List);

    for (ULONG i = 0; i < Count; i++)
    {
        if ((LONG)i < rows)
        {
            LVITEM item = { 0 };

            PhSetListViewSubItem(List, i, 0, Rows[i].Name);
            item.mask = LVIF_PARAM;
            item.iItem = i;
            item.lParam = Rows[i].Param;
            ListView_SetItem(List, &item);
        }
        else
        {
            PhAddListViewItem(List, MAXINT, Rows[i].Name, (PVOID)Rows[i].Param);
        }

        for (ULONG c = 1; c < Columns; c++)
            PhSetListViewSubItem(List, i, c, Rows[i].Cols[c - 1]);
    }

    while (ListView_GetItemCount(List) > (LONG)Count)
        ListView_DeleteItem(List, ListView_GetItemCount(List) - 1);

    SendMessage(List, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(List, NULL, FALSE);
}

static VOID ShowProcessProperties(_In_ HWND Parent, _In_ ULONG ProcessId)
{
    PPH_PROCESS_ITEM processItem = PhReferenceProcessItem(UlongToHandle(ProcessId));

    if (processItem)
    {
        PPH_PROCESS_PROPCONTEXT propContext = PhCreateProcessPropContext(NULL, processItem);

        if (propContext)
        {
            PhShowProcessProperties(propContext);
            PhDereferenceObject(propContext);
        }

        PhDereferenceObject(processItem);
    }
    else
    {
        PhShowError2(Parent, L"Unable to open the process.", L"Process %lu is no longer running.", ProcessId);
    }
}

// ---- fast sampling (provider thread) ---------------------------------------

static VOID SampleFast(VOID)
{
    PPH_PROCESS_ITEM* processes;
    ULONG count;
    ULONG64 privateWs = 0;
    ULONG64 standby, modified, free, active, inUse;
    MB_FAST fast;

    MbQueryFast(&fast);

    PhEnumProcessItems(&processes, &count);

    {
        for (ULONG i = 0; i < count; i++)
        {
            privateWs += processes[i]->WorkingSetPrivateSize;
            PhDereferenceObject(processes[i]);
        }

        PhFree(processes);
        ProcessCount = count;
    }

    standby = PagesToBytes(StandbyPages(&fast.Lists));
    modified = PagesToBytes(fast.Lists.ModifiedPageCount + fast.Lists.ModifiedNoWritePageCount);
    free = PagesToBytes(fast.Lists.ZeroPageCount + fast.Lists.FreePageCount);
    inUse = PagesToBytes(fast.PhysicalPages);
    inUse = inUse > standby + modified + free + PagesToBytes(fast.Lists.BadPageCount) ? inUse - standby - modified - free - PagesToBytes(fast.Lists.BadPageCount) : 0;
    active = inUse;

    Fast = fast;
    PrivateWsBytes = privateWs;
    Values[MetricPrivate] = privateWs < active ? privateWs : active;
    Values[MetricOther] = active - Values[MetricPrivate];
    Values[MetricStandby] = standby;
    Values[MetricModified] = modified;
    Values[MetricFree] = free;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
        PhAddItemCircularBuffer_FLOAT(&History[m], (FLOAT)Values[m]);
    PhAddItemCircularBuffer_FLOAT(&InUseHistory, (FLOAT)(active + modified));

    Sampled = TRUE;
}

// ---- PFN scan (scan thread) ------------------------------------------------

// Names for the top file objects, from any open handle that points at the same FILE_OBJECT.
static VOID ResolveFileNames(_In_ PMB_PFN_SCAN Scan, _Out_writes_(MB_MAX_FILES) PPH_STRING* Names)
{
    PSYSTEM_HANDLE_INFORMATION_EX handles;
    HANDLE processHandle = NULL;
    HANDLE processId = (HANDLE)ULONG_PTR_MAX;
    ULONG remaining = Scan->TopFileCount;

    memset(Names, 0, sizeof(PPH_STRING) * MB_MAX_FILES);

    if (!remaining || !NT_SUCCESS(PhEnumHandlesEx(&handles)))
        return;

    for (ULONG_PTR i = 0; i < handles->NumberOfHandles && remaining; i++)
    {
        PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX handle = &handles->Handles[i];

        for (ULONG j = 0; j < Scan->TopFileCount; j++)
        {
            PPH_STRING bestName = NULL;

            if (Names[j] || (ULONG_PTR)handle->Object != Scan->TopFiles[j].FileObject)
                continue;

            if (handle->UniqueProcessId != processId)
            {
                if (processHandle)
                    NtClose(processHandle);
                processHandle = NULL;
                processId = handle->UniqueProcessId;
                PhOpenProcess(&processHandle, PROCESS_DUP_HANDLE, processId);
            }

            if (processHandle && NT_SUCCESS(PhGetHandleInformation(processHandle, (HANDLE)handle->HandleValue,
                handle->ObjectTypeIndex, NULL, NULL, NULL, &bestName)) && bestName)
            {
                if (bestName->Length)
                {
                    Names[j] = bestName;
                    remaining--;
                }
                else
                {
                    PhDereferenceObject(bestName);
                }
            }

            break;
        }
    }

    if (processHandle)
        NtClose(processHandle);

    PhFree(handles);
}

// %APPDATA%\SystemInformer\MemBreakdown.log: status and totals of the last scan (numbers only, no file names).
static VOID WriteScanLog(_In_ PMB_PFN_SCAN Scan, _In_ ULONG ResolvedFiles)
{
    WCHAR path[MAX_PATH];
    PH_STRING_BUILDER sb;
    HANDLE file;
    ULONG64 standbyBefore = StandbyPages(&Scan->ListsBefore);

    if (!ExpandEnvironmentStringsW(L"%APPDATA%\\SystemInformer\\MemBreakdown.log", path, RTL_NUMBER_OF(path)))
        return;

    PhInitializeStringBuilder(&sb, 0x1000);
    PhAppendFormatStringBuilder(&sb, L"MemBreakdown scan: status 0x%08lx, ranges 0x%08lx (v%lu, %lu ranges), %llu of %llu pages in %lu ms\r\n",
        Scan->QueryStatus, Scan->RangesStatus, Scan->RangesVersion, Scan->RangeCount, Scan->ScannedPages, Scan->TotalPages, Scan->ElapsedMs);
    PhAppendFormatStringBuilder(&sb, L"installed %llu bytes, physical pages %llu\r\n", Fast.InstalledBytes, Fast.PhysicalPages);

    for (ULONG u = 0; u < MB_USE_COUNT; u++)
    {
        PhAppendFormatStringBuilder(&sb, L"use %2lu %-30s", u, MbUseName(u));
        for (ULONG l = 0; l < MB_LIST_COUNT; l++)
            PhAppendFormatStringBuilder(&sb, L" %10llu", Scan->Counts[u][l]);
        PhAppendStringBuilder2(&sb, L"\r\n");
    }

    PhAppendFormatStringBuilder(&sb, L"lists scan/kernel: zero %llu/%llu free %llu/%llu standby %llu/%llu modified %llu/%llu\r\n",
        Scan->ListTotals[MMPFNLIST_ZERO], (ULONG64)Scan->ListsBefore.ZeroPageCount,
        Scan->ListTotals[MMPFNLIST_FREE], (ULONG64)Scan->ListsBefore.FreePageCount,
        Scan->ListTotals[MMPFNLIST_STANDBY], standbyBefore,
        Scan->ListTotals[MMPFNLIST_MODIFIED], (ULONG64)Scan->ListsBefore.ModifiedPageCount);
    PhAppendFormatStringBuilder(&sb, L"file objects %lu, top %lu, names resolved %lu\r\n", Scan->FileObjectCount, Scan->TopFileCount, ResolvedFiles);
    PhAppendFormatStringBuilder(&sb, L"process private WS (sum) %llu bytes over %lu processes\r\n", PrivateWsBytes, ProcessCount);

    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file != INVALID_HANDLE_VALUE)
    {
        static CONST WCHAR bom = 0xFEFF;
        ULONG written;

        WriteFile(file, &bom, sizeof(bom), &written, NULL);
        WriteFile(file, sb.String->Buffer, (ULONG)sb.String->Length, &written, NULL);
        CloseHandle(file);
    }

    PhDeleteStringBuilder(&sb);
}

static BOOLEAN IsSectionVisible(VOID)
{
    HWND dialog = Dialog;

    return dialog && IsWindowVisible(dialog);
}

_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI ScanThreadStart(_In_ PVOID Parameter)
{
    for (;;)
    {
        LARGE_INTEGER timeout;
        PMB_PFN_SCAN scan;
        PMB_PFN_SCAN oldScan;
        PPH_STRING names[MB_MAX_FILES];
        PPH_STRING oldNames[MB_MAX_FILES];
        ULONG resolved = 0;

        timeout.QuadPart = -(LONGLONG)ScanIntervalSeconds * 1000 * PH_TIMEOUT_MS;
        NtWaitForSingleObject(ScanEvent, FALSE, ScanIntervalSeconds ? &timeout : NULL);

        if (!ScanRequested && !(ScanIntervalSeconds && IsSectionVisible()))
            continue;

        ScanRequested = FALSE;
        ScanRunning = TRUE;

        if (!(scan = PhAllocateSafe(sizeof(MB_PFN_SCAN))))
        {
            ScanRunning = FALSE;
            continue;
        }

        MbScanPfns(scan, TRUE);
        ResolveFileNames(scan, names);

        for (ULONG i = 0; i < MB_MAX_FILES; i++)
            if (names[i]) resolved++;

        PhAcquireQueuedLockExclusive(&ScanLock);
        oldScan = LastScan;
        memcpy(oldNames, FileNames, sizeof(FileNames));
        LastScan = scan;
        memcpy(FileNames, names, sizeof(FileNames));
        ScanSequence++;
        PhReleaseQueuedLockExclusive(&ScanLock);

        LastScanTick = NtGetTickCount64();
        ScanRunning = FALSE;

        if (oldScan)
            PhFree(oldScan);
        for (ULONG i = 0; i < MB_MAX_FILES; i++)
            PhClearReference(&oldNames[i]);

        WriteScanLog(scan, resolved);
    }

    return STATUS_SUCCESS;
}

static VOID RequestScan(_In_ BOOLEAN IfStale)
{
    if (!ScanEvent)
        return;

    if (IfStale && LastScanTick && ScanIntervalSeconds && NtGetTickCount64() - LastScanTick < (ULONG64)ScanIntervalSeconds * 1000)
        return;
    if (IfStale && LastScanTick && !ScanIntervalSeconds)
        return;

    ScanRequested = TRUE;
    NtSetEvent(ScanEvent, NULL);
}

// ---- pool tags -------------------------------------------------------------

static int __cdecl PoolDbCompare(_In_ const void* A, _In_ const void* B)
{
    ULONG a = ((const POOLTAG_ENTRY*)A)->Tag;
    ULONG b = ((const POOLTAG_ENTRY*)B)->Tag;

    return a < b ? -1 : a > b;
}

static PPH_STRING TrimToString(_In_ PCSTR Start, _In_ PCSTR End)
{
    while (Start < End && (*Start == ' ' || *Start == '\t'))
        Start++;
    while (End > Start && (End[-1] == ' ' || End[-1] == '\t' || End[-1] == '\r'))
        End--;

    return PhConvertMultiByteToUtf16Ex((PSTR)Start, End - Start);
}

// "<Tag> - <binary> - <description>" lines of System Informer's PoolTag.txt.
static VOID LoadPoolDb(VOID)
{
    WCHAR path[MAX_PATH];
    PWSTR slash;
    HANDLE file = INVALID_HANDLE_VALUE;
    PSTR text;
    ULONG size;
    ULONG read = 0;
    ULONG capacity = 4096;
    PCSTR line;
    PCSTR end;
    static CONST PCWSTR candidates[] = { L"PoolTag.txt", L"Resources\\pooltag.txt" };

    PoolDbLoaded = TRUE;

    for (ULONG c = 0; c < RTL_NUMBER_OF(candidates) && file == INVALID_HANDLE_VALUE; c++)
    {
        if (!GetModuleFileNameW(NULL, path, RTL_NUMBER_OF(path)) || !(slash = wcsrchr(path, L'\\')))
            return;

        slash[1] = UNICODE_NULL;
        if (wcscat_s(path, RTL_NUMBER_OF(path), candidates[c]))
            continue;

        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    if (file == INVALID_HANDLE_VALUE)
        return;

    size = GetFileSize(file, NULL);

    if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024 || !(text = PhAllocateSafe(size + 1)))
    {
        CloseHandle(file);
        return;
    }

    ReadFile(file, text, size, &read, NULL);
    CloseHandle(file);
    text[read] = 0;

    PoolDb = PhAllocate(capacity * sizeof(POOLTAG_ENTRY));
    line = text;
    end = text + read;

    while (line < end)
    {
        PCSTR next = memchr(line, '\n', end - line);
        PCSTR lineEnd = next ? next : end;
        PCSTR sep1 = NULL;
        PCSTR sep2 = NULL;

        for (PCSTR p = line; p + 2 < lineEnd; p++)
        {
            if (p[0] == ' ' && p[1] == '-' && p[2] == ' ')
            {
                if (!sep1) sep1 = p;
                else { sep2 = p; break; }
            }
        }

        if (sep1 && sep1 > line && line[0] != ' ' && line[0] != '\t' && line[0] != '/' &&
            !(lineEnd - line >= 3 && _strnicmp(line, "rem", 3) == 0 && (line[3] == ' ' || line[3] == '\r' || line + 3 == lineEnd)))
        {
            CHAR tag[4] = { ' ', ' ', ' ', ' ' };
            ULONG tagLength = (ULONG)min(sep1 - line, 4);

            memcpy(tag, line, tagLength);

            if (PoolDbCount == capacity)
            {
                capacity *= 2;
                PoolDb = PhReAllocate(PoolDb, capacity * sizeof(POOLTAG_ENTRY));
            }

            PoolDb[PoolDbCount].Tag = (ULONG)(UCHAR)tag[0] | ((ULONG)(UCHAR)tag[1] << 8) | ((ULONG)(UCHAR)tag[2] << 16) | ((ULONG)(UCHAR)tag[3] << 24);
            PoolDb[PoolDbCount].Driver = TrimToString(sep1 + 3, sep2 ? sep2 : lineEnd);
            PoolDb[PoolDbCount].Description = sep2 ? TrimToString(sep2 + 3, lineEnd) : PhReferenceEmptyString();
            PoolDbCount++;
        }

        line = lineEnd + 1;
    }

    PhFree(text);
    qsort(PoolDb, PoolDbCount, sizeof(POOLTAG_ENTRY), PoolDbCompare);
}

static PPOOLTAG_ENTRY FindPoolTag(_In_ ULONG Tag)
{
    POOLTAG_ENTRY key;

    key.Tag = Tag;
    return PoolDbCount ? bsearch(&key, PoolDb, PoolDbCount, sizeof(POOLTAG_ENTRY), PoolDbCompare) : NULL;
}

static int __cdecl PoolUsageCompare(_In_ const void* A, _In_ const void* B)
{
    const SYSTEM_POOLTAG* a = A;
    const SYSTEM_POOLTAG* b = B;
    ULONG64 ua = (ULONG64)a->PagedUsed + a->NonPagedUsed;
    ULONG64 ub = (ULONG64)b->PagedUsed + b->NonPagedUsed;

    return ua > ub ? -1 : ua < ub;
}

static VOID UpdatePoolList(VOID)
{
    static PVOID buffer;
    static ULONG bufferSize = 0x40000;
    PSYSTEM_POOLTAG_INFORMATION info;
    NTSTATUS status;
    ULONG returnLength = 0;
    ULONG count = 0;

    if (!PoolDbLoaded)
        LoadPoolDb();

    for (ULONG attempt = 0; attempt < 5; attempt++)
    {
        if (!buffer)
            buffer = PhAllocate(bufferSize);

        status = NtQuerySystemInformation(SystemPoolTagInformation, buffer, bufferSize, &returnLength);

        if (status != STATUS_INFO_LENGTH_MISMATCH && status != STATUS_BUFFER_TOO_SMALL)
            break;

        PhFree(buffer);
        buffer = NULL;
        bufferSize = max(bufferSize * 2, returnLength + 0x1000);
    }

    if (NT_SUCCESS(status) && buffer)
    {
        info = buffer;
        qsort(info->TagInfo, info->Count, sizeof(SYSTEM_POOLTAG), PoolUsageCompare);

        for (ULONG i = 0; i < info->Count && count < 100; i++)
        {
            PSYSTEM_POOLTAG tag = &info->TagInfo[i];
            PPOOLTAG_ENTRY entry = FindPoolTag(tag->TagUlong);
            PLIST_ROW row = &Rows[count++];

            for (ULONG c = 0; c < 4; c++)
            {
                UCHAR ch = tag->Tag[c];
                row->Name[c] = ch >= 0x20 && ch < 0x7f ? ch : L'.';
            }
            row->Name[4] = UNICODE_NULL;

            FormatSize(tag->NonPagedUsed, row->Cols[0], RTL_NUMBER_OF(row->Cols[0]));
            FormatSize(tag->PagedUsed, row->Cols[1], RTL_NUMBER_OF(row->Cols[1]));
            CopyText(row->Cols[2], RTL_NUMBER_OF(row->Cols[2]), entry ? PhGetString(entry->Driver) : L"");
            CopyText(row->Cols[3], RTL_NUMBER_OF(row->Cols[3]), entry ? PhGetString(entry->Description) : L"");
            row->Param = 0;
        }
    }
    else
    {
        PH_FORMAT format[2];

        PhInitFormatS(&format[0], L"Pool tag query failed: 0x");
        PhInitFormatX(&format[1], status);
        PhFormatToBuffer(format, 2, Rows[0].Name, sizeof(Rows[0].Name), NULL);
        memset(Rows[0].Cols, 0, sizeof(Rows[0].Cols));
        Rows[0].Param = 0;
        count = 1;
    }

    FillList(Lists[TabPool], count, 5);
}

// ---- breakdown -------------------------------------------------------------

typedef struct _BD_ROW
{
    PCWSTR Name;
    ULONG64 Total;
    ULONG64 Active;
    ULONG64 Standby;
    ULONG64 Modified;
    BOOLEAN Split;      // Active/Standby/Modified columns apply
    BOOLEAN Percent;    // part of the installed-RAM total
} BD_ROW, *PBD_ROW;

static int __cdecl BdRowCompare(_In_ const void* A, _In_ const void* B)
{
    const BD_ROW* a = A;
    const BD_ROW* b = B;

    return a->Total > b->Total ? -1 : a->Total < b->Total;
}

static VOID EmitBdRow(_Inout_ PULONG Count, _In_ PBD_ROW Row, _In_ ULONG64 Installed)
{
    PLIST_ROW row;

    if (*Count >= MAX_ROWS)
        return;

    row = &Rows[(*Count)++];
    memset(row->Cols, 0, sizeof(row->Cols));
    CopyText(row->Name, RTL_NUMBER_OF(row->Name), Row->Name);
    row->Param = 0;

    if (Row->Total || Row->Percent)
        FormatSize(Row->Total, row->Cols[0], RTL_NUMBER_OF(row->Cols[0]));

    if (Row->Split)
    {
        if (Row->Active) FormatSize(Row->Active, row->Cols[1], RTL_NUMBER_OF(row->Cols[1]));
        if (Row->Standby) FormatSize(Row->Standby, row->Cols[2], RTL_NUMBER_OF(row->Cols[2]));
        if (Row->Modified) FormatSize(Row->Modified, row->Cols[3], RTL_NUMBER_OF(row->Cols[3]));
    }

    if (Row->Percent)
        FormatPercent(Row->Total, Installed, row->Cols[4], RTL_NUMBER_OF(row->Cols[4]));
}

static VOID UpdateBreakdownList(VOID)
{
    static CONST PCWSTR priorityNames[8] =
    {
        L"    Standby priority 0 (lowest)", L"    Standby priority 1", L"    Standby priority 2", L"    Standby priority 3",
        L"    Standby priority 4", L"    Standby priority 5", L"    Standby priority 6", L"    Standby priority 7 (highest)"
    };
    BD_ROW rows[MB_USE_COUNT];
    BD_ROW row;
    ULONG useRows = 0;
    ULONG count = 0;
    ULONG64 installed = Fast.InstalledBytes;
    ULONG64 sumActive = 0, sumStandby = 0, sumModified = 0, sumTotal = 0;
    WCHAR commitName[64];

    PhAcquireQueuedLockShared(&ScanLock);

    if (LastScan && NT_SUCCESS(LastScan->QueryStatus))
    {
        PMB_PFN_SCAN scan = LastScan;
        ULONG64 reserved;

        for (ULONG u = 0; u < MB_USE_COUNT; u++)
        {
            ULONG64 active = PagesToBytes(scan->Counts[u][MMPFNLIST_ACTIVE] + scan->Counts[u][MMPFNLIST_TRANSITION]);
            ULONG64 standby = PagesToBytes(scan->Counts[u][MMPFNLIST_STANDBY]);
            ULONG64 modified = PagesToBytes(scan->Counts[u][MMPFNLIST_MODIFIED] + scan->Counts[u][MMPFNLIST_MODIFIEDNOWRITE]);

            if (active + standby + modified == 0)
                continue;

            memset(&rows[useRows], 0, sizeof(BD_ROW));
            rows[useRows].Name = MbUseName(u);
            rows[useRows].Active = active;
            rows[useRows].Standby = standby;
            rows[useRows].Modified = modified;
            rows[useRows].Total = active + standby + modified;
            rows[useRows].Split = TRUE;
            rows[useRows].Percent = TRUE;
            useRows++;

            sumActive += active;
            sumStandby += standby;
            sumModified += modified;
        }

        qsort(rows, useRows, sizeof(BD_ROW), BdRowCompare);

        for (ULONG i = 0; i < useRows; i++)
        {
            EmitBdRow(&count, &rows[i], installed);
            sumTotal += rows[i].Total;
        }

        memset(&row, 0, sizeof(row));
        row.Percent = TRUE;

        row.Name = L"Zeroed";
        row.Total = PagesToBytes(scan->ListTotals[MMPFNLIST_ZERO]);
        EmitBdRow(&count, &row, installed);
        sumTotal += row.Total;

        row.Name = L"Free";
        row.Total = PagesToBytes(scan->ListTotals[MMPFNLIST_FREE]);
        EmitBdRow(&count, &row, installed);
        sumTotal += row.Total;

        if (scan->ListTotals[MMPFNLIST_BAD])
        {
            row.Name = L"Bad (or moving between lists)";
            row.Total = PagesToBytes(scan->ListTotals[MMPFNLIST_BAD]);
            EmitBdRow(&count, &row, installed);
            sumTotal += row.Total;
        }

        reserved = installed > PagesToBytes(scan->TotalPages) ? installed - PagesToBytes(scan->TotalPages) : 0;
        row.Name = L"Hardware / firmware reserved";
        row.Total = reserved;
        EmitBdRow(&count, &row, installed);
        sumTotal += reserved;

        if (sumTotal != installed)
        {
            row.Name = L"Unaccounted";
            row.Total = installed > sumTotal ? installed - sumTotal : 0;
            EmitBdRow(&count, &row, installed);
            sumTotal += row.Total;
        }

        row.Name = L"Total installed RAM";
        row.Total = installed;
        row.Active = sumActive;
        row.Standby = sumStandby;
        row.Modified = sumModified;
        row.Split = TRUE;
        EmitBdRow(&count, &row, installed);
    }
    else
    {
        // No PFN scan (yet): the live counters, with the unexplained part labelled as such.
        ULONG64 bad = PagesToBytes(Fast.Lists.BadPageCount);
        ULONG64 reserved = installed > PagesToBytes(Fast.PhysicalPages) ? installed - PagesToBytes(Fast.PhysicalPages) : 0;

        memset(&row, 0, sizeof(row));
        row.Percent = TRUE;

        row.Name = L"Process private working sets";
        row.Total = Values[MetricPrivate];
        EmitBdRow(&count, &row, installed);
        row.Name = L"Other in use: kernel, drivers, mapped files (details after the PFN scan)";
        row.Total = Values[MetricOther];
        EmitBdRow(&count, &row, installed);
        row.Name = L"Standby (file cache)";
        row.Total = Values[MetricStandby];
        EmitBdRow(&count, &row, installed);
        row.Name = L"Modified";
        row.Total = Values[MetricModified];
        EmitBdRow(&count, &row, installed);
        row.Name = L"Zeroed";
        row.Total = PagesToBytes(Fast.Lists.ZeroPageCount);
        EmitBdRow(&count, &row, installed);
        row.Name = L"Free";
        row.Total = PagesToBytes(Fast.Lists.FreePageCount);
        EmitBdRow(&count, &row, installed);
        if (bad)
        {
            row.Name = L"Bad";
            row.Total = bad;
            EmitBdRow(&count, &row, installed);
        }
        row.Name = L"Hardware / firmware reserved";
        row.Total = reserved;
        EmitBdRow(&count, &row, installed);
        row.Name = L"Total installed RAM";
        row.Total = installed;
        EmitBdRow(&count, &row, installed);
    }

    PhReleaseQueuedLockShared(&ScanLock);

    // Live extras, not part of the total above.
    memset(&row, 0, sizeof(row));
    row.Name = L"";
    EmitBdRow(&count, &row, installed);
    row.Name = L"Live (not added to the total):";
    EmitBdRow(&count, &row, installed);
    row.Name = L"  Process private working sets, sum over processes";
    row.Total = PrivateWsBytes;
    EmitBdRow(&count, &row, installed);
    row.Name = L"  Standby (file cache) by priority:";
    row.Total = PagesToBytes(StandbyPages(&Fast.Lists));
    EmitBdRow(&count, &row, installed);

    for (LONG p = 7; p >= 0; p--)
    {
        row.Name = priorityNames[p];
        row.Total = PagesToBytes(Fast.Lists.PageCountByPriority[p]);
        EmitBdRow(&count, &row, installed);
    }

    row.Name = L"  Kernel paged pool (resident)";
    row.Total = PagesToBytes(Fast.Perf.ResidentPagedPoolPage);
    EmitBdRow(&count, &row, installed);
    row.Name = L"  Kernel nonpaged pool";
    row.Total = PagesToBytes(Fast.Perf.NonPagedPoolPages);
    EmitBdRow(&count, &row, installed);
    row.Name = L"  System file cache working set";
    row.Total = Fast.Cache.CurrentSize;
    EmitBdRow(&count, &row, installed);

    {
        PH_FORMAT format[3];
        WCHAR limit[32];

        PhInitFormatS(&format[0], L"  Commit charge (virtual, not RAM) of ");
        PhInitFormatS(&format[1], FormatSize(PagesToBytes(Fast.Perf.CommitLimit), limit, RTL_NUMBER_OF(limit)));
        PhInitFormatS(&format[2], L" limit");
        if (!PhFormatToBuffer(format, 3, commitName, sizeof(commitName), NULL))
            commitName[0] = UNICODE_NULL;
        row.Name = commitName;
        row.Total = PagesToBytes(Fast.Perf.CommittedPages);
        EmitBdRow(&count, &row, installed);
    }

    FillList(Lists[TabBreakdown], count, 6);
}

// ---- processes -------------------------------------------------------------

static int __cdecl ProcessCompare(_In_ const void* A, _In_ const void* B)
{
    PPH_PROCESS_ITEM a = *(PPH_PROCESS_ITEM*)A;
    PPH_PROCESS_ITEM b = *(PPH_PROCESS_ITEM*)B;

    if (a->WorkingSetPrivateSize != b->WorkingSetPrivateSize)
        return a->WorkingSetPrivateSize > b->WorkingSetPrivateSize ? -1 : 1;

    return a->VmCounters.PagefileUsage > b->VmCounters.PagefileUsage ? -1 : a->VmCounters.PagefileUsage < b->VmCounters.PagefileUsage;
}

static VOID UpdateProcessList(VOID)
{
    PPH_PROCESS_ITEM* processes;
    ULONG processCount;
    ULONG count = 0;

    PhEnumProcessItems(&processes, &processCount);

    qsort(processes, processCount, sizeof(PPH_PROCESS_ITEM), ProcessCompare);

    for (ULONG i = 0; i < processCount; i++)
    {
        PPH_PROCESS_ITEM process = processes[i];

        if (count < 100 && process->ProcessId != SYSTEM_IDLE_PROCESS_ID && (process->WorkingSetPrivateSize || process->VmCounters.PagefileUsage))
        {
            PLIST_ROW row = &Rows[count++];
            PH_FORMAT format[1];

            CopyText(row->Name, RTL_NUMBER_OF(row->Name), PhGetString(process->ProcessName));
            PhInitFormatU(&format[0], HandleToUlong(process->ProcessId));
            if (!PhFormatToBuffer(format, 1, row->Cols[0], sizeof(row->Cols[0]), NULL))
                row->Cols[0][0] = UNICODE_NULL;
            FormatSize(process->WorkingSetPrivateSize, row->Cols[1], RTL_NUMBER_OF(row->Cols[1]));
            FormatSize(process->VmCounters.PagefileUsage, row->Cols[2], RTL_NUMBER_OF(row->Cols[2]));
            FormatSize(process->VmCounters.WorkingSetSize, row->Cols[3], RTL_NUMBER_OF(row->Cols[3]));
            row->Param = HandleToUlong(process->ProcessId);
        }

        PhDereferenceObject(process);
    }

    PhFree(processes);
    FillList(Lists[TabProcesses], count, 5);
}

// ---- files -----------------------------------------------------------------

static VOID UpdateFilesList(VOID)
{
    ULONG count = 0;

    PhAcquireQueuedLockShared(&ScanLock);

    if (LastScan && NT_SUCCESS(LastScan->QueryStatus))
    {
        PMB_PFN_SCAN scan = LastScan;
        ULONG64 listed[3] = { 0 };
        ULONG64 all[3];

        all[0] = scan->Counts[MMPFNUSE_FILE][MMPFNLIST_ACTIVE] + scan->Counts[MMPFNUSE_FILE][MMPFNLIST_TRANSITION];
        all[1] = scan->Counts[MMPFNUSE_FILE][MMPFNLIST_STANDBY];
        all[2] = scan->Counts[MMPFNUSE_FILE][MMPFNLIST_MODIFIED] + scan->Counts[MMPFNUSE_FILE][MMPFNLIST_MODIFIEDNOWRITE];

        for (ULONG i = 0; i < scan->TopFileCount && count < MAX_ROWS - 1; i++)
        {
            PMB_FILE_USAGE file = &scan->TopFiles[i];
            PLIST_ROW row = &Rows[count++];

            if (FileNames[i])
            {
                CopyText(row->Name, RTL_NUMBER_OF(row->Name), PhGetString(FileNames[i]));
            }
            else
            {
                PH_FORMAT format[2];

                PhInitFormatS(&format[0], L"(no open handle: file closed, or opened by the kernel) #");
                PhInitFormatU(&format[1], i + 1);
                if (!PhFormatToBuffer(format, 2, row->Name, sizeof(row->Name), NULL))
                    row->Name[0] = UNICODE_NULL;
            }

            FormatSize(PagesToBytes(file->Pages[0] + file->Pages[1] + file->Pages[2]), row->Cols[0], RTL_NUMBER_OF(row->Cols[0]));
            FormatSize(PagesToBytes(file->Pages[0]), row->Cols[1], RTL_NUMBER_OF(row->Cols[1]));
            FormatSize(PagesToBytes(file->Pages[1]), row->Cols[2], RTL_NUMBER_OF(row->Cols[2]));
            FormatSize(PagesToBytes(file->Pages[2]), row->Cols[3], RTL_NUMBER_OF(row->Cols[3]));
            row->Param = 0;

            for (ULONG c = 0; c < 3; c++)
                listed[c] += file->Pages[c];
        }

        if (scan->FileObjectCount > scan->TopFileCount)
        {
            PLIST_ROW row = &Rows[count++];
            PH_FORMAT format[3];
            ULONG64 rest[3];

            for (ULONG c = 0; c < 3; c++)
                rest[c] = all[c] > listed[c] ? all[c] - listed[c] : 0;

            PhInitFormatS(&format[0], L"Other files (");
            PhInitFormatU(&format[1], scan->FileObjectCount - scan->TopFileCount);
            PhInitFormatS(&format[2], L" more)");
            if (!PhFormatToBuffer(format, 3, row->Name, sizeof(row->Name), NULL))
                row->Name[0] = UNICODE_NULL;
            FormatSize(PagesToBytes(rest[0] + rest[1] + rest[2]), row->Cols[0], RTL_NUMBER_OF(row->Cols[0]));
            FormatSize(PagesToBytes(rest[0]), row->Cols[1], RTL_NUMBER_OF(row->Cols[1]));
            FormatSize(PagesToBytes(rest[1]), row->Cols[2], RTL_NUMBER_OF(row->Cols[2]));
            FormatSize(PagesToBytes(rest[2]), row->Cols[3], RTL_NUMBER_OF(row->Cols[3]));
            row->Param = 0;
        }
    }

    ShownFilesSequence = ScanSequence;
    PhReleaseQueuedLockShared(&ScanLock);

    FillList(Lists[TabFiles], count, 5);
}

// ---- status line -----------------------------------------------------------

static VOID UpdateStatus(VOID)
{
    PH_STRING_BUILDER sb;

    PhInitializeStringBuilder(&sb, 200);
    PhAcquireQueuedLockShared(&ScanLock);

    if (!LastScan)
    {
        PhAppendStringBuilder2(&sb, ScanRunning ? L"Scanning every physical page (a few seconds)..." :
            L"Showing live counters; the page-by-page scan runs while this section is open.");
    }
    else if (!NT_SUCCESS(LastScan->QueryStatus))
    {
        PhAppendFormatStringBuilder(&sb, L"Page-by-page scan unavailable (status 0x%08lx; needs System Informer running as administrator). Showing live counters.",
            LastScan->QueryStatus);
    }
    else
    {
        SYSTEMTIME systemTime;
        FILETIME utc;
        FILETIME local;
        WCHAR size[32];
        LONG64 diff = (LONG64)LastScan->ListTotals[MMPFNLIST_STANDBY] - (LONG64)StandbyPages(&LastScan->ListsBefore);

        utc.dwLowDateTime = LastScan->Time.LowPart;
        utc.dwHighDateTime = LastScan->Time.HighPart;
        FileTimeToLocalFileTime(&utc, &local);
        FileTimeToSystemTime(&local, &systemTime);

        PhAppendFormatStringBuilder(&sb, L"Scanned %s page by page in %.1f s at %02u:%02u:%02u",
            FormatSize(PagesToBytes(LastScan->ScannedPages), size, RTL_NUMBER_OF(size)), LastScan->ElapsedMs / 1000.0,
            systemTime.wHour, systemTime.wMinute, systemTime.wSecond);

        if (ScanIntervalSeconds)
            PhAppendFormatStringBuilder(&sb, L" (every %lu s while open)", ScanIntervalSeconds);

        PhAppendFormatStringBuilder(&sb, L"; standby differs from the kernel's count by %s.%s",
            FormatSize(PagesToBytes(diff < 0 ? -diff : diff), size, RTL_NUMBER_OF(size)), ScanRunning ? L" Rescanning..." : L"");
    }

    PhReleaseQueuedLockShared(&ScanLock);
    PhSetWindowText(StatusLabel, sb.String->Buffer);
    PhDeleteStringBuilder(&sb);
}

static VOID UpdateCurrentTab(_In_ BOOLEAN Force)
{
    switch (CurrentTab)
    {
    case TabBreakdown:
        UpdateBreakdownList();
        break;
    case TabProcesses:
        UpdateProcessList();
        break;
    case TabPool:
        UpdatePoolList();
        break;
    case TabFiles:
        if (Force || ShownFilesSequence != ScanSequence)
            UpdateFilesList();
        break;
    }
}

// ---- graphs ----------------------------------------------------------------

static FLOAT GraphMax(VOID)
{
    return Fast.PhysicalPages ? (FLOAT)PagesToBytes(Fast.PhysicalPages) : 1.0f;
}

_Function_class_(PH_GRAPH_LABEL_Y_FUNCTION)
static PPH_STRING NTAPI LabelYSize(_In_ PPH_GRAPH_DRAW_INFO DrawInfo, _In_ ULONG DataIndex, _In_ FLOAT Value, _In_ FLOAT Parameter)
{
    return PhFormatSize((ULONG64)(Value * Parameter), ULONG_MAX);
}

static PCWSTR MetricColorSetting(_In_ ULONG Metric)
{
    switch (Metric)
    {
    case MetricPrivate: return SETTING_COLOR_PRIVATE;
    case MetricOther: return SETTING_COLOR_CPU_KERNEL;
    case MetricStandby: return SETTING_COLOR_IO_READ_OTHER;
    case MetricModified: return SETTING_COLOR_IO_WRITE;
    default: return SETTING_COLOR_PHYSICAL;
    }
}

static VOID FillGraphData(_In_ PPH_CIRCULAR_BUFFER_FLOAT Buffer, _Out_writes_(Count) PFLOAT Data, _In_ ULONG Count)
{
    FLOAT max = GraphMax();

    for (ULONG i = 0; i < Count; i++)
    {
        FLOAT v = PhGetItemCircularBuffer_FLOAT(Buffer, i) / max;
        Data[i] = v > 1.0f ? 1.0f : v;
    }
}

_Function_class_(PH_GRAPH_MESSAGE_CALLBACK)
static BOOLEAN NTAPI GraphMessageCallback(
    _In_ HWND WindowHandle,
    _In_ ULONG Message,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2,
    _In_opt_ PVOID Context
    )
{
    NMHDR* header = Parameter1;
    ULONG metric = ULONG_MAX;

    if (!header)
        return TRUE;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        if (header->hwndFrom == GraphHandle[m])
            metric = m;
    }

    if (metric == ULONG_MAX)
        return TRUE;

    switch (header->code)
    {
    case GCN_GETDRAWINFO:
        {
            PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)header;
            PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;
            PPH_GRAPH_STATE state = &GraphState[metric];

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_LABEL_MAX_Y;
            Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(MetricColorSetting(metric)), 0, DialogDpi);
            PhGraphStateGetDrawInfo(state, getDrawInfo, History[metric].Count);

            if (!state->Valid)
            {
                FillGraphData(&History[metric], state->Data1, drawInfo->LineDataCount);
                drawInfo->LabelYFunction = LabelYSize;
                drawInfo->LabelYFunctionParameter = GraphMax();
                state->Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)header;
            PPH_GRAPH_STATE state = &GraphState[metric];

            if (getTooltipText->Index < getTooltipText->TotalCount)
            {
                if (state->TooltipIndex != getTooltipText->Index)
                {
                    FLOAT value = PhGetItemCircularBuffer_FLOAT(&History[metric], getTooltipText->Index);
                    PH_FORMAT format[5];
                    WCHAR percent[16];

                    PhInitFormatSR(&format[0], PH_AUTO_T(PH_STRING, PhFormatSize((ULONG64)value, ULONG_MAX))->sr);
                    PhInitFormatS(&format[1], L" (");
                    PhInitFormatS(&format[2], FormatPercent((ULONG64)value, (ULONG64)GraphMax(), percent, RTL_NUMBER_OF(percent)));
                    PhInitFormatS(&format[3], L")\n");
                    PhInitFormatSR(&format[4], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);

                    PhMoveReference(&state->TooltipText, PhFormat(format, RTL_NUMBER_OF(format), 0));
                }

                getTooltipText->Text = PhGetStringRef(state->TooltipText);
            }
        }
        break;
    }

    return TRUE;
}

static VOID CreateGraphs(VOID)
{
    PH_GRAPH_CREATEPARAMS createParams;

    memset(&createParams, 0, sizeof(PH_GRAPH_CREATEPARAMS));
    createParams.Size = sizeof(PH_GRAPH_CREATEPARAMS);
    createParams.Callback = GraphMessageCallback;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        GraphHandle[m] = PhCreateWindow(PH_GRAPH_CLASSNAME, NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS,
            0, 0, 0, 0, Dialog, NULL, NULL, &createParams);
        Graph_SetTooltip(GraphHandle[m], TRUE);
    }
}

// Graphs take the top part of the dialog, the panel (status, tabs, lists) the rest.
static VOID LayoutDialog(VOID)
{
    RECT clientRect;
    RECT labelRect;
    RECT margin = GraphMarginScaled;
    LONG padding = PhScaleToDisplay(GRAPH_PADDING, DialogDpi);
    LONG available;
    LONG panelHeight;
    LONG graphsBottom;
    LONG graphWidth;
    LONG graphHeight;
    LONG y;
    HDWP defer;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        GraphState[m].Valid = FALSE;
        GraphState[m].TooltipIndex = ULONG_MAX;
    }

    PhGetClientRect(Dialog, &clientRect);
    PhGetClientRect(GetDlgItem(Dialog, IDC_LABEL0), &labelRect);

    available = clientRect.bottom - margin.top - margin.bottom;
    panelHeight = available * 55 / 100;
    if (panelHeight < PhScaleToDisplay(140, DialogDpi))
        panelHeight = min(PhScaleToDisplay(140, DialogDpi), available / 2);
    graphsBottom = clientRect.bottom - margin.bottom - panelHeight - padding * 2;

    graphWidth = clientRect.right - margin.left - margin.right;
    graphHeight = (graphsBottom - margin.top - labelRect.bottom * METRIC_COUNT - padding * (METRIC_COUNT * 2 - 1)) / METRIC_COUNT;
    if (graphHeight < 4)
        graphHeight = 4;

    defer = BeginDeferWindowPos(METRIC_COUNT * 2 + 1);
    y = margin.top;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        defer = DeferWindowPos(defer, GetDlgItem(Dialog, MetricLabelIds[m]), NULL, margin.left, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        y += labelRect.bottom + padding;
        defer = DeferWindowPos(defer, GraphHandle[m], NULL, margin.left, y, graphWidth, graphHeight, SWP_NOACTIVATE | SWP_NOZORDER);
        y += graphHeight + padding;
    }

    if (Panel)
        defer = DeferWindowPos(defer, Panel, NULL, margin.left, clientRect.bottom - margin.bottom - panelHeight, graphWidth, panelHeight, SWP_NOACTIVATE | SWP_NOZORDER);

    EndDeferWindowPos(defer);
}

static VOID UpdateGraphLabels(VOID)
{
    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        PH_FORMAT format[6];
        WCHAR size[32];
        WCHAR percent[16];
        WCHAR text[160];

        PhInitFormatS(&format[0], MetricNames[m]);
        PhInitFormatS(&format[1], L": ");
        PhInitFormatS(&format[2], FormatSize(Values[m], size, RTL_NUMBER_OF(size)));
        PhInitFormatS(&format[3], L" (");
        PhInitFormatS(&format[4], FormatPercent(Values[m], (ULONG64)GraphMax(), percent, RTL_NUMBER_OF(percent)));
        PhInitFormatS(&format[5], L")");

        if (PhFormatToBuffer(format, 6, text, sizeof(text), NULL))
            PhSetDialogItemText(Dialog, MetricLabelIds[m], text);
    }
}

static VOID UpdateSubtitle(VOID)
{
    PH_FORMAT format[4];
    WCHAR installed[32];
    WCHAR inUse[32];
    WCHAR text[128];

    PhInitFormatS(&format[0], FormatSize(Values[MetricPrivate] + Values[MetricOther] + Values[MetricModified], inUse, RTL_NUMBER_OF(inUse)));
    PhInitFormatS(&format[1], L" in use of ");
    PhInitFormatS(&format[2], FormatSize(Fast.InstalledBytes, installed, RTL_NUMBER_OF(installed)));
    PhInitFormatS(&format[3], L" installed");

    if (PhFormatToBuffer(format, 4, text, sizeof(text), NULL))
        PhSetDialogItemText(Dialog, IDC_SUBTITLE, text);
}

static VOID TickDialog(VOID)
{
    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        GraphState[m].Valid = FALSE;
        GraphState[m].TooltipIndex = ULONG_MAX;
        Graph_MoveGrid(GraphHandle[m], 1);
        Graph_Draw(GraphHandle[m]);
        Graph_UpdateTooltip(GraphHandle[m]);
        InvalidateRect(GraphHandle[m], NULL, FALSE);
    }

    UpdateGraphLabels();
    UpdateSubtitle();

    if (Panel)
    {
        UpdateStatus();
        UpdateCurrentTab(FALSE);
    }
}

// ---- panel -----------------------------------------------------------------

static VOID SelectTab(_In_ ULONG Tab)
{
    CurrentTab = Tab;

    for (ULONG t = 0; t < TAB_COUNT; t++)
        ShowWindow(Lists[t], t == Tab ? SW_SHOW : SW_HIDE);

    UpdateCurrentTab(TRUE);
}

static VOID LayoutPanel(_In_ HWND WindowHandle)
{
    RECT clientRect;
    RECT statusRect;
    RECT tabRect;
    RECT displayRect;

    PhGetClientRect(WindowHandle, &clientRect);
    PhGetClientRect(StatusLabel, &statusRect);

    SetWindowPos(StatusLabel, NULL, 0, 0, clientRect.right, statusRect.bottom, SWP_NOZORDER | SWP_NOACTIVATE);

    tabRect.left = 0;
    tabRect.top = statusRect.bottom + PhScaleToDisplay(2, DialogDpi);
    tabRect.right = clientRect.right;
    tabRect.bottom = clientRect.bottom;
    SetWindowPos(Tabs, HWND_BOTTOM, tabRect.left, tabRect.top, tabRect.right - tabRect.left, tabRect.bottom - tabRect.top, SWP_NOACTIVATE);

    displayRect = tabRect;
    TabCtrl_AdjustRect(Tabs, FALSE, &displayRect);

    for (ULONG t = 0; t < TAB_COUNT; t++)
    {
        SetWindowPos(Lists[t], HWND_TOP, displayRect.left, displayRect.top, displayRect.right - displayRect.left,
            displayRect.bottom - displayRect.top, SWP_NOACTIVATE);
    }
}

static VOID InitList(_In_ HWND List, _In_ ULONG Count, _In_reads_(Count) CONST PCWSTR* Names, _In_reads_(Count) CONST LONG* Widths, _In_ LONG Dpi)
{
    ListView_SetExtendedListViewStyle(List, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    for (ULONG c = 0; c < Count; c++)
        PhAddListViewColumn(List, c, c, c, c == 0 || Widths[c] > 150 ? LVCFMT_LEFT : LVCFMT_RIGHT, PhScaleToDisplay(Widths[c], Dpi), (PWSTR)Names[c]);
}

static INT_PTR CALLBACK PanelDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            static CONST PCWSTR breakdownColumns[] = { L"Category", L"Total", L"Active", L"Standby", L"Modified", L"% of RAM" };
            static CONST LONG breakdownWidths[] = { 300, 80, 80, 80, 80, 64 };
            static CONST PCWSTR processColumns[] = { L"Process (double-click for properties)", L"PID", L"Private WS", L"Commit", L"Working set" };
            static CONST LONG processWidths[] = { 260, 64, 90, 90, 90 };
            static CONST PCWSTR poolColumns[] = { L"Tag", L"Nonpaged", L"Paged", L"Driver", L"Description" };
            static CONST LONG poolWidths[] = { 50, 90, 90, 151, 320 };
            static CONST PCWSTR fileColumns[] = { L"File", L"Total", L"Active", L"Standby", L"Modified" };
            static CONST LONG fileWidths[] = { 420, 80, 80, 80, 80 };
            LONG dpi = PhGetWindowDpi(WindowHandle);

            StatusLabel = GetDlgItem(WindowHandle, IDC_STATUS);
            Tabs = GetDlgItem(WindowHandle, IDC_TABS);
            Lists[TabBreakdown] = GetDlgItem(WindowHandle, IDC_LIST_BREAKDOWN);
            Lists[TabProcesses] = GetDlgItem(WindowHandle, IDC_LIST_PROCESSES);
            Lists[TabPool] = GetDlgItem(WindowHandle, IDC_LIST_POOL);
            Lists[TabFiles] = GetDlgItem(WindowHandle, IDC_LIST_FILES);

            PhAddTabControlTab(Tabs, TabBreakdown, L"Breakdown");
            PhAddTabControlTab(Tabs, TabProcesses, L"Processes");
            PhAddTabControlTab(Tabs, TabPool, L"Pool tags");
            PhAddTabControlTab(Tabs, TabFiles, L"Cached files");

            InitList(Lists[TabBreakdown], RTL_NUMBER_OF(breakdownColumns), breakdownColumns, breakdownWidths, dpi);
            InitList(Lists[TabProcesses], RTL_NUMBER_OF(processColumns), processColumns, processWidths, dpi);
            InitList(Lists[TabPool], RTL_NUMBER_OF(poolColumns), poolColumns, poolWidths, dpi);
            InitList(Lists[TabFiles], RTL_NUMBER_OF(fileColumns), fileColumns, fileWidths, dpi);

            TabCtrl_SetCurSel(Tabs, CurrentTab);
            for (ULONG t = 0; t < TAB_COUNT; t++)
                ShowWindow(Lists[t], t == CurrentTab ? SW_SHOW : SW_HIDE);
            ShownFilesSequence = ULONG_MAX;
        }
        break;
    case WM_SIZE:
        if (StatusLabel && Tabs)
            LayoutPanel(WindowHandle);
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if (header->hwndFrom == Tabs && header->code == TCN_SELCHANGE)
            {
                LONG tab = TabCtrl_GetCurSel(Tabs);

                if (tab >= 0 && tab < TAB_COUNT)
                    SelectTab(tab);
            }
            else if (header->hwndFrom == Lists[TabProcesses] && header->code == NM_DBLCLK)
            {
                LPNMITEMACTIVATE activate = (LPNMITEMACTIVATE)lParam;
                LVITEM item = { 0 };

                item.mask = LVIF_PARAM;
                item.iItem = activate->iItem;

                if (activate->iItem >= 0 && ListView_GetItem(Lists[TabProcesses], &item) && item.lParam)
                    ShowProcessProperties(WindowHandle, (ULONG)item.lParam);
            }
        }
        break;
    case WM_DESTROY:
        {
            StatusLabel = NULL;
            Tabs = NULL;
            memset(Lists, 0, sizeof(Lists));
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}

// ---- section dialog --------------------------------------------------------

static INT_PTR CALLBACK MemDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    // Dialogs get WM_SIZE (and others) before WM_INITDIALOG: nothing is set up yet then.
    if (WindowMessage != WM_INITDIALOG && WindowMessage != WM_DESTROY && Dialog != WindowHandle)
        return FALSE;

    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            PPH_LAYOUT_ITEM graphItem;

            for (ULONG m = 0; m < METRIC_COUNT; m++)
                PhInitializeGraphState(&GraphState[m]);

            Dialog = WindowHandle;
            DialogDpi = PhGetWindowDpi(WindowHandle);
            PhSetWindowExStyle(WindowHandle, WS_EX_TRANSPARENT, 0);

            PhInitializeLayoutManager(&LayoutManager, WindowHandle);
            PhAddLayoutItem(&LayoutManager, GetDlgItem(WindowHandle, IDC_SUBTITLE), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_TOP | PH_ANCHOR_RIGHT | PH_LAYOUT_FORCE_INVALIDATE);
            graphItem = PhAddLayoutItem(&LayoutManager, GetDlgItem(WindowHandle, IDC_GRAPH_LAYOUT), NULL, PH_ANCHOR_ALL);
            GraphMargin = graphItem->Margin;
            GraphMargin.bottom = 0;
            GraphMarginScaled = GraphMargin;
            PhGetMarginDpiValue(&GraphMarginScaled, DialogDpi, TRUE);

            SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), Section->Parameters->LargeFont, FALSE);
            SetWindowFont(GetDlgItem(WindowHandle, IDC_SUBTITLE), Section->Parameters->MediumFont, FALSE);

            Panel = PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_MEMBD_PANEL), WindowHandle, PanelDialogProc, NULL);
            ShowWindow(Panel, SW_SHOW);

            CreateGraphs();
            LayoutDialog();
            TickDialog();

            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
            RequestScan(TRUE);
        }
        break;
    case WM_DESTROY:
        {
            PhDeleteLayoutManager(&LayoutManager);
        }
        break;
    case WM_DPICHANGED_AFTERPARENT:
        {
            DialogDpi = PhGetWindowDpi(WindowHandle);
            GraphMarginScaled = GraphMargin;
            PhGetMarginDpiValue(&GraphMarginScaled, DialogDpi, TRUE);

            if (Section->Parameters->LargeFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), Section->Parameters->LargeFont, FALSE);
            if (Section->Parameters->MediumFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_SUBTITLE), Section->Parameters->MediumFont, FALSE);

            PhLayoutManagerUpdate(&LayoutManager, LOWORD(wParam));
            PhLayoutManagerLayout(&LayoutManager);
            LayoutDialog();
        }
        break;
    case WM_SIZE:
        {
            PhLayoutManagerLayout(&LayoutManager);
            LayoutDialog();
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}

// ---- sysinfo section -------------------------------------------------------

_Function_class_(PH_SYSINFO_SECTION_CALLBACK)
static BOOLEAN NTAPI SectionCallback(
    _In_ PPH_SYSINFO_SECTION Section_,
    _In_ PH_SYSINFO_SECTION_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2
    )
{
    switch (Message)
    {
    case SysInfoDestroy:
        {
            if (Dialog)
            {
                for (ULONG m = 0; m < METRIC_COUNT; m++)
                {
                    PhDeleteGraphState(&GraphState[m]);
                    GraphHandle[m] = NULL;
                }

                Dialog = NULL;
                Panel = NULL;
            }
        }
        return TRUE;
    case SysInfoTick:
        {
            if (Dialog)
                TickDialog();
        }
        return TRUE;
    case SysInfoViewChanging:
        {
            PH_SYSINFO_VIEW_TYPE view = (PH_SYSINFO_VIEW_TYPE)PtrToUlong(Parameter1);

            if (view == SysInfoSummaryView || (PPH_SYSINFO_SECTION)Parameter2 != Section_)
                return TRUE;

            for (ULONG m = 0; m < METRIC_COUNT; m++)
            {
                if (GraphHandle[m])
                {
                    GraphState[m].Valid = FALSE;
                    GraphState[m].TooltipIndex = ULONG_MAX;
                    Graph_Draw(GraphHandle[m]);
                }
            }

            if (Dialog)
                RequestScan(TRUE);
        }
        return TRUE;
    case SysInfoCreateDialog:
        {
            PPH_SYSINFO_CREATE_DIALOG createDialog = Parameter1;

            createDialog->Instance = PluginInstance->DllBase;
            createDialog->Template = MAKEINTRESOURCE(IDD_MEMBD_DIALOG);
            createDialog->DialogProc = MemDialogProc;
            createDialog->Parameter = NULL;
        }
        return TRUE;
    case SysInfoGraphGetDrawInfo:
        {
            PPH_GRAPH_DRAW_INFO drawInfo = Parameter1;

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_USE_LINE_2;
            Section_->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_PHYSICAL),
                PhGetIntegerSetting(SETTING_COLOR_IO_READ_OTHER), Section_->Parameters->WindowDpi);
            PhGetDrawInfoGraphBuffers(&Section_->GraphState.Buffers, drawInfo, InUseHistory.Count);

            if (!Section_->GraphState.Valid)
            {
                FillGraphData(&InUseHistory, Section_->GraphState.Data1, drawInfo->LineDataCount);
                FillGraphData(&History[MetricStandby], Section_->GraphState.Data2, drawInfo->LineDataCount);
                Section_->GraphState.Valid = TRUE;
            }
        }
        return TRUE;
    case SysInfoGraphGetTooltipText:
        {
            PPH_SYSINFO_GRAPH_GET_TOOLTIP_TEXT getTooltipText = Parameter1;
            PH_FORMAT format[6];

            if (!getTooltipText)
                break;

            PhInitFormatS(&format[0], L"In use ");
            PhInitFormatSR(&format[1], PH_AUTO_T(PH_STRING, PhFormatSize((ULONG64)PhGetItemCircularBuffer_FLOAT(&InUseHistory, getTooltipText->Index), ULONG_MAX))->sr);
            PhInitFormatS(&format[2], L", standby ");
            PhInitFormatSR(&format[3], PH_AUTO_T(PH_STRING, PhFormatSize((ULONG64)PhGetItemCircularBuffer_FLOAT(&History[MetricStandby], getTooltipText->Index), ULONG_MAX))->sr);
            PhInitFormatC(&format[4], L'\n');
            PhInitFormatSR(&format[5], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);

            PhMoveReference(&Section_->GraphState.TooltipText, PhFormat(format, RTL_NUMBER_OF(format), 0));
            getTooltipText->Text = PhGetStringRef(Section_->GraphState.TooltipText);
        }
        return TRUE;
    case SysInfoGraphDrawPanel:
        {
            PPH_SYSINFO_DRAW_PANEL drawPanel = Parameter1;
            PH_FORMAT format[4];

            if (!drawPanel)
                break;

            drawPanel->Title = PhCreateString(L"RAM breakdown");
            PhInitFormatS(&format[0], L"In use ");
            PhInitFormatSize(&format[1], Values[MetricPrivate] + Values[MetricOther] + Values[MetricModified]);
            PhInitFormatS(&format[2], L"\nCache ");
            PhInitFormatSize(&format[3], Values[MetricStandby]);
            drawPanel->SubTitle = PhFormat(format, 4, 64);
            drawPanel->SubTitleOverflow = PhFormat(format, 2, 64);
        }
        return TRUE;
    }

    return FALSE;
}

// ---- plugin callbacks ------------------------------------------------------

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    ULONG sampleCount = PhGetIntegerSetting(SETTING_SAMPLE_COUNT);

    ScanIntervalSeconds = PhGetIntegerSetting(SETTING_NAME_SCAN_INTERVAL);
    if (ScanIntervalSeconds && ScanIntervalSeconds < 5)
        ScanIntervalSeconds = 5;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
        PhInitializeCircularBuffer_FLOAT(&History[m], sampleCount);
    PhInitializeCircularBuffer_FLOAT(&InUseHistory, sampleCount);

    if (NT_SUCCESS(NtCreateEvent(&ScanEvent, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE)))
        PhCreateThread2(ScanThreadStart, NULL);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    SampleFast();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI SystemInformationInitializingCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    PPH_PLUGIN_SYSINFO_POINTERS pointers = Parameter;
    PH_SYSINFO_SECTION section;

    if (!pointers)
        return;

    memset(&section, 0, sizeof(PH_SYSINFO_SECTION));
    PhInitializeStringRef(&section.Name, L"RAM breakdown");
    section.Flags = 0;
    section.Callback = SectionCallback;

    Section = pointers->CreateSection(&section);
}

LOGICAL DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _Reserved_ PVOID Reserved
    )
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        PPH_PLUGIN_INFORMATION info;
        PH_SETTING_CREATE settings[] =
        {
            { IntegerSettingType, SETTING_NAME_SCAN_INTERVAL, L"1e" }, // hex: 30 seconds
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Memory breakdown";
        info->Description = L"Shows where physical memory goes: every page by use and list (PFN scan), top processes, pool tags and cached files.";
        info->Author = L"Alihkhawaher";
        info->Url = L"https://github.com/Alihkhawaher/systeminformer-plugins";

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackProcessProviderUpdatedEvent), ProcessesUpdatedCallback, NULL, &ProcessesUpdatedCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackSystemInformationInitializing), SystemInformationInitializingCallback, NULL, &SystemInformationInitializingCallbackRegistration);

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
