/*
 * ProcessLog window: virtual list view over the log with search, "only short-lived" / "only running" filters,
 * column sorting, details on double-click, copy (Ctrl+C) and CSV export.
 */

#include "processlog.h"
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "shlwapi.lib")

#define TIMER_REFRESH 1
#define TIMER_SEARCH 2

typedef enum _PL_COLUMN
{
    COL_START,
    COL_EXIT,
    COL_DURATION,
    COL_NAME,
    COL_PID,
    COL_PARENT,
    COL_EXITCODE,
    COL_USER,
    COL_SESSION,
    COL_IMAGE,
    COL_COMMANDLINE,
    COL_NOTES,
    COL_COUNT
} PL_COLUMN;

static const struct { PCWSTR Name; LONG Width; LONG Format; } PlColumns[COL_COUNT] =
{
    { L"Start", 150, LVCFMT_LEFT },
    { L"Exit", 150, LVCFMT_LEFT },
    { L"Duration", 75, LVCFMT_RIGHT },
    { L"Process", 140, LVCFMT_LEFT },
    { L"PID", 55, LVCFMT_RIGHT },
    { L"Parent", 170, LVCFMT_LEFT },
    { L"Exit code", 80, LVCFMT_RIGHT },
    { L"User", 150, LVCFMT_LEFT },
    { L"Session", 50, LVCFMT_RIGHT },
    { L"Image path", 260, LVCFMT_LEFT },
    { L"Command line", 400, LVCFMT_LEFT },
    { L"Notes", 200, LVCFMT_LEFT },
};

static HWND PlWindow = NULL;
static HWND PlList = NULL;
static PH_LAYOUT_MANAGER PlLayout;
static ULONG64 *PlView = NULL;
static ULONG PlViewCount = 0;
static ULONG PlViewCapacity = 0;
static LONG PlViewVersion = -1;
static ULONG PlSortColumn = COL_START;
static BOOLEAN PlSortAscending = FALSE;
static WCHAR PlSearch[256];
static BOOLEAN PlShortOnly = FALSE;
static BOOLEAN PlRunningOnly = FALSE;
static BOOLEAN PlDarkTheme = FALSE;

// ---- formatting --------------------------------------------------------------------------------------------

BOOLEAN PlIsShortLived(
    _In_ PLOG_ENTRY Entry
    )
{
    return Entry->StartTime && Entry->ExitTime && Entry->ExitTime >= Entry->StartTime &&
        Entry->ExitTime - Entry->StartTime < PL_SHORT_LIVED_TICKS;
}

VOID PlFormatTime(
    _In_ LONG64 Time,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count
    )
{
    FILETIME fileTime;
    SYSTEMTIME utc;
    SYSTEMTIME local;

    Buffer[0] = UNICODE_NULL;

    if (Time <= 0)
        return;

    fileTime.dwLowDateTime = (DWORD)Time;
    fileTime.dwHighDateTime = (DWORD)(Time >> 32);

    if (FileTimeToSystemTime(&fileTime, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &local))
    {
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
            local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    }
}

VOID PlFormatDuration(
    _In_ PLOG_ENTRY Entry,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count
    )
{
    LONG64 ticks;
    LONG64 ms;

    Buffer[0] = UNICODE_NULL;

    if (Entry->Flags & LE_RUNNING)
    {
        wcsncpy_s(Buffer, Count, L"running", _TRUNCATE);
        return;
    }

    if (!Entry->StartTime || !Entry->ExitTime || Entry->ExitTime < Entry->StartTime)
        return;

    ticks = Entry->ExitTime - Entry->StartTime;
    ms = ticks / 10000;

    if (ms < 60 * 1000)
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lld.%03lld s", ms / 1000, ms % 1000);
    else if (ms < 3600 * 1000)
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lldm %02llds", ms / 60000, (ms / 1000) % 60);
    else if (ms < 86400LL * 1000)
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lldh %02lldm", ms / 3600000, (ms / 60000) % 60);
    else
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lldd %02lldh", ms / 86400000, (ms / 3600000) % 24);
}

