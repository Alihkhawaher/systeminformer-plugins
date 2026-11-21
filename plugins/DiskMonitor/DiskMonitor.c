/*
 * DiskMonitor - System Informer plugin: one System Information section per physical disk, found automatically.
 *
 * Graphs: read/write speed, active time, average response time, temperature.
 * Panel: health verdict with the reasons (rules from the remote support agent's health check), temperature,
 * power-on hours, power cycles, wear, lifetime data written/read, the disk's volumes with free space, and the
 * full SMART table (NVMe health log or ATA attributes, also through USB bridges; see smart.c).
 *
 * SMART is read once a minute per disk, one disk per tick, and never from a hard disk that is spun down
 * (GetDevicePowerState), so the monitor doesn't keep sleeping drives awake.
 */

#include "diskmon.h"

#define PLUGIN_NAME L"DiskMonitor"
#define SETTING_NAME_SMART_INTERVAL (PLUGIN_NAME L".SmartIntervalSeconds")
#define SETTING_NAME_SHOW_VIRTUAL (PLUGIN_NAME L".ShowVirtualDisks")
#define GRAPH_PADDING 3
#define MAX_DISKS 64
#define SLOT_COUNT 4

typedef enum _SLOT
{
    SlotReadWrite,
    SlotActive,
    SlotResponse,
    SlotTemperature
} SLOT;

static PCWSTR SlotLabels[SLOT_COUNT] = { L"Read / write:", L"Active time:", L"Average response time:", L"Temperature:" };
static CONST ULONG LabelIds[SLOT_COUNT] = { IDC_LABEL0, IDC_LABEL1, IDC_LABEL2, IDC_LABEL3 };

typedef struct _DISK
{
    ULONG Number;
    HANDLE Handle;              // query-only handle for IOCTL_DISK_PERFORMANCE / power state
    STORAGE_BUS_TYPE BusType;
    BOOLEAN IsSsd;
    ULONG64 Size;
    PPH_STRING Model;
    PPH_STRING Serial;
    PPH_STRING Firmware;
    PPH_STRING Title;           // "Disk 4 (C:)"
    WCHAR Letters[32];          // "C: K:"

    // performance
    BOOLEAN HavePrevious;
    DISK_PERFORMANCE Previous;
    FLOAT ReadRate;             // bytes/s
    FLOAT WriteRate;
    FLOAT Active;               // 0..1
    FLOAT ResponseMs;
    ULONG64 WindowOps;          // for the latency health rule
    ULONG64 WindowTime;         // 100 ns

    PH_CIRCULAR_BUFFER_FLOAT ReadHistory;
    PH_CIRCULAR_BUFFER_FLOAT WriteHistory;
    PH_CIRCULAR_BUFFER_FLOAT ActiveHistory;
    PH_CIRCULAR_BUFFER_FLOAT ResponseHistory;
    PH_CIRCULAR_BUFFER_FLOAT TemperatureHistory;

    // SMART and health (written on the provider thread under Lock, read by the UI)
    PH_QUEUED_LOCK Lock;
    ULONG SmartVersion;
    BOOLEAN SmartTried;
    BOOLEAN Asleep;
    ULONG NextSmartTick;
    DISK_SMART Smart;
    SMART_STATUS Health;
    WCHAR Problems[512];
    WCHAR Volumes[256];         // "C: 120 GB free of 476 GB   K: ..."

    // System Information window
    PPH_SYSINFO_SECTION Section;
    HWND Dialog;
    LONG DialogDpi;
    PH_LAYOUT_MANAGER LayoutManager;
    RECT GraphMargin;
    RECT GraphMarginScaled;
    HWND GraphHandle[SLOT_COUNT];
    PH_GRAPH_STATE GraphState[SLOT_COUNT];
    HWND Panel;
    HWND SmartList;
    ULONG ShownSmartVersion;
} DISK, *PDISK;

static PPH_PLUGIN PluginInstance;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION SystemInformationInitializingCallbackRegistration;
static PDISK Disks[MAX_DISKS];
static ULONG DiskCount;
static ULONG TickCount;
static ULONG SmartIntervalTicks = 60;
static BOOLEAN UseFahrenheit;
static WCHAR SystemLetter;

// ---- discovery ---------------------------------------------------------------------------------

static CONST PH_STRINGREF WhitespaceStringRef = PH_STRINGREF_INIT(L" \t\r\n");
static CONST PH_STRINGREF SpaceStringRef = PH_STRINGREF_INIT(L" ");

static PPH_STRING TrimAnsi(_In_ PSTORAGE_DEVICE_DESCRIPTOR Descriptor, _In_ ULONG Offset, _In_ ULONG Size)
{
    PSTR text;
    PPH_STRING string;

    if (!Offset || Offset >= Size)
        return PhReferenceEmptyString();

    text = (PSTR)Descriptor + Offset;
    string = PhConvertMultiByteToUtf16(text);

    {
        PH_STRINGREF trimmed = string->sr;
        PPH_STRING result;

        PhTrimStringRef(&trimmed, &WhitespaceStringRef, 0);
        result = PhCreateString2(&trimmed);
        PhDereferenceObject(string);
        return result;
    }
}

static VOID QueryDescriptor(_In_ HANDLE Handle, _Inout_ PDISK Disk)
{
    STORAGE_PROPERTY_QUERY query = { StorageDeviceProperty, PropertyStandardQuery };
    UCHAR buffer[2048];
    PSTORAGE_DEVICE_DESCRIPTOR descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)buffer;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek = { 0 };
    GET_LENGTH_INFORMATION length = { 0 };
    DWORD returned;

    memset(buffer, 0, sizeof(buffer));

    if (DeviceIoControl(Handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &returned, NULL))
    {
        PPH_STRING vendor = TrimAnsi(descriptor, descriptor->VendorIdOffset, returned);
        PPH_STRING product = TrimAnsi(descriptor, descriptor->ProductIdOffset, returned);

        Disk->BusType = descriptor->BusType;
        Disk->Model = vendor->Length && !PhStartsWithString(product, vendor, TRUE) ?
            PhConcatStringRef3(&vendor->sr, &SpaceStringRef, &product->sr) : PhReferenceObject(product);
        Disk->Serial = TrimAnsi(descriptor, descriptor->SerialNumberOffset, returned);
        Disk->Firmware = TrimAnsi(descriptor, descriptor->ProductRevisionOffset, returned);

        PhDereferenceObject(vendor);
        PhDereferenceObject(product);
    }
    else
    {
        Disk->BusType = BusTypeUnknown;
        Disk->Model = PhCreateString(L"Disk");
        Disk->Serial = PhReferenceEmptyString();
        Disk->Firmware = PhReferenceEmptyString();
    }

    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    if (DeviceIoControl(Handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &returned, NULL))
        Disk->IsSsd = !seek.IncursSeekPenalty;
    else
        Disk->IsSsd = Disk->BusType == BusTypeNvme;

    if (DeviceIoControl(Handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length, sizeof(length), &returned, NULL))
        Disk->Size = length.Length.QuadPart;
}

