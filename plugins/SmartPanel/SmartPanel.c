/*
 * SmartPanel - System Informer plugin that streams live readings to a 3.5" USB HID
 * "PC status monitor" sensor panel (VID 0483, PID 0065; vendor llhmi.com).
 *
 * The panel has no framebuffer. A theme (img.dat: background + number widgets bound to
 * channels) is uploaded to its flash once by a separate tool; afterwards the host only sends
 * (channel, value) pairs: 64-byte HID output reports
 *     [0x02][pair count][channel u8][value u16 BE]...   (up to 20 pairs)
 * and a clock report every minute
 *     [0x03][0x01][0x15][yy][mm][dd][HH][MM][SS][ISO weekday][0x64].
 *
 * Which reading goes to which channel comes from a layout file (setting SmartPanel.LayoutFile,
 * default %APPDATA%\SystemInformer\SmartPanel.txt), reloaded when it changes:
 *
 *     # field                  mode    channels
 *     cpu                      digits  17 18
 *     gpu:P40:temp             digits  1 2
 *     ram_gb                   value   9
 *
 *   digits: one channel per decimal digit, most significant first (values clipped to the
 *           width). Lets a theme draw each digit itself, e.g. on panels that mirror the image.
 *   value:  the whole value in one channel.
 *   fields: cpu, ram_pct, ram_gb, net_down_mbs, net_up_mbs (MB/s), and gpu:<name part>:<metric>
 *           with metric temp, load, mem_load, vram_gb, vram_pct, power, clock (NVML, any driver
 *           model, so TCC cards such as the Tesla P40 work too).
 *
 * While a tool holds the mutex Local\SmartPanel.Flash (theme upload), the plugin closes the
 * device and sends nothing.
 */

#include <phdk.h>
#include <settings.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <iphlpapi.h>

#define PLUGIN_NAME L"SmartPanel"
#define SETTING_LAYOUT_FILE (PLUGIN_NAME L".LayoutFile")
#define SETTING_INTERVAL (PLUGIN_NAME L".IntervalMs")
#define FLASH_MUTEX L"Local\\SmartPanel.Flash"
#define PANEL_VID 0x0483
#define PANEL_PID 0x0065
#define REPORT_SIZE 64
#define MAX_PAIRS 20
#define MAX_FIELDS 64
#define MAX_CHANNELS 8

// ---- minimal NVML ABI (same subset as NvTccGpu) ------------------------------

typedef int nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef struct { unsigned long long total; unsigned long long free; unsigned long long used; } nvmlMemory_t;
#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_SM 1

static struct
{
    nvmlReturn_t (__cdecl* Init)(void);
    nvmlReturn_t (__cdecl* GetCount)(unsigned int*);
    nvmlReturn_t (__cdecl* GetHandleByIndex)(unsigned int, nvmlDevice_t*);
    nvmlReturn_t (__cdecl* GetName)(nvmlDevice_t, char*, unsigned int);
    nvmlReturn_t (__cdecl* GetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*);
    nvmlReturn_t (__cdecl* GetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*);
    nvmlReturn_t (__cdecl* GetPowerUsage)(nvmlDevice_t, unsigned int*);
    nvmlReturn_t (__cdecl* GetTemperature)(nvmlDevice_t, int, unsigned int*);
    nvmlReturn_t (__cdecl* GetClockInfo)(nvmlDevice_t, int, unsigned int*);
    BOOLEAN Loaded;
} Nvml;

#define MAX_GPUS 8
typedef struct
{
    char Name[96];
    nvmlDevice_t Device;
    LONG Temp, Load, MemLoad, VramGb, VramPct, Power, Clock;
} PANEL_GPU;

static PANEL_GPU Gpus[MAX_GPUS];
static ULONG GpuCount;

// ---- layout ------------------------------------------------------------------

typedef enum { FieldCpu, FieldRamPct, FieldRamGb, FieldNetDown, FieldNetUp, FieldGpu } FIELD_KIND;
typedef enum { GpuTemp, GpuLoad, GpuMemLoad, GpuVramGb, GpuVramPct, GpuPower, GpuClock } GPU_METRIC;

typedef struct
{
    FIELD_KIND Kind;
    GPU_METRIC Metric;
    char GpuName[48];      // substring of the NVML name
    BOOLEAN Digits;
    ULONG ChannelCount;
    UCHAR Channels[MAX_CHANNELS];
} PANEL_FIELD;

