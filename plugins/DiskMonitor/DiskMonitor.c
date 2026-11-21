/*
 * DiskMonitor - System Informer plugin: one "Disks" System Information section for all physical disks.
 *
 * Top of the page: one tile per disk (name, health, temperature, read/write mini graph); click a tile to select it.
 * Below: the selected disk's graphs (read/write, active time, average response time, temperature), its health
 * panel (verdict with reasons, using the remote support agent's rules), its volumes with free space, the apps
 * doing I/O on it (ETW, see etw.c) and its full SMART table (NVMe log or ATA attributes, also through USB bridges,
 * see smart.c). Read/write tooltips name the busiest app at that moment.
 *
 * SMART is read once a minute per disk, one disk per refresh, and never from a hard disk that is spun down
 * (GetDevicePowerState), so the monitor doesn't keep sleeping drives awake.
 */

#include "diskmon.h"

#define PLUGIN_NAME L"DiskMonitor"
#define SETTING_NAME_SMART_INTERVAL (PLUGIN_NAME L".SmartIntervalSeconds")
#define SETTING_NAME_SHOW_VIRTUAL (PLUGIN_NAME L".ShowVirtualDisks")
#define SETTING_NAME_SELECTED (PLUGIN_NAME L".SelectedDisk")
#define GRAPH_PADDING 3
#define MAX_DISKS 32
#define MAX_APPS 64
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

typedef struct _DISK_APP
{
    ULONG ProcessId;
    FLOAT ReadRate;         // bytes/s
    FLOAT WriteRate;
} DISK_APP, *PDISK_APP;

typedef struct _DISK
{
    ULONG Index;
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
    FLOAT ReadRate;
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
    PH_CIRCULAR_BUFFER_ULONG TopAppHistory;
    PH_CIRCULAR_BUFFER_FLOAT TopAppReadHistory;
    PH_CIRCULAR_BUFFER_FLOAT TopAppWriteHistory;

    // everything below is written on the provider thread under Lock and read by the UI
    PH_QUEUED_LOCK Lock;
    ULONG SmartVersion;
    BOOLEAN SmartTried;
    BOOLEAN Asleep;
    ULONG NextSmartTick;
    DISK_SMART Smart;
    SMART_STATUS Health;
    WCHAR Problems[512];
    WCHAR Volumes[256];
    ULONG AppCount;
    DISK_APP Apps[MAX_APPS];

    // per-refresh scratch (provider thread)
    ULONG ScratchCount;
    DISK_APP Scratch[MAX_APPS];
} DISK, *PDISK;

// The one System Information page.
static struct
{
    PPH_SYSINFO_SECTION Section;
    HWND Dialog;
    LONG Dpi;
    PH_LAYOUT_MANAGER LayoutManager;
    RECT GraphMargin;
    RECT GraphMarginScaled;
    HWND TilesLayout;
    HWND TileLabel[MAX_DISKS];
    HWND TileGraph[MAX_DISKS];
    PH_GRAPH_STATE TileState[MAX_DISKS];
    HWND GraphHandle[SLOT_COUNT];
    PH_GRAPH_STATE GraphState[SLOT_COUNT];
    HWND Panel;
    HWND AppsList;
    HWND SmartList;
    ULONG Selected;
    ULONG ShownSmartVersion;
    ULONG ShownSmartDisk;
} Page;

static PPH_PLUGIN PluginInstance;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION SystemInformationInitializingCallbackRegistration;
static PDISK Disks[MAX_DISKS];
static ULONG DiskCount;
static ULONG TickCount;
static ULONG SmartIntervalTicks = 60;
static FLOAT TickSeconds = 1.0f;
static BOOLEAN UseFahrenheit;
static WCHAR SystemLetter;
static PH_CIRCULAR_BUFFER_FLOAT TotalReadHistory;
static PH_CIRCULAR_BUFFER_FLOAT TotalWriteHistory;

static CONST PH_STRINGREF WhitespaceStringRef = PH_STRINGREF_INIT(L" \t\r\n");
static CONST PH_STRINGREF SpaceStringRef = PH_STRINGREF_INIT(L" ");

// ---- discovery ---------------------------------------------------------------------------------

static PPH_STRING TrimAnsi(_In_ PSTORAGE_DEVICE_DESCRIPTOR Descriptor, _In_ ULONG Offset, _In_ ULONG Size)
{
    PPH_STRING string;
    PH_STRINGREF trimmed;
    PPH_STRING result;

    if (!Offset || Offset >= Size)
        return PhReferenceEmptyString();

    string = PhConvertMultiByteToUtf16((PSTR)Descriptor + Offset);
    trimmed = string->sr;
    PhTrimStringRef(&trimmed, &WhitespaceStringRef, 0);
    result = PhCreateString2(&trimmed);
    PhDereferenceObject(string);
    return result;
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

    PhInitializeCircularBuffer_FLOAT(&TotalReadHistory, sampleCount);
    PhInitializeCircularBuffer_FLOAT(&TotalWriteHistory, sampleCount);

    for (ULONG n = 0; n < 64 && DiskCount < MAX_DISKS; n++)
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

        disk->Index = DiskCount;
        disk->Handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        disk->Health = SmartUnknown;
        disk->Smart.TemperatureC = LONG_MIN;
        disk->Smart.WearPercent = -1;
        disk->Smart.PowerOnHours = ULLONG_MAX;
        disk->Smart.PowerCycles = ULLONG_MAX;
        disk->Smart.BytesWritten = ULLONG_MAX;
        disk->Smart.BytesRead = ULLONG_MAX;
        disk->NextSmartTick = 1 + DiskCount;    // first reads spread over the first refreshes
        PhInitializeQueuedLock(&disk->Lock);
        PhInitializeCircularBuffer_FLOAT(&disk->ReadHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->WriteHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->ActiveHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->ResponseHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->TemperatureHistory, sampleCount);
        PhInitializeCircularBuffer_ULONG(&disk->TopAppHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->TopAppReadHistory, sampleCount);
        PhInitializeCircularBuffer_FLOAT(&disk->TopAppWriteHistory, sampleCount);

        Disks[DiskCount++] = disk;
    }
}

