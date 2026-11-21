/*
 * GpuMonitor - System Informer plugin: one System Information section per GPU.
 *
 * Sources:
 *  - D3DKMT (gdi32): enumerate WDDM adapters (name, PCI bus, memory sizes).
 *  - Windows "GPU Engine" / "GPU Process Memory" / "GPU Adapter Memory" performance counters (PDH):
 *    GPU load like Task Manager (busiest engine), per-process load and dedicated memory, adapter memory.
 *  - NVML (nvml.dll, NVIDIA driver): power, temperature, clocks, fan, memory-controller load, and the
 *    whole picture for TCC/MCDM GPUs that Windows does not see (e.g. Tesla P40).
 *
 * Meant to replace ExtendedTools' combined GPU page (turn off ExtendedTools.EnableGpuMonitor).
 */

#include <phdk.h>
#include <settings.h>
#include <pdh.h>
#include <pdhmsg.h>
#include "resource.h"

#define PLUGIN_NAME L"GpuMonitor"
#define GRAPH_PADDING 3
#define MAX_GPUS 16
#define MAX_APPS 128
#define MAX_ENGINES 128
#define MAX_SLOTS 6

// ---- D3DKMT (minimal, values from the WDK d3dkmthk.h) ------------------------------------------

typedef UINT GM_D3DKMT_HANDLE;
typedef struct { GM_D3DKMT_HANDLE hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPrecisePresentRegionsPreferred; } GM_ADAPTERINFO;
typedef struct { ULONG NumAdapters; GM_ADAPTERINFO* pAdapters; } GM_ENUMADAPTERS2;
typedef struct { GM_D3DKMT_HANDLE hAdapter; INT Type; PVOID pPrivateDriverData; UINT PrivateDriverDataSize; } GM_QUERYADAPTERINFO;
typedef struct { GM_D3DKMT_HANDLE hAdapter; } GM_CLOSEADAPTER;
typedef struct { UINT BusNumber; UINT DeviceNumber; UINT FunctionNumber; } GM_ADAPTERADDRESS;
typedef struct { WCHAR AdapterString[MAX_PATH]; WCHAR BiosString[MAX_PATH]; WCHAR DacType[MAX_PATH]; WCHAR ChipType[MAX_PATH]; } GM_ADAPTERREGISTRYINFO;
typedef struct { ULONGLONG DedicatedVideoMemorySize; ULONGLONG DedicatedSystemMemorySize; ULONGLONG SharedSystemMemorySize; } GM_SEGMENTSIZEINFO;

#define KMTQAITYPE_GETSEGMENTSIZE 3
#define KMTQAITYPE_ADAPTERADDRESS 6
#define KMTQAITYPE_ADAPTERREGISTRYINFO 8
#define KMTQAITYPE_ADAPTERTYPE 15
#define ADAPTERTYPE_RENDER 0x1
#define ADAPTERTYPE_SOFTWARE 0x4

typedef NTSTATUS (APIENTRY* _D3DKMTEnumAdapters2)(GM_ENUMADAPTERS2*);
typedef NTSTATUS (APIENTRY* _D3DKMTQueryAdapterInfo)(GM_QUERYADAPTERINFO*);
typedef NTSTATUS (APIENTRY* _D3DKMTCloseAdapter)(GM_CLOSEADAPTER*);

// ---- NVML (minimal ABI) ------------------------------------------------------------------------

typedef int nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef struct { unsigned long long total; unsigned long long free; unsigned long long used; } nvmlMemory_t;
typedef struct { char busIdLegacy[16]; unsigned int domain; unsigned int bus; unsigned int device; unsigned int pciDeviceId; unsigned int pciSubSystemId; char busId[32]; } nvmlPciInfo_t;
typedef struct { unsigned int pid; unsigned long long usedGpuMemory; unsigned int gpuInstanceId; unsigned int computeInstanceId; } nvmlProcessInfo_t;
typedef struct { unsigned int pid; unsigned long long usedGpuMemory; } nvmlProcessInfo_v1_t;

#define NVML_SUCCESS 0
#define NVML_DRIVER_WDDM 0
#define NVML_DRIVER_WDM 1
#define NVML_DRIVER_MCDM 2
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_SM 1
#define NVML_CLOCK_MEM 2
#define NVML_VALUE_NOT_AVAILABLE ULLONG_MAX

typedef nvmlReturn_t (__cdecl* _nvmlVoid)(void);
typedef nvmlReturn_t (__cdecl* _nvmlCount)(unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlHandle)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (__cdecl* _nvmlName)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (__cdecl* _nvmlIntInt)(nvmlDevice_t, int*, int*);
typedef nvmlReturn_t (__cdecl* _nvmlUtil)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (__cdecl* _nvmlMem)(nvmlDevice_t, nvmlMemory_t*);
typedef nvmlReturn_t (__cdecl* _nvmlPci)(nvmlDevice_t, nvmlPciInfo_t*);
typedef nvmlReturn_t (__cdecl* _nvmlUint)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlIntUint)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlProcs)(nvmlDevice_t, unsigned int*, PVOID);

static struct
{
    BOOLEAN Loaded;
    _nvmlVoid Init;
    _nvmlCount GetCount;
    _nvmlHandle GetHandleByIndex;
    _nvmlName GetName;
    _nvmlIntInt GetDriverModel;
    _nvmlPci GetPciInfo;
    _nvmlUtil GetUtilizationRates;
    _nvmlMem GetMemoryInfo;
    _nvmlUint GetPowerUsage;
    _nvmlUint GetEnforcedPowerLimit;
    _nvmlIntUint GetTemperature;
    _nvmlUint GetFanSpeed;
    _nvmlIntUint GetClockInfo;
    _nvmlProcs GetComputeProcesses;
    _nvmlProcs GetGraphicsProcesses;
    BOOLEAN ProcessInfoV2;
} Nvml;

// ---- model -------------------------------------------------------------------------------------

typedef enum _METRIC
{
    MetricGpu,
    MetricDedicated,
    MetricShared,
    MetricBandwidth,
    MetricPower,
    MetricTemperature,
    METRIC_COUNT
} METRIC;

static PCWSTR MetricLabels[METRIC_COUNT] =
{
    L"GPU:", L"Dedicated memory:", L"Shared memory:", L"Memory controller (bandwidth):", L"Power usage:", L"Temperature:"
};

static CONST ULONG LabelIds[MAX_SLOTS] = { IDC_LABEL0, IDC_LABEL1, IDC_LABEL2, IDC_LABEL3, IDC_LABEL4, IDC_LABEL5 };

typedef struct _GPU_APP
{
    ULONG ProcessId;
    FLOAT Usage;            // 0..1, or -1 when not available (TCC)
    ULONG64 Memory;         // dedicated bytes, 0 = not reported
} GPU_APP, *PGPU_APP;

typedef struct _GPU
{
    PPH_STRING Name;
    PPH_STRING ShortName;
    PCWSTR DriverModel;
    BOOLEAN Numbered;

    BOOLEAN HasLuid;        // WDDM adapter seen by Windows
    LUID Luid;
    BOOLEAN HasBus;
    ULONG Bus;

    BOOLEAN HasNvml;
    nvmlDevice_t Device;
    ULONG NvmlIndex;

    ULONG Slots[MAX_SLOTS];  // metrics shown, in order
    ULONG SlotCount;

    // latest sample
    FLOAT Value[METRIC_COUNT];
    ULONG64 DedicatedLimit;
    ULONG64 SharedLimit;
    FLOAT PowerLimit;
    ULONG SmClock;
    ULONG MemClock;
    ULONG FanPercent;
    BOOLEAN FanSupported;

    PH_CIRCULAR_BUFFER_FLOAT History[METRIC_COUNT];
    PH_CIRCULAR_BUFFER_ULONG TopAppHistory;
    PH_CIRCULAR_BUFFER_FLOAT TopAppValueHistory;   // usage (0..1) when TopByUsage, else bytes
    BOOLEAN TopByUsage;

    PH_QUEUED_LOCK AppsLock;
    ULONG AppCount;
    GPU_APP Apps[MAX_APPS];

    // per-sample scratch (provider thread only)
    FLOAT EngineSum[MAX_ENGINES];
    ULONG ScratchCount;
    GPU_APP Scratch[MAX_APPS];
    ULONG64 ScratchDedicated;
    ULONG64 ScratchShared;

    // System Information window
    PPH_SYSINFO_SECTION Section;
    HWND Dialog;
    LONG DialogDpi;
    PH_LAYOUT_MANAGER LayoutManager;
    RECT GraphMargin;
    RECT GraphMarginScaled;
    HWND GraphHandle[MAX_SLOTS];
    PH_GRAPH_STATE GraphState[MAX_SLOTS];
    HWND Panel;
    HWND AppsList;
} GPU, *PGPU;

