/*
 * ProcessLog - System Informer plugin that logs every process start and exit, including processes that live
 * only a few milliseconds, from ETW kernel process events.
 */

#ifndef PROCESSLOG_H
#define PROCESSLOG_H

#include <phdk.h>
#include <settings.h>
#include <evntrace.h>
#include <evntcons.h>
#include "resource.h"

#define PLUGIN_NAME L"ProcessLog"
#define SETTING_NAME_WINDOW_POSITION (PLUGIN_NAME L".WindowPosition")
#define SETTING_NAME_WINDOW_SIZE (PLUGIN_NAME L".WindowSize")
#define SETTING_NAME_COLUMNS (PLUGIN_NAME L".Columns")
#define SETTING_NAME_MAX_FILE_MB (PLUGIN_NAME L".MaxLogFileMB")      // hex, like every System Informer integer
#define SETTING_NAME_MAX_ENTRIES (PLUGIN_NAME L".MaxEntries")        // hex

#define PL_TICKS_PER_SECOND 10000000LL
#define PL_SHORT_LIVED_TICKS PL_TICKS_PER_SECOND   // < 1 s counts as short-lived

// LOG_ENTRY.Flags
#define LE_RUNNING       0x01   // no exit seen yet
#define LE_PREEXISTING   0x02   // already running when logging started; start = the process's create time
#define LE_EXIT_UNKNOWN  0x04   // exit was not seen (System Informer was not running)
#define LE_START_UNKNOWN 0x08   // start was not seen
#define LE_VISIBLE       0x10   // in the log (internal, not saved)
#define LE_SAVED_MASK    (LE_RUNNING | LE_PREEXISTING | LE_EXIT_UNKNOWN | LE_START_UNKNOWN)

typedef struct _LOG_ENTRY
{
    ULONG64 Seq;            // position in the log, increasing
    ULONG64 Key;            // kernel UniqueProcessKey (EPROCESS), 0 = unknown
    LONG64 StartTime;       // UTC FILETIME, 0 = unknown
    LONG64 ExitTime;        // UTC FILETIME, 0 = unknown / running
    ULONG ProcessId;
    ULONG ParentId;
    ULONG CreatorId;        // process that actually called CreateProcess, if different from the parent
    ULONG SessionId;
    LONG ExitCode;
    ULONG Flags;
    PWSTR Name;
    PWSTR ParentName;
    PWSTR CreatorName;
    PWSTR ImagePath;
    PWSTR CommandLine;
    PWSTR User;
    struct _LOG_ENTRY *HashNext;
} LOG_ENTRY, *PLOG_ENTRY;

typedef struct _PL_STATS
{
    ULONG SessionStatus;    // StartTrace result (Win32 error)
    BOOLEAN Active;
    ULONG64 Starts;
    ULONG64 Ends;
    ULONG64 Images;
    ULONG EventsLost;
    ULONG BuffersLost;
    ULONG Entries;
} PL_STATS, *PPL_STATS;

extern PPH_PLUGIN PluginInstance;

// capture.c - everything below is protected by PlLock
extern SRWLOCK PlLock;
extern PLOG_ENTRY *PlEntries;   // in Seq order
extern ULONG PlEntryCount;
extern volatile LONG PlVersion; // bumped on every change
extern WCHAR PlLogFilePath[MAX_PATH];

VOID PlStartCapture(VOID);
VOID PlStopCapture(VOID);
PLOG_ENTRY PlFindEntryBySeq(_In_ ULONG64 Seq);
VOID PlGetStats(_Out_ PPL_STATS Stats);
VOID PlWriteStatusFile(VOID);

// Growable text buffer (capture.c).
typedef struct _PL_BUF
{
    PWSTR Data;
    SIZE_T Length;      // characters, without the terminator
    SIZE_T Capacity;
} PL_BUF, *PPL_BUF;

VOID PlBufAppendN(_Inout_ PPL_BUF Buf, _In_reads_(Count) PCWSTR Text, _In_ SIZE_T Count);
VOID PlBufAppend(_Inout_ PPL_BUF Buf, _In_opt_ PCWSTR Text);
VOID PlBufAppendF(_Inout_ PPL_BUF Buf, _In_z_ _Printf_format_string_ PCWSTR Format, ...);
VOID PlBufFree(_Inout_ PPL_BUF Buf);
PWSTR PlDup(_In_opt_ PCWSTR Text);

// window.c
VOID PlShowWindow(VOID);

BOOLEAN PlIsShortLived(_In_ PLOG_ENTRY Entry);
VOID PlFormatTime(_In_ LONG64 Time, _Out_writes_(Count) PWSTR Buffer, _In_ SIZE_T Count);
VOID PlFormatDuration(_In_ PLOG_ENTRY Entry, _Out_writes_(Count) PWSTR Buffer, _In_ SIZE_T Count);

#endif