static VOID PlpFormatNotes(
    _In_ PLOG_ENTRY Entry,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count
    )
{
    PL_BUF notes = { 0 };

    if (PlIsShortLived(Entry))
        PlBufAppend(&notes, L"short-lived; ");
    if (Entry->Flags & LE_PREEXISTING)
        PlBufAppend(&notes, L"started before logging; ");
    if (Entry->Flags & LE_START_UNKNOWN)
        PlBufAppend(&notes, L"start not seen; ");
    if (Entry->Flags & LE_EXIT_UNKNOWN)
        PlBufAppend(&notes, L"exit not seen (System Informer was not running); ");
    if (Entry->CreatorId)
        PlBufAppendF(&notes, L"created by %s (%lu); ", Entry->CreatorName ? Entry->CreatorName : L"?", Entry->CreatorId);

    Buffer[0] = UNICODE_NULL;

    if (notes.Length >= 2)
    {
        notes.Data[notes.Length - 2] = UNICODE_NULL; // drop the last "; "
        wcsncpy_s(Buffer, Count, notes.Data, _TRUNCATE);
    }

    PlBufFree(&notes);
}

// Text of one cell. Returns Buffer or a string owned by the entry (valid while PlLock is held).
static PCWSTR PlpColumnText(
    _In_ PLOG_ENTRY Entry,
    _In_ ULONG Column,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count
    )
{
    Buffer[0] = UNICODE_NULL;

    switch (Column)
    {
    case COL_START:
        if (Entry->StartTime)
            PlFormatTime(Entry->StartTime, Buffer, Count);
        else
            wcsncpy_s(Buffer, Count, L"?", _TRUNCATE);
        return Buffer;
    case COL_EXIT:
        if (Entry->ExitTime)
            PlFormatTime(Entry->ExitTime, Buffer, Count);
        else if (Entry->Flags & LE_EXIT_UNKNOWN)
            wcsncpy_s(Buffer, Count, L"?", _TRUNCATE);
        return Buffer;
    case COL_DURATION:
        PlFormatDuration(Entry, Buffer, Count);
        return Buffer;
    case COL_NAME:
        return Entry->Name ? Entry->Name : L"";
    case COL_PID:
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lu", Entry->ProcessId);
        return Buffer;
    case COL_PARENT:
        if (Entry->ParentId)
            _snwprintf_s(Buffer, Count, _TRUNCATE, L"%s (%lu)", Entry->ParentName ? Entry->ParentName : L"exited", Entry->ParentId);
        return Buffer;
    case COL_EXITCODE:
        if (Entry->ExitTime)
        {
            if ((ULONG)Entry->ExitCode > 0xffff)
                _snwprintf_s(Buffer, Count, _TRUNCATE, L"0x%08lX", (ULONG)Entry->ExitCode);
            else
                _snwprintf_s(Buffer, Count, _TRUNCATE, L"%ld", Entry->ExitCode);
        }
        return Buffer;
    case COL_USER:
        return Entry->User ? Entry->User : L"";
    case COL_SESSION:
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lu", Entry->SessionId);
        return Buffer;
    case COL_IMAGE:
        return Entry->ImagePath ? Entry->ImagePath : L"";
    case COL_COMMANDLINE:
        return Entry->CommandLine ? Entry->CommandLine : L"";
    case COL_NOTES:
        PlpFormatNotes(Entry, Buffer, Count);
        return Buffer;
    }

    return Buffer;
}

// ---- view (filter + sort) ----------------------------------------------------------------------------------

static BOOLEAN PlpMatches(
    _In_ PLOG_ENTRY Entry
    )
{
    WCHAR number[16];

    if (PlShortOnly && !PlIsShortLived(Entry))
        return FALSE;
    if (PlRunningOnly && !(Entry->Flags & LE_RUNNING))
        return FALSE;
    if (!PlSearch[0])
        return TRUE;

    if ((Entry->Name && StrStrIW(Entry->Name, PlSearch)) ||
        (Entry->ImagePath && StrStrIW(Entry->ImagePath, PlSearch)) ||
        (Entry->CommandLine && StrStrIW(Entry->CommandLine, PlSearch)) ||
        (Entry->ParentName && StrStrIW(Entry->ParentName, PlSearch)) ||
        (Entry->CreatorName && StrStrIW(Entry->CreatorName, PlSearch)) ||
        (Entry->User && StrStrIW(Entry->User, PlSearch)))
    {
        return TRUE;
    }

    _snwprintf_s(number, RTL_NUMBER_OF(number), _TRUNCATE, L"%lu", Entry->ProcessId);
    if (wcscmp(number, PlSearch) == 0)
        return TRUE;
    _snwprintf_s(number, RTL_NUMBER_OF(number), _TRUNCATE, L"%lu", Entry->ParentId);
    return wcscmp(number, PlSearch) == 0;
}

