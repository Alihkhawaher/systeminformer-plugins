/*
 * MemBreakdown sampling core: see memcore.h.
 */

#include <phnt_windows.h>
#include <phnt.h>
#include "memcore.h"

#define MB_CHUNK_PAGES 65536    // PFNs per Superfetch query (65536 * 24 bytes = 1.5 MB buffer)

// ---- fast counters ---------------------------------------------------------

VOID MbQueryFast(
    _Out_ PMB_FAST Fast
    )
{
    SYSTEM_BASIC_INFORMATION basic;
    ULONGLONG installedKb = 0;

    RtlZeroMemory(Fast, sizeof(MB_FAST));

    Fast->ListsStatus = NtQuerySystemInformation(SystemMemoryListInformation, &Fast->Lists, sizeof(Fast->Lists), NULL);
    Fast->PerfStatus = NtQuerySystemInformation(SystemPerformanceInformation, &Fast->Perf, sizeof(Fast->Perf), NULL);
    Fast->CacheStatus = NtQuerySystemInformation(SystemFileCacheInformationEx, &Fast->Cache, sizeof(Fast->Cache), NULL);

    if (NT_SUCCESS(NtQuerySystemInformation(SystemBasicInformation, &basic, sizeof(basic), NULL)))
        Fast->PhysicalPages = basic.NumberOfPhysicalPages;

    if (GetPhysicallyInstalledSystemMemory(&installedKb))
        Fast->InstalledBytes = installedKb * 1024;
    if (Fast->InstalledBytes < Fast->PhysicalPages * PAGE_SIZE)
        Fast->InstalledBytes = Fast->PhysicalPages * PAGE_SIZE;
}

// ---- superfetch ------------------------------------------------------------

static NTSTATUS MbpQuerySuperfetch(
    _In_ SUPERFETCH_INFORMATION_CLASS InformationClass,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG ReturnLength
    )
{
    SUPERFETCH_INFORMATION info;
    ULONG returnLength = 0;
    NTSTATUS status;

    info.Version = SUPERFETCH_INFORMATION_VERSION;
    info.Magic = SUPERFETCH_INFORMATION_MAGIC;
    info.SuperfetchInformationClass = InformationClass;
    info.SuperfetchInformation = Buffer;
    info.SuperfetchInformationLength = Length;

    status = NtQuerySystemInformation(SystemSuperfetchInformation, &info, sizeof(info), &returnLength);

    if (ReturnLength)
        *ReturnLength = returnLength;

    return status;
}

static PVOID MbpAlloc(_In_ SIZE_T Size)
{
    return RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

static VOID MbpFree(_In_opt_ PVOID Memory)
{
    if (Memory)
        RtlFreeHeap(RtlProcessHeap(), 0, Memory);
}

// Physical memory ranges. Tries the V1 layout first, then V2. Returns a heap array of ranges.
static NTSTATUS MbpQueryRanges(
    _Out_ PPF_PHYSICAL_MEMORY_RANGE* Ranges,
    _Out_ PULONG RangeCount,
    _Out_ PULONG Version
    )
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    *Ranges = NULL;
    *RangeCount = 0;
    *Version = 0;

    for (ULONG version = 1; version <= 2; version++)
    {
        ULONG length = 0x1000;

        for (ULONG attempt = 0; attempt < 4; attempt++)
        {
            PUCHAR buffer = MbpAlloc(length);
            ULONG returnLength = 0;

            if (!buffer)
                return STATUS_NO_MEMORY;

            *(PULONG)buffer = version;
            status = MbpQuerySuperfetch(SuperfetchMemoryRangesQuery, buffer, length, &returnLength);

            if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_INFO_LENGTH_MISMATCH)
            {
                MbpFree(buffer);
                length = returnLength > length ? returnLength : length * 4;
                continue;
            }

            if (NT_SUCCESS(status))
            {
                SIZE_T count;
                PPF_PHYSICAL_MEMORY_RANGE source;

                if (version == 1)
                {
                    PPF_PHYSICAL_MEMORY_RANGE_INFO_V1 info = (PPF_PHYSICAL_MEMORY_RANGE_INFO_V1)buffer;
                    count = info->RangeCount;
                    source = info->Ranges;
                }
                else
                {
                    PPF_PHYSICAL_MEMORY_RANGE_INFO_V2 info = (PPF_PHYSICAL_MEMORY_RANGE_INFO_V2)buffer;
                    count = info->RangeCount;
                    source = info->Ranges;
                }

                if ((PUCHAR)(source + count) > buffer + length || count == 0 || count > 4096)
                {
                    status = STATUS_INVALID_BUFFER_SIZE;
                }
                else
                {
                    *Ranges = MbpAlloc(count * sizeof(PF_PHYSICAL_MEMORY_RANGE));

                    if (*Ranges)
                    {
                        RtlCopyMemory(*Ranges, source, count * sizeof(PF_PHYSICAL_MEMORY_RANGE));
                        *RangeCount = (ULONG)count;
                        *Version = version;
                    }
                    else
                    {
                        status = STATUS_NO_MEMORY;
                    }
                }
            }

            MbpFree(buffer);
            break;
        }

        if (NT_SUCCESS(status))
            break;
    }

    return status;
}