static PPH_PLUGIN PluginInstance;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION SystemInformationInitializingCallbackRegistration;
static PGPU Gpus[MAX_GPUS];
static ULONG GpuCount;
static BOOLEAN UseFahrenheit;

static PDH_HQUERY PdhQuery;
static PDH_HCOUNTER PdhEngine;
static PDH_HCOUNTER PdhProcessDedicated;
static PDH_HCOUNTER PdhAdapterDedicated;
static PDH_HCOUNTER PdhAdapterShared;

// ---- helpers -----------------------------------------------------------------------------------

// "NVIDIA GeForce GTX 1070" -> "GTX 1070", "Tesla P40" -> "P40".
static PPH_STRING MakeShortName(_In_ PPH_STRING Name)
{
    static PCWSTR prefixes[] = { L"NVIDIA ", L"GeForce ", L"Tesla ", L"Quadro ", L"AMD ", L"Radeon ", L"Intel(R) " };
    PH_STRINGREF remaining = Name->sr;

    for (ULONG i = 0; i < RTL_NUMBER_OF(prefixes); i++)
    {
        PH_STRINGREF prefix;

        PhInitializeStringRef(&prefix, (PWSTR)prefixes[i]);
        if (remaining.Length > prefix.Length && PhStartsWithStringRef(&remaining, &prefix, TRUE))
            PhSkipStringRef(&remaining, prefix.Length);
    }

    return PhCreateString2(&remaining);
}

static PGPU NewGpu(_In_ PPH_STRING Name)
{
    PGPU gpu = PhAllocateZero(sizeof(GPU));
    ULONG sampleCount = PhGetIntegerSetting(SETTING_SAMPLE_COUNT);

    gpu->Name = Name;
    gpu->ShortName = MakeShortName(Name);

    for (ULONG m = 0; m < METRIC_COUNT; m++)
        PhInitializeCircularBuffer_FLOAT(&gpu->History[m], sampleCount);
    PhInitializeCircularBuffer_ULONG(&gpu->TopAppHistory, sampleCount);
    PhInitializeCircularBuffer_FLOAT(&gpu->TopAppValueHistory, sampleCount);
    PhInitializeQueuedLock(&gpu->AppsLock);

    return gpu;
}

static VOID NumberDuplicateNames(VOID)
{
    for (ULONG i = 0; i < GpuCount; i++)
    {
        ULONG n = 0;

        for (ULONG j = 0; j < GpuCount; j++)
        {
            if (PhEqualString(Gpus[i]->ShortName, Gpus[j]->ShortName, TRUE))
                n++;
        }

        if (n > 1)
        {
            // Number every card that shares this name, in discovery order: "P40 #1", "P40 #2".
            PPH_STRING base = PhReferenceObject(Gpus[i]->ShortName);
            ULONG k = 0;

            for (ULONG j = 0; j < GpuCount; j++)
            {
                if (PhEqualString(Gpus[j]->ShortName, base, TRUE))
                {
                    PH_FORMAT format[3];

                    PhInitFormatSR(&format[0], base->sr);
                    PhInitFormatS(&format[1], L" #");
                    PhInitFormatU(&format[2], ++k);
                    PhMoveReference(&Gpus[j]->ShortName, PhFormat(format, 3, 0));
                    Gpus[j]->Numbered = TRUE;
                }
            }

            PhDereferenceObject(base);
        }
    }
}

static PGPU FindGpuByLuid(_In_ ULONG High, _In_ ULONG Low)
{
    for (ULONG i = 0; i < GpuCount; i++)
    {
        if (Gpus[i]->HasLuid && (ULONG)Gpus[i]->Luid.HighPart == High && Gpus[i]->Luid.LowPart == Low)
            return Gpus[i];
    }

    return NULL;
}

static PGPU_APP ScratchApp(_In_ PGPU Gpu, _In_ ULONG ProcessId)
{
    for (ULONG i = 0; i < Gpu->ScratchCount; i++)
    {
        if (Gpu->Scratch[i].ProcessId == ProcessId)
            return &Gpu->Scratch[i];
    }

    if (Gpu->ScratchCount >= MAX_APPS)
        return NULL;

    Gpu->Scratch[Gpu->ScratchCount].ProcessId = ProcessId;
    Gpu->Scratch[Gpu->ScratchCount].Usage = Gpu->HasLuid ? 0.0f : -1.0f;
    Gpu->Scratch[Gpu->ScratchCount].Memory = 0;
    return &Gpu->Scratch[Gpu->ScratchCount++];
}

// ---- discovery ---------------------------------------------------------------------------------

static VOID NvmlLoad(VOID)
{
    HMODULE module = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    WCHAR path[MAX_PATH];

    if (!module && ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, RTL_NUMBER_OF(path)))
        module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
        return;

#define NVML_GET(Field, Name) if (!(Nvml.Field = (PVOID)GetProcAddress(module, Name))) return
    NVML_GET(Init, "nvmlInit_v2");
    NVML_GET(GetCount, "nvmlDeviceGetCount_v2");
    NVML_GET(GetHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
    NVML_GET(GetName, "nvmlDeviceGetName");
    NVML_GET(GetDriverModel, "nvmlDeviceGetDriverModel");
    NVML_GET(GetPciInfo, "nvmlDeviceGetPciInfo_v3");
    NVML_GET(GetUtilizationRates, "nvmlDeviceGetUtilizationRates");
    NVML_GET(GetMemoryInfo, "nvmlDeviceGetMemoryInfo");
    NVML_GET(GetPowerUsage, "nvmlDeviceGetPowerUsage");
    NVML_GET(GetEnforcedPowerLimit, "nvmlDeviceGetEnforcedPowerLimit");
    NVML_GET(GetTemperature, "nvmlDeviceGetTemperature");
    NVML_GET(GetFanSpeed, "nvmlDeviceGetFanSpeed");
    NVML_GET(GetClockInfo, "nvmlDeviceGetClockInfo");
#undef NVML_GET

    if ((Nvml.GetComputeProcesses = (PVOID)GetProcAddress(module, "nvmlDeviceGetComputeRunningProcesses_v3")) ||
        (Nvml.GetComputeProcesses = (PVOID)GetProcAddress(module, "nvmlDeviceGetComputeRunningProcesses_v2")))
    {
        Nvml.ProcessInfoV2 = TRUE;
        if (!(Nvml.GetGraphicsProcesses = (PVOID)GetProcAddress(module, "nvmlDeviceGetGraphicsRunningProcesses_v3")))
            Nvml.GetGraphicsProcesses = (PVOID)GetProcAddress(module, "nvmlDeviceGetGraphicsRunningProcesses_v2");
    }
    else
    {
        Nvml.GetComputeProcesses = (PVOID)GetProcAddress(module, "nvmlDeviceGetComputeRunningProcesses");
        Nvml.GetGraphicsProcesses = (PVOID)GetProcAddress(module, "nvmlDeviceGetGraphicsRunningProcesses");
    }

    Nvml.Loaded = Nvml.Init() == NVML_SUCCESS;
}

