#ifndef DISKMON_H
#define DISKMON_H

#include <phdk.h>
#include <settings.h>
#include <ntddscsi.h>   // winioctl.h already comes in through phdk.h; including it again redefines its GUIDs
#include <trayicon.h>
#include "resource.h"

#define SMART_MAX_ROWS 48

typedef enum _SMART_STATUS
{
    SmartUnknown = -1,
    SmartGood = 0,
    SmartWarn = 1,
    SmartFail = 2
} SMART_STATUS;

typedef struct _SMART_ROW
{
    WCHAR Id[8];
    WCHAR Name[48];
    WCHAR Current[12];
    WCHAR Worst[12];
    WCHAR Threshold[12];
    WCHAR Raw[32];
    SMART_STATUS Status;
} SMART_ROW, *PSMART_ROW;

typedef struct _DISK_SMART
{
    ULONG Kind;                 // 0 = none, 1 = NVMe log, 2 = ATA attributes
    WCHAR Source[128];          // where the data came from
    WCHAR Note[192];            // why there is no data
    LONG TemperatureC;          // LONG_MIN = unknown
    ULONG64 PowerOnHours;       // ULLONG_MAX = unknown
    ULONG64 PowerCycles;
    LONG WearPercent;           // -1 = unknown (NVMe percentage used, ATA life used)
    ULONG64 BytesWritten;       // ULLONG_MAX = unknown
    ULONG64 BytesRead;
    // NVMe extras used by the health rules
    UCHAR CriticalWarning;
    ULONG64 MediaErrors;
    ULONG CriticalTemperatureMinutes;
    // table
    ULONG RowCount;
    SMART_ROW Rows[SMART_MAX_ROWS];
} DISK_SMART, *PDISK_SMART;

// smart.c
VOID SmartRead(
    _In_ ULONG DiskNumber,
    _In_ STORAGE_BUS_TYPE BusType,
    _Out_ PDISK_SMART Smart
    );

// Appends "text" to a ", "-separated problem list and raises *Status to at least Level.
VOID HealthAddProblem(
    _Inout_updates_(Count) PWSTR Buffer,
    _In_ SIZE_T Count,
    _Inout_ SMART_STATUS* Status,
    _In_ SMART_STATUS Level,
    _In_ PCWSTR Format,
    ...
    );

VOID SmartEvaluate(
    _In_ PDISK_SMART Smart,
    _Inout_updates_(ProblemsCount) PWSTR Problems,
    _In_ SIZE_T ProblemsCount,
    _Inout_ SMART_STATUS* Status
    );

// etw.c
VOID DiskEtwStart(VOID);
VOID DiskEtwStop(VOID);
ULONG DiskEtwStatus(VOID);
VOID DiskEtwDrain(
    _In_ VOID (*Callback)(_In_ ULONG Disk, _In_ ULONG ProcessId, _In_ ULONG64 ReadBytes, _In_ ULONG64 WriteBytes, _In_opt_ PVOID Context),
    _In_opt_ PVOID Context
    );

#endif