// ---- file-object table (open addressing) -----------------------------------

typedef struct _MB_FILE_TABLE
{
    PMB_FILE_USAGE Entries;
    ULONG Capacity;     // power of two
    ULONG Count;
    BOOLEAN Failed;
} MB_FILE_TABLE, *PMB_FILE_TABLE;

static ULONG MbpHash(_In_ ULONG_PTR Key)
{
    ULONG64 x = Key >> 4;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (ULONG)x;
}

static BOOLEAN MbpFileTableInit(_Out_ PMB_FILE_TABLE Table, _In_ ULONG Capacity)
{
    Table->Capacity = Capacity;
    Table->Count = 0;
    Table->Failed = FALSE;
    Table->Entries = MbpAlloc((SIZE_T)Capacity * sizeof(MB_FILE_USAGE));
    return Table->Entries != NULL;
}

static PMB_FILE_USAGE MbpFileTableSlot(_In_ PMB_FILE_TABLE Table, _In_ ULONG_PTR Key)
{
    ULONG mask = Table->Capacity - 1;
    ULONG i = MbpHash(Key) & mask;

    while (Table->Entries[i].FileObject && Table->Entries[i].FileObject != Key)
        i = (i + 1) & mask;

    return &Table->Entries[i];
}

static VOID MbpFileTableAdd(_Inout_ PMB_FILE_TABLE Table, _In_ ULONG_PTR Key, _In_ ULONG Column)
{
    PMB_FILE_USAGE slot;

    if (Table->Failed || !Key)
        return;

    if ((Table->Count + 1) * 10 > Table->Capacity * 7)
    {
        MB_FILE_TABLE bigger;

        if (!MbpFileTableInit(&bigger, Table->Capacity * 2))
        {
            Table->Failed = TRUE;
            return;
        }

        for (ULONG i = 0; i < Table->Capacity; i++)
        {
            if (Table->Entries[i].FileObject)
                *MbpFileTableSlot(&bigger, Table->Entries[i].FileObject) = Table->Entries[i];
        }

        bigger.Count = Table->Count;
        MbpFree(Table->Entries);
        *Table = bigger;
    }

    slot = MbpFileTableSlot(Table, Key);

    if (!slot->FileObject)
    {
        slot->FileObject = Key;
        Table->Count++;
    }

    slot->Pages[Column]++;
}

static ULONG64 MbpFileTotal(_In_ PMB_FILE_USAGE Usage)
{
    return Usage->Pages[0] + Usage->Pages[1] + Usage->Pages[2];
}

static VOID MbpSelectTopFiles(_In_ PMB_FILE_TABLE Table, _Inout_ PMB_PFN_SCAN Scan)
{
    Scan->FileObjectCount = Table->Count;
    Scan->TopFileCount = 0;

    for (ULONG i = 0; i < Table->Capacity; i++)
    {
        PMB_FILE_USAGE entry = &Table->Entries[i];
        ULONG64 total;
        ULONG pos;

        if (!entry->FileObject)
            continue;

        total = MbpFileTotal(entry);

        if (Scan->TopFileCount == MB_MAX_FILES && total <= MbpFileTotal(&Scan->TopFiles[MB_MAX_FILES - 1]))
            continue;

        pos = Scan->TopFileCount < MB_MAX_FILES ? Scan->TopFileCount++ : MB_MAX_FILES - 1;

        while (pos > 0 && MbpFileTotal(&Scan->TopFiles[pos - 1]) < total)
        {
            Scan->TopFiles[pos] = Scan->TopFiles[pos - 1];
            pos--;
        }

        Scan->TopFiles[pos] = *entry;
    }
}

// ---- PFN scan --------------------------------------------------------------

