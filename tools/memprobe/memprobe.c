/*
 * memprobe - console check of the MemBreakdown sampling core (plugins\MemBreakdown\memcore.c), outside
 * System Informer. Prints the fast counters, runs one PFN scan, prints the use x list matrix and checks the
 * scan's list totals against SystemMemoryListInformation. No file names are printed (they would be user paths).
 *
 *   .\build.ps1 -Probe                 -> bin\memprobe.exe
 *   bin\memprobe.exe [output.txt]      (run elevated: needs admin for the PFN query)
 */

#include <phnt_windows.h>
#include <phnt.h>
#include <stdio.h>
#include "memcore.h"
#include "filenames.h"

static FILE* Out;

static double Gb(ULONG64 Pages)
{
    return (double)Pages * PAGE_SIZE / (1024.0 * 1024 * 1024);
}

int wmain(int argc, wchar_t** argv)
{
    MB_FAST fast;
    MB_PFN_SCAN* scan;
    BOOLEAN wasEnabled;
    NTSTATUS status;
    ULONG64 sumLists = 0;

    Out = stdout;
    if (argc > 1 && _wfopen_s(&Out, argv[1], L"w") != 0)
        return 2;

    status = RtlAdjustPrivilege(SE_PROF_SINGLE_PROCESS_PRIVILEGE, TRUE, FALSE, &wasEnabled);
    fprintf(Out, "SeProfileSingleProcessPrivilege: 0x%08lx\n", status);

    MbQueryFast(&fast);
    fprintf(Out, "fast: lists 0x%08lx perf 0x%08lx cache 0x%08lx\n", fast.ListsStatus, fast.PerfStatus, fast.CacheStatus);
    fprintf(Out, "installed %.2f GB, physical pages %.2f GB\n", fast.InstalledBytes / 1073741824.0, Gb(fast.PhysicalPages));
    fprintf(Out, "zero %.2f free %.2f modified %.2f modnowrite %.2f bad %.4f GB\n",
        Gb(fast.Lists.ZeroPageCount), Gb(fast.Lists.FreePageCount), Gb(fast.Lists.ModifiedPageCount),
        Gb(fast.Lists.ModifiedNoWritePageCount), Gb(fast.Lists.BadPageCount));
    for (ULONG p = 0; p < 8; p++)
        fprintf(Out, "standby p%lu %.2f GB\n", p, Gb(fast.Lists.PageCountByPriority[p]));
    fprintf(Out, "perf: available %.2f committed %.2f limit %.2f GB; resident paged pool %.2f, nonpaged pool %.2f, "
        "driver %.2f, code %.2f, cache WS %.2f GB\n",
        Gb(fast.Perf.AvailablePages), Gb(fast.Perf.CommittedPages), Gb(fast.Perf.CommitLimit),
        Gb(fast.Perf.ResidentPagedPoolPage), Gb(fast.Perf.NonPagedPoolPages), Gb(fast.Perf.ResidentSystemDriverPage),
        Gb(fast.Perf.ResidentSystemCodePage), Gb(fast.Perf.ResidentSystemCachePage));
    fprintf(Out, "file cache WS current %.2f GB\n", fast.Cache.CurrentSize / 1073741824.0);

    scan = calloc(1, sizeof(MB_PFN_SCAN));
    if (!scan)
        return 3;

    status = MbScanPfns(scan, TRUE);
    fprintf(Out, "\nscan: status 0x%08lx ranges 0x%08lx (v%lu, %lu ranges, %.2f GB) scanned %.2f GB in %lu ms\n",
        status, scan->RangesStatus, scan->RangesVersion, scan->RangeCount, Gb(scan->TotalPages), Gb(scan->ScannedPages), scan->ElapsedMs);

    fprintf(Out, "\n%-30ls", L"use \\ list (GB)");
    for (ULONG l = 0; l < MB_LIST_COUNT; l++)
        fprintf(Out, " %9.9ls", MbListName(l));
    fprintf(Out, "\n");

    for (ULONG u = 0; u < MB_USE_COUNT; u++)
    {
        ULONG64 row = 0;
        for (ULONG l = 0; l < MB_LIST_COUNT; l++)
            row += scan->Counts[u][l];
        if (!row)
            continue;
        fprintf(Out, "%-30ls", MbUseName(u));
        for (ULONG l = 0; l < MB_LIST_COUNT; l++)
            fprintf(Out, " %9.3f", Gb(scan->Counts[u][l]));
        fprintf(Out, "\n");
    }

    fprintf(Out, "%-30ls", L"total");
    for (ULONG l = 0; l < MB_LIST_COUNT; l++)
    {
        fprintf(Out, " %9.3f", Gb(scan->ListTotals[l]));
        sumLists += scan->ListTotals[l];
    }
    fprintf(Out, "\nsum %.3f GB\n", Gb(sumLists));

    {
        ULONG64 standby = 0;
        for (ULONG p = 0; p < 8; p++)
            standby += scan->ListsBefore.PageCountByPriority[p];
        fprintf(Out, "\nself-check (scan vs SystemMemoryListInformation, GB): zero %.3f/%.3f free %.3f/%.3f standby %.3f/%.3f "
            "modified %.3f/%.3f modnowrite %.3f/%.3f\n",
            Gb(scan->ListTotals[MMPFNLIST_ZERO]), Gb(scan->ListsBefore.ZeroPageCount),
            Gb(scan->ListTotals[MMPFNLIST_FREE]), Gb(scan->ListsBefore.FreePageCount),
            Gb(scan->ListTotals[MMPFNLIST_STANDBY]), Gb(standby),
            Gb(scan->ListTotals[MMPFNLIST_MODIFIED]), Gb(scan->ListsBefore.ModifiedPageCount),
            Gb(scan->ListTotals[MMPFNLIST_MODIFIEDNOWRITE]), Gb(scan->ListsBefore.ModifiedNoWritePageCount));
    }

    fprintf(Out, "\nfile objects with cached pages: %lu; top %lu (active/standby/modified GB):\n", scan->FileObjectCount, scan->TopFileCount);
    for (ULONG i = 0; i < scan->TopFileCount && i < 15; i++)
    {
        fprintf(Out, "  #%2lu %8.3f %8.3f %8.3f\n", i, Gb(scan->TopFiles[i].Pages[0]), Gb(scan->TopFiles[i].Pages[1]), Gb(scan->TopFiles[i].Pages[2]));
    }

    // File names for the top file keys, from the ETW file-name rundown (paths are not printed, only extensions).
    {
        MB_FILE_NAMES names;
        ULONG64 start = GetTickCount64();
        ULONG error = MbCollectFileNames(&names, 1000);
        ULONG64 matchedPages = 0, topPages = 0;

        fprintf(Out, "\nfile names: error %lu, %lu FileIo events, %lu names, %llu ms\n", error, names.Events, names.Count, GetTickCount64() - start);

        for (ULONG f = 0; f < scan->TopFileCount; f++)
        {
            PMB_FILE_USAGE file = &scan->TopFiles[f];
            PCWSTR name = MbFindFileName(&names, file->FileObject);
            PCWSTR ext = name ? wcsrchr(name, L'.') : NULL;
            ULONG64 pages = file->Pages[0] + file->Pages[1] + file->Pages[2];

            topPages += pages;
            if (name) matchedPages += pages;
            if (f < 15)
                fprintf(Out, "  #%2lu %8.3f GB %ls %ls\n", f, Gb(pages), name ? L"named" : L"-", ext && wcslen(ext) < 12 ? ext : L"");
        }

        fprintf(Out, "named %.2f of %.2f GB in the top %lu files\n", Gb(matchedPages), Gb(topPages), scan->TopFileCount);
        MbFreeFileNames(&names);
    }

    if (Out != stdout)
        fclose(Out);

    return NT_SUCCESS(status) ? 0 : 1;
}