static PANEL_FIELD Fields[MAX_FIELDS];
static ULONG FieldCount;
static FILETIME LayoutTime;
static WCHAR LayoutPath[MAX_PATH];

// ---- state -------------------------------------------------------------------

static PPH_PLUGIN PluginInstance;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static HANDLE StopEvent;
static HANDLE WorkerThread;
static HANDLE PanelHandle = INVALID_HANDLE_VALUE;
static HANDLE WriteEvent;
static ULONG64 LastClockTick;

static ULONG64 PrevIdle, PrevTotal;
static ULONG64 PrevRx, PrevTx, PrevNetTick;
static LONG CpuPct, RamPct, RamGb, NetDownMbs, NetUpMbs;

// ---- NVML --------------------------------------------------------------------

static VOID NvmlLoad(VOID)
{
    HMODULE module = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    WCHAR path[MAX_PATH];
    unsigned int count = 0;

    if (!module && ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, RTL_NUMBER_OF(path)))
        module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
        return;

#define NVML_GET(Field, Name) if (!(*(FARPROC*)&Nvml.Field = GetProcAddress(module, Name))) return
    NVML_GET(Init, "nvmlInit_v2");
    NVML_GET(GetCount, "nvmlDeviceGetCount_v2");
    NVML_GET(GetHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
    NVML_GET(GetName, "nvmlDeviceGetName");
    NVML_GET(GetUtilizationRates, "nvmlDeviceGetUtilizationRates");
    NVML_GET(GetMemoryInfo, "nvmlDeviceGetMemoryInfo");
    NVML_GET(GetPowerUsage, "nvmlDeviceGetPowerUsage");
    NVML_GET(GetTemperature, "nvmlDeviceGetTemperature");
    NVML_GET(GetClockInfo, "nvmlDeviceGetClockInfo");
#undef NVML_GET

    if (Nvml.Init() != NVML_SUCCESS || Nvml.GetCount(&count) != NVML_SUCCESS)
        return;

    for (unsigned int i = 0; i < count && GpuCount < MAX_GPUS; i++)
    {
        PANEL_GPU* gpu = &Gpus[GpuCount];
        if (Nvml.GetHandleByIndex(i, &gpu->Device) != NVML_SUCCESS)
            continue;
        if (Nvml.GetName(gpu->Device, gpu->Name, sizeof(gpu->Name)) != NVML_SUCCESS)
            gpu->Name[0] = 0;
        GpuCount++;
    }

    Nvml.Loaded = TRUE;
}