static VOID DiscoverDisks(VOID)
{
    BOOLEAN showVirtual = !!PhGetIntegerSetting(SETTING_NAME_SHOW_VIRTUAL);
    ULONG sampleCount = PhGetIntegerSetting(SETTING_SAMPLE_COUNT);

    for (ULONG n = 0; n < MAX_DISKS && DiskCount < MAX_DISKS; n++)
    {
        WCHAR path[32];
        HANDLE readHandle;
        PDISK disk;

        _snwprintf_s(path, RTL_NUMBER_OF(path), _TRUNCATE, L"\\\\.\\PhysicalDrive%lu", n);
        readHandle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

        if (readHandle == INVALID_HANDLE_VALUE)
            readHandle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (readHandle == INVALID_HANDLE_VALUE)
            continue;

        disk = PhAllocateZero(sizeof(DISK));
        disk->Number = n;
        QueryDescriptor(readHandle, disk);
        CloseHandle(readHandle);

        if (!showVirtual && (disk->BusType == BusTypeFileBackedVirtual || disk->BusType == BusTypeVirtual))
        {
            PhDereferenceObject(disk->Model);
            PhDereferenceObject(disk->Serial);
            PhDereferenceObject(disk->Firmware);
            PhFree(disk);
            continue;
        }

        disk->Handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        disk->Health = SmartUnknown;
        disk->Smart.TemperatureC = LONG_MIN;
        disk->Smart.WearPercent = -1;
        disk->Smart.PowerOnHours = ULLONG_MAX;
        disk->Smart.PowerCycles = ULLONG_MAX;
        disk->Smart.BytesWritten = ULLONG_MAX;
        disk->Smart.BytesRead = ULLONG_MAX;
        disk->NextSmartTick = 1 + DiskCount; // first reads spread over the first ticks
        PhInitializeQueuedLock(&disk->Lock);
        PhInitializeCircularBuffer_FLOAT(&disk->ReadHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->WriteHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->ActiveHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->ResponseHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->TemperatureHistory, sampleCount);

        Disks[DiskCount++] = disk;
    }
}

// Drive letters per disk ("C: K:") and the volumes line with free space.
static VOID RefreshVolumes(VOID)
{
    WCHAR letters[MAX_DISKS][32] = { 0 };
    WCHAR volumes[MAX_DISKS][256] = { 0 };
    DWORD mask = GetLogicalDrives();

    for (WCHAR letter = L'A'; letter <= L'Z'; letter++)
    {
        WCHAR path[8];
        WCHAR root[4] = { letter, L':', L'\\', 0 };
        UCHAR buffer[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
        PVOLUME_DISK_EXTENTS extents = (PVOLUME_DISK_EXTENTS)buffer;
        ULARGE_INTEGER freeBytes, totalBytes;
        DWORD returned;
        HANDLE handle;

        if (!(mask & (1 << (letter - L'A'))))
            continue;
        if (GetDriveTypeW(root) != DRIVE_FIXED && GetDriveTypeW(root) != DRIVE_REMOVABLE)
            continue;

        _snwprintf_s(path, RTL_NUMBER_OF(path), _TRUNCATE, L"\\\\.\\%c:", letter);
        handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buffer, sizeof(buffer), &returned, NULL))
        {
            BOOLEAN haveSpace = !!GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, NULL);

            for (ULONG e = 0; e < extents->NumberOfDiskExtents && e < 8; e++)
            {
                ULONG number = extents->Extents[e].DiskNumber;
                WCHAR entry[96];

                if (number >= MAX_DISKS)
                    continue;

                _snwprintf_s(entry, RTL_NUMBER_OF(entry), _TRUNCATE, L"%s%c:", letters[number][0] ? L" " : L"", letter);
                wcsncat_s(letters[number], RTL_NUMBER_OF(letters[number]), entry, _TRUNCATE);

                if (haveSpace)
                {
                    PH_FORMAT format[6];
                    WCHAR text[96];

                    PhInitFormatC(&format[0], letter);
                    PhInitFormatS(&format[1], L": ");
                    PhInitFormatSize(&format[2], freeBytes.QuadPart);
                    PhInitFormatS(&format[3], L" free of ");
                    PhInitFormatSize(&format[4], totalBytes.QuadPart);
                    PhInitFormatS(&format[5], L"     ");
                    if (PhFormatToBuffer(format, 6, text, sizeof(text), NULL))
                        wcsncat_s(volumes[number], RTL_NUMBER_OF(volumes[number]), text, _TRUNCATE);
                }
            }
        }

        CloseHandle(handle);
    }

    for (ULONG i = 0; i < DiskCount; i++)
    {
        PDISK disk = Disks[i];

        PhAcquireQueuedLockExclusive(&disk->Lock);
        wcsncpy_s(disk->Letters, RTL_NUMBER_OF(disk->Letters), letters[disk->Number], _TRUNCATE);
        wcsncpy_s(disk->Volumes, RTL_NUMBER_OF(disk->Volumes), volumes[disk->Number][0] ? volumes[disk->Number] : L"No volumes with a drive letter", _TRUNCATE);
        PhReleaseQueuedLockExclusive(&disk->Lock);
    }
}

static VOID MakeTitles(VOID)
{
    for (ULONG i = 0; i < DiskCount; i++)
    {
        PDISK disk = Disks[i];
        WCHAR title[64];

        if (disk->Letters[0])
            _snwprintf_s(title, RTL_NUMBER_OF(title), _TRUNCATE, L"Disk %lu (%s)", disk->Number, disk->Letters);
        else
            _snwprintf_s(title, RTL_NUMBER_OF(title), _TRUNCATE, L"Disk %lu", disk->Number);

        disk->Title = PhCreateString(title);
    }
}