static VOID DiscoverWddmAdapters(VOID)
{
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    _D3DKMTEnumAdapters2 enumAdapters;
    _D3DKMTQueryAdapterInfo queryInfo;
    _D3DKMTCloseAdapter closeAdapter;
    GM_ENUMADAPTERS2 enumInfo = { 0 };
    GM_ADAPTERINFO* adapters;

    if (!gdi32)
        gdi32 = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!gdi32)
        return;

    enumAdapters = (PVOID)GetProcAddress(gdi32, "D3DKMTEnumAdapters2");
    queryInfo = (PVOID)GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
    closeAdapter = (PVOID)GetProcAddress(gdi32, "D3DKMTCloseAdapter");

    if (!enumAdapters || !queryInfo || !closeAdapter)
        return;
    if (!NT_SUCCESS(enumAdapters(&enumInfo)) || enumInfo.NumAdapters == 0)
        return;

    adapters = PhAllocateZero(enumInfo.NumAdapters * sizeof(GM_ADAPTERINFO));
    enumInfo.pAdapters = adapters;

    if (NT_SUCCESS(enumAdapters(&enumInfo)))
    {
        for (ULONG i = 0; i < enumInfo.NumAdapters && GpuCount < MAX_GPUS; i++)
        {
            GM_QUERYADAPTERINFO query;
            UINT type = 0;
            GM_ADAPTERREGISTRYINFO registry;
            GM_ADAPTERADDRESS address;
            GM_SEGMENTSIZEINFO segments;
            GM_CLOSEADAPTER close;
            PGPU gpu;

            query.hAdapter = adapters[i].hAdapter;
            query.Type = KMTQAITYPE_ADAPTERTYPE;
            query.pPrivateDriverData = &type;
            query.PrivateDriverDataSize = sizeof(type);

            if (NT_SUCCESS(queryInfo(&query)) && (!(type & ADAPTERTYPE_RENDER) || (type & ADAPTERTYPE_SOFTWARE)))
                goto Next;

            memset(&registry, 0, sizeof(registry));
            query.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
            query.pPrivateDriverData = &registry;
            query.PrivateDriverDataSize = sizeof(registry);
            queryInfo(&query);

            gpu = NewGpu(PhCreateString(registry.AdapterString[0] ? registry.AdapterString : L"GPU"));
            gpu->HasLuid = TRUE;
            gpu->Luid = adapters[i].AdapterLuid;
            gpu->DriverModel = L"WDDM";

            query.Type = KMTQAITYPE_ADAPTERADDRESS;
            query.pPrivateDriverData = &address;
            query.PrivateDriverDataSize = sizeof(address);
            if (NT_SUCCESS(queryInfo(&query)))
            {
                gpu->HasBus = TRUE;
                gpu->Bus = address.BusNumber;
            }

            memset(&segments, 0, sizeof(segments));
            query.Type = KMTQAITYPE_GETSEGMENTSIZE;
            query.pPrivateDriverData = &segments;
            query.PrivateDriverDataSize = sizeof(segments);
            if (NT_SUCCESS(queryInfo(&query)))
            {
                gpu->DedicatedLimit = segments.DedicatedVideoMemorySize;
                gpu->SharedLimit = segments.SharedSystemMemorySize;
            }

            Gpus[GpuCount++] = gpu;
Next:
            close.hAdapter = adapters[i].hAdapter;
            closeAdapter(&close);
        }
    }

    PhFree(adapters);
}

static VOID DiscoverNvmlDevices(VOID)
{
    unsigned int count = 0;

    if (!Nvml.Loaded || Nvml.GetCount(&count) != NVML_SUCCESS)
        return;

    for (unsigned int i = 0; i < count; i++)
    {
        nvmlDevice_t device;
        int current = -1, pending = -1;
        nvmlPciInfo_t pci;
        char name[96] = { 0 };
        unsigned int limit = 0;
        PGPU gpu = NULL;

        if (Nvml.GetHandleByIndex(i, &device) != NVML_SUCCESS)
            continue;

        Nvml.GetDriverModel(device, &current, &pending);
        Nvml.GetName(device, name, sizeof(name));
        memset(&pci, 0, sizeof(pci));
        Nvml.GetPciInfo(device, &pci);

        if (current == NVML_DRIVER_WDDM)
        {
            // Attach NVML extras to the Windows adapter on the same PCI bus.
            for (ULONG g = 0; g < GpuCount; g++)
            {
                if (Gpus[g]->HasLuid && Gpus[g]->HasBus && !Gpus[g]->HasNvml && Gpus[g]->Bus == pci.bus)
                {
                    gpu = Gpus[g];
                    break;
                }
            }

            if (!gpu)
                continue;

            PhMoveReference(&gpu->Name, PhConvertUtf8ToUtf16(name));
            PhMoveReference(&gpu->ShortName, MakeShortName(gpu->Name));
        }
        else
        {
            if (GpuCount >= MAX_GPUS)
                continue;

            gpu = NewGpu(PhConvertUtf8ToUtf16(name));
            gpu->DriverModel = current == NVML_DRIVER_WDM ? L"TCC" : current == NVML_DRIVER_MCDM ? L"MCDM" : L"?";
            gpu->HasBus = TRUE;
            gpu->Bus = pci.bus;
            Gpus[GpuCount++] = gpu;
        }

        gpu->HasNvml = TRUE;
        gpu->Device = device;
        gpu->NvmlIndex = i;
        gpu->PowerLimit = Nvml.GetEnforcedPowerLimit(device, &limit) == NVML_SUCCESS && limit ? limit / 1000.0f : 250.0f;

        {
            nvmlMemory_t memory;

            if (Nvml.GetMemoryInfo(device, &memory) == NVML_SUCCESS && memory.total)
                gpu->DedicatedLimit = memory.total;
        }
    }
}

static VOID ChooseMetrics(_In_ PGPU Gpu)
{
    Gpu->SlotCount = 0;
    Gpu->Slots[Gpu->SlotCount++] = MetricGpu;
    Gpu->Slots[Gpu->SlotCount++] = MetricDedicated;
    if (Gpu->HasLuid)
        Gpu->Slots[Gpu->SlotCount++] = MetricShared;
    if (Gpu->HasNvml)
    {
        Gpu->Slots[Gpu->SlotCount++] = MetricBandwidth;
        Gpu->Slots[Gpu->SlotCount++] = MetricPower;
        Gpu->Slots[Gpu->SlotCount++] = MetricTemperature;
    }

    Gpu->TopByUsage = Gpu->HasLuid;
}

static VOID PdhInitialize(VOID)
{
    if (PdhOpenQueryW(NULL, 0, &PdhQuery) != ERROR_SUCCESS)
    {
        PdhQuery = NULL;
        return;
    }

    PdhAddEnglishCounterW(PdhQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &PdhEngine);
    PdhAddEnglishCounterW(PdhQuery, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &PdhProcessDedicated);
    PdhAddEnglishCounterW(PdhQuery, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &PdhAdapterDedicated);
    PdhAddEnglishCounterW(PdhQuery, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &PdhAdapterShared);
    PdhCollectQueryData(PdhQuery); // rate counters need a first sample
}

// ---- sampling (provider thread) ----------------------------------------------------------------

static PPDH_FMT_COUNTERVALUE_ITEM_W PdhRead(_In_opt_ PDH_HCOUNTER Counter, _In_ ULONG Format, _Out_ PULONG Count)
{
    static PVOID buffers[4];
    static ULONG sizes[4];
    static ULONG next;
    ULONG slot = next++ % RTL_NUMBER_OF(buffers);
    DWORD size = sizes[slot];
    DWORD count = 0;
    PDH_STATUS status;

    *Count = 0;

    if (!Counter)
        return NULL;

    status = PdhGetFormattedCounterArrayW(Counter, Format | PDH_FMT_NOCAP100, &size, &count, buffers[slot]);

    if (status == PDH_MORE_DATA)
    {
        if (buffers[slot])
            PhFree(buffers[slot]);
        size += 4096;
        buffers[slot] = PhAllocate(size);
        sizes[slot] = size;
        status = PdhGetFormattedCounterArrayW(Counter, Format | PDH_FMT_NOCAP100, &size, &count, buffers[slot]);
    }

    if (status != ERROR_SUCCESS)
        return NULL;

    *Count = count;
    return buffers[slot];
}