static VOID NvmlSample(VOID)
{
    for (ULONG i = 0; i < GpuCount; i++)
    {
        PANEL_GPU* gpu = &Gpus[i];
        nvmlUtilization_t util;
        nvmlMemory_t mem;
        unsigned int value;

        if (Nvml.GetTemperature(gpu->Device, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS)
            gpu->Temp = value;
        if (Nvml.GetUtilizationRates(gpu->Device, &util) == NVML_SUCCESS)
        {
            gpu->Load = util.gpu;
            gpu->MemLoad = util.memory;
        }
        if (Nvml.GetMemoryInfo(gpu->Device, &mem) == NVML_SUCCESS && mem.total)
        {
            gpu->VramGb = (LONG)((mem.used + (512ULL << 20)) >> 30);
            gpu->VramPct = (LONG)(mem.used * 100 / mem.total);
        }
        if (Nvml.GetPowerUsage(gpu->Device, &value) == NVML_SUCCESS)
            gpu->Power = (value + 500) / 1000;
        if (Nvml.GetClockInfo(gpu->Device, NVML_CLOCK_SM, &value) == NVML_SUCCESS)
            gpu->Clock = value;
    }
}

// ---- system readings ---------------------------------------------------------

static ULONG64 FileTimeValue(FILETIME Time)
{
    return ((ULONG64)Time.dwHighDateTime << 32) | Time.dwLowDateTime;
}

static VOID SystemSample(VOID)
{
    FILETIME idle, kernel, user;
    MEMORYSTATUSEX memory = { sizeof(MEMORYSTATUSEX) };
    PMIB_IF_TABLE2 table = NULL;

    if (GetSystemTimes(&idle, &kernel, &user))
    {
        ULONG64 idleNow = FileTimeValue(idle);
        ULONG64 totalNow = FileTimeValue(kernel) + FileTimeValue(user);   // kernel includes idle

        if (PrevTotal && totalNow > PrevTotal)
            CpuPct = (LONG)(100 - (idleNow - PrevIdle) * 100 / (totalNow - PrevTotal));
        PrevIdle = idleNow;
        PrevTotal = totalNow;
    }

    if (GlobalMemoryStatusEx(&memory))
    {
        RamPct = memory.dwMemoryLoad;
        RamGb = (LONG)(((memory.ullTotalPhys - memory.ullAvailPhys) + (512ULL << 20)) >> 30);
    }

    if (GetIfTable2(&table) == NO_ERROR)
    {
        ULONG64 rx = 0, tx = 0, now = GetTickCount64();

        for (ULONG i = 0; i < table->NumEntries; i++)
        {
            PMIB_IF_ROW2 row = &table->Table[i];
            if (row->InterfaceAndOperStatusFlags.HardwareInterface && !row->InterfaceAndOperStatusFlags.FilterInterface &&
                row->OperStatus == IfOperStatusUp)
            {
                rx += row->InOctets;
                tx += row->OutOctets;
            }
        }
        FreeMibTable(table);

        if (PrevNetTick && now > PrevNetTick && rx >= PrevRx && tx >= PrevTx)
        {
            ULONG64 ms = now - PrevNetTick;
            NetDownMbs = (LONG)((rx - PrevRx) * 1000 / ms / 1000000);
            NetUpMbs = (LONG)((tx - PrevTx) * 1000 / ms / 1000000);
        }
        PrevRx = rx;
        PrevTx = tx;
        PrevNetTick = now;
    }
}

// ---- layout file -------------------------------------------------------------

static BOOLEAN ParseField(PSTR Line, PANEL_FIELD* Field)
{
    PSTR context = NULL;
    PSTR name = strtok_s(Line, " \t\r\n", &context);
    PSTR mode = name ? strtok_s(NULL, " \t\r\n", &context) : NULL;
    PSTR token;

    if (!name || name[0] == '#' || !mode)
        return FALSE;

    memset(Field, 0, sizeof(PANEL_FIELD));

    if (!_stricmp(name, "cpu")) Field->Kind = FieldCpu;
    else if (!_stricmp(name, "ram_pct")) Field->Kind = FieldRamPct;
    else if (!_stricmp(name, "ram_gb")) Field->Kind = FieldRamGb;
    else if (!_stricmp(name, "net_down_mbs")) Field->Kind = FieldNetDown;
    else if (!_stricmp(name, "net_up_mbs")) Field->Kind = FieldNetUp;
    else if (!_strnicmp(name, "gpu:", 4))
    {
        PSTR gpuName = name + 4;
        PSTR metric = strrchr(gpuName, ':');

        if (!metric || metric == gpuName)
            return FALSE;
        *metric++ = 0;
        Field->Kind = FieldGpu;
        strncpy_s(Field->GpuName, sizeof(Field->GpuName), gpuName, _TRUNCATE);

        if (!_stricmp(metric, "temp")) Field->Metric = GpuTemp;
        else if (!_stricmp(metric, "load")) Field->Metric = GpuLoad;
        else if (!_stricmp(metric, "mem_load")) Field->Metric = GpuMemLoad;
        else if (!_stricmp(metric, "vram_gb")) Field->Metric = GpuVramGb;
        else if (!_stricmp(metric, "vram_pct")) Field->Metric = GpuVramPct;
        else if (!_stricmp(metric, "power")) Field->Metric = GpuPower;
        else if (!_stricmp(metric, "clock")) Field->Metric = GpuClock;
        else return FALSE;
    }
    else
        return FALSE;

    if (!_stricmp(mode, "digits")) Field->Digits = TRUE;
    else if (_stricmp(mode, "value")) return FALSE;

    while ((token = strtok_s(NULL, " \t\r\n", &context)) && Field->ChannelCount < MAX_CHANNELS)
    {
        LONG channel = atol(token);
        if (channel < 1 || channel > 255)
            return FALSE;
        Field->Channels[Field->ChannelCount++] = (UCHAR)channel;
    }

    return Field->ChannelCount != 0;
}

// Reloads the layout file when its timestamp changes.
static VOID LayoutRefresh(VOID)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    HANDLE file;
    CHAR buffer[16384];
    DWORD read = 0;
    PSTR context = NULL;
    PSTR line;

    if (!GetFileAttributesExW(LayoutPath, GetFileExInfoStandard, &attributes))
    {
        FieldCount = 0;
        return;
    }
    if (FieldCount && CompareFileTime(&attributes.ftLastWriteTime, &LayoutTime) == 0)
        return;

    file = CreateFileW(LayoutPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    ReadFile(file, buffer, sizeof(buffer) - 1, &read, NULL);
    CloseHandle(file);
    buffer[read] = 0;

    FieldCount = 0;
    for (line = strtok_s(buffer, "\n", &context); line && FieldCount < MAX_FIELDS; line = strtok_s(NULL, "\n", &context))
    {
        if (ParseField(line, &Fields[FieldCount]))
            FieldCount++;
    }
    LayoutTime = attributes.ftLastWriteTime;
}

static LONG FieldValue(PANEL_FIELD* Field)
{
    switch (Field->Kind)
    {
    case FieldCpu: return CpuPct;
    case FieldRamPct: return RamPct;
    case FieldRamGb: return RamGb;
    case FieldNetDown: return NetDownMbs;
    case FieldNetUp: return NetUpMbs;
    case FieldGpu:
        for (ULONG i = 0; i < GpuCount; i++)
        {
            PANEL_GPU* gpu = &Gpus[i];
            if (!strstr(gpu->Name, Field->GpuName))
                continue;
            switch (Field->Metric)
            {
            case GpuTemp: return gpu->Temp;
            case GpuLoad: return gpu->Load;
            case GpuMemLoad: return gpu->MemLoad;
            case GpuVramGb: return gpu->VramGb;
            case GpuVramPct: return gpu->VramPct;
            case GpuPower: return gpu->Power;
            case GpuClock: return gpu->Clock;
            }
        }
        return 0;
    }
    return 0;
}

// ---- HID ---------------------------------------------------------------------

static VOID PanelClose(VOID)
{
    if (PanelHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(PanelHandle);
        PanelHandle = INVALID_HANDLE_VALUE;
    }
}

static BOOLEAN PanelOpen(VOID)
{
    GUID hidGuid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA data = { sizeof(SP_DEVICE_INTERFACE_DATA) };

    if (PanelHandle != INVALID_HANDLE_VALUE)
        return TRUE;

    HidD_GetHidGuid(&hidGuid);
    set = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return FALSE;

    for (ULONG index = 0; PanelHandle == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &hidGuid, index, &data); index++)
    {
        DWORD size = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        HANDLE handle;
        HIDD_ATTRIBUTES attributes = { sizeof(HIDD_ATTRIBUTES) };

        SetupDiGetDeviceInterfaceDetailW(set, &data, NULL, 0, &size, NULL);
        if (!size || !(detail = malloc(size)))
            continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, size, NULL, NULL))
        {
            handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (handle != INVALID_HANDLE_VALUE)
            {
                if (HidD_GetAttributes(handle, &attributes) && attributes.VendorID == PANEL_VID && attributes.ProductID == PANEL_PID)
                    PanelHandle = handle;
                else
                    CloseHandle(handle);
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(set);
    return PanelHandle != INVALID_HANDLE_VALUE;
}

static BOOLEAN PanelWrite(PUCHAR Payload)
{
    UCHAR report[REPORT_SIZE + 1] = { 0 };   // leading report id 0
    OVERLAPPED overlapped = { 0 };
    DWORD written = 0;

    memcpy(report + 1, Payload, REPORT_SIZE);
    overlapped.hEvent = WriteEvent;
    ResetEvent(WriteEvent);

    if (!WriteFile(PanelHandle, report, sizeof(report), &written, &overlapped))
    {
        if (GetLastError() != ERROR_IO_PENDING)
            return FALSE;
        if (WaitForSingleObject(WriteEvent, 500) != WAIT_OBJECT_0)
        {
            CancelIo(PanelHandle);
            return FALSE;
        }
        if (!GetOverlappedResult(PanelHandle, &overlapped, &written, FALSE))
            return FALSE;
    }
    return TRUE;
}

static BOOLEAN PanelSendClock(VOID)
{
    UCHAR payload[REPORT_SIZE] = { 0 };
    SYSTEMTIME now;

    GetLocalTime(&now);
    payload[0] = 0x03;
    payload[1] = 0x01;
    payload[2] = 0x15;
    payload[3] = (UCHAR)(now.wYear - 2000);
    payload[4] = (UCHAR)now.wMonth;
    payload[5] = (UCHAR)now.wDay;
    payload[6] = (UCHAR)now.wHour;
    payload[7] = (UCHAR)now.wMinute;
    payload[8] = (UCHAR)now.wSecond;
    payload[9] = (UCHAR)(now.wDayOfWeek ? now.wDayOfWeek : 7);
    payload[10] = 0x64;
    return PanelWrite(payload);
}

// Builds (channel, value) pairs for every field and sends them, 20 per report.
static BOOLEAN PanelSendValues(VOID)
{
    UCHAR channels[MAX_FIELDS * MAX_CHANNELS];
    USHORT values[MAX_FIELDS * MAX_CHANNELS];
    ULONG pairCount = 0;

    for (ULONG i = 0; i < FieldCount; i++)
    {
        PANEL_FIELD* field = &Fields[i];
        LONG value = max(0, FieldValue(field));

        if (field->Digits)
        {
            LONG limit = 1;
            for (ULONG d = 0; d < field->ChannelCount; d++)
                limit *= 10;
            value = min(value, limit - 1);
            for (ULONG d = field->ChannelCount; d-- > 0;)   // least significant digit = last channel
            {
                channels[pairCount] = field->Channels[d];
                values[pairCount++] = (USHORT)(value % 10);
                value /= 10;
            }
        }
        else
        {
            channels[pairCount] = field->Channels[0];
            values[pairCount++] = (USHORT)min(value, 0xFFFF);
        }
    }

    for (ULONG start = 0; start < pairCount; start += MAX_PAIRS)
    {
        UCHAR payload[REPORT_SIZE] = { 0 };
        ULONG count = min(MAX_PAIRS, pairCount - start);

        payload[0] = 0x02;
        payload[1] = (UCHAR)count;
        for (ULONG j = 0; j < count; j++)
        {
            payload[2 + j * 3] = channels[start + j];
            payload[3 + j * 3] = (UCHAR)(values[start + j] >> 8);
            payload[4 + j * 3] = (UCHAR)(values[start + j] & 0xFF);
        }
        if (!PanelWrite(payload))
            return FALSE;
    }
    return TRUE;
}

// ---- worker ------------------------------------------------------------------

static BOOLEAN FlashInProgress(VOID)
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, FLASH_MUTEX);
    if (!mutex)
        return FALSE;
    CloseHandle(mutex);
    return TRUE;
}