// ---- sampling (provider thread) ----------------------------------------------------------------

static VOID SamplePerformance(_In_ PDISK Disk)
{
    DISK_PERFORMANCE perf;
    DWORD returned;

    Disk->ReadRate = Disk->WriteRate = Disk->Active = Disk->ResponseMs = 0;

    if (Disk->Handle != INVALID_HANDLE_VALUE &&
        DeviceIoControl(Disk->Handle, IOCTL_DISK_PERFORMANCE, NULL, 0, &perf, sizeof(perf), &returned, NULL))
    {
        if (Disk->HavePrevious)
        {
            LONG64 dt = perf.QueryTime.QuadPart - Disk->Previous.QueryTime.QuadPart;     // 100 ns
            LONG64 idle = perf.IdleTime.QuadPart - Disk->Previous.IdleTime.QuadPart;
            ULONG64 ops = (ULONG64)(perf.ReadCount - Disk->Previous.ReadCount) + (ULONG64)(perf.WriteCount - Disk->Previous.WriteCount);
            LONG64 ioTime = (perf.ReadTime.QuadPart - Disk->Previous.ReadTime.QuadPart) + (perf.WriteTime.QuadPart - Disk->Previous.WriteTime.QuadPart);

            if (dt > 0)
            {
                FLOAT seconds = dt / 1e7f;
                FLOAT active = 1.0f - (FLOAT)idle / (FLOAT)dt;

                Disk->ReadRate = (perf.BytesRead.QuadPart - Disk->Previous.BytesRead.QuadPart) / seconds;
                Disk->WriteRate = (perf.BytesWritten.QuadPart - Disk->Previous.BytesWritten.QuadPart) / seconds;
                Disk->Active = active < 0 ? 0 : active > 1 ? 1 : active;
            }

            if (ops && ioTime > 0)
            {
                Disk->ResponseMs = (FLOAT)ioTime / ops / 1e4f;
                Disk->WindowOps += ops;
                Disk->WindowTime += ioTime;
            }
        }

        Disk->Previous = perf;
        Disk->HavePrevious = TRUE;
    }

    PhAddItemCircularBuffer_FLOAT(&Disk->ReadHistory, Disk->ReadRate);
    PhAddItemCircularBuffer_FLOAT(&Disk->WriteHistory, Disk->WriteRate);
    PhAddItemCircularBuffer_FLOAT(&Disk->ActiveHistory, Disk->Active);
    PhAddItemCircularBuffer_FLOAT(&Disk->ResponseHistory, Disk->ResponseMs);
    PhAddItemCircularBuffer_FLOAT(&Disk->TemperatureHistory, Disk->Smart.TemperatureC == LONG_MIN ? 0.0f : (FLOAT)Disk->Smart.TemperatureC);
}

static VOID EvaluateHealth(_In_ PDISK Disk, _In_ PDISK_SMART Smart, _Out_writes_(512) PWSTR Problems, _Out_ SMART_STATUS* Status)
{
    Problems[0] = 0;
    *Status = Smart->Kind ? SmartGood : SmartUnknown;

    SmartEvaluate(Smart, Problems, 512, Status);

    // Latency over the last SMART interval (remote support agent: >= 20 ms warn, >= 100 ms fail).
    if (Disk->WindowOps >= 10)
    {
        FLOAT ms = (FLOAT)Disk->WindowTime / Disk->WindowOps / 1e4f;

        if (*Status == SmartUnknown)
            *Status = SmartGood;
        if (ms >= 100)
            HealthAddProblem(Problems, 512, Status, SmartFail, L"response %.0f ms", ms);
        else if (ms >= 20)
            HealthAddProblem(Problems, 512, Status, SmartWarn, L"response %.0f ms", ms);
    }

    Disk->WindowOps = 0;
    Disk->WindowTime = 0;

    // Free space on this disk's volumes: < 10 % warn, < 5 % fail; system drive < 10 GB warn, < 3 GB fail.
    for (PWSTR l = Disk->Letters; *l; l++)
    {
        WCHAR root[4] = { *l, L':', L'\\', 0 };
        ULARGE_INTEGER freeBytes, totalBytes;

        if (*l < L'A' || *l > L'Z' || l[1] != L':')
            continue;
        if (!GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, NULL) || !totalBytes.QuadPart)
            continue;

        {
            FLOAT percent = 100.0f * freeBytes.QuadPart / totalBytes.QuadPart;
            FLOAT gb = freeBytes.QuadPart / 1073741824.0f;
            BOOLEAN system = *l == SystemLetter;

            if (*Status == SmartUnknown)
                *Status = SmartGood;
            if (percent < 5 || (system && gb < 3))
                HealthAddProblem(Problems, 512, Status, SmartFail, L"%c: %.0f%% free", *l, percent);
            else if (percent < 10 || (system && gb < 10))
                HealthAddProblem(Problems, 512, Status, SmartWarn, L"%c: %.0f%% free", *l, percent);
        }
    }

    if (!Problems[0])
    {
        // Healthy: a short summary instead.
        WCHAR part[64];

        if (Smart->WearPercent >= 0)
        {
            _snwprintf_s(part, RTL_NUMBER_OF(part), _TRUNCATE, L"wear %ld%%", Smart->WearPercent);
            wcsncat_s(Problems, 512, part, _TRUNCATE);
        }
        if (Smart->PowerOnHours != ULLONG_MAX)
        {
            _snwprintf_s(part, RTL_NUMBER_OF(part), _TRUNCATE, L"%s%I64u h on", Problems[0] ? L", " : L"", Smart->PowerOnHours);
            wcsncat_s(Problems, 512, part, _TRUNCATE);
        }
        if (!Smart->Kind && Smart->Note[0])
            wcsncat_s(Problems, 512, Smart->Note, _TRUNCATE);
    }
}