static int PlpCompareInt64(
    _In_ LONG64 A,
    _In_ LONG64 B
    )
{
    return A < B ? -1 : (A > B ? 1 : 0);
}

static int PlpCompareText(
    _In_opt_ PCWSTR A,
    _In_opt_ PCWSTR B
    )
{
    return _wcsicmp(A ? A : L"", B ? B : L"");
}

static LONG64 PlpDurationValue(
    _In_ PLOG_ENTRY Entry
    )
{
    if (Entry->Flags & LE_RUNNING)
        return MAXLONG64;
    if (!Entry->StartTime || !Entry->ExitTime)
        return -1;
    return Entry->ExitTime - Entry->StartTime;
}

static int __cdecl PlpCompareEntries(
    _In_ const void *Left,
    _In_ const void *Right
    )
{
    PLOG_ENTRY a = *(PLOG_ENTRY const *)Left;
    PLOG_ENTRY b = *(PLOG_ENTRY const *)Right;
    int result = 0;

    switch (PlSortColumn)
    {
    case COL_START: result = PlpCompareInt64(a->StartTime, b->StartTime); break;
    case COL_EXIT: result = PlpCompareInt64(a->ExitTime, b->ExitTime); break;
    case COL_DURATION: result = PlpCompareInt64(PlpDurationValue(a), PlpDurationValue(b)); break;
    case COL_NAME: result = PlpCompareText(a->Name, b->Name); break;
    case COL_PID: result = PlpCompareInt64(a->ProcessId, b->ProcessId); break;
    case COL_PARENT: result = PlpCompareText(a->ParentName, b->ParentName); if (!result) result = PlpCompareInt64(a->ParentId, b->ParentId); break;
    case COL_EXITCODE: result = PlpCompareInt64(a->ExitCode, b->ExitCode); break;
    case COL_USER: result = PlpCompareText(a->User, b->User); break;
    case COL_SESSION: result = PlpCompareInt64(a->SessionId, b->SessionId); break;
    case COL_IMAGE: result = PlpCompareText(a->ImagePath, b->ImagePath); break;
    case COL_COMMANDLINE: result = PlpCompareText(a->CommandLine, b->CommandLine); break;
    case COL_NOTES: result = PlpCompareInt64(a->Flags, b->Flags); break;
    }

    if (!result)
        result = PlpCompareInt64((LONG64)a->Seq, (LONG64)b->Seq);

    return PlSortAscending ? result : -result;
}

static int __cdecl PlpCompareSeq(
    _In_ const void *Left,
    _In_ const void *Right
    )
{
    ULONG64 a = *(const ULONG64 *)Left;
    ULONG64 b = *(const ULONG64 *)Right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static VOID PlpUpdateStatus(
    VOID
    )
{
    PL_STATS stats;
    WCHAR text[1024];

    PlGetStats(&stats);

    if (stats.Active)
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE,
            L"%lu of %lu processes shown.  Capture: active (ETW kernel events).  Lost events: %lu.  Log file: %s",
            PlViewCount, stats.Entries, stats.EventsLost + stats.BuffersLost, PlLogFilePath);
    }
    else
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE,
            L"%lu of %lu processes shown.  Capture: NOT running (StartTrace error %lu).  Log file: %s",
            PlViewCount, stats.Entries, stats.SessionStatus, PlLogFilePath);
    }

    SetDlgItemTextW(PlWindow, IDC_STATUS, text);
}