static PDISK FindDiskByNumber(_In_ ULONG Number)
{
    for (ULONG i = 0; i < DiskCount; i++)
    {
        if (Disks[i]->Number == Number)
            return Disks[i];
    }

    return NULL;
}

// Drive letters per disk ("C: K:") and the volumes line with free space.
static VOID RefreshVolumes(VOID)
{
    static WCHAR letters[64][32];
    static WCHAR volumes[64][256];
    DWORD mask = GetLogicalDrives();

    memset(letters, 0, sizeof(letters));
    memset(volumes, 0, sizeof(volumes));

    for (WCHAR letter = L'A'; letter <= L'Z'; letter++)
    {
        WCHAR path[8];
        WCHAR root[4] = { letter, L':', L'\\', 0 };
        UCHAR buffer[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
        PVOLUME_DISK_EXTENTS extents = (PVOLUME_DISK_EXTENTS)buffer;
        ULARGE_INTEGER freeBytes, totalBytes;
        DWORD returned;
        HANDLE handle;
        UINT type;

        if (!(mask & (1 << (letter - L'A'))))
            continue;
        type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
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
                WCHAR entry[8];

                if (number >= 64)
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

static VOID EtwBucketCallback(_In_ ULONG DiskNumber, _In_ ULONG ProcessId, _In_ ULONG64 ReadBytes, _In_ ULONG64 WriteBytes, _In_opt_ PVOID Context)
{
    PDISK disk = FindDiskByNumber(DiskNumber);
    PDISK_APP app = NULL;

    if (!disk)
        return;

    for (ULONG i = 0; i < disk->ScratchCount; i++)
    {
        if (disk->Scratch[i].ProcessId == ProcessId)
        {
            app = &disk->Scratch[i];
            break;
        }
    }

    if (!app)
    {
        if (disk->ScratchCount >= MAX_APPS)
            return;

        app = &disk->Scratch[disk->ScratchCount++];
        app->ProcessId = ProcessId;
        app->ReadRate = app->WriteRate = 0;
    }

    app->ReadRate += ReadBytes / TickSeconds;
    app->WriteRate += WriteBytes / TickSeconds;
}

static int __cdecl AppCompare(_In_ const void* A, _In_ const void* B)
{
    const DISK_APP* a = A;
    const DISK_APP* b = B;
    FLOAT ta = a->ReadRate + a->WriteRate;
    FLOAT tb = b->ReadRate + b->WriteRate;

    return ta > tb ? -1 : ta < tb ? 1 : 0;
}

static VOID SampleApps(VOID)
{
    for (ULONG i = 0; i < DiskCount; i++)
        Disks[i]->ScratchCount = 0;

    DiskEtwDrain(EtwBucketCallback, NULL);

    for (ULONG i = 0; i < DiskCount; i++)
    {
        PDISK disk = Disks[i];

        qsort(disk->Scratch, disk->ScratchCount, sizeof(DISK_APP), AppCompare);

        PhAcquireQueuedLockExclusive(&disk->Lock);
        memcpy(disk->Apps, disk->Scratch, disk->ScratchCount * sizeof(DISK_APP));
        disk->AppCount = disk->ScratchCount;
        PhReleaseQueuedLockExclusive(&disk->Lock);

        PhAddItemCircularBuffer_ULONG(&disk->TopAppHistory, disk->ScratchCount ? disk->Scratch[0].ProcessId : 0);
        PhAddItemCircularBuffer_FLOAT(&disk->TopAppReadHistory, disk->ScratchCount ? disk->Scratch[0].ReadRate : 0);
        PhAddItemCircularBuffer_FLOAT(&disk->TopAppWriteHistory, disk->ScratchCount ? disk->Scratch[0].WriteRate : 0);
    }
}

static VOID EvaluateHealth(_In_ PDISK Disk, _In_ PDISK_SMART Smart, _Out_writes_(512) PWSTR Problems, _Out_ SMART_STATUS* Status)
{
    Problems[0] = 0;
    *Status = Smart->Kind ? SmartGood : SmartUnknown;

    SmartEvaluate(Smart, Problems, 512, Status);

    // Latency over the last SMART interval (remote support agent: >= 20 ms caution, >= 100 ms bad).
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

    // Free space: < 10 % caution, < 5 % bad; system drive < 10 GB caution, < 3 GB bad.
    for (PWSTR l = Disk->Letters; *l; l++)
    {
        WCHAR root[4] = { *l, L':', L'\\', 0 };
        ULARGE_INTEGER freeBytes, totalBytes;
        FLOAT percent, gb;
        BOOLEAN system;

        if (*l < L'A' || *l > L'Z' || l[1] != L':')
            continue;
        if (!GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, NULL) || !totalBytes.QuadPart)
            continue;

        percent = 100.0f * freeBytes.QuadPart / totalBytes.QuadPart;
        gb = freeBytes.QuadPart / 1073741824.0f;
        system = *l == SystemLetter;

        if (*Status == SmartUnknown)
            *Status = SmartGood;
        if (percent < 5 || (system && gb < 3))
            HealthAddProblem(Problems, 512, Status, SmartFail, L"%c: %.0f%% free", *l, percent);
        else if (percent < 10 || (system && gb < 10))
            HealthAddProblem(Problems, 512, Status, SmartWarn, L"%c: %.0f%% free", *l, percent);
    }

    if (!Problems[0])
    {
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
    DISK_SMART* smart = PhAllocate(sizeof(DISK_SMART));
    WCHAR problems[512];
    SMART_STATUS status;
    BOOL poweredOn = TRUE;

    // Never wake a sleeping hard disk just to read SMART: try again next refresh.
    if (!Disk->IsSsd && Disk->Handle != INVALID_HANDLE_VALUE && GetDevicePowerState(Disk->Handle, &poweredOn) && !poweredOn)
    {
        Disk->Asleep = TRUE;
        Disk->NextSmartTick = TickCount + 1;
        PhFree(smart);
        return;
    }

    Disk->Asleep = FALSE;
    SmartRead(Disk->Number, Disk->BusType, smart);
    EvaluateHealth(Disk, smart, problems, &status);

    PhAcquireQueuedLockExclusive(&Disk->Lock);
    Disk->Smart = *smart;
    Disk->Health = status;
    wcsncpy_s(Disk->Problems, RTL_NUMBER_OF(Disk->Problems), problems, _TRUNCATE);
    Disk->SmartTried = TRUE;
    Disk->SmartVersion++;
    PhReleaseQueuedLockExclusive(&Disk->Lock);

    Disk->NextSmartTick = TickCount + SmartIntervalTicks;
    PhFree(smart);
}

static VOID SampleAll(VOID)
{
    BOOLEAN smartDone = FALSE;
    FLOAT totalRead = 0, totalWrite = 0;

    TickCount++;

    if (TickCount % 10 == 1)
        RefreshVolumes();

    for (ULONG i = 0; i < DiskCount; i++)
    {
        PDISK disk = Disks[i];

        // At most one SMART read per refresh, so the provider thread never stalls on all disks at once.
        if (!smartDone && TickCount >= disk->NextSmartTick)
        {
            SampleSmart(disk);
            smartDone = !disk->Asleep;
        }

        SamplePerformance(disk);
        totalRead += disk->ReadRate;
        totalWrite += disk->WriteRate;
    }

    PhAddItemCircularBuffer_FLOAT(&TotalReadHistory, totalRead);
    PhAddItemCircularBuffer_FLOAT(&TotalWriteHistory, totalWrite);

    SampleApps();
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

static PPH_STRING ProcessNameOf(_In_ ULONG ProcessId)
{
    PPH_PROCESS_ITEM item = PhReferenceProcessItem(UlongToHandle(ProcessId));
    PPH_STRING name;

    if (item)
    {
        name = PhReferenceObject(item->ProcessName);
        PhDereferenceObject(item);
        return name;
    }

    return PhCreateString(ProcessId == 4 ? L"System" : L"(exited)");
}

// ---- graphs ------------------------------------------------------------------------------------

static PDISK SelectedDisk(VOID)
{
    return Page.Selected < DiskCount ? Disks[Page.Selected] : NULL;
}

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

// Fills Data1 (and Data2 for read/write) scaled to 0..1, returns the scale used.
static FLOAT FillSeries(
    _In_ PPH_CIRCULAR_BUFFER_FLOAT Series1,
    _In_opt_ PPH_CIRCULAR_BUFFER_FLOAT Series2,
    _In_ ULONG Slot,
    _Out_writes_(Count) PFLOAT Data1,
    _Out_writes_opt_(Count) PFLOAT Data2,
    _In_ ULONG Count
    )
{
    FLOAT max = 0;

    for (ULONG i = 0; i < Count; i++)
    {
        Data1[i] = PhGetItemCircularBuffer_FLOAT(Series1, i);
        if (Data1[i] > max) max = Data1[i];

        if (Series2 && Data2)
        {
            Data2[i] = PhGetItemCircularBuffer_FLOAT(Series2, i);
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
        if (Series2 && Data2)
            Data2[i] = min(Data2[i] / max, 1.0f);
    }

    return max;
}

// "R: 12 MB/s\nW: 3 MB/s\nchrome.exe (1234): R 10 MB/s, W 1 MB/s\n<time>"
static PPH_STRING ReadWriteTooltip(_In_ PDISK Disk, _In_ ULONG Index)
{
    PH_STRING_BUILDER sb;
    ULONG pid = PhGetItemCircularBuffer_ULONG(&Disk->TopAppHistory, Index);

    PhInitializeStringBuilder(&sb, 128);
    PhAppendStringBuilder2(&sb, L"R: ");
    PhAppendStringBuilder(&sb, &PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&Disk->ReadHistory, Index)))->sr);
    PhAppendStringBuilder2(&sb, L"\nW: ");
    PhAppendStringBuilder(&sb, &PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&Disk->WriteHistory, Index)))->sr);

    if (pid)
    {
        LARGE_INTEGER time;
        PPH_PROCESS_RECORD record = NULL;
        WCHAR pidText[16];

        if (PhGetStatisticsTime(NULL, Index, &time))
        {
            time.QuadPart += PH_TICKS_PER_SEC - 1;
            record = PhFindProcessRecord(UlongToHandle(pid), &time);
        }

        PhAppendStringBuilder2(&sb, L"\n");
        if (record)
            PhAppendStringBuilder(&sb, &record->ProcessName->sr);
        else
            PhAppendStringBuilder(&sb, &PH_AUTO_T(PH_STRING, ProcessNameOf(pid))->sr);
        _snwprintf_s(pidText, RTL_NUMBER_OF(pidText), _TRUNCATE, L" (%lu): R ", pid);
        PhAppendStringBuilder2(&sb, pidText);
        PhAppendStringBuilder(&sb, &PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&Disk->TopAppReadHistory, Index)))->sr);
        PhAppendStringBuilder2(&sb, L", W ");
        PhAppendStringBuilder(&sb, &PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&Disk->TopAppWriteHistory, Index)))->sr);

        if (record)
            PhDereferenceProcessRecord(record);
    }

    PhAppendStringBuilder2(&sb, L"\n");
    PhAppendStringBuilder(&sb, &PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, Index))->sr);
    return PhFinalStringBuilderString(&sb);
}