static VOID SampleSmart(_In_ PDISK Disk)
{
    DISK_SMART smart;
    WCHAR problems[512];
    SMART_STATUS status;
    BOOL poweredOn = TRUE;

    // Never wake a sleeping hard disk just to read SMART: try again next tick.
    if (!Disk->IsSsd && Disk->Handle != INVALID_HANDLE_VALUE && GetDevicePowerState(Disk->Handle, &poweredOn) && !poweredOn)
    {
        Disk->Asleep = TRUE;
        Disk->NextSmartTick = TickCount + 1;
        return;
    }

    Disk->Asleep = FALSE;
    SmartRead(Disk->Number, Disk->BusType, &smart);
    EvaluateHealth(Disk, &smart, problems, &status);

    PhAcquireQueuedLockExclusive(&Disk->Lock);
    Disk->Smart = smart;
    Disk->Health = status;
    wcsncpy_s(Disk->Problems, RTL_NUMBER_OF(Disk->Problems), problems, _TRUNCATE);
    Disk->SmartTried = TRUE;
    Disk->SmartVersion++;
    PhReleaseQueuedLockExclusive(&Disk->Lock);

    Disk->NextSmartTick = TickCount + SmartIntervalTicks;
}

static VOID SampleAll(VOID)
{
    BOOLEAN smartDone = FALSE;

    TickCount++;

    if (TickCount % 10 == 1)
        RefreshVolumes();

    for (ULONG i = 0; i < DiskCount; i++)
    {
        PDISK disk = Disks[i];

        // At most one SMART read per tick, so the provider thread never stalls on all disks at once.
        if (!smartDone && TickCount >= disk->NextSmartTick)
        {
            SampleSmart(disk);
            smartDone = !disk->Asleep;
        }

        SamplePerformance(disk);
    }
}

// ---- formatting --------------------------------------------------------------------------------

static PPH_STRING FormatRate(_In_ FLOAT BytesPerSecond)
{
    PH_FORMAT format[2];

    PhInitFormatSize(&format[0], (ULONG64)BytesPerSecond);
    PhInitFormatS(&format[1], L"/s");
    return PhFormat(format, 2, 0);
}

static PPH_STRING FormatTemperature(_In_ FLOAT Celsius)
{
    PH_FORMAT format[2];

    if (UseFahrenheit)
    {
        PhInitFormatF(&format[0], Celsius * 1.8f + 32.0f, 0);
        PhInitFormatS(&format[1], L" °F");
    }
    else
    {
        PhInitFormatF(&format[0], Celsius, 0);
        PhInitFormatS(&format[1], L" °C");
    }

    return PhFormat(format, 2, 0);
}

static PPH_STRING FormatSlot(_In_ ULONG Slot, _In_ FLOAT Value)
{
    PH_FORMAT format[2];

    switch (Slot)
    {
    case SlotActive:
        PhInitFormatF(&format[0], Value * 100, 0);
        PhInitFormatC(&format[1], L'%');
        return PhFormat(format, 2, 0);
    case SlotResponse:
        PhInitFormatF(&format[0], Value, 1);
        PhInitFormatS(&format[1], L" ms");
        return PhFormat(format, 2, 0);
    case SlotTemperature:
        return FormatTemperature(Value);
    default:
        return FormatRate(Value);
    }
}

#define DEFINE_LABEL_Y(Slot) \
    _Function_class_(PH_GRAPH_LABEL_Y_FUNCTION) \
    static PPH_STRING NTAPI LabelY##Slot(_In_ PPH_GRAPH_DRAW_INFO DrawInfo, _In_ ULONG DataIndex, _In_ FLOAT Value, _In_ FLOAT Parameter) \
    { return FormatSlot(Slot, Value * Parameter); }

DEFINE_LABEL_Y(SlotReadWrite)
DEFINE_LABEL_Y(SlotActive)
DEFINE_LABEL_Y(SlotResponse)
DEFINE_LABEL_Y(SlotTemperature)

static PPH_GRAPH_LABEL_Y_FUNCTION LabelYFunctions[SLOT_COUNT] = { LabelYSlotReadWrite, LabelYSlotActive, LabelYSlotResponse, LabelYSlotTemperature };

static PCWSTR HealthText(_In_ SMART_STATUS Status)
{
    switch (Status)
    {
    case SmartGood: return L"Good";
    case SmartWarn: return L"Caution";
    case SmartFail: return L"Bad";
    default: return L"Unknown";
    }
}

static PCWSTR BusText(_In_ STORAGE_BUS_TYPE Bus)
{
    switch (Bus)
    {
    case BusTypeNvme: return L"NVMe";
    case BusTypeSata: return L"SATA";
    case BusTypeAta: return L"ATA";
    case BusTypeUsb: return L"USB";
    case BusTypeSas: return L"SAS";
    case BusTypeScsi: return L"SCSI";
    case BusTypeRAID: return L"RAID";
    case BusTypeSd: return L"SD";
    case BusTypeMmc: return L"MMC";
    case BusTypeVirtual: case BusTypeFileBackedVirtual: return L"Virtual";
    default: return L"Disk";
    }
}

// ---- graphs ------------------------------------------------------------------------------------

static PPH_CIRCULAR_BUFFER_FLOAT SlotHistory(_In_ PDISK Disk, _In_ ULONG Slot)
{
    switch (Slot)
    {
    case SlotActive: return &Disk->ActiveHistory;
    case SlotResponse: return &Disk->ResponseHistory;
    case SlotTemperature: return &Disk->TemperatureHistory;
    default: return &Disk->ReadHistory;
    }
}

// Fills Data1 (and Data2 for read/write), returns the scale used.
static FLOAT FillSlot(_In_ PDISK Disk, _In_ ULONG Slot, _Out_writes_(Count) PFLOAT Data1, _Out_writes_opt_(Count) PFLOAT Data2, _In_ ULONG Count)
{
    FLOAT max = 0;

    for (ULONG i = 0; i < Count; i++)
    {
        Data1[i] = PhGetItemCircularBuffer_FLOAT(SlotHistory(Disk, Slot), i);
        if (Data1[i] > max) max = Data1[i];

        if (Slot == SlotReadWrite && Data2)
        {
            Data2[i] = PhGetItemCircularBuffer_FLOAT(&Disk->WriteHistory, i);
            if (Data2[i] > max) max = Data2[i];
        }
    }

    switch (Slot)
    {
    case SlotActive: max = 1.0f; break;
    case SlotTemperature: max = 100.0f; break;
    case SlotResponse: if (max < 10.0f) max = 10.0f; break;
    default: if (max < 1024.0f * 1024.0f) max = 1024.0f * 1024.0f; break;   // at least 1 MB/s
    }

    for (ULONG i = 0; i < Count; i++)
    {
        Data1[i] = min(Data1[i] / max, 1.0f);
        if (Slot == SlotReadWrite && Data2)
            Data2[i] = min(Data2[i] / max, 1.0f);
    }

    return max;
}