static BOOLEAN PdhItemValid(_In_ PPDH_FMT_COUNTERVALUE_ITEM_W Item)
{
    return Item->FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || Item->FmtValue.CStatus == PDH_CSTATUS_NEW_DATA;
}

static VOID SampleWindowsCounters(VOID)
{
    PPDH_FMT_COUNTERVALUE_ITEM_W items;
    ULONG count;

    if (!PdhQuery || PdhCollectQueryData(PdhQuery) != ERROR_SUCCESS)
        return;

    // pid_12220_luid_0x00000000_0x0000C85A_phys_0_eng_5_engtype_Copy
    if (items = PdhRead(PdhEngine, PDH_FMT_DOUBLE, &count))
    {
        for (ULONG i = 0; i < count; i++)
        {
            ULONG pid, high, low, phys, eng;
            PGPU gpu;
            PGPU_APP app;
            FLOAT v;

            if (!PdhItemValid(&items[i]))
                continue;
            if (swscanf_s(items[i].szName, L"pid_%lu_luid_0x%lx_0x%lx_phys_%lu_eng_%lu", &pid, &high, &low, &phys, &eng) != 5)
                continue;
            if (!(gpu = FindGpuByLuid(high, low)))
                continue;

            v = (FLOAT)items[i].FmtValue.doubleValue;
            if (eng < MAX_ENGINES)
                gpu->EngineSum[eng] += v;
            if ((app = ScratchApp(gpu, pid)) && v / 100.0f > app->Usage)
                app->Usage = v / 100.0f;
        }
    }

    // pid_1116_luid_0x00000000_0x0000D3EC_phys_0
    if (items = PdhRead(PdhProcessDedicated, PDH_FMT_LARGE, &count))
    {
        for (ULONG i = 0; i < count; i++)
        {
            ULONG pid, high, low, phys;
            PGPU gpu;
            PGPU_APP app;

            if (!PdhItemValid(&items[i]))
                continue;
            if (swscanf_s(items[i].szName, L"pid_%lu_luid_0x%lx_0x%lx_phys_%lu", &pid, &high, &low, &phys) != 4)
                continue;
            if (!(gpu = FindGpuByLuid(high, low)) || items[i].FmtValue.largeValue <= 0)
                continue;
            if (app = ScratchApp(gpu, pid))
                app->Memory += (ULONG64)items[i].FmtValue.largeValue;
        }
    }

    // luid_0x00000000_0x0000C4CF_phys_0
    for (ULONG pass = 0; pass < 2; pass++)
    {
        if (!(items = PdhRead(pass == 0 ? PdhAdapterDedicated : PdhAdapterShared, PDH_FMT_LARGE, &count)))
            continue;

        for (ULONG i = 0; i < count; i++)
        {
            ULONG high, low, phys;
            PGPU gpu;

            if (!PdhItemValid(&items[i]))
                continue;
            if (swscanf_s(items[i].szName, L"luid_0x%lx_0x%lx_phys_%lu", &high, &low, &phys) != 3)
                continue;
            if (!(gpu = FindGpuByLuid(high, low)) || items[i].FmtValue.largeValue <= 0)
                continue;

            if (pass == 0)
                gpu->ScratchDedicated += (ULONG64)items[i].FmtValue.largeValue;
            else
                gpu->ScratchShared += (ULONG64)items[i].FmtValue.largeValue;
        }
    }
}

static VOID SampleNvmlProcesses(_In_ PGPU Gpu, _In_opt_ _nvmlProcs Function)
{
    union { nvmlProcessInfo_t v2[MAX_APPS]; nvmlProcessInfo_v1_t v1[MAX_APPS]; } buffer;
    unsigned int count = MAX_APPS;

    if (!Function || Function(Gpu->Device, &count, &buffer) != NVML_SUCCESS)
        return;

    for (unsigned int i = 0; i < count && i < MAX_APPS; i++)
    {
        ULONG pid = Nvml.ProcessInfoV2 ? buffer.v2[i].pid : buffer.v1[i].pid;
        ULONG64 used = Nvml.ProcessInfoV2 ? buffer.v2[i].usedGpuMemory : buffer.v1[i].usedGpuMemory;
        PGPU_APP app = ScratchApp(Gpu, pid);

        if (app && used != NVML_VALUE_NOT_AVAILABLE && used > app->Memory)
            app->Memory = used;
    }
}