static VOID SelectDisk(_In_ ULONG Index);

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
    PDISK disk = NULL;
    PPH_GRAPH_STATE state = NULL;
    ULONG slot = SlotReadWrite;
    BOOLEAN tile = FALSE;

    if (!header)
        return TRUE;

    for (ULONG i = 0; i < DiskCount; i++)
    {
        if (header->hwndFrom == Page.TileGraph[i])
        {
            disk = Disks[i];
            state = &Page.TileState[i];
            tile = TRUE;
        }
    }

    if (!tile)
    {
        for (ULONG s = 0; s < SLOT_COUNT; s++)
        {
            if (header->hwndFrom == Page.GraphHandle[s])
            {
                disk = SelectedDisk();
                state = &Page.GraphState[s];
                slot = s;
            }
        }
    }

    if (!disk || !state)
        return TRUE;

    switch (header->code)
    {
    case GCN_GETDRAWINFO:
        {
            PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)header;
            PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;

            if (slot == SlotReadWrite)
            {
                drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_USE_LINE_2 | (tile ? 0 : PH_GRAPH_LABEL_MAX_Y);
                Page.Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_IO_READ_OTHER), PhGetIntegerSetting(SETTING_COLOR_IO_WRITE), Page.Dpi);
            }
            else
            {
                drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_LABEL_MAX_Y;
                Page.Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(
                    slot == SlotActive ? SETTING_COLOR_CPU_KERNEL : slot == SlotResponse ? SETTING_COLOR_PRIVATE : SETTING_COLOR_TEMPERATURE), 0, Page.Dpi);
            }

            PhGraphStateGetDrawInfo(state, getDrawInfo, disk->ReadHistory.Count);

            if (!state->Valid)
            {
                FLOAT max = FillSeries(SlotHistory(disk, slot), slot == SlotReadWrite ? &disk->WriteHistory : NULL, slot,
                    state->Data1, slot == SlotReadWrite ? state->Data2 : NULL, drawInfo->LineDataCount);

                drawInfo->LabelYFunction = LabelYFunctions[slot];
                drawInfo->LabelYFunctionParameter = max;
                state->Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)header;

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
    case GCN_MOUSEEVENT:
        {
            PPH_GRAPH_MOUSEEVENT mouseEvent = (PPH_GRAPH_MOUSEEVENT)header;

            if (tile && mouseEvent->Message == WM_LBUTTONUP)
                SelectDisk(disk->Index);
        }
        break;
    }

    return TRUE;
}