static PPH_STRING ReadWriteTooltip(_In_ PDISK Disk, _In_ ULONG Index)
{
    PH_FORMAT format[6];

    PhInitFormatS(&format[0], L"R: ");
    PhInitFormatSR(&format[1], PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&Disk->ReadHistory, Index)))->sr);
    PhInitFormatS(&format[2], L"\nW: ");
    PhInitFormatSR(&format[3], PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&Disk->WriteHistory, Index)))->sr);
    PhInitFormatC(&format[4], L'\n');
    PhInitFormatSR(&format[5], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, Index))->sr);
    return PhFormat(format, RTL_NUMBER_OF(format), 0);
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
    PDISK disk = Context;
    NMHDR* header = Parameter1;
    ULONG slot = ULONG_MAX;

    if (!disk || !header)
        return TRUE;

    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        if (header->hwndFrom == disk->GraphHandle[s])
            slot = s;
    }

    if (slot == ULONG_MAX)
        return TRUE;

    switch (header->code)
    {
    case GCN_GETDRAWINFO:
        {
            PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)header;
            PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;
            PPH_GRAPH_STATE state = &disk->GraphState[slot];

            if (slot == SlotReadWrite)
            {
                drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_LABEL_MAX_Y | PH_GRAPH_USE_LINE_2;
                disk->Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_IO_READ_OTHER), PhGetIntegerSetting(SETTING_COLOR_IO_WRITE), disk->DialogDpi);
            }
            else
            {
                drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_LABEL_MAX_Y;
                disk->Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(
                    slot == SlotActive ? SETTING_COLOR_CPU_KERNEL : slot == SlotResponse ? SETTING_COLOR_PRIVATE : SETTING_COLOR_TEMPERATURE), 0, disk->DialogDpi);
            }

            PhGraphStateGetDrawInfo(state, getDrawInfo, disk->ReadHistory.Count);

            if (!state->Valid)
            {
                FLOAT max = FillSlot(disk, slot, state->Data1, slot == SlotReadWrite ? state->Data2 : NULL, drawInfo->LineDataCount);

                drawInfo->LabelYFunction = LabelYFunctions[slot];
                drawInfo->LabelYFunctionParameter = max;
                state->Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)header;
            PPH_GRAPH_STATE state = &disk->GraphState[slot];

            if (getTooltipText->Index < getTooltipText->TotalCount)
            {
                if (state->TooltipIndex != getTooltipText->Index)
                {
                    if (slot == SlotReadWrite)
                    {
                        PhMoveReference(&state->TooltipText, ReadWriteTooltip(disk, getTooltipText->Index));
                    }
                    else
                    {
                        PH_FORMAT format[3];

                        PhInitFormatSR(&format[0], PH_AUTO_T(PH_STRING, FormatSlot(slot, PhGetItemCircularBuffer_FLOAT(SlotHistory(disk, slot), getTooltipText->Index)))->sr);
                        PhInitFormatC(&format[1], L'\n');
                        PhInitFormatSR(&format[2], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);
                        PhMoveReference(&state->TooltipText, PhFormat(format, 3, 0));
                    }
                }

                getTooltipText->Text = PhGetStringRef(state->TooltipText);
            }
        }
        break;
    }

    return TRUE;
}

static VOID CreateGraphs(_In_ PDISK Disk)
{
    PH_GRAPH_CREATEPARAMS createParams;

    memset(&createParams, 0, sizeof(PH_GRAPH_CREATEPARAMS));
    createParams.Size = sizeof(PH_GRAPH_CREATEPARAMS);
    createParams.Callback = GraphMessageCallback;
    createParams.Context = Disk;

    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        Disk->GraphHandle[s] = PhCreateWindow(PH_GRAPH_CLASSNAME, NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS,
            0, 0, 0, 0, Disk->Dialog, NULL, NULL, &createParams);
        Graph_SetTooltip(Disk->GraphHandle[s], TRUE);
    }
}

static VOID LayoutGraphs(_In_ PDISK Disk)
{
    RECT clientRect;
    RECT labelRect;
    RECT margin = Disk->GraphMarginScaled;
    LONG padding = PhScaleToDisplay(GRAPH_PADDING, Disk->DialogDpi);
    LONG graphWidth;
    LONG graphHeight;
    LONG y;
    HDWP defer;

    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        Disk->GraphState[s].Valid = FALSE;
        Disk->GraphState[s].TooltipIndex = ULONG_MAX;
    }

    PhGetClientRect(Disk->Dialog, &clientRect);
    PhGetClientRect(GetDlgItem(Disk->Dialog, IDC_LABEL0), &labelRect);
    graphWidth = clientRect.right - margin.left - margin.right;
    graphHeight = (clientRect.bottom - margin.top - margin.bottom - labelRect.bottom * SLOT_COUNT - padding * (SLOT_COUNT + 2)) / SLOT_COUNT;

    defer = BeginDeferWindowPos(SLOT_COUNT * 2);
    y = margin.top;

    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        BOOLEAN last = s == SLOT_COUNT - 1;

        defer = DeferWindowPos(defer, GetDlgItem(Disk->Dialog, LabelIds[s]), NULL, margin.left, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        y += labelRect.bottom + padding;
        defer = DeferWindowPos(defer, Disk->GraphHandle[s], NULL, margin.left, y, graphWidth,
            last ? clientRect.bottom - margin.bottom - y : graphHeight, SWP_NOACTIVATE | SWP_NOZORDER);
        y += graphHeight + padding;
    }

    EndDeferWindowPos(defer);
}

// ---- panel -------------------------------------------------------------------------------------

static VOID SetLabelFormat(_In_ HWND Dialog, _In_ ULONG Id, _In_ PPH_STRING String)
{
    PhSetDialogItemText(Dialog, Id, PhGetString(String));
    PhDereferenceObject(String);
}