static VOID PlpRebuildView(
    _In_ BOOLEAN Force
    )
{
    LONG version = PlVersion;
    ULONG64 *selected = NULL;
    ULONG selectedCount = 0;
    ULONG64 topSeq = 0;
    ULONG64 focusSeq = 0;
    LONG top;
    LONG focus;
    LONG index;
    PLOG_ENTRY *items = NULL;
    ULONG count = 0;
    ULONG i;

    if (!Force && version == PlViewVersion)
    {
        PlpUpdateStatus();
        return;
    }

    PlViewVersion = version;

    // Remember selection, focus and scroll position by entry, not by row.
    if ((index = ListView_GetSelectedCount(PlList)) > 0 && (selected = malloc(index * sizeof(ULONG64))))
    {
        for (index = ListView_GetNextItem(PlList, -1, LVNI_SELECTED); index != -1; index = ListView_GetNextItem(PlList, index, LVNI_SELECTED))
        {
            if ((ULONG)index < PlViewCount && selectedCount < (ULONG)ListView_GetSelectedCount(PlList))
                selected[selectedCount++] = PlView[index];
        }

        qsort(selected, selectedCount, sizeof(ULONG64), PlpCompareSeq);
    }

    top = ListView_GetTopIndex(PlList);
    if (top > 0 && (ULONG)top < PlViewCount)
        topSeq = PlView[top];
    focus = ListView_GetNextItem(PlList, -1, LVNI_FOCUSED);
    if (focus >= 0 && (ULONG)focus < PlViewCount)
        focusSeq = PlView[focus];

    AcquireSRWLockShared(&PlLock);

    if (PlEntryCount && (items = malloc(PlEntryCount * sizeof(PLOG_ENTRY))))
    {
        for (i = 0; i < PlEntryCount; i++)
        {
            if (PlpMatches(PlEntries[i]))
                items[count++] = PlEntries[i];
        }

        qsort(items, count, sizeof(PLOG_ENTRY), PlpCompareEntries);
    }

    if (count > PlViewCapacity)
    {
        ULONG64 *view = realloc(PlView, count * sizeof(ULONG64));

        if (view)
        {
            PlView = view;
            PlViewCapacity = count;
        }
        else
        {
            count = PlViewCapacity;
        }
    }

    for (i = 0; i < count; i++)
        PlView[i] = items[i]->Seq;

    PlViewCount = count;
    ReleaseSRWLockShared(&PlLock);
    free(items);

    SendMessage(PlList, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemCountEx(PlList, PlViewCount, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);

    if (selectedCount || focusSeq)
    {
        ListView_SetItemState(PlList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);

        for (i = 0; i < PlViewCount; i++)
        {
            UINT state = 0;

            if (selectedCount && bsearch(&PlView[i], selected, selectedCount, sizeof(ULONG64), PlpCompareSeq))
                state |= LVIS_SELECTED;
            if (PlView[i] == focusSeq)
                state |= LVIS_FOCUSED;
            if (state)
                ListView_SetItemState(PlList, i, state, state);
        }
    }

    if (topSeq)
    {
        for (i = 0; i < PlViewCount; i++)
        {
            if (PlView[i] == topSeq)
            {
                LONG current = ListView_GetTopIndex(PlList);
                RECT rect;

                if ((LONG)i != current && ListView_GetItemRect(PlList, 0, &rect, LVIR_BOUNDS))
                    ListView_Scroll(PlList, 0, ((LONG)i - current) * (rect.bottom - rect.top));
                break;
            }
        }
    }

    SendMessage(PlList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(PlList, NULL, FALSE);
    free(selected);

    PlpUpdateStatus();
}

// ---- copy / export / details -------------------------------------------------------------------------------

static VOID PlpAppendCsvField(
    _Inout_ PPL_BUF Buf,
    _In_ PCWSTR Text
    )
{
    PCWSTR start = Text;

    PlBufAppendN(Buf, L"\"", 1);

    for (; *Text; Text++)
    {
        if (*Text == L'"')
        {
            PlBufAppendN(Buf, start, Text - start + 1);
            PlBufAppendN(Buf, L"\"", 1);
            start = Text + 1;
        }
    }

    PlBufAppendN(Buf, start, Text - start);
    PlBufAppendN(Buf, L"\"", 1);
}

static VOID PlpAppendTsvField(
    _Inout_ PPL_BUF Buf,
    _In_ PCWSTR Text
    )
{
    for (; *Text; Text++)
        PlBufAppendN(Buf, (*Text == L'\t' || *Text == L'\r' || *Text == L'\n') ? L" " : Text, 1);
}

// Header + the given rows (all view rows when SelectedOnly is FALSE), as CSV or tab-separated text.
static VOID PlpBuildTable(
    _Inout_ PPL_BUF Buf,
    _In_ BOOLEAN SelectedOnly,
    _In_ BOOLEAN Csv
    )
{
    WCHAR buffer[1024];
    ULONG column;
    LONG index;

    for (column = 0; column < COL_COUNT; column++)
    {
        if (column)
            PlBufAppend(Buf, Csv ? L"," : L"\t");
        if (Csv)
            PlpAppendCsvField(Buf, PlColumns[column].Name);
        else
            PlBufAppend(Buf, PlColumns[column].Name);
    }

    PlBufAppend(Buf, L"\r\n");

    AcquireSRWLockShared(&PlLock);

    for (index = SelectedOnly ? ListView_GetNextItem(PlList, -1, LVNI_SELECTED) : 0;
        index != -1 && (ULONG)index < PlViewCount;
        index = SelectedOnly ? ListView_GetNextItem(PlList, index, LVNI_SELECTED) : index + 1)
    {
        PLOG_ENTRY entry = PlFindEntryBySeq(PlView[index]);

        if (!entry)
            continue;

        for (column = 0; column < COL_COUNT; column++)
        {
            PCWSTR text = PlpColumnText(entry, column, buffer, RTL_NUMBER_OF(buffer));

            if (column)
                PlBufAppend(Buf, Csv ? L"," : L"\t");
            if (Csv)
                PlpAppendCsvField(Buf, text);
            else
                PlpAppendTsvField(Buf, text);
        }

        PlBufAppend(Buf, L"\r\n");
    }

    ReleaseSRWLockShared(&PlLock);
}

static VOID PlpCopySelected(
    VOID
    )
{
    PL_BUF text = { 0 };
    PH_STRINGREF string;

    if (ListView_GetSelectedCount(PlList) == 0)
        return;

    PlpBuildTable(&text, TRUE, FALSE);

    if (text.Data)
    {
        string.Buffer = text.Data;
        string.Length = text.Length * sizeof(WCHAR);
        PhSetClipboardString(PlWindow, &string);
    }

    PlBufFree(&text);
}

static VOID PlpExportCsv(
    VOID
    )
{
    static PH_FILETYPE_FILTER filters[] =
    {
        { L"CSV files (*.csv)", L"*.csv" },
        { L"All files (*.*)", L"*.*" }
    };
    PVOID fileDialog;
    PPH_STRING fileName;

    if (!(fileDialog = PhCreateSaveFileDialog()))
        return;

    PhSetFileDialogFilter(fileDialog, filters, RTL_NUMBER_OF(filters));
    PhSetFileDialogFileName(fileDialog, L"processlog.csv");

    if (PhShowFileDialog(PlWindow, fileDialog) && (fileName = PhGetFileDialogFileName(fileDialog)))
    {
        PL_BUF text = { 0 };
        HANDLE file;
        BOOLEAN ok = FALSE;

        PlpBuildTable(&text, FALSE, TRUE);

        file = CreateFileW(fileName->Buffer, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (file != INVALID_HANDLE_VALUE && text.Data)
        {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, text.Data, (int)text.Length, NULL, 0, NULL, NULL);
            PSTR utf8 = bytes > 0 ? malloc(bytes + 3) : NULL;

            if (utf8)
            {
                DWORD written;

                utf8[0] = (CHAR)0xEF; utf8[1] = (CHAR)0xBB; utf8[2] = (CHAR)0xBF; // BOM so Excel reads UTF-8
                WideCharToMultiByte(CP_UTF8, 0, text.Data, (int)text.Length, utf8 + 3, bytes, NULL, NULL);
                ok = !!WriteFile(file, utf8, bytes + 3, &written, NULL);
                free(utf8);
            }
        }

        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);

        if (!ok)
            MessageBoxW(PlWindow, L"The CSV file could not be written.", L"Process log", MB_ICONERROR);

        PlBufFree(&text);
        PhDereferenceObject(fileName);
    }

    PhFreeFileDialog(fileDialog);
}

static INT_PTR CALLBACK PlpDetailsDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    static PH_LAYOUT_MANAGER layout;

    switch (Message)
    {
    case WM_INITDIALOG:
        SetDlgItemTextW(WindowHandle, IDC_DETAILS_TEXT, (PCWSTR)lParam);
        PhInitializeLayoutManager(&layout, WindowHandle);
        PhAddLayoutItem(&layout, GetDlgItem(WindowHandle, IDC_DETAILS_TEXT), NULL, PH_ANCHOR_ALL);
        PhAddLayoutItem(&layout, GetDlgItem(WindowHandle, IDC_DETAILS_COPY), NULL, PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
        PhAddLayoutItem(&layout, GetDlgItem(WindowHandle, IDOK), NULL, PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
        PhCenterWindow(WindowHandle, GetParent(WindowHandle));
        PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
        SendDlgItemMessageW(WindowHandle, IDC_DETAILS_TEXT, EM_SETSEL, 0, 0);
        SetFocus(GetDlgItem(WindowHandle, IDOK));
        return FALSE;
    case WM_SIZE:
        PhLayoutManagerLayout(&layout);
        break;
    case WM_DESTROY:
        PhDeleteLayoutManager(&layout);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        case IDCANCEL:
            EndDialog(WindowHandle, 0);
            return TRUE;
        case IDC_DETAILS_COPY:
            {
                HWND edit = GetDlgItem(WindowHandle, IDC_DETAILS_TEXT);
                int length = GetWindowTextLengthW(edit);
                PWSTR text = malloc((length + 1) * sizeof(WCHAR));

                if (text)
                {
                    PH_STRINGREF string;

                    GetWindowTextW(edit, text, length + 1);
                    string.Buffer = text;
                    string.Length = (SIZE_T)length * sizeof(WCHAR);
                    PhSetClipboardString(WindowHandle, &string);
                    free(text);
                }
            }
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static VOID PlpShowDetails(
    _In_ LONG Index
    )
{
    static const struct { ULONG Column; PCWSTR Label; } rows[] =
    {
        { COL_NAME, L"Process" }, { COL_PID, L"PID" }, { COL_PARENT, L"Parent" }, { COL_START, L"Started" },
        { COL_EXIT, L"Exited" }, { COL_DURATION, L"Duration" }, { COL_EXITCODE, L"Exit code" }, { COL_USER, L"User" },
        { COL_SESSION, L"Session" }, { COL_IMAGE, L"Image path" }, { COL_COMMANDLINE, L"Command line" }, { COL_NOTES, L"Notes" },
    };
    WCHAR buffer[1024];
    PL_BUF text = { 0 };
    PLOG_ENTRY entry;

    if (Index < 0 || (ULONG)Index >= PlViewCount)
        return;

    AcquireSRWLockShared(&PlLock);

    if (entry = PlFindEntryBySeq(PlView[Index]))
    {
        for (ULONG i = 0; i < RTL_NUMBER_OF(rows); i++)
        {
            PCWSTR value = PlpColumnText(entry, rows[i].Column, buffer, RTL_NUMBER_OF(buffer));

            PlBufAppendF(&text, L"%-14s", rows[i].Label);
            PlBufAppend(&text, value);
            PlBufAppend(&text, L"\r\n");
        }

        if (entry->ExitTime && entry->StartTime)
            PlBufAppendF(&text, L"%-14s%lld ms\r\n", L"Lifetime", (entry->ExitTime - entry->StartTime) / 10000);
        if (entry->Key)
            PlBufAppendF(&text, L"%-14s0x%llx\r\n", L"Kernel key", entry->Key);
    }

    ReleaseSRWLockShared(&PlLock);

    if (text.Data)
        DialogBoxParamW(PluginInstance->DllBase, MAKEINTRESOURCEW(IDD_DETAILS), PlWindow, PlpDetailsDlgProc, (LPARAM)text.Data);

    PlBufFree(&text);
}

// ---- main window -------------------------------------------------------------------------------------------

static VOID PlpSetSortArrow(
    VOID
    )
{
    HWND header = ListView_GetHeader(PlList);
    HDITEMW item;
    int i;

    for (i = 0; i < COL_COUNT; i++)
    {
        item.mask = HDI_FORMAT;
        if (!Header_GetItem(header, i, &item))
            continue;
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if ((ULONG)i == PlSortColumn)
            item.fmt |= PlSortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, i, &item);
    }
}

static INT_PTR CALLBACK PlpDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (Message)
    {
    case WM_INITDIALOG:
        {
            HWND mainWindow;
            HICON icon;
            ULONG i;

            PlWindow = WindowHandle;
            PlList = GetDlgItem(WindowHandle, IDC_LIST);
            PlDarkTheme = !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT);
            PlSearch[0] = UNICODE_NULL; // the controls start empty: so do the filters
            PlShortOnly = FALSE;
            PlRunningOnly = FALSE;
            PlViewCount = 0;

            ListView_SetExtendedListViewStyleEx(PlList,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);

            for (i = 0; i < COL_COUNT; i++)
                PhAddListViewColumn(PlList, i, i, i, PlColumns[i].Format, PlColumns[i].Width * (LONG)PhGetWindowDpi(WindowHandle) / 96, (PWSTR)PlColumns[i].Name);

            PhLoadListViewColumnsFromSetting(SETTING_NAME_COLUMNS, PlList);
            PlpSetSortArrow();

            PhInitializeLayoutManager(&PlLayout, WindowHandle);
            PhAddLayoutItem(&PlLayout, GetDlgItem(WindowHandle, IDC_SEARCH), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_TOP);
            PhAddLayoutItem(&PlLayout, GetDlgItem(WindowHandle, IDC_COPY), NULL, PH_ANCHOR_RIGHT | PH_ANCHOR_TOP);
            PhAddLayoutItem(&PlLayout, GetDlgItem(WindowHandle, IDC_EXPORT), NULL, PH_ANCHOR_RIGHT | PH_ANCHOR_TOP);
            PhAddLayoutItem(&PlLayout, PlList, NULL, PH_ANCHOR_ALL);
            PhAddLayoutItem(&PlLayout, GetDlgItem(WindowHandle, IDC_STATUS), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);

            PhLoadWindowPlacementFromSetting(SETTING_NAME_WINDOW_POSITION, SETTING_NAME_WINDOW_SIZE, WindowHandle);
            mainWindow = SystemInformer_GetWindowHandle();

            {
                PH_INTEGER_PAIR position = PhGetIntegerPairSetting(SETTING_NAME_WINDOW_POSITION);

                if (position.X == 0 && position.Y == 0) // first open: no saved position
                    PhCenterWindow(WindowHandle, mainWindow && IsWindowVisible(mainWindow) ? mainWindow : NULL);
            }

            if (mainWindow &&
                (icon = (HICON)SendMessage(mainWindow, WM_GETICON, ICON_SMALL, 0)))
            {
                SendMessage(WindowHandle, WM_SETICON, ICON_SMALL, (LPARAM)icon);
                SendMessage(WindowHandle, WM_SETICON, ICON_BIG, SendMessage(mainWindow, WM_GETICON, ICON_BIG, 0));
            }

            PhInitializeWindowTheme(WindowHandle, PlDarkTheme);
            PhLayoutManagerLayout(&PlLayout);

            PlViewVersion = -1;
            PlpRebuildView(TRUE);
            SetTimer(WindowHandle, TIMER_REFRESH, 1000, NULL);
            SetFocus(GetDlgItem(WindowHandle, IDC_SEARCH));
        }
        return FALSE;
    case WM_DESTROY:
        KillTimer(WindowHandle, TIMER_REFRESH);
        KillTimer(WindowHandle, TIMER_SEARCH);
        PhSaveWindowPlacementToSetting(SETTING_NAME_WINDOW_POSITION, SETTING_NAME_WINDOW_SIZE, WindowHandle);
        PhSaveListViewColumnsToSetting(SETTING_NAME_COLUMNS, PlList);
        PhDeleteLayoutManager(&PlLayout);
        PhUnregisterDialog(WindowHandle);
        PlWindow = NULL;
        PlList = NULL;
        PlViewCount = 0;
        break;
    case WM_SIZE:
        PhLayoutManagerLayout(&PlLayout);
        break;
    case WM_TIMER:
        if (wParam == TIMER_SEARCH)
        {
            KillTimer(WindowHandle, TIMER_SEARCH);
            GetDlgItemTextW(WindowHandle, IDC_SEARCH, PlSearch, RTL_NUMBER_OF(PlSearch));
            PlpRebuildView(TRUE);
        }
        else if (wParam == TIMER_REFRESH)
        {
            PlpRebuildView(FALSE);
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SEARCH:
            if (HIWORD(wParam) == EN_CHANGE)
                SetTimer(WindowHandle, TIMER_SEARCH, 250, NULL);
            break;
        case IDC_SHORTONLY:
            PlShortOnly = Button_GetCheck(GetDlgItem(WindowHandle, IDC_SHORTONLY)) == BST_CHECKED;
            PlpRebuildView(TRUE);
            break;
        case IDC_RUNNINGONLY:
            PlRunningOnly = Button_GetCheck(GetDlgItem(WindowHandle, IDC_RUNNINGONLY)) == BST_CHECKED;
            PlpRebuildView(TRUE);
            break;
        case IDC_COPY:
            PlpCopySelected();
            break;
        case IDC_EXPORT:
            PlpExportCsv();
            break;
        case IDOK: // Enter
            if (GetFocus() == PlList)
                PlpShowDetails(ListView_GetNextItem(PlList, -1, LVNI_FOCUSED));
            break;
        case IDCANCEL:
            DestroyWindow(WindowHandle);
            break;
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if (header->hwndFrom != PlList)
                break;

            switch (header->code)
            {
            case LVN_GETDISPINFOW:
                {
                    NMLVDISPINFOW *info = (NMLVDISPINFOW *)lParam;

                    if ((info->item.mask & LVIF_TEXT) && info->item.pszText && info->item.cchTextMax > 0)
                    {
                        WCHAR buffer[1024];
                        PLOG_ENTRY entry;

                        info->item.pszText[0] = UNICODE_NULL;

                        if (info->item.iItem >= 0 && (ULONG)info->item.iItem < PlViewCount && info->item.iSubItem < COL_COUNT)
                        {
                            AcquireSRWLockShared(&PlLock);
                            if (entry = PlFindEntryBySeq(PlView[info->item.iItem]))
                                wcsncpy_s(info->item.pszText, info->item.cchTextMax, PlpColumnText(entry, info->item.iSubItem, buffer, RTL_NUMBER_OF(buffer)), _TRUNCATE);
                            ReleaseSRWLockShared(&PlLock);
                        }
                    }
                }
                break;
            case LVN_COLUMNCLICK:
                {
                    LPNMLISTVIEW info = (LPNMLISTVIEW)lParam;

                    if ((ULONG)info->iSubItem == PlSortColumn)
                    {
                        PlSortAscending = !PlSortAscending;
                    }
                    else
                    {
                        PlSortColumn = info->iSubItem;
                        PlSortAscending = info->iSubItem != COL_START && info->iSubItem != COL_EXIT && info->iSubItem != COL_DURATION;
                    }

                    PlpSetSortArrow();
                    PlpRebuildView(TRUE);
                }
                break;
            case NM_DBLCLK:
                PlpShowDetails(((LPNMITEMACTIVATE)lParam)->iItem);
                break;
            case LVN_KEYDOWN:
                {
                    LPNMLVKEYDOWN key = (LPNMLVKEYDOWN)lParam;

                    if (GetKeyState(VK_CONTROL) < 0)
                    {
                        if (key->wVKey == 'C')
                            PlpCopySelected();
                        else if (key->wVKey == 'A')
                            ListView_SetItemState(PlList, -1, LVIS_SELECTED, LVIS_SELECTED);
                    }
                }
                break;
            case NM_CUSTOMDRAW:
                {
                    LPNMLVCUSTOMDRAW draw = (LPNMLVCUSTOMDRAW)lParam;
                    LRESULT result = CDRF_DODEFAULT;

                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                    {
                        result = CDRF_NOTIFYITEMDRAW;
                    }
                    else if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && draw->nmcd.dwItemSpec < PlViewCount)
                    {
                        PLOG_ENTRY entry;
                        BOOLEAN isShort = FALSE;

                        AcquireSRWLockShared(&PlLock);
                        if (entry = PlFindEntryBySeq(PlView[draw->nmcd.dwItemSpec]))
                            isShort = PlIsShortLived(entry);
                        ReleaseSRWLockShared(&PlLock);

                        if (isShort)
                        {
                            draw->clrTextBk = PlDarkTheme ? RGB(0x70, 0x2c, 0x2c) : RGB(0xff, 0xdc, 0xdc);
                            draw->clrText = PlDarkTheme ? RGB(0xff, 0xff, 0xff) : RGB(0x00, 0x00, 0x00);
                            result = CDRF_NEWFONT;
                        }
                    }

                    SetWindowLongPtrW(WindowHandle, DWLP_MSGRESULT, result);
                    return TRUE;
                }
            }
        }
        break;
    }

    return FALSE;
}

VOID PlShowWindow(
    VOID
    )
{
    if (PlWindow)
    {
        if (IsIconic(PlWindow))
            ShowWindow(PlWindow, SW_RESTORE);
        else
            ShowWindow(PlWindow, SW_SHOW);

        SetForegroundWindow(PlWindow);
        return;
    }

    if (PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCEW(IDD_PROCESSLOG), NULL, PlpDlgProc, NULL))
    {
        PhRegisterDialog(PlWindow);
        ShowWindow(PlWindow, SW_SHOW);
        SetForegroundWindow(PlWindow);
    }
}