static VOID SampleNvml(_In_ PGPU Gpu)
{
    nvmlUtilization_t util;
    unsigned int value;

    if (Nvml.GetUtilizationRates(Gpu->Device, &util) == NVML_SUCCESS)
    {
        Gpu->Value[MetricBandwidth] = util.memory / 100.0f;
        if (!Gpu->HasLuid)
            Gpu->Value[MetricGpu] = util.gpu / 100.0f;
    }

    if (!Gpu->HasLuid)
    {
        nvmlMemory_t memory;

        if (Nvml.GetMemoryInfo(Gpu->Device, &memory) == NVML_SUCCESS)
        {
            Gpu->Value[MetricDedicated] = (FLOAT)memory.used;
            Gpu->DedicatedLimit = memory.total;
        }

        // TCC: Windows has no counters, so NVML provides the app list (memory only).
        SampleNvmlProcesses(Gpu, Nvml.GetComputeProcesses);
        SampleNvmlProcesses(Gpu, Nvml.GetGraphicsProcesses);
    }

    Gpu->Value[MetricPower] = Nvml.GetPowerUsage(Gpu->Device, &value) == NVML_SUCCESS ? value / 1000.0f : 0;
    Gpu->Value[MetricTemperature] = Nvml.GetTemperature(Gpu->Device, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS ? (FLOAT)value : 0;
    Gpu->FanSupported = Nvml.GetFanSpeed(Gpu->Device, &value) == NVML_SUCCESS;
    Gpu->FanPercent = Gpu->FanSupported ? value : 0;
    Gpu->SmClock = Nvml.GetClockInfo(Gpu->Device, NVML_CLOCK_SM, &value) == NVML_SUCCESS ? value : 0;
    Gpu->MemClock = Nvml.GetClockInfo(Gpu->Device, NVML_CLOCK_MEM, &value) == NVML_SUCCESS ? value : 0;
}

static int __cdecl AppCompareUsage(_In_ const void* A, _In_ const void* B)
{
    const GPU_APP* a = A;
    const GPU_APP* b = B;

    if (a->Usage != b->Usage)
        return a->Usage > b->Usage ? -1 : 1;
    if (a->Memory != b->Memory)
        return a->Memory > b->Memory ? -1 : 1;
    return a->ProcessId < b->ProcessId ? -1 : a->ProcessId > b->ProcessId;
}

static int __cdecl AppCompareMemory(_In_ const void* A, _In_ const void* B)
{
    const GPU_APP* a = A;
    const GPU_APP* b = B;

    if (a->Memory != b->Memory)
        return a->Memory > b->Memory ? -1 : 1;
    return a->ProcessId < b->ProcessId ? -1 : a->ProcessId > b->ProcessId;
}

static VOID SampleAll(VOID)
{
    for (ULONG i = 0; i < GpuCount; i++)
    {
        PGPU gpu = Gpus[i];

        memset(gpu->EngineSum, 0, sizeof(gpu->EngineSum));
        gpu->ScratchCount = 0;
        gpu->ScratchDedicated = 0;
        gpu->ScratchShared = 0;
    }

    SampleWindowsCounters();

    for (ULONG i = 0; i < GpuCount; i++)
    {
        PGPU gpu = Gpus[i];
        ULONG kept = 0;

        if (gpu->HasLuid)
        {
            FLOAT busiest = 0;

            for (ULONG e = 0; e < MAX_ENGINES; e++)
            {
                if (gpu->EngineSum[e] > busiest)
                    busiest = gpu->EngineSum[e];
            }

            gpu->Value[MetricGpu] = min(busiest, 100.0f) / 100.0f;
            gpu->Value[MetricDedicated] = (FLOAT)gpu->ScratchDedicated;
            gpu->Value[MetricShared] = (FLOAT)gpu->ScratchShared;
        }

        if (gpu->HasNvml)
            SampleNvml(gpu);

        for (ULONG m = 0; m < METRIC_COUNT; m++)
            PhAddItemCircularBuffer_FLOAT(&gpu->History[m], gpu->Value[m]);

        // Keep apps that use the GPU (some load or some memory).
        for (ULONG a = 0; a < gpu->ScratchCount; a++)
        {
            if (gpu->Scratch[a].Usage > 0.0005f || gpu->Scratch[a].Memory > 0 || !gpu->HasLuid)
                gpu->Scratch[kept++] = gpu->Scratch[a];
        }

        qsort(gpu->Scratch, kept, sizeof(GPU_APP), gpu->TopByUsage ? AppCompareUsage : AppCompareMemory);

        PhAcquireQueuedLockExclusive(&gpu->AppsLock);
        memcpy(gpu->Apps, gpu->Scratch, kept * sizeof(GPU_APP));
        gpu->AppCount = kept;
        PhReleaseQueuedLockExclusive(&gpu->AppsLock);

        PhAddItemCircularBuffer_ULONG(&gpu->TopAppHistory, kept ? gpu->Scratch[0].ProcessId : 0);
        PhAddItemCircularBuffer_FLOAT(&gpu->TopAppValueHistory,
            kept ? (gpu->TopByUsage ? gpu->Scratch[0].Usage : (FLOAT)gpu->Scratch[0].Memory) : 0);
    }
}

// ---- formatting --------------------------------------------------------------------------------

static FLOAT MetricMax(_In_ PGPU Gpu, _In_ ULONG Metric)
{
    switch (Metric)
    {
    case MetricDedicated: return Gpu->DedicatedLimit ? (FLOAT)Gpu->DedicatedLimit : 1.0f;
    case MetricShared: return Gpu->SharedLimit ? (FLOAT)Gpu->SharedLimit : 1.0f;
    case MetricPower: return Gpu->PowerLimit > 0 ? Gpu->PowerLimit : 1.0f;
    case MetricTemperature: return 100.0f;
    default: return 1.0f;
    }
}

static PPH_STRING FormatMetric(_In_ ULONG Metric, _In_ FLOAT Value)
{
    PH_FORMAT format[2];

    switch (Metric)
    {
    case MetricDedicated:
    case MetricShared:
        PhInitFormatSize(&format[0], (ULONG64)Value);
        return PhFormat(format, 1, 0);
    case MetricPower:
        PhInitFormatF(&format[0], Value, 1);
        PhInitFormatS(&format[1], L" W");
        return PhFormat(format, 2, 0);
    case MetricTemperature:
        if (UseFahrenheit)
        {
            PhInitFormatF(&format[0], Value * 1.8f + 32.0f, 1);
            PhInitFormatS(&format[1], L"°F");
        }
        else
        {
            PhInitFormatF(&format[0], Value, 0);
            PhInitFormatS(&format[1], L"°C");
        }
        return PhFormat(format, 2, 0);
    default:
        PhInitFormatF(&format[0], Value * 100, 0);
        PhInitFormatC(&format[1], L'%');
        return PhFormat(format, 2, 0);
    }
}

#define DEFINE_LABEL_Y(Metric) \
    _Function_class_(PH_GRAPH_LABEL_Y_FUNCTION) \
    static PPH_STRING NTAPI LabelY##Metric(_In_ PPH_GRAPH_DRAW_INFO DrawInfo, _In_ ULONG DataIndex, _In_ FLOAT Value, _In_ FLOAT Parameter) \
    { return FormatMetric(Metric, Value * Parameter); }

DEFINE_LABEL_Y(MetricGpu)
DEFINE_LABEL_Y(MetricDedicated)
DEFINE_LABEL_Y(MetricShared)
DEFINE_LABEL_Y(MetricBandwidth)
DEFINE_LABEL_Y(MetricPower)
DEFINE_LABEL_Y(MetricTemperature)

static PPH_GRAPH_LABEL_Y_FUNCTION LabelYFunctions[METRIC_COUNT] =
{
    LabelYMetricGpu, LabelYMetricDedicated, LabelYMetricShared, LabelYMetricBandwidth, LabelYMetricPower, LabelYMetricTemperature
};

static PCWSTR MetricColorSetting(_In_ ULONG Metric)
{
    switch (Metric)
    {
    case MetricDedicated: return SETTING_COLOR_PRIVATE;
    case MetricShared: return SETTING_COLOR_PHYSICAL;
    case MetricBandwidth: return SETTING_COLOR_CPU_USER;
    case MetricPower: return SETTING_COLOR_POWER_USAGE;
    case MetricTemperature: return SETTING_COLOR_TEMPERATURE;
    default: return SETTING_COLOR_CPU_KERNEL;
    }
}

// ---- apps --------------------------------------------------------------------------------------

static PPH_PROCESS_RECORD ReferenceTopAppRecord(_In_ PGPU Gpu, _In_ ULONG Index)
{
    LARGE_INTEGER time;
    ULONG pid = PhGetItemCircularBuffer_ULONG(&Gpu->TopAppHistory, Index);

    if (!pid || !PhGetStatisticsTime(NULL, Index, &time))
        return NULL;

    time.QuadPart += PH_TICKS_PER_SEC - 1;
    return PhFindProcessRecord(UlongToHandle(pid), &time);
}

// "\nname.exe (1234): 97%" (WDDM, busiest app) or "\nname.exe (1234): 15.8 GB" (TCC, largest VRAM).
static PPH_STRING GetTopAppString(_In_ PGPU Gpu, _In_ ULONG Index)
{
    PPH_PROCESS_RECORD record = ReferenceTopAppRecord(Gpu, Index);
    FLOAT value = PhGetItemCircularBuffer_FLOAT(&Gpu->TopAppValueHistory, Index);
    PPH_STRING string;
    PH_FORMAT format[7];
    ULONG n = 0;

    if (!record)
        return PhReferenceEmptyString();

    PhInitFormatC(&format[n++], L'\n');
    PhInitFormatSR(&format[n++], record->ProcessName->sr);
    PhInitFormatS(&format[n++], L" (");
    PhInitFormatU(&format[n++], HandleToUlong(record->ProcessId));
    PhInitFormatS(&format[n++], L"): ");

    if (Gpu->TopByUsage)
    {
        PhInitFormatF(&format[n++], value * 100, 0);
        PhInitFormatC(&format[n++], L'%');
    }
    else
    {
        PhInitFormatSize(&format[n++], (ULONG64)value);
    }

    string = PhFormat(format, n, 0);
    PhDereferenceProcessRecord(record);
    return string;
}

static VOID ShowProcessProperties(_In_ HWND Parent, _In_ ULONG ProcessId)
{
    PPH_PROCESS_ITEM processItem = PhReferenceProcessItem(UlongToHandle(ProcessId));

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
    else
    {
        PhShowError2(Parent, L"Unable to open the process.", L"Process %lu is no longer running.", ProcessId);
    }
}

// ---- graphs ------------------------------------------------------------------------------------

static VOID FillGraphData(_In_ PGPU Gpu, _In_ ULONG Metric, _Out_writes_(Count) PFLOAT Data, _In_ ULONG Count)
{
    FLOAT max = MetricMax(Gpu, Metric);

    for (ULONG i = 0; i < Count; i++)
    {
        FLOAT v = PhGetItemCircularBuffer_FLOAT(&Gpu->History[Metric], i) / max;
        Data[i] = v > 1.0f ? 1.0f : v;
    }
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
    PGPU gpu = Context;
    NMHDR* header = Parameter1;
    ULONG slot = ULONG_MAX;
    ULONG metric;

    if (!gpu || !header)
        return TRUE;

    for (ULONG s = 0; s < gpu->SlotCount; s++)
    {
        if (header->hwndFrom == gpu->GraphHandle[s])
            slot = s;
    }

    if (slot == ULONG_MAX)
        return TRUE;

    metric = gpu->Slots[slot];

    switch (header->code)
    {
    case GCN_GETDRAWINFO:
        {
            PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)header;
            PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;
            PPH_GRAPH_STATE state = &gpu->GraphState[slot];

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_LABEL_MAX_Y;
            gpu->Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(MetricColorSetting(metric)), 0, gpu->DialogDpi);
            PhGraphStateGetDrawInfo(state, getDrawInfo, gpu->History[metric].Count);

            if (!state->Valid)
            {
                FillGraphData(gpu, metric, state->Data1, drawInfo->LineDataCount);
                drawInfo->LabelYFunction = LabelYFunctions[metric];
                drawInfo->LabelYFunctionParameter = MetricMax(gpu, metric);
                state->Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)header;
            PPH_GRAPH_STATE state = &gpu->GraphState[slot];

            if (getTooltipText->Index < getTooltipText->TotalCount)
            {
                if (state->TooltipIndex != getTooltipText->Index)
                {
                    FLOAT value = PhGetItemCircularBuffer_FLOAT(&gpu->History[metric], getTooltipText->Index);
                    PH_FORMAT format[4];

                    PhInitFormatSR(&format[0], PH_AUTO_T(PH_STRING, FormatMetric(metric, value))->sr);
                    PhInitFormatSR(&format[1], PH_AUTO_T(PH_STRING, (metric == MetricGpu || metric == MetricDedicated) ?
                        GetTopAppString(gpu, getTooltipText->Index) : PhReferenceEmptyString())->sr);
                    PhInitFormatC(&format[2], L'\n');
                    PhInitFormatSR(&format[3], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);

                    PhMoveReference(&state->TooltipText, PhFormat(format, RTL_NUMBER_OF(format), 0));
                }

                getTooltipText->Text = PhGetStringRef(state->TooltipText);
            }
        }
        break;
    case GCN_MOUSEEVENT:
        {
            PPH_GRAPH_MOUSEEVENT mouseEvent = (PPH_GRAPH_MOUSEEVENT)header;

            if ((metric == MetricGpu || metric == MetricDedicated) &&
                mouseEvent->Message == WM_LBUTTONDBLCLK && mouseEvent->Index < mouseEvent->TotalCount)
            {
                PPH_PROCESS_RECORD record = ReferenceTopAppRecord(gpu, mouseEvent->Index);

                if (record)
                {
                    PhShowProcessRecordDialog(gpu->Dialog, record);
                    PhDereferenceProcessRecord(record);
                }
            }
        }
        break;
    }

    return TRUE;
}