static VOID FillSmartList(_In_ PDISK Disk, _In_ PDISK_SMART Smart)
{
    PCWSTR statusText[] = { L"OK", L"Warning", L"FAIL" };

    SendMessage(Disk->SmartList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(Disk->SmartList);

    if (!Smart->Kind)
    {
        PhAddListViewItem(Disk->SmartList, MAXINT, L"", NULL);
        PhSetListViewSubItem(Disk->SmartList, 0, 1, Smart->Note[0] ? Smart->Note : L"SMART not read yet");
    }

    for (ULONG i = 0; i < Smart->RowCount; i++)
    {
        PSMART_ROW row = &Smart->Rows[i];
        LONG index = PhAddListViewItem(Disk->SmartList, MAXINT, row->Id, NULL);

        PhSetListViewSubItem(Disk->SmartList, index, 1, row->Name);
        PhSetListViewSubItem(Disk->SmartList, index, 2, row->Current);
        PhSetListViewSubItem(Disk->SmartList, index, 3, row->Worst);
        PhSetListViewSubItem(Disk->SmartList, index, 4, row->Threshold);
        PhSetListViewSubItem(Disk->SmartList, index, 5, row->Raw);
        PhSetListViewSubItem(Disk->SmartList, index, 6, row->Status >= SmartGood && row->Status <= SmartFail ? statusText[row->Status] : L"");
    }

    SendMessage(Disk->SmartList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(Disk->SmartList, NULL, FALSE);
}

static VOID UpdatePanel(_In_ PDISK Disk)
{
    DISK_SMART* smart;
    SMART_STATUS health;
    WCHAR problems[512];
    WCHAR volumes[256];
    WCHAR text[128];
    BOOLEAN tried, asleep;
    ULONG version;

    if (!Disk->Panel)
        return;

    smart = PhAllocate(sizeof(DISK_SMART));

    PhAcquireQueuedLockShared(&Disk->Lock);
    *smart = Disk->Smart;
    health = Disk->Health;
    wcsncpy_s(problems, RTL_NUMBER_OF(problems), Disk->Problems, _TRUNCATE);
    wcsncpy_s(volumes, RTL_NUMBER_OF(volumes), Disk->Volumes, _TRUNCATE);
    tried = Disk->SmartTried;
    asleep = Disk->Asleep;
    version = Disk->SmartVersion;
    PhReleaseQueuedLockShared(&Disk->Lock);

    if (!tried)
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, asleep ? L"Drive asleep (SMART on wake)" : L"Reading SMART...");
    else
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%s%s", HealthText(health), asleep ? L" (asleep)" : L"");
    PhSetDialogItemText(Disk->Panel, IDC_HEALTH_V, text);
    PhSetDialogItemText(Disk->Panel, IDC_PROBLEMS_V, problems);

    if (smart->TemperatureC != LONG_MIN)
        SetLabelFormat(Disk->Panel, IDC_TEMP_V, FormatTemperature((FLOAT)smart->TemperatureC));
    else
        PhSetDialogItemText(Disk->Panel, IDC_TEMP_V, L"-");

    if (smart->PowerOnHours != ULLONG_MAX)
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%I64u h (%.1f y)", smart->PowerOnHours, smart->PowerOnHours / 8766.0);
        PhSetDialogItemText(Disk->Panel, IDC_POWERON_V, text);
    }
    else
        PhSetDialogItemText(Disk->Panel, IDC_POWERON_V, L"-");

    if (smart->PowerCycles != ULLONG_MAX)
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%I64u", smart->PowerCycles);
        PhSetDialogItemText(Disk->Panel, IDC_CYCLES_V, text);
    }
    else
        PhSetDialogItemText(Disk->Panel, IDC_CYCLES_V, L"-");

    if (smart->WearPercent >= 0)
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%ld%% used", smart->WearPercent);
        PhSetDialogItemText(Disk->Panel, IDC_WEAR_V, text);
    }
    else
        PhSetDialogItemText(Disk->Panel, IDC_WEAR_V, Disk->IsSsd ? L"-" : L"n/a (HDD)");

    if (smart->BytesWritten != ULLONG_MAX)
    {
        PH_FORMAT format[1];
        PhInitFormatSize(&format[0], smart->BytesWritten);
        SetLabelFormat(Disk->Panel, IDC_WRITTEN_V, PhFormat(format, 1, 0));
    }
    else
        PhSetDialogItemText(Disk->Panel, IDC_WRITTEN_V, L"-");

    if (smart->BytesRead != ULLONG_MAX)
    {
        PH_FORMAT format[1];
        PhInitFormatSize(&format[0], smart->BytesRead);
        SetLabelFormat(Disk->Panel, IDC_READ_V, PhFormat(format, 1, 0));
    }
    else
        PhSetDialogItemText(Disk->Panel, IDC_READ_V, L"-");

    PhSetDialogItemText(Disk->Panel, IDC_VOLUMES_V, volumes);

    if (Disk->SmartList && version != Disk->ShownSmartVersion)
    {
        FillSmartList(Disk, smart);
        Disk->ShownSmartVersion = version;
    }

    PhFree(smart);
}

static VOID TickDialog(_In_ PDISK Disk)
{
    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        Disk->GraphState[s].Valid = FALSE;
        Disk->GraphState[s].TooltipIndex = ULONG_MAX;
        Graph_MoveGrid(Disk->GraphHandle[s], 1);
        Graph_Draw(Disk->GraphHandle[s]);
        Graph_UpdateTooltip(Disk->GraphHandle[s]);
        InvalidateRect(Disk->GraphHandle[s], NULL, FALSE);
    }

    UpdatePanel(Disk);
}