static HWND CreateGraph(VOID)
{
    PH_GRAPH_CREATEPARAMS createParams;
    HWND graph;

    memset(&createParams, 0, sizeof(PH_GRAPH_CREATEPARAMS));
    createParams.Size = sizeof(PH_GRAPH_CREATEPARAMS);
    createParams.Callback = GraphMessageCallback;

    graph = PhCreateWindow(PH_GRAPH_CLASSNAME, NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS,
        0, 0, 0, 0, Page.Dialog, NULL, NULL, &createParams);
    Graph_SetTooltip(graph, TRUE);
    return graph;
}

static VOID InvalidateGraphs(VOID)
{
    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        Page.GraphState[s].Valid = FALSE;
        Page.GraphState[s].TooltipIndex = ULONG_MAX;
    }

    for (ULONG i = 0; i < DiskCount; i++)
    {
        Page.TileState[i].Valid = FALSE;
        Page.TileState[i].TooltipIndex = ULONG_MAX;
    }
}

static VOID LayoutPage(VOID)
{
    RECT clientRect;
    RECT labelRect;
    RECT tilesRect;
    RECT margin = Page.GraphMarginScaled;
    LONG padding = PhScaleToDisplay(GRAPH_PADDING, Page.Dpi);
    LONG graphWidth;
    LONG graphHeight;
    LONG tileWidth;
    LONG tileLabelHeight;
    LONG y;
    HDWP defer;

    if (!Page.Dialog)
        return;

    InvalidateGraphs();

    PhGetClientRect(Page.Dialog, &clientRect);
    PhGetClientRect(GetDlgItem(Page.Dialog, IDC_LABEL0), &labelRect);
    GetWindowRect(Page.TilesLayout, &tilesRect);
    MapWindowRect(NULL, Page.Dialog, &tilesRect);

    defer = BeginDeferWindowPos(SLOT_COUNT * 2 + DiskCount * 2);

    // Tiles: one row across the top, label (2 lines) over a mini read/write graph.
    tileLabelHeight = labelRect.bottom * 2 + PhScaleToDisplay(2, Page.Dpi);
    tileWidth = DiskCount ? (tilesRect.right - tilesRect.left - padding * ((LONG)DiskCount - 1)) / (LONG)DiskCount : 0;

    for (ULONG i = 0; i < DiskCount; i++)
    {
        LONG x = tilesRect.left + (LONG)i * (tileWidth + padding);

        defer = DeferWindowPos(defer, Page.TileLabel[i], NULL, x, tilesRect.top, tileWidth, tileLabelHeight, SWP_NOACTIVATE | SWP_NOZORDER);
        defer = DeferWindowPos(defer, Page.TileGraph[i], NULL, x, tilesRect.top + tileLabelHeight, tileWidth,
            tilesRect.bottom - tilesRect.top - tileLabelHeight, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    // Selected disk's graphs fill the space between the tiles and the panel.
    graphWidth = clientRect.right - margin.left - margin.right;
    graphHeight = (clientRect.bottom - margin.top - margin.bottom - labelRect.bottom * SLOT_COUNT - padding * (SLOT_COUNT + 2)) / SLOT_COUNT;
    y = margin.top;

    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        BOOLEAN last = s == SLOT_COUNT - 1;

        defer = DeferWindowPos(defer, GetDlgItem(Page.Dialog, LabelIds[s]), NULL, margin.left, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        y += labelRect.bottom + padding;
        defer = DeferWindowPos(defer, Page.GraphHandle[s], NULL, margin.left, y, graphWidth,
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

static VOID UpdateTileLabel(_In_ PDISK Disk)
{
    SMART_STATUS health;
    LONG temperature;
    WCHAR text[160];
    WCHAR temp[32] = L"";

    PhAcquireQueuedLockShared(&Disk->Lock);
    health = Disk->Health;
    temperature = Disk->Smart.TemperatureC;
    PhReleaseQueuedLockShared(&Disk->Lock);

    if (temperature != LONG_MIN)
    {
        PPH_STRING formatted = FormatTemperature((FLOAT)temperature);

        wcsncpy_s(temp, RTL_NUMBER_OF(temp), formatted->Buffer, _TRUNCATE);
        PhDereferenceObject(formatted);
    }

    _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%s%s\n%s%s%s",
        Disk->Index == Page.Selected ? L"▶ " : L"",
        PhGetString(Disk->Title),
        HealthText(health),
        temp[0] ? L"  •  " : L"",
        temp);

    PhSetWindowText(Page.TileLabel[Disk->Index], text);
}

static VOID FillSmartList(_In_ PDISK_SMART Smart)
{
    PCWSTR statusText[] = { L"OK", L"Warning", L"FAIL" };

    SendMessage(Page.SmartList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(Page.SmartList);

    if (!Smart->Kind)
    {
        PhAddListViewItem(Page.SmartList, MAXINT, L"", NULL);
        PhSetListViewSubItem(Page.SmartList, 0, 1, Smart->Note[0] ? Smart->Note : L"SMART not read yet");
    }

    for (ULONG i = 0; i < Smart->RowCount; i++)
    {
        PSMART_ROW row = &Smart->Rows[i];
        LONG index = PhAddListViewItem(Page.SmartList, MAXINT, row->Id, NULL);

        PhSetListViewSubItem(Page.SmartList, index, 1, row->Name);
        PhSetListViewSubItem(Page.SmartList, index, 2, row->Current);
        PhSetListViewSubItem(Page.SmartList, index, 3, row->Worst);
        PhSetListViewSubItem(Page.SmartList, index, 4, row->Threshold);
        PhSetListViewSubItem(Page.SmartList, index, 5, row->Raw);
        PhSetListViewSubItem(Page.SmartList, index, 6, row->Status >= SmartGood && row->Status <= SmartFail ? statusText[row->Status] : L"");
    }

    SendMessage(Page.SmartList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(Page.SmartList, NULL, FALSE);
}

// Rows updated in place so the selection stays put.
static VOID FillAppsList(_In_ PDISK_APP Apps, _In_ ULONG Count)
{
    LONG rows;
    WCHAR text[64];

    SendMessage(Page.AppsList, WM_SETREDRAW, FALSE, 0);
    rows = ListView_GetItemCount(Page.AppsList);

    for (ULONG i = 0; i < Count; i++)
    {
        PPH_STRING name = ProcessNameOf(Apps[i].ProcessId);

        if ((LONG)i < rows)
        {
            LVITEM item = { 0 };

            PhSetListViewSubItem(Page.AppsList, i, 0, name->Buffer);
            item.mask = LVIF_PARAM;
            item.iItem = i;
            item.lParam = Apps[i].ProcessId;
            ListView_SetItem(Page.AppsList, &item);
        }
        else
        {
            PhAddListViewItem(Page.AppsList, MAXINT, name->Buffer, UlongToPtr(Apps[i].ProcessId));
        }

        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%lu", Apps[i].ProcessId);
        PhSetListViewSubItem(Page.AppsList, i, 1, text);
        PhSetListViewSubItem(Page.AppsList, i, 2, PH_AUTO_T(PH_STRING, FormatRate(Apps[i].ReadRate))->Buffer);
        PhSetListViewSubItem(Page.AppsList, i, 3, PH_AUTO_T(PH_STRING, FormatRate(Apps[i].WriteRate))->Buffer);
        PhDereferenceObject(name);
    }

    while (ListView_GetItemCount(Page.AppsList) > (LONG)Count)
        ListView_DeleteItem(Page.AppsList, ListView_GetItemCount(Page.AppsList) - 1);

    SendMessage(Page.AppsList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(Page.AppsList, NULL, FALSE);
}

static VOID UpdatePanel(VOID)
{
    PDISK disk = SelectedDisk();
    DISK_SMART* smart;
    DISK_APP apps[MAX_APPS];
    ULONG appCount;
    SMART_STATUS health;
    WCHAR problems[512];
    WCHAR volumes[256];
    WCHAR text[128];
    BOOLEAN tried, asleep;
    ULONG version;

    if (!Page.Panel || !disk)
        return;

    smart = PhAllocate(sizeof(DISK_SMART));

    PhAcquireQueuedLockShared(&disk->Lock);
    *smart = disk->Smart;
    health = disk->Health;
    wcsncpy_s(problems, RTL_NUMBER_OF(problems), disk->Problems, _TRUNCATE);
    wcsncpy_s(volumes, RTL_NUMBER_OF(volumes), disk->Volumes, _TRUNCATE);
    tried = disk->SmartTried;
    asleep = disk->Asleep;
    version = disk->SmartVersion;
    appCount = disk->AppCount;
    memcpy(apps, disk->Apps, appCount * sizeof(DISK_APP));
    PhReleaseQueuedLockShared(&disk->Lock);

    if (!tried)
        wcsncpy_s(text, RTL_NUMBER_OF(text), asleep ? L"Drive asleep (SMART on wake)" : L"Reading SMART...", _TRUNCATE);
    else
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%s%s", HealthText(health), asleep ? L" (asleep)" : L"");
    PhSetDialogItemText(Page.Panel, IDC_HEALTH_V, text);
    PhSetDialogItemText(Page.Panel, IDC_PROBLEMS_V, problems);

    if (smart->TemperatureC != LONG_MIN)
        SetLabelFormat(Page.Panel, IDC_TEMP_V, FormatTemperature((FLOAT)smart->TemperatureC));
    else
        PhSetDialogItemText(Page.Panel, IDC_TEMP_V, L"-");

    if (smart->PowerOnHours != ULLONG_MAX)
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%I64u h", smart->PowerOnHours);
        PhSetDialogItemText(Page.Panel, IDC_POWERON_V, text);
    }
    else
        PhSetDialogItemText(Page.Panel, IDC_POWERON_V, L"-");

    if (smart->PowerCycles != ULLONG_MAX)
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%I64u", smart->PowerCycles);
        PhSetDialogItemText(Page.Panel, IDC_CYCLES_V, text);
    }
    else
        PhSetDialogItemText(Page.Panel, IDC_CYCLES_V, L"-");

    if (smart->WearPercent >= 0)
    {
        _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%ld%% used", smart->WearPercent);
        PhSetDialogItemText(Page.Panel, IDC_WEAR_V, text);
    }
    else
        PhSetDialogItemText(Page.Panel, IDC_WEAR_V, disk->IsSsd ? L"-" : L"n/a (HDD)");

    if (smart->BytesWritten != ULLONG_MAX)
    {
        PH_FORMAT format[1];
        PhInitFormatSize(&format[0], smart->BytesWritten);
        SetLabelFormat(Page.Panel, IDC_WRITTEN_V, PhFormat(format, 1, 0));
    }
    else
        PhSetDialogItemText(Page.Panel, IDC_WRITTEN_V, L"-");

    if (smart->BytesRead != ULLONG_MAX)
    {
        PH_FORMAT format[1];
        PhInitFormatSize(&format[0], smart->BytesRead);
        SetLabelFormat(Page.Panel, IDC_READ_V, PhFormat(format, 1, 0));
    }
    else
        PhSetDialogItemText(Page.Panel, IDC_READ_V, L"-");

    PhSetDialogItemText(Page.Panel, IDC_VOLUMES_V, volumes);

    if (Page.SmartList && (version != Page.ShownSmartVersion || disk->Index != Page.ShownSmartDisk))
    {
        FillSmartList(smart);
        Page.ShownSmartVersion = version;
        Page.ShownSmartDisk = disk->Index;
    }

    if (Page.AppsList)
        FillAppsList(apps, appCount);

    PhFree(smart);
}

static VOID UpdateHeader(VOID)
{
    PDISK disk = SelectedDisk();
    WCHAR name[256];
    WCHAR size[32] = L"";
    PH_FORMAT format[1];

    if (!disk || !Page.Dialog)
        return;

    PhInitFormatSize(&format[0], disk->Size);
    PhFormatToBuffer(format, 1, size, sizeof(size), NULL);
    _snwprintf_s(name, RTL_NUMBER_OF(name), _TRUNCATE, L"%s  •  %s  •  %s %s  •  %s%s%s",
        PhGetString(disk->Title), PhGetString(disk->Model), BusText(disk->BusType), disk->IsSsd ? L"SSD" : L"HDD", size,
        disk->Firmware->Length ? L"  •  FW " : L"", PhGetString(disk->Firmware));
    PhSetDialogItemText(Page.Dialog, IDC_DISKNAME, name);
}

static VOID TickPage(VOID)
{
    for (ULONG s = 0; s < SLOT_COUNT; s++)
    {
        Page.GraphState[s].Valid = FALSE;
        Page.GraphState[s].TooltipIndex = ULONG_MAX;
        Graph_MoveGrid(Page.GraphHandle[s], 1);
        Graph_Draw(Page.GraphHandle[s]);
        Graph_UpdateTooltip(Page.GraphHandle[s]);
        InvalidateRect(Page.GraphHandle[s], NULL, FALSE);
    }

    for (ULONG i = 0; i < DiskCount; i++)
    {
        Page.TileState[i].Valid = FALSE;
        Page.TileState[i].TooltipIndex = ULONG_MAX;
        Graph_MoveGrid(Page.TileGraph[i], 1);
        Graph_Draw(Page.TileGraph[i]);
        Graph_UpdateTooltip(Page.TileGraph[i]);
        InvalidateRect(Page.TileGraph[i], NULL, FALSE);
        UpdateTileLabel(Disks[i]);
    }

    UpdatePanel();
}

static VOID SelectDisk(_In_ ULONG Index)
{
    if (Index >= DiskCount)
        return;

    Page.Selected = Index;
    PhSetIntegerSetting(SETTING_NAME_SELECTED, Disks[Index]->Number);

    if (Page.AppsList)
        ListView_DeleteAllItems(Page.AppsList);

    InvalidateGraphs();
    UpdateHeader();

    for (ULONG s = 0; s < SLOT_COUNT; s++)
        Graph_Draw(Page.GraphHandle[s]), InvalidateRect(Page.GraphHandle[s], NULL, FALSE);
    for (ULONG i = 0; i < DiskCount; i++)
        UpdateTileLabel(Disks[i]);

    UpdatePanel();
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
            LONG dpi = PhGetWindowDpi(WindowHandle);

            Page.AppsList = GetDlgItem(WindowHandle, IDC_APPS);
            ListView_SetExtendedListViewStyle(Page.AppsList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            PhAddListViewColumn(Page.AppsList, 0, 0, 0, LVCFMT_LEFT, PhScaleToDisplay(150, dpi), L"Process (double-click)");
            PhAddListViewColumn(Page.AppsList, 1, 1, 1, LVCFMT_RIGHT, PhScaleToDisplay(52, dpi), L"PID");
            PhAddListViewColumn(Page.AppsList, 2, 2, 2, LVCFMT_RIGHT, PhScaleToDisplay(76, dpi), L"Read");
            PhAddListViewColumn(Page.AppsList, 3, 3, 3, LVCFMT_RIGHT, PhScaleToDisplay(76, dpi), L"Write");

            Page.SmartList = GetDlgItem(WindowHandle, IDC_SMART);
            Page.ShownSmartVersion = ULONG_MAX;
            ListView_SetExtendedListViewStyle(Page.SmartList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            PhAddListViewColumn(Page.SmartList, 0, 0, 0, LVCFMT_LEFT, PhScaleToDisplay(30, dpi), L"ID");
            PhAddListViewColumn(Page.SmartList, 1, 1, 1, LVCFMT_LEFT, PhScaleToDisplay(210, dpi), L"Attribute");
            PhAddListViewColumn(Page.SmartList, 2, 2, 2, LVCFMT_RIGHT, PhScaleToDisplay(52, dpi), L"Current");
            PhAddListViewColumn(Page.SmartList, 3, 3, 3, LVCFMT_RIGHT, PhScaleToDisplay(46, dpi), L"Worst");
            PhAddListViewColumn(Page.SmartList, 4, 4, 4, LVCFMT_RIGHT, PhScaleToDisplay(60, dpi), L"Threshold");
            PhAddListViewColumn(Page.SmartList, 5, 5, 5, LVCFMT_RIGHT, PhScaleToDisplay(160, dpi), L"Raw / value");
            PhAddListViewColumn(Page.SmartList, 6, 6, 6, LVCFMT_LEFT, PhScaleToDisplay(56, dpi), L"Status");
        }
        break;
    case WM_SIZE:
        {
            RECT clientRect;
            RECT listRect;
            RECT volumesRect;
            HWND volumes = GetDlgItem(WindowHandle, IDC_VOLUMES_V);
            LONG gap = PhScaleToDisplay(6, PhGetWindowDpi(WindowHandle));
            LONG appsWidth;

            if (!Page.SmartList || !Page.AppsList)
                break;

            PhGetClientRect(WindowHandle, &clientRect);
            GetWindowRect(Page.AppsList, &listRect);
            MapWindowRect(NULL, WindowHandle, &listRect);
            appsWidth = (clientRect.right * 2) / 5;

            SetWindowPos(Page.AppsList, NULL, 0, listRect.top, appsWidth, clientRect.bottom - listRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(Page.SmartList, NULL, appsWidth + gap, listRect.top, clientRect.right - appsWidth - gap,
                clientRect.bottom - listRect.top, SWP_NOZORDER | SWP_NOACTIVATE);

            GetWindowRect(volumes, &volumesRect);
            SetWindowPos(volumes, NULL, 0, 0, clientRect.right, volumesRect.bottom - volumesRect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if (header->hwndFrom == Page.AppsList && header->code == NM_DBLCLK)
            {
                LPNMITEMACTIVATE activate = (LPNMITEMACTIVATE)lParam;
                LVITEM item = { 0 };

                item.mask = LVIF_PARAM;
                item.iItem = activate->iItem;

                if (activate->iItem >= 0 && ListView_GetItem(Page.AppsList, &item))
                {
                    PPH_PROCESS_ITEM processItem = PhReferenceProcessItem(UlongToHandle((ULONG)item.lParam));

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
                }
            }
        }
        break;
    case WM_DESTROY:
        {
            Page.AppsList = NULL;
            Page.SmartList = NULL;
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

static INT_PTR CALLBACK DisksDialogProc(
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
            PPH_LAYOUT_ITEM graphItem;
            PPH_LAYOUT_ITEM panelItem;
            HFONT font = (HFONT)SendMessage(WindowHandle, WM_GETFONT, 0, 0);

            Page.Dialog = WindowHandle;
            Page.Dpi = PhGetWindowDpi(WindowHandle);
            Page.TilesLayout = GetDlgItem(WindowHandle, IDC_TILES_LAYOUT);
            PhSetWindowExStyle(WindowHandle, WS_EX_TRANSPARENT, 0);

            for (ULONG s = 0; s < SLOT_COUNT; s++)
            {
                PhInitializeGraphState(&Page.GraphState[s]);
                PhSetDialogItemText(WindowHandle, LabelIds[s], SlotLabels[s]);
            }

            PhInitializeLayoutManager(&Page.LayoutManager, WindowHandle);
            PhAddLayoutItem(&Page.LayoutManager, GetDlgItem(WindowHandle, IDC_DISKNAME), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_TOP | PH_ANCHOR_RIGHT | PH_LAYOUT_FORCE_INVALIDATE);
            PhAddLayoutItem(&Page.LayoutManager, Page.TilesLayout, NULL, PH_ANCHOR_LEFT | PH_ANCHOR_TOP | PH_ANCHOR_RIGHT);
            graphItem = PhAddLayoutItem(&Page.LayoutManager, GetDlgItem(WindowHandle, IDC_GRAPH_LAYOUT), NULL, PH_ANCHOR_ALL);
            panelItem = PhAddLayoutItem(&Page.LayoutManager, GetDlgItem(WindowHandle, IDC_PANEL_LAYOUT), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            Page.GraphMargin = graphItem->Margin;
            Page.GraphMarginScaled = Page.GraphMargin;
            PhGetMarginDpiValue(&Page.GraphMarginScaled, Page.Dpi, TRUE);

            SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), Page.Section->Parameters->LargeFont, FALSE);
            SetWindowFont(GetDlgItem(WindowHandle, IDC_DISKNAME), Page.Section->Parameters->MediumFont, FALSE);

            for (ULONG i = 0; i < DiskCount; i++)
            {
                Page.TileLabel[i] = CreateWindowExW(0, WC_STATIC, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY | SS_NOPREFIX,
                    0, 0, 0, 0, WindowHandle, (HMENU)(ULONG_PTR)(IDC_TILE_LABEL_BASE + i), PluginInstance->DllBase, NULL);
                SetWindowFont(Page.TileLabel[i], font, FALSE);
                PhInitializeGraphState(&Page.TileState[i]);
                Page.TileGraph[i] = CreateGraph();
            }

            for (ULONG s = 0; s < SLOT_COUNT; s++)
                Page.GraphHandle[s] = CreateGraph();

            Page.Panel = PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_DISK_PANEL), WindowHandle, PanelDialogProc, NULL);
            ShowWindow(Page.Panel, SW_SHOW);
            PhAddLayoutItemEx(&Page.LayoutManager, Page.Panel, NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM, &panelItem->Margin);

            UpdateHeader();
            TickPage();

            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
        }
        break;
    case WM_DESTROY:
        {
            PhDeleteLayoutManager(&Page.LayoutManager);
        }
        break;
    case WM_COMMAND:
        {
            ULONG id = GET_WM_COMMAND_ID(wParam, lParam);

            if (id >= IDC_TILE_LABEL_BASE && id < IDC_TILE_LABEL_BASE + DiskCount && GET_WM_COMMAND_CMD(wParam, lParam) == STN_CLICKED)
                SelectDisk(id - IDC_TILE_LABEL_BASE);
        }
        break;
    case WM_DPICHANGED_AFTERPARENT:
        {
            Page.Dpi = PhGetWindowDpi(WindowHandle);
            Page.GraphMarginScaled = Page.GraphMargin;
            PhGetMarginDpiValue(&Page.GraphMarginScaled, Page.Dpi, TRUE);

            if (Page.Section->Parameters->LargeFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), Page.Section->Parameters->LargeFont, FALSE);
            if (Page.Section->Parameters->MediumFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_DISKNAME), Page.Section->Parameters->MediumFont, FALSE);

            PhLayoutManagerUpdate(&Page.LayoutManager, LOWORD(wParam));
            PhLayoutManagerLayout(&Page.LayoutManager);
            LayoutPage();
        }
        break;
    case WM_SIZE:
        {
            PhLayoutManagerLayout(&Page.LayoutManager);
            LayoutPage();
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
    switch (Message)
    {
    case SysInfoDestroy:
        {
            if (Page.Dialog)
            {
                for (ULONG s = 0; s < SLOT_COUNT; s++)
                {
                    PhDeleteGraphState(&Page.GraphState[s]);
                    Page.GraphHandle[s] = NULL;
                }

                for (ULONG i = 0; i < DiskCount; i++)
                {
                    PhDeleteGraphState(&Page.TileState[i]);
                    Page.TileGraph[i] = NULL;
                    Page.TileLabel[i] = NULL;
                }

                Page.Dialog = NULL;
                Page.Panel = NULL;
            }
        }
        return TRUE;
    case SysInfoTick:
        {
            if (Page.Dialog)
                TickPage();
        }
        return TRUE;
    case SysInfoViewChanging:
        {
            PH_SYSINFO_VIEW_TYPE view = (PH_SYSINFO_VIEW_TYPE)PtrToUlong(Parameter1);

            if (view == SysInfoSummaryView || (PPH_SYSINFO_SECTION)Parameter2 != Section || !Page.Dialog)
                return TRUE;

            InvalidateGraphs();
            for (ULONG s = 0; s < SLOT_COUNT; s++)
                Graph_Draw(Page.GraphHandle[s]);
            for (ULONG i = 0; i < DiskCount; i++)
                Graph_Draw(Page.TileGraph[i]);
        }
        return TRUE;
    case SysInfoCreateDialog:
        {
            PPH_SYSINFO_CREATE_DIALOG createDialog = Parameter1;

            createDialog->Instance = PluginInstance->DllBase;
            createDialog->Template = MAKEINTRESOURCE(IDD_DISKS_DIALOG);
            createDialog->DialogProc = DisksDialogProc;
        }
        return TRUE;
    case SysInfoGraphGetDrawInfo:
        {
            PPH_GRAPH_DRAW_INFO drawInfo = Parameter1;

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_USE_LINE_2;
            Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_IO_READ_OTHER), PhGetIntegerSetting(SETTING_COLOR_IO_WRITE), Section->Parameters->WindowDpi);
            PhGetDrawInfoGraphBuffers(&Section->GraphState.Buffers, drawInfo, TotalReadHistory.Count);

            if (!Section->GraphState.Valid)
            {
                FillSeries(&TotalReadHistory, &TotalWriteHistory, SlotReadWrite, Section->GraphState.Data1, Section->GraphState.Data2, drawInfo->LineDataCount);
                Section->GraphState.Valid = TRUE;
            }
        }
        return TRUE;
    case SysInfoGraphGetTooltipText:
        {
            PPH_SYSINFO_GRAPH_GET_TOOLTIP_TEXT getTooltipText = Parameter1;
            PH_FORMAT format[6];

            if (!getTooltipText)
                break;

            PhInitFormatS(&format[0], L"R: ");
            PhInitFormatSR(&format[1], PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&TotalReadHistory, getTooltipText->Index)))->sr);
            PhInitFormatS(&format[2], L"\nW: ");
            PhInitFormatSR(&format[3], PH_AUTO_T(PH_STRING, FormatRate(PhGetItemCircularBuffer_FLOAT(&TotalWriteHistory, getTooltipText->Index)))->sr);
            PhInitFormatC(&format[4], L'\n');
            PhInitFormatSR(&format[5], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);
            PhMoveReference(&Section->GraphState.TooltipText, PhFormat(format, 6, 0));
            getTooltipText->Text = PhGetStringRef(Section->GraphState.TooltipText);
        }
        return TRUE;
    case SysInfoGraphDrawPanel:
        {
            PPH_SYSINFO_DRAW_PANEL drawPanel = Parameter1;
            ULONG caution = 0;
            WCHAR text[96];
            FLOAT read = 0, write = 0;

            if (!drawPanel)
                break;

            for (ULONG i = 0; i < DiskCount; i++)
            {
                SMART_STATUS health;

                PhAcquireQueuedLockShared(&Disks[i]->Lock);
                health = Disks[i]->Health;
                PhReleaseQueuedLockShared(&Disks[i]->Lock);

                if (health >= SmartWarn)
                    caution++;
                read += Disks[i]->ReadRate;
                write += Disks[i]->WriteRate;
            }

            drawPanel->Title = PhCreateString(L"Disks");

            if (caution)
                _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%lu disks, ⚠ %lu\nR+W: %s",
                    DiskCount, caution, PH_AUTO_T(PH_STRING, FormatRate(read + write))->Buffer);
            else
                _snwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, L"%lu disks\nR+W: %s",
                    DiskCount, PH_AUTO_T(PH_STRING, FormatRate(read + write))->Buffer);

            drawPanel->SubTitle = PhCreateString(text);
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
    ULONG updateMs;
    ULONG selected;

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

    updateMs = max(PhGetIntegerSetting(SETTING_UPDATE_INTERVAL), 100);
    TickSeconds = updateMs / 1000.0f;
    interval = PhGetIntegerSetting(SETTING_NAME_SMART_INTERVAL);
    SmartIntervalTicks = max(interval * 1000 / updateMs, 5);

    DiscoverDisks();
    RefreshVolumes();
    MakeTitles();

    // Select the remembered disk, else the system disk, else the first.
    selected = PhGetIntegerSetting(SETTING_NAME_SELECTED);
    Page.Selected = 0;

    for (ULONG i = 0; i < DiskCount; i++)
    {
        if (Disks[i]->Number == selected)
        {
            Page.Selected = i;
            break;
        }

        if (wcschr(Disks[i]->Letters, SystemLetter))
            Page.Selected = i;
    }

    DiskEtwStart();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    DiskEtwStop();
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
    static CONST PH_STRINGREF name = PH_STRINGREF_INIT(L"Disks");
    PH_SYSINFO_SECTION section;

    if (!pointers || !DiskCount)
        return;

    memset(&section, 0, sizeof(PH_SYSINFO_SECTION));
    section.Name = name;
    section.Callback = SectionCallback;
    Page.Section = pointers->CreateSection(&section);
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
            { IntegerSettingType, SETTING_NAME_SELECTED, L"ffffffff" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Disk Monitor";
        info->Description = L"One Disks page: a tile per disk, per-disk speed, active/response time, temperature, SMART health and per-app I/O.";
        info->Author = L"Alihkhawaher";
        info->Url = L"https://github.com/Alihkhawaher/systeminformer-plugins";

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackUnload), UnloadCallback, NULL, &PluginUnloadCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackProcessProviderUpdatedEvent), ProcessesUpdatedCallback, NULL, &ProcessesUpdatedCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackSystemInformationInitializing), SystemInformationInitializingCallback, NULL, &SystemInformationInitializingCallbackRegistration);

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
