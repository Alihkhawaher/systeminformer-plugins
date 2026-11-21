/*
 * MemBreakdown sampling core. Plain ntdll (phnt) only, so tools\memprobe can reuse it outside System Informer.
 *
 * Two tiers:
 *  - MbQueryFast: cheap counters for every tick (memory lists, performance info, system cache).
 *  - MbScanPfns: walks every physical page frame through the Superfetch PFN query (what RAMMap does) and counts
 *    pages by use (process private, mapped file, page table, pool, ...) and by list (active, standby, modified,
 *    free, ...). Needs admin + SeProfileSingleProcessPrivilege. Costs about a second of CPU per 128 GB.
 */

#pragma once

#define MB_USE_COUNT 16     // MMPFN_IDENTITY UseDescription is 4 bits
#define MB_LIST_COUNT 8     // ListDescription is 3 bits
#define MB_MAX_FILES 64     // top file objects kept per scan

typedef struct _MB_FAST
{
    NTSTATUS ListsStatus;
    NTSTATUS PerfStatus;
    NTSTATUS CacheStatus;
    SYSTEM_MEMORY_LIST_INFORMATION Lists;      // pages
    SYSTEM_PERFORMANCE_INFORMATION Perf;       // pages
    SYSTEM_FILECACHE_INFORMATION Cache;        // bytes
    ULONG64 PhysicalPages;                     // pages Windows manages (SystemBasicInformation)
    ULONG64 InstalledBytes;                    // DIMMs (GetPhysicallyInstalledSystemMemory)
} MB_FAST, *PMB_FAST;

typedef struct _MB_FILE_USAGE
{
    ULONG_PTR FileObject;   // kernel FILE_OBJECT address (the section's file object)
    ULONG64 Pages[3];       // active (incl. transition), standby, modified (incl. no-write)
} MB_FILE_USAGE, *PMB_FILE_USAGE;

typedef struct _MB_PFN_SCAN
{
    NTSTATUS RangesStatus;
    NTSTATUS QueryStatus;
    ULONG RangesVersion;                       // 1 or 2
    ULONG RangeCount;
    ULONG64 TotalPages;                        // sum of all ranges
    ULONG64 ScannedPages;
    ULONG64 Counts[MB_USE_COUNT][MB_LIST_COUNT];
    ULONG64 ListTotals[MB_LIST_COUNT];
    SYSTEM_MEMORY_LIST_INFORMATION ListsBefore; // for the self-check against the scan's own list totals
    ULONG ElapsedMs;
    LARGE_INTEGER Time;                        // system time when the scan finished

    // file pages (UseDescription == MMPFNUSE_FILE) grouped by file object
    ULONG FileObjectCount;                     // distinct file objects seen
    ULONG TopFileCount;
    MB_FILE_USAGE TopFiles[MB_MAX_FILES];      // by total pages, descending
} MB_PFN_SCAN, *PMB_PFN_SCAN;

VOID MbQueryFast(
    _Out_ PMB_FAST Fast
    );

NTSTATUS MbScanPfns(
    _Out_ PMB_PFN_SCAN Scan,
    _In_ BOOLEAN CollectFiles
    );

PCWSTR MbUseName(
    _In_ ULONG Use
    );

PCWSTR MbListName(
    _In_ ULONG List
    );