static INT_PTR CALLBACK PanelDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PDISK disk;

    if (WindowMessage == WM_INITDIALOG)
    {
        disk = (PDISK)lParam;
        PhSetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT, disk);
    }
    else
    {
        disk = PhGetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
    }

    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            LONG dpi = PhGetWindowDpi(WindowHandle);

            disk->SmartList = GetDlgItem(WindowHandle, IDC_SMART);
            disk->ShownSmartVersion = ULONG_MAX;
            ListView_SetExtendedListViewStyle(disk->SmartList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            PhAddListViewColumn(disk->SmartList, 0, 0, 0, LVCFMT_LEFT, PhScaleToDisplay(34, dpi), L"ID");
            PhAddListViewColumn(disk->SmartList, 1, 1, 1, LVCFMT_LEFT, PhScaleToDisplay(230, dpi), L"Attribute");
            PhAddListViewColumn(disk->SmartList, 2, 2, 2, LVCFMT_RIGHT, PhScaleToDisplay(56, dpi), L"Current");
            PhAddListViewColumn(disk->SmartList, 3, 3, 3, LVCFMT_RIGHT, PhScaleToDisplay(50, dpi), L"Worst");
            PhAddListViewColumn(disk->SmartList, 4, 4, 4, LVCFMT_RIGHT, PhScaleToDisplay(64, dpi), L"Threshold");
            PhAddListViewColumn(disk->SmartList, 5, 5, 5, LVCFMT_RIGHT, PhScaleToDisplay(170, dpi), L"Raw / value");
            PhAddListViewColumn(disk->SmartList, 6, 6, 6, LVCFMT_LEFT, PhScaleToDisplay(60, dpi), L"Status");
        }
        break;
    case WM_SIZE:
        {
            RECT clientRect;
            RECT listRect;
            RECT volumesRect;
            HWND volumes = GetDlgItem(WindowHandle, IDC_VOLUMES_V);

            if (disk && disk->SmartList)
            {
                PhGetClientRect(WindowHandle, &clientRect);
                GetWindowRect(disk->SmartList, &listRect);
                MapWindowRect(NULL, WindowHandle, &listRect);
                SetWindowPos(disk->SmartList, NULL, 0, 0, clientRect.right - listRect.left,
                    clientRect.bottom - listRect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

                // Stretch the volumes line to the full width.
                GetWindowRect(volumes, &volumesRect);
                SetWindowPos(volumes, NULL, 0, 0, clientRect.right, volumesRect.bottom - volumesRect.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        break;
    case WM_DESTROY:
        {
            if (disk)
                disk->SmartList = NULL;
            PhRemoveWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
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

static INT_PTR CALLBACK DiskDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PDISK disk;

    if (WindowMessage == WM_INITDIALOG)
    {
        disk = (PDISK)lParam;
        PhSetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT, disk);
    }
    else
    {
        disk = PhGetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
    }

    if (!disk)
        return FALSE;

    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            PPH_LAYOUT_ITEM graphItem;
            PPH_LAYOUT_ITEM panelItem;
            WCHAR title[32];
            WCHAR name[256];
            PH_FORMAT format[1];
            WCHAR size[32] = L"";

            for (ULONG s = 0; s < SLOT_COUNT; s++)
            {
                PhInitializeGraphState(&disk->GraphState[s]);
                PhSetDialogItemText(WindowHandle, LabelIds[s], SlotLabels[s]);
            }

            disk->Dialog = WindowHandle;
            disk->DialogDpi = PhGetWindowDpi(WindowHandle);
            PhSetWindowExStyle(WindowHandle, WS_EX_TRANSPARENT, 0);

            PhInitializeLayoutManager(&disk->LayoutManager, WindowHandle);
            PhAddLayoutItem(&disk->LayoutManager, GetDlgItem(WindowHandle, IDC_DISKNAME), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_TOP | PH_ANCHOR_RIGHT | PH_LAYOUT_FORCE_INVALIDATE);
            graphItem = PhAddLayoutItem(&disk->LayoutManager, GetDlgItem(WindowHandle, IDC_GRAPH_LAYOUT), NULL, PH_ANCHOR_ALL);
            panelItem = PhAddLayoutItem(&disk->LayoutManager, GetDlgItem(WindowHandle, IDC_PANEL_LAYOUT), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            disk->GraphMargin = graphItem->Margin;
            disk->GraphMarginScaled = disk->GraphMargin;
            PhGetMarginDpiValue(&disk->GraphMarginScaled, disk->DialogDpi, TRUE);

            SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), disk->Section->Parameters->LargeFont, FALSE);
            SetWindowFont(GetDlgItem(WindowHandle, IDC_DISKNAME), disk->Section->Parameters->MediumFont, FALSE);

            _snwprintf_s(title, RTL_NUMBER_OF(title), _TRUNCATE, L"Disk %lu", disk->Number);
            PhSetDialogItemText(WindowHandle, IDC_TITLE, title);

            PhInitFormatSize(&format[0], disk->Size);
            PhFormatToBuffer(format, 1, size, sizeof(size), NULL);
            _snwprintf_s(name, RTL_NUMBER_OF(name), _TRUNCATE, L"%s  •  %s %s  •  %s%s%s",
                PhGetString(disk->Model), BusText(disk->BusType), disk->IsSsd ? L"SSD" : L"HDD", size,
                disk->Firmware->Length ? L"  •  FW " : L"", PhGetString(disk->Firmware));
            PhSetDialogItemText(WindowHandle, IDC_DISKNAME, name);

            disk->Panel = PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_DISK_PANEL), WindowHandle, PanelDialogProc, disk);
            ShowWindow(disk->Panel, SW_SHOW);
            PhAddLayoutItemEx(&disk->LayoutManager, disk->Panel, NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM, &panelItem->Margin);

            CreateGraphs(disk);
            TickDialog(disk);

            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
        }
        break;
    case WM_DESTROY:
        {
            PhDeleteLayoutManager(&disk->LayoutManager);
            PhRemoveWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
        }
        break;
    case WM_DPICHANGED_AFTERPARENT:
        {
            disk->DialogDpi = PhGetWindowDpi(WindowHandle);
            disk->GraphMarginScaled = disk->GraphMargin;
            PhGetMarginDpiValue(&disk->GraphMarginScaled, disk->DialogDpi, TRUE);

            if (disk->Section->Parameters->LargeFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), disk->Section->Parameters->LargeFont, FALSE);
            if (disk->Section->Parameters->MediumFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_DISKNAME), disk->Section->Parameters->MediumFont, FALSE);

            PhLayoutManagerUpdate(&disk->LayoutManager, LOWORD(wParam));
            PhLayoutManagerLayout(&disk->LayoutManager);
            LayoutGraphs(disk);
        }
        break;
    case WM_SIZE:
        {
            PhLayoutManagerLayout(&disk->LayoutManager);
            LayoutGraphs(disk);
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

// ---- System Information section ----------------------------------------------------------------

_Function_class_(PH_SYSINFO_SECTION_CALLBACK)
static BOOLEAN NTAPI SectionCallback(
    _In_ PPH_SYSINFO_SECTION Section,
    _In_ PH_SYSINFO_SECTION_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2
    )
{
    PDISK disk = Section->Context;

    switch (Message)
    {
    case SysInfoDestroy:
        {
            if (disk->Dialog)
            {
                for (ULONG s = 0; s < SLOT_COUNT; s++)
                {
                    PhDeleteGraphState(&disk->GraphState[s]);
                    disk->GraphHandle[s] = NULL;
                }

                disk->Dialog = NULL;
                disk->Panel = NULL;
            }
        }
        return TRUE;
    case SysInfoTick:
        {
            if (disk->Dialog)
                TickDialog(disk);
        }
        return TRUE;
    case SysInfoViewChanging:
        {
            PH_SYSINFO_VIEW_TYPE view = (PH_SYSINFO_VIEW_TYPE)PtrToUlong(Parameter1);

            if (view == SysInfoSummaryView || (PPH_SYSINFO_SECTION)Parameter2 != Section)
                return TRUE;

            for (ULONG s = 0; s < SLOT_COUNT; s++)
            {
                if (disk->GraphHandle[s])
                {
                    disk->GraphState[s].Valid = FALSE;
                    disk->GraphState[s].TooltipIndex = ULONG_MAX;
                    Graph_Draw(disk->GraphHandle[s]);
                }
            }
        }
        return TRUE;
    case SysInfoCreateDialog:
        {
            PPH_SYSINFO_CREATE_DIALOG createDialog = Parameter1;

            createDialog->Instance = PluginInstance->DllBase;
            createDialog->Template = MAKEINTRESOURCE(IDD_DISK_DIALOG);
            createDialog->DialogProc = DiskDialogProc;
            createDialog->Parameter = disk;
        }
        return TRUE;
    case SysInfoGraphGetDrawInfo:
        {
            PPH_GRAPH_DRAW_INFO drawInfo = Parameter1;

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_USE_LINE_2;
            Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_IO_READ_OTHER), PhGetIntegerSetting(SETTING_COLOR_IO_WRITE), Section->Parameters->WindowDpi);
            PhGetDrawInfoGraphBuffers(&Section->GraphState.Buffers, drawInfo, disk->ReadHistory.Count);

            if (!Section->GraphState.Valid)
            {
                FillSlot(disk, SlotReadWrite, Section->GraphState.Data1, Section->GraphState.Data2, drawInfo->LineDataCount);
                Section->GraphState.Valid = TRUE;
            }
        }
        return TRUE;
    case SysInfoGraphGetTooltipText:
        {
            PPH_SYSINFO_GRAPH_GET_TOOLTIP_TEXT getTooltipText = Parameter1;

            if (!getTooltipText)
                break;

            PhMoveReference(&Section->GraphState.TooltipText, ReadWriteTooltip(disk, getTooltipText->Index));
            getTooltipText->Text = PhGetStringRef(Section->GraphState.TooltipText);
        }
        return TRUE;
    case SysInfoGraphDrawPanel:
        {
            PPH_SYSINFO_DRAW_PANEL drawPanel = Parameter1;
            PH_FORMAT format[6];
            SMART_STATUS health;

            if (!drawPanel)
                break;

            PhAcquireQueuedLockShared(&disk->Lock);
            health = disk->Health;
            PhReleaseQueuedLockShared(&disk->Lock);

            drawPanel->Title = PhReferenceObject(disk->Title);

            PhInitFormatS(&format[0], L"R: ");
            PhInitFormatSR(&format[1], PH_AUTO_T(PH_STRING, FormatRate(disk->ReadRate))->sr);
            PhInitFormatS(&format[2], L"\nW: ");
            PhInitFormatSR(&format[3], PH_AUTO_T(PH_STRING, FormatRate(disk->WriteRate))->sr);
            PhInitFormatS(&format[4], health >= SmartWarn ? L"  ⚠ " : L"");
            PhInitFormatS(&format[5], health >= SmartWarn ? (PWSTR)HealthText(health) : L"");
            drawPanel->SubTitle = PhFormat(format, 6, 64);
        }
        return TRUE;
    }

    return FALSE;
}

