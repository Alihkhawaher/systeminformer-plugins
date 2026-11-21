// Console check of smart.c outside System Informer:  smarttest.exe <disk number> <bus: nvme|sata|usb>
// Build: see plugins\DiskMonitor\README.md. ATA and USB reads need an elevated prompt; NVMe usually doesn't.
#include "..\diskmon.h"
#include <stdio.h>

int wmain(int argc, wchar_t** argv)
{
    static DISK_SMART smart;
    WCHAR problems[512] = L"";
    SMART_STATUS status;
    ULONG disk;
    STORAGE_BUS_TYPE bus;

    if (argc < 3)
    {
        wprintf(L"usage: smarttest <disk> nvme|sata|usb\n");
        return 2;
    }

    disk = wcstoul(argv[1], NULL, 10);
    bus = _wcsicmp(argv[2], L"nvme") == 0 ? BusTypeNvme : _wcsicmp(argv[2], L"usb") == 0 ? BusTypeUsb : BusTypeSata;

    SmartRead(disk, bus, &smart);
    status = smart.Kind ? SmartGood : SmartUnknown;
    SmartEvaluate(&smart, problems, RTL_NUMBER_OF(problems), &status);

    wprintf(L"disk %lu  kind %lu  source '%s'  note '%s'\n", disk, smart.Kind, smart.Source, smart.Note);
    wprintf(L"temp %ld C  hours %I64d  cycles %I64d  wear %ld%%  written %.2f TB  read %.2f TB\n",
        smart.TemperatureC, (LONG64)smart.PowerOnHours, (LONG64)smart.PowerCycles, smart.WearPercent,
        smart.BytesWritten == ULLONG_MAX ? -1.0 : smart.BytesWritten / 1e12, smart.BytesRead == ULLONG_MAX ? -1.0 : smart.BytesRead / 1e12);
    wprintf(L"health %d  problems '%s'\n", status, problems);

    for (ULONG i = 0; i < smart.RowCount; i++)
        wprintf(L"  %-3s %-40s %5s %5s %5s %-28s %d\n", smart.Rows[i].Id, smart.Rows[i].Name, smart.Rows[i].Current,
            smart.Rows[i].Worst, smart.Rows[i].Threshold, smart.Rows[i].Raw, smart.Rows[i].Status);

    return 0;
}