static NTSTATUS NTAPI WorkerStart(_In_ PVOID Parameter)
{
    ULONG interval = PhGetIntegerSetting(SETTING_INTERVAL);

    if (interval < 250)
        interval = 1000;

    NvmlLoad();
    SystemSample();   // first CPU/network sample is only a baseline

    while (WaitForSingleObject(StopEvent, interval) == WAIT_TIMEOUT)
    {
        SystemSample();
        if (Nvml.Loaded)
            NvmlSample();
        LayoutRefresh();

        if (!FieldCount || FlashInProgress())
        {
            PanelClose();
            continue;
        }
        if (!PanelOpen())
            continue;

        if (!LastClockTick || GetTickCount64() - LastClockTick > 60000)
        {
            if (PanelSendClock())
                LastClockTick = GetTickCount64();
        }
        if (!PanelSendValues())
        {
            PanelClose();          // unplugged or re-enumerating: reopen next time
            LastClockTick = 0;
        }
    }

    PanelClose();
    return STATUS_SUCCESS;
}

// ---- plugin callbacks --------------------------------------------------------

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    PPH_STRING layout = PhGetStringSetting(SETTING_LAYOUT_FILE);

    if (layout && layout->Length)
        ExpandEnvironmentStringsW(layout->Buffer, LayoutPath, RTL_NUMBER_OF(LayoutPath));
    else
        ExpandEnvironmentStringsW(L"%APPDATA%\\SystemInformer\\SmartPanel.txt", LayoutPath, RTL_NUMBER_OF(LayoutPath));
    if (layout)
        PhDereferenceObject(layout);

    StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    WorkerThread = PhCreateThread(0, WorkerStart, NULL);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    if (StopEvent)
        SetEvent(StopEvent);
    if (WorkerThread)
    {
        WaitForSingleObject(WorkerThread, 3000);
        CloseHandle(WorkerThread);
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
            { StringSettingType, SETTING_LAYOUT_FILE, L"" },
            { IntegerSettingType, SETTING_INTERVAL, L"3e8" },   // 1000 ms (hex)
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Smart Panel (USB sensor display)";
        info->Description = L"Streams CPU, RAM, network and NVIDIA GPU readings to a 3.5\" USB HID sensor panel (0483:0065).";
        info->Author = L"Alihkhawaher";
        info->Url = L"https://github.com/Alihkhawaher/systeminformer-plugins";

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackUnload), UnloadCallback, NULL, &PluginUnloadCallbackRegistration);

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