// ---- plugin ------------------------------------------------------------------------------------

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    static CONST PH_STRINGREF extendedTools = PH_STRINGREF_INIT(L"ExtendedTools");
    WCHAR systemDirectory[MAX_PATH];
    ULONG interval;

    if (PhFindPlugin2(&extendedTools))
    {
        __try
        {
            UseFahrenheit = !!PhGetIntegerSetting(L"ExtendedTools.EnableFahrenheit");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            UseFahrenheit = FALSE;
        }
    }

    if (GetSystemDirectoryW(systemDirectory, RTL_NUMBER_OF(systemDirectory)))
        SystemLetter = (WCHAR)towupper(systemDirectory[0]);

    interval = PhGetIntegerSetting(SETTING_NAME_SMART_INTERVAL);
    SmartIntervalTicks = max(interval * 1000 / max(PhGetIntegerSetting(SETTING_UPDATE_INTERVAL), 100), 5);

    DiscoverDisks();
    RefreshVolumes();
    MakeTitles();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    SampleAll();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI SystemInformationInitializingCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    PPH_PLUGIN_SYSINFO_POINTERS pointers = Parameter;

    if (!pointers)
        return;

    for (ULONG i = 0; i < DiskCount; i++)
    {
        PDISK disk = Disks[i];
        PH_SYSINFO_SECTION section;

        memset(&section, 0, sizeof(PH_SYSINFO_SECTION));
        section.Name = disk->Title->sr;
        section.Callback = SectionCallback;
        section.Context = disk;

        disk->Section = pointers->CreateSection(&section);
    }
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
            { IntegerSettingType, SETTING_NAME_SMART_INTERVAL, L"3c" },   // 60 s
            { IntegerSettingType, SETTING_NAME_SHOW_VIRTUAL, L"0" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Disk Monitor";
        info->Description = L"One System Information section per physical disk with speed, active time, response time, temperature and SMART health.";
        info->Author = L"Alihkhawaher";
        info->Url = L"https://github.com/Alihkhawaher/systeminformer-plugins";

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackProcessProviderUpdatedEvent), ProcessesUpdatedCallback, NULL, &ProcessesUpdatedCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackSystemInformationInitializing), SystemInformationInitializingCallback, NULL, &SystemInformationInitializingCallbackRegistration);

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