NTSTATUS MbScanPfns(
    _Out_ PMB_PFN_SCAN Scan,
    _In_ BOOLEAN CollectFiles
    )
{
    PPF_PHYSICAL_MEMORY_RANGE ranges = NULL;
    PPF_PFN_PRIO_REQUEST request = NULL;
    SIZE_T requestSize = FIELD_OFFSET(PF_PFN_PRIO_REQUEST, PageIdentities) + MB_CHUNK_PAGES * sizeof(MMPFN_IDENTITY);
    SIZE_T regionSize = requestSize;
    MB_FILE_TABLE files = { 0 };
    ULONG64 start = GetTickCount64();
    NTSTATUS status;

    RtlZeroMemory(Scan, sizeof(MB_PFN_SCAN));

    NtQuerySystemInformation(SystemMemoryListInformation, &Scan->ListsBefore, sizeof(Scan->ListsBefore), NULL);

    status = MbpQueryRanges(&ranges, &Scan->RangeCount, &Scan->RangesVersion);
    Scan->RangesStatus = status;

    if (!NT_SUCCESS(status))
        goto CleanupExit;

    for (ULONG r = 0; r < Scan->RangeCount; r++)
        Scan->TotalPages += ranges[r].PageCount;

    status = NtAllocateVirtualMemory(NtCurrentProcess(), &request, 0, &regionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!NT_SUCCESS(status))
    {
        request = NULL;
        goto CleanupExit;
    }

    if (CollectFiles && !MbpFileTableInit(&files, 1 << 16))
        CollectFiles = FALSE;

    for (ULONG r = 0; r < Scan->RangeCount && NT_SUCCESS(status); r++)
    {
        ULONG_PTR pfn = ranges[r].BasePfn;
        ULONG_PTR end = ranges[r].BasePfn + ranges[r].PageCount;

        while (pfn < end)
        {
            ULONG count = (ULONG)min(end - pfn, (ULONG_PTR)MB_CHUNK_PAGES);
            ULONG length = (ULONG)(FIELD_OFFSET(PF_PFN_PRIO_REQUEST, PageIdentities) + count * sizeof(MMPFN_IDENTITY));

            RtlZeroMemory(request, FIELD_OFFSET(PF_PFN_PRIO_REQUEST, PageIdentities));
            request->Version = PF_PFN_PRIO_REQUEST_VERSION;
            request->RequestFlags = PF_PFN_PRIO_REQUEST_QUERY_MEMORY_LIST;
            request->PfnCount = count;

            for (ULONG i = 0; i < count; i++)
            {
                request->PageIdentities[i].u1.e1 = (MEMORY_FRAME_INFORMATION){ 0 };
                request->PageIdentities[i].PageFrameIndex = pfn + i;
                request->PageIdentities[i].u2.FileObject = 0;
            }

            status = MbpQuerySuperfetch(SuperfetchPfnQuery, request, length, NULL);

            if (!NT_SUCCESS(status))
                break;

            for (ULONG i = 0; i < count; i++)
            {
                PMMPFN_IDENTITY identity = &request->PageIdentities[i];
                ULONG use = (ULONG)identity->u1.e1.UseDescription;
                ULONG list = (ULONG)identity->u1.e1.ListDescription;

                Scan->Counts[use][list]++;
                Scan->ListTotals[list]++;

                if (CollectFiles && use == MMPFNUSE_FILE)
                {
                    ULONG column;

                    switch (list)
                    {
                    case MMPFNLIST_ACTIVE:
                    case MMPFNLIST_TRANSITION:
                        column = 0;
                        break;
                    case MMPFNLIST_STANDBY:
                        column = 1;
                        break;
                    case MMPFNLIST_MODIFIED:
                    case MMPFNLIST_MODIFIEDNOWRITE:
                        column = 2;
                        break;
                    default:
                        column = ULONG_MAX;
                        break;
                    }

                    if (column != ULONG_MAX)
                        MbpFileTableAdd(&files, identity->u2.FileObject & ~(ULONG_PTR)0xF, column);
                }
            }

            Scan->ScannedPages += count;
            pfn += count;
        }
    }

    if (CollectFiles && !files.Failed)
        MbpSelectTopFiles(&files, Scan);

CleanupExit:
    Scan->QueryStatus = status;
    Scan->ElapsedMs = (ULONG)(GetTickCount64() - start);
    NtQuerySystemTime(&Scan->Time);

    if (request)
    {
        regionSize = 0;
        NtFreeVirtualMemory(NtCurrentProcess(), &request, &regionSize, MEM_RELEASE);
    }

    MbpFree(files.Entries);
    MbpFree(ranges);

    return status;
}

// ---- names -----------------------------------------------------------------

PCWSTR MbUseName(
    _In_ ULONG Use
    )
{
    static PCWSTR names[] =
    {
        L"Process private",
        L"Mapped file",
        L"Shareable (pagefile-backed)",
        L"Page table",
        L"Paged pool",
        L"Nonpaged pool",
        L"System PTE (drivers, kernel)",
        L"Session private",
        L"Metafile (NTFS metadata)",
        L"AWE / large pages",
        L"Driver locked",
        L"Kernel stack",
    };

    return Use < RTL_NUMBER_OF(names) ? names[Use] : L"Other (unknown use)";
}

PCWSTR MbListName(
    _In_ ULONG List
    )
{
    static PCWSTR names[] = { L"Zero", L"Free", L"Standby", L"Modified", L"Modified no-write", L"Bad", L"Active", L"Transition" };

    return List < RTL_NUMBER_OF(names) ? names[List] : L"?";
}