static VOID CreateGraphs(_In_ PGPU Gpu)
{
    PH_GRAPH_CREATEPARAMS createParams;

    memset(&createParams, 0, sizeof(PH_GRAPH_CREATEPARAMS));
    createParams.Size = sizeof(PH_GRAPH_CREATEPARAMS);
    createParams.Callback = GraphMessageCallback;
    createParams.Context = Gpu;

    for (ULONG s = 0; s < Gpu->SlotCount; s++)
    {
        Gpu->GraphHandle[s] = PhCreateWindow(
            PH_GRAPH_CLASSNAME,
            NULL,
            WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS,
            0, 0, 0, 0,
            Gpu->Dialog,
            NULL,
            NULL,
            &createParams
            );
        Graph_SetTooltip(Gpu->GraphHandle[s], TRUE);
    }
}

static VOID LayoutGraphs(_In_ PGPU Gpu)
{
    RECT clientRect;
    RECT labelRect;
    RECT margin = Gpu->GraphMarginScaled;
    LONG padding = PhScaleToDisplay(GRAPH_PADDING, Gpu->DialogDpi);
    LONG graphWidth;
    LONG graphHeight;
    LONG y;
    HDWP defer;
    ULONG n = Gpu->SlotCount;

    for (ULONG s = 0; s < n; s++)
    {
        Gpu->GraphState[s].Valid = FALSE;
        Gpu->GraphState[s].TooltipIndex = ULONG_MAX;
    }

    PhGetClientRect(Gpu->Dialog, &clientRect);
    PhGetClientRect(GetDlgItem(Gpu->Dialog, IDC_LABEL0), &labelRect);
    graphWidth = clientRect.right - margin.left - margin.right;
    graphHeight = (clientRect.bottom - margin.top - margin.bottom - labelRect.bottom * (LONG)n - padding * ((LONG)n + 2)) / (LONG)n;

    defer = BeginDeferWindowPos(n * 2);
    y = margin.top;

    for (ULONG s = 0; s < n; s++)
    {
        BOOLEAN last = s == n - 1;

        defer = DeferWindowPos(defer, GetDlgItem(Gpu->Dialog, LabelIds[s]), NULL, margin.left, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        y += labelRect.bottom + padding;
        defer = DeferWindowPos(defer, Gpu->GraphHandle[s], NULL, margin.left, y, graphWidth,
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

static PPH_STRING FormatUsedOfLimit(_In_ ULONG64 Used, _In_ ULONG64 Limit)
{
    PH_FORMAT format[3];

    PhInitFormatSize(&format[0], Used);
    PhInitFormatS(&format[1], L" / ");
    PhInitFormatSize(&format[2], Limit);
    return PhFormat(format, Limit ? 3 : 1, 0);
}

static VOID UpdateAppsList(_In_ PGPU Gpu)
{
    GPU_APP apps[MAX_APPS];
    ULONG count;
    LONG rows;
    WCHAR text[64];

    if (!Gpu->AppsList)
        return;

    PhAcquireQueuedLockShared(&Gpu->AppsLock);
    count = Gpu->AppCount;
    memcpy(apps, Gpu->Apps, count * sizeof(GPU_APP));
    PhReleaseQueuedLockShared(&Gpu->AppsLock);

    SendMessage(Gpu->AppsList, WM_SETREDRAW, FALSE, 0);
    rows = ListView_GetItemCount(Gpu->AppsList);

    for (ULONG i = 0; i < count; i++)
    {
        PPH_PROCESS_ITEM processItem = PhReferenceProcessItem(UlongToHandle(apps[i].ProcessId));
        PCWSTR name = processItem ? PhGetString(processItem->ProcessName) : L"(no access)";
        PH_FORMAT format[2];

        if ((LONG)i < rows)
        {
            LVITEM item = { 0 };

            PhSetListViewSubItem(Gpu->AppsList, i, 0, name);
            item.mask = LVIF_PARAM;
            item.iItem = i;
            item.lParam = apps[i].ProcessId;
            ListView_SetItem(Gpu->AppsList, &item);
        }
        else
        {
            PhAddListViewItem(Gpu->AppsList, MAXINT, name, UlongToPtr(apps[i].ProcessId));
        }

        PhInitFormatU(&format[0], apps[i].ProcessId);
        if (PhFormatToBuffer(format, 1, text, sizeof(text), NULL))
            PhSetListViewSubItem(Gpu->AppsList, i, 1, text);

        if (apps[i].Usage >= 0)
        {
            PhInitFormatF(&format[0], apps[i].Usage * 100, 0);
            PhInitFormatC(&format[1], L'%');
            if (PhFormatToBuffer(format, 2, text, sizeof(text), NULL))
                PhSetListViewSubItem(Gpu->AppsList, i, 2, text);
        }
        else
        {
            PhSetListViewSubItem(Gpu->AppsList, i, 2, L"n/a");
        }

        if (apps[i].Memory)
        {
            PhInitFormatSize(&format[0], apps[i].Memory);
            if (PhFormatToBuffer(format, 1, text, sizeof(text), NULL))
                PhSetListViewSubItem(Gpu->AppsList, i, 3, text);
        }
        else
        {
            PhSetListViewSubItem(Gpu->AppsList, i, 3, L"-");
        }

        if (processItem)
            PhDereferenceObject(processItem);
    }

    while (ListView_GetItemCount(Gpu->AppsList) > (LONG)count)
        ListView_DeleteItem(Gpu->AppsList, ListView_GetItemCount(Gpu->AppsList) - 1);

    SendMessage(Gpu->AppsList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(Gpu->AppsList, NULL, FALSE);
}

static VOID UpdatePanel(_In_ PGPU Gpu)
{
    PH_FORMAT format[3];

    if (!Gpu->Panel)
        return;

    SetLabelFormat(Gpu->Panel, IDC_DEDICATED_V, FormatUsedOfLimit((ULONG64)Gpu->Value[MetricDedicated], Gpu->DedicatedLimit));

    if (Gpu->HasLuid)
        SetLabelFormat(Gpu->Panel, IDC_SHARED_V, FormatUsedOfLimit((ULONG64)Gpu->Value[MetricShared], Gpu->SharedLimit));
    else
        PhSetDialogItemText(Gpu->Panel, IDC_SHARED_V, L"n/a");

    if (Gpu->HasNvml)
    {
        SetLabelFormat(Gpu->Panel, IDC_POWER_V, FormatMetric(MetricPower, Gpu->Value[MetricPower]));
        SetLabelFormat(Gpu->Panel, IDC_POWERLIMIT_V, FormatMetric(MetricPower, Gpu->PowerLimit));

        PhInitFormatU(&format[0], Gpu->SmClock);
        PhInitFormatS(&format[1], L" MHz");
        SetLabelFormat(Gpu->Panel, IDC_SMCLOCK_V, PhFormat(format, 2, 0));
        PhInitFormatU(&format[0], Gpu->MemClock);
        SetLabelFormat(Gpu->Panel, IDC_MEMCLOCK_V, PhFormat(format, 2, 0));

        if (Gpu->FanSupported)
        {
            PhInitFormatS(&format[0], L"Fan ");
            PhInitFormatU(&format[1], Gpu->FanPercent);
            PhInitFormatC(&format[2], L'%');
            SetLabelFormat(Gpu->Panel, IDC_FAN_V, PhFormat(format, 3, 0));
        }
        else
        {
            PhSetDialogItemText(Gpu->Panel, IDC_FAN_V, L"Passive");
        }
    }
    else
    {
        PhSetDialogItemText(Gpu->Panel, IDC_POWER_V, L"n/a");
        PhSetDialogItemText(Gpu->Panel, IDC_POWERLIMIT_V, L"n/a");
        PhSetDialogItemText(Gpu->Panel, IDC_SMCLOCK_V, L"n/a");
        PhSetDialogItemText(Gpu->Panel, IDC_MEMCLOCK_V, L"n/a");
        PhSetDialogItemText(Gpu->Panel, IDC_FAN_V, L"");
    }

    PhSetDialogItemText(Gpu->Panel, IDC_DRIVER_V, Gpu->DriverModel);
    UpdateAppsList(Gpu);
}

static VOID TickDialog(_In_ PGPU Gpu)
{
    for (ULONG s = 0; s < Gpu->SlotCount; s++)
    {
        Gpu->GraphState[s].Valid = FALSE;
        Gpu->GraphState[s].TooltipIndex = ULONG_MAX;
        Graph_MoveGrid(Gpu->GraphHandle[s], 1);
        Graph_Draw(Gpu->GraphHandle[s]);
        Graph_UpdateTooltip(Gpu->GraphHandle[s]);
        InvalidateRect(Gpu->GraphHandle[s], NULL, FALSE);
    }

    UpdatePanel(Gpu);
}

static INT_PTR CALLBACK PanelDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PGPU gpu;

    if (WindowMessage == WM_INITDIALOG)
    {
        gpu = (PGPU)lParam;
        PhSetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT, gpu);
    }
    else
    {
        gpu = PhGetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
    }

    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            LONG dpi = PhGetWindowDpi(WindowHandle);

            gpu->AppsList = GetDlgItem(WindowHandle, IDC_APPS);
            ListView_SetExtendedListViewStyle(gpu->AppsList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            PhAddListViewColumn(gpu->AppsList, 0, 0, 0, LVCFMT_LEFT, PhScaleToDisplay(260, dpi), L"Process (double-click for properties)");
            PhAddListViewColumn(gpu->AppsList, 1, 1, 1, LVCFMT_RIGHT, PhScaleToDisplay(70, dpi), L"PID");
            PhAddListViewColumn(gpu->AppsList, 2, 2, 2, LVCFMT_RIGHT, PhScaleToDisplay(70, dpi), L"GPU");
            PhAddListViewColumn(gpu->AppsList, 3, 3, 3, LVCFMT_RIGHT, PhScaleToDisplay(100, dpi), L"GPU memory");
        }
        break;
    case WM_SIZE:
        {
            RECT clientRect;
            RECT listRect;

            if (gpu && gpu->AppsList)
            {
                PhGetClientRect(WindowHandle, &clientRect);
                GetWindowRect(gpu->AppsList, &listRect);
                MapWindowRect(NULL, WindowHandle, &listRect);
                SetWindowPos(gpu->AppsList, NULL, 0, 0, clientRect.right - listRect.left,
                    clientRect.bottom - listRect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if (gpu && header->hwndFrom == gpu->AppsList && header->code == NM_DBLCLK)
            {
                LPNMITEMACTIVATE activate = (LPNMITEMACTIVATE)lParam;
                LVITEM item = { 0 };

                item.mask = LVIF_PARAM;
                item.iItem = activate->iItem;

                if (activate->iItem >= 0 && ListView_GetItem(gpu->AppsList, &item))
                    ShowProcessProperties(WindowHandle, (ULONG)item.lParam);
            }
        }
        break;
    case WM_DESTROY:
        {
            if (gpu)
                gpu->AppsList = NULL;
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

static INT_PTR CALLBACK GpuDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PGPU gpu;

    if (WindowMessage == WM_INITDIALOG)
    {
        gpu = (PGPU)lParam;
        PhSetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT, gpu);
    }
    else
    {
        gpu = PhGetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
    }

    if (!gpu)
        return FALSE;

    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            PPH_LAYOUT_ITEM graphItem;
            PPH_LAYOUT_ITEM panelItem;
            PH_FORMAT format[4];

            for (ULONG s = 0; s < MAX_SLOTS; s++)
                PhInitializeGraphState(&gpu->GraphState[s]);

            gpu->Dialog = WindowHandle;
            gpu->DialogDpi = PhGetWindowDpi(WindowHandle);
            PhSetWindowExStyle(WindowHandle, WS_EX_TRANSPARENT, 0);

            for (ULONG s = 0; s < MAX_SLOTS; s++)
            {
                if (s < gpu->SlotCount)
                    PhSetDialogItemText(WindowHandle, LabelIds[s], MetricLabels[gpu->Slots[s]]);
                else
                    ShowWindow(GetDlgItem(WindowHandle, LabelIds[s]), SW_HIDE);
            }

            PhInitializeLayoutManager(&gpu->LayoutManager, WindowHandle);
            PhAddLayoutItem(&gpu->LayoutManager, GetDlgItem(WindowHandle, IDC_GPUNAME), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_TOP | PH_ANCHOR_RIGHT | PH_LAYOUT_FORCE_INVALIDATE);
            graphItem = PhAddLayoutItem(&gpu->LayoutManager, GetDlgItem(WindowHandle, IDC_GRAPH_LAYOUT), NULL, PH_ANCHOR_ALL);
            panelItem = PhAddLayoutItem(&gpu->LayoutManager, GetDlgItem(WindowHandle, IDC_PANEL_LAYOUT), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            gpu->GraphMargin = graphItem->Margin;
            gpu->GraphMarginScaled = gpu->GraphMargin;
            PhGetMarginDpiValue(&gpu->GraphMarginScaled, gpu->DialogDpi, TRUE);

            SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), gpu->Section->Parameters->LargeFont, FALSE);
            SetWindowFont(GetDlgItem(WindowHandle, IDC_GPUNAME), gpu->Section->Parameters->MediumFont, FALSE);
            PhSetDialogItemText(WindowHandle, IDC_TITLE, PhGetString(gpu->ShortName));

            // "NVIDIA GeForce GTX 1070  •  WDDM"
            PhInitFormatSR(&format[0], gpu->Name->sr);
            PhInitFormatS(&format[1], L"  •  ");
            PhInitFormatS(&format[2], (PWSTR)gpu->DriverModel);
            SetLabelFormat(WindowHandle, IDC_GPUNAME, PhFormat(format, 3, 0));

            gpu->Panel = PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_GPU_PANEL), WindowHandle, PanelDialogProc, gpu);
            ShowWindow(gpu->Panel, SW_SHOW);
            PhAddLayoutItemEx(&gpu->LayoutManager, gpu->Panel, NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM, &panelItem->Margin);

            CreateGraphs(gpu);
            TickDialog(gpu);

            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
        }
        break;
    case WM_DESTROY:
        {
            PhDeleteLayoutManager(&gpu->LayoutManager);
            PhRemoveWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
        }
        break;
    case WM_DPICHANGED_AFTERPARENT:
        {
            gpu->DialogDpi = PhGetWindowDpi(WindowHandle);
            gpu->GraphMarginScaled = gpu->GraphMargin;
            PhGetMarginDpiValue(&gpu->GraphMarginScaled, gpu->DialogDpi, TRUE);

            if (gpu->Section->Parameters->LargeFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), gpu->Section->Parameters->LargeFont, FALSE);
            if (gpu->Section->Parameters->MediumFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_GPUNAME), gpu->Section->Parameters->MediumFont, FALSE);

            PhLayoutManagerUpdate(&gpu->LayoutManager, LOWORD(wParam));
            PhLayoutManagerLayout(&gpu->LayoutManager);
            LayoutGraphs(gpu);
        }
        break;
    case WM_SIZE:
        {
            PhLayoutManagerLayout(&gpu->LayoutManager);
            LayoutGraphs(gpu);
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
    PGPU gpu = Section->Context;

    switch (Message)
    {
    case SysInfoDestroy:
        {
            if (gpu->Dialog)
            {
                for (ULONG s = 0; s < MAX_SLOTS; s++)
                {
                    PhDeleteGraphState(&gpu->GraphState[s]);
                    gpu->GraphHandle[s] = NULL;
                }

                gpu->Dialog = NULL;
                gpu->Panel = NULL;
            }
        }
        return TRUE;
    case SysInfoTick:
        {
            if (gpu->Dialog)
                TickDialog(gpu);
        }
        return TRUE;
    case SysInfoViewChanging:
        {
            PH_SYSINFO_VIEW_TYPE view = (PH_SYSINFO_VIEW_TYPE)PtrToUlong(Parameter1);

            if (view == SysInfoSummaryView || (PPH_SYSINFO_SECTION)Parameter2 != Section)
                return TRUE;

            for (ULONG s = 0; s < gpu->SlotCount; s++)
            {
                if (gpu->GraphHandle[s])
                {
                    gpu->GraphState[s].Valid = FALSE;
                    gpu->GraphState[s].TooltipIndex = ULONG_MAX;
                    Graph_Draw(gpu->GraphHandle[s]);
                }
            }
        }
        return TRUE;
    case SysInfoCreateDialog:
        {
            PPH_SYSINFO_CREATE_DIALOG createDialog = Parameter1;

            createDialog->Instance = PluginInstance->DllBase;
            createDialog->Template = MAKEINTRESOURCE(IDD_GPU_DIALOG);
            createDialog->DialogProc = GpuDialogProc;
            createDialog->Parameter = gpu;
        }
        return TRUE;
    case SysInfoGraphGetDrawInfo:
        {
            PPH_GRAPH_DRAW_INFO drawInfo = Parameter1;

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y;
            Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_CPU_KERNEL), 0, Section->Parameters->WindowDpi);
            PhGetDrawInfoGraphBuffers(&Section->GraphState.Buffers, drawInfo, gpu->History[MetricGpu].Count);

            if (!Section->GraphState.Valid)
            {
                FillGraphData(gpu, MetricGpu, Section->GraphState.Data1, drawInfo->LineDataCount);
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

            PhInitFormatSR(&format[0], PH_AUTO_T(PH_STRING, FormatMetric(MetricGpu, PhGetItemCircularBuffer_FLOAT(&gpu->History[MetricGpu], getTooltipText->Index)))->sr);
            PhInitFormatS(&format[1], L"  •  ");
            PhInitFormatSR(&format[2], PH_AUTO_T(PH_STRING, FormatMetric(MetricDedicated, PhGetItemCircularBuffer_FLOAT(&gpu->History[MetricDedicated], getTooltipText->Index)))->sr);
            PhInitFormatSR(&format[3], PH_AUTO_T(PH_STRING, GetTopAppString(gpu, getTooltipText->Index))->sr);
            PhInitFormatC(&format[4], L'\n');
            PhInitFormatSR(&format[5], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);

            PhMoveReference(&Section->GraphState.TooltipText, PhFormat(format, RTL_NUMBER_OF(format), 0));
            getTooltipText->Text = PhGetStringRef(Section->GraphState.TooltipText);
        }
        return TRUE;
    case SysInfoGraphDrawPanel:
        {
            PPH_SYSINFO_DRAW_PANEL drawPanel = Parameter1;
            PH_FORMAT format[5];

            if (!drawPanel)
                break;

            drawPanel->Title = PhReferenceObject(gpu->ShortName);

            PhInitFormatF(&format[0], gpu->Value[MetricGpu] * 100, 0);
            PhInitFormatS(&format[1], L"%\n");
            PhInitFormatSize(&format[2], (ULONG64)gpu->Value[MetricDedicated]);
            PhInitFormatS(&format[3], L" / ");
            PhInitFormatSize(&format[4], gpu->DedicatedLimit);
            drawPanel->SubTitle = PhFormat(format, 5, 64);
            drawPanel->SubTitleOverflow = PhFormat(format, 3, 64);
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

    NvmlLoad();
    DiscoverWddmAdapters();
    DiscoverNvmlDevices();
    NumberDuplicateNames();

    for (ULONG i = 0; i < GpuCount; i++)
        ChooseMetrics(Gpus[i]);

    PdhInitialize();
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

    for (ULONG i = 0; i < GpuCount; i++)
    {
        PGPU gpu = Gpus[i];
        PH_SYSINFO_SECTION section;

        memset(&section, 0, sizeof(PH_SYSINFO_SECTION));
        section.Name = gpu->ShortName->sr;
        section.Flags = 0;
        section.Callback = SectionCallback;
        section.Context = gpu;

        gpu->Section = pointers->CreateSection(&section);
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

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"GPU Monitor";
        info->Description = L"One System Information section per GPU (WDDM and NVIDIA TCC), with per-app GPU load and memory.";
        info->Author = L"Alihkhawaher";
        info->Url = L"https://github.com/Alihkhawaher/systeminformer-plugins";

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackProcessProviderUpdatedEvent), ProcessesUpdatedCallback, NULL, &ProcessesUpdatedCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackSystemInformationInitializing), SystemInformationInitializingCallback, NULL, &SystemInformationInitializingCallbackRegistration);
    }

    return TRUE;
}
