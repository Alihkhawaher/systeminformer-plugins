/*
 * NvTccGpu - System Informer plugin that shows NVIDIA GPUs running in TCC (or MCDM) mode,
 * e.g. a Tesla P40. System Informer's own GPU page uses D3DKMT/WDDM and cannot see them.
 * Data comes from NVML (nvml.dll, installed with the NVIDIA driver).
 *
 * One System Information section per non-WDDM NVIDIA GPU, with graphs for
 * GPU load, dedicated memory, memory-controller load, power and temperature.
 */

#include <phdk.h>
#include <settings.h>
#include "resource.h"

#define PLUGIN_NAME L"NvTccGpu"
#define SETTING_NAME_SHOW_WDDM (PLUGIN_NAME L".ShowWddmGpus")
#define GRAPH_PADDING 3
#define METRIC_COUNT 5

// ---- minimal NVML ABI ------------------------------------------------------

typedef int nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef struct { unsigned long long total; unsigned long long free; unsigned long long used; } nvmlMemory_t;

#define NVML_SUCCESS 0
#define NVML_DRIVER_WDDM 0
#define NVML_DRIVER_WDM 1   // TCC
#define NVML_DRIVER_MCDM 2
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_SM 1
#define NVML_CLOCK_MEM 2

typedef nvmlReturn_t (__cdecl* _nvmlInit_v2)(void);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetCount_v2)(unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetName)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetDriverModel)(nvmlDevice_t, int*, int*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetEnforcedPowerLimit)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetTemperature)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetFanSpeed)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (__cdecl* _nvmlDeviceGetClockInfo)(nvmlDevice_t, int, unsigned int*);

static struct
{
    _nvmlInit_v2 Init;
    _nvmlDeviceGetCount_v2 GetCount;
    _nvmlDeviceGetHandleByIndex_v2 GetHandleByIndex;
    _nvmlDeviceGetName GetName;
    _nvmlDeviceGetDriverModel GetDriverModel;
    _nvmlDeviceGetUtilizationRates GetUtilizationRates;
    _nvmlDeviceGetMemoryInfo GetMemoryInfo;
    _nvmlDeviceGetPowerUsage GetPowerUsage;
    _nvmlDeviceGetEnforcedPowerLimit GetEnforcedPowerLimit;
    _nvmlDeviceGetTemperature GetTemperature;
    _nvmlDeviceGetFanSpeed GetFanSpeed;
    _nvmlDeviceGetClockInfo GetClockInfo;
} Nvml;

// ---- state -----------------------------------------------------------------

typedef enum _METRIC
{
    MetricGpu,
    MetricMemory,
    MetricBandwidth,
    MetricPower,
    MetricTemperature
} METRIC;

typedef struct _TCC_GPU
{
    nvmlDevice_t Device;
    ULONG Index;
    PPH_STRING Name;        // "Tesla P40"
    PPH_STRING ShortName;   // "P40"
    PWSTR DriverModel;      // "TCC"
    BOOLEAN Numbered;       // ShortName carries " #<index>" (identical cards)

    // latest sample
    BOOLEAN Valid;
    FLOAT GpuUsage;         // 0..1
    FLOAT BandwidthUsage;   // 0..1
    ULONG64 MemoryUsed;
    ULONG64 MemoryTotal;
    FLOAT PowerWatts;
    FLOAT PowerLimitWatts;
    FLOAT TemperatureC;
    ULONG FanPercent;
    BOOLEAN FanSupported;
    ULONG SmClock;
    ULONG MemClock;

    // history; all values stored in display units
    PH_CIRCULAR_BUFFER_FLOAT History[METRIC_COUNT];

    // sysinfo window
    PPH_SYSINFO_SECTION Section;
    HWND Dialog;
    LONG DialogDpi;
    PH_LAYOUT_MANAGER LayoutManager;
    RECT GraphMargin;
    RECT GraphMarginScaled;
    HWND GraphHandle[METRIC_COUNT];
    PH_GRAPH_STATE GraphState[METRIC_COUNT];
    HWND Panel;
} TCC_GPU, *PTCC_GPU;

static PPH_PLUGIN PluginInstance;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION SystemInformationInitializingCallbackRegistration;
static PTCC_GPU Gpus[16];
static ULONG GpuCount;
static BOOLEAN UseFahrenheit;

static CONST ULONG MetricLabelIds[METRIC_COUNT] = { IDC_LABEL0, IDC_LABEL1, IDC_LABEL2, IDC_LABEL3, IDC_LABEL4 };

// ---- NVML ------------------------------------------------------------------

static BOOLEAN NvmlLoad(VOID)
{
    HMODULE module;
    WCHAR path[MAX_PATH];

    module = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!module)
    {
        if (ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, RTL_NUMBER_OF(path)))
            module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    if (!module)
        return FALSE;

#define NVML_GET(Field, Name) if (!(Nvml.Field = (PVOID)GetProcAddress(module, Name))) return FALSE
    NVML_GET(Init, "nvmlInit_v2");
    NVML_GET(GetCount, "nvmlDeviceGetCount_v2");
    NVML_GET(GetHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
    NVML_GET(GetName, "nvmlDeviceGetName");
    NVML_GET(GetDriverModel, "nvmlDeviceGetDriverModel");
    NVML_GET(GetUtilizationRates, "nvmlDeviceGetUtilizationRates");
    NVML_GET(GetMemoryInfo, "nvmlDeviceGetMemoryInfo");
    NVML_GET(GetPowerUsage, "nvmlDeviceGetPowerUsage");
    NVML_GET(GetEnforcedPowerLimit, "nvmlDeviceGetEnforcedPowerLimit");
    NVML_GET(GetTemperature, "nvmlDeviceGetTemperature");
    NVML_GET(GetFanSpeed, "nvmlDeviceGetFanSpeed");
    NVML_GET(GetClockInfo, "nvmlDeviceGetClockInfo");
#undef NVML_GET

    return Nvml.Init() == NVML_SUCCESS;
}

static VOID NvmlDiscover(VOID)
{
    unsigned int count = 0;
    BOOLEAN showWddm = !!PhGetIntegerSetting(SETTING_NAME_SHOW_WDDM);
    ULONG sampleCount = PhGetIntegerSetting(SETTING_SAMPLE_COUNT);

    if (Nvml.GetCount(&count) != NVML_SUCCESS)
        return;

    for (unsigned int i = 0; i < count && GpuCount < RTL_NUMBER_OF(Gpus); i++)
    {
        nvmlDevice_t device;
        int current = -1, pending = -1;
        char name[96] = { 0 };
        PTCC_GPU gpu;

        if (Nvml.GetHandleByIndex(i, &device) != NVML_SUCCESS)
            continue;

        Nvml.GetDriverModel(device, &current, &pending);

        if (current == NVML_DRIVER_WDDM && !showWddm)
            continue; // System Informer's own GPU page already covers WDDM adapters.

        gpu = PhAllocateZero(sizeof(TCC_GPU));
        gpu->Device = device;
        gpu->Index = i;
        gpu->DriverModel = current == NVML_DRIVER_WDM ? L"TCC" : current == NVML_DRIVER_MCDM ? L"MCDM" : current == NVML_DRIVER_WDDM ? L"WDDM" : L"?";

        Nvml.GetName(device, name, sizeof(name));
        gpu->Name = PhConvertUtf8ToUtf16(name);

        {
            PH_STRINGREF remaining = gpu->Name->sr;
            PH_STRINGREF prefix;

            PhInitializeStringRef(&prefix, L"Tesla ");
            if (PhStartsWithStringRef(&remaining, &prefix, TRUE)) PhSkipStringRef(&remaining, prefix.Length);
            PhInitializeStringRef(&prefix, L"NVIDIA ");
            if (PhStartsWithStringRef(&remaining, &prefix, TRUE)) PhSkipStringRef(&remaining, prefix.Length);

            PhInitializeStringRef(&prefix, L"Quadro ");
            if (PhStartsWithStringRef(&remaining, &prefix, TRUE)) PhSkipStringRef(&remaining, prefix.Length);

            gpu->ShortName = PhCreateString2(&remaining);

            // Several identical cards (e.g. two P40s): number every one of them, "P40 #0", "P40 #1".
            for (ULONG j = 0; j < GpuCount; j++)
            {
                if (PhEqualString(Gpus[j]->Name, gpu->Name, TRUE))
                {
                    PH_FORMAT format[3];

                    if (!Gpus[j]->Numbered)
                    {
                        Gpus[j]->Numbered = TRUE;
                        PhInitFormatSR(&format[0], Gpus[j]->ShortName->sr);
                        PhInitFormatS(&format[1], L" #");
                        PhInitFormatU(&format[2], Gpus[j]->Index);
                        PhMoveReference(&Gpus[j]->ShortName, PhFormat(format, 3, 0));
                    }

                    PhInitFormatSR(&format[0], gpu->ShortName->sr);
                    PhInitFormatS(&format[1], L" #");
                    PhInitFormatU(&format[2], gpu->Index);
                    PhMoveReference(&gpu->ShortName, PhFormat(format, 3, 0));
                    gpu->Numbered = TRUE;
                    break;
                }
            }
        }

        {
            unsigned int limit = 0;

            if (Nvml.GetEnforcedPowerLimit(device, &limit) == NVML_SUCCESS && limit)
                gpu->PowerLimitWatts = limit / 1000.0f;
            else
                gpu->PowerLimitWatts = 250.0f;
        }

        for (ULONG m = 0; m < METRIC_COUNT; m++)
            PhInitializeCircularBuffer_FLOAT(&gpu->History[m], sampleCount);

        Gpus[GpuCount++] = gpu;
    }
}

static VOID NvmlSample(_In_ PTCC_GPU Gpu)
{
    nvmlUtilization_t util;
    nvmlMemory_t memory;
    unsigned int value;

    Gpu->Valid = TRUE;

    if (Nvml.GetUtilizationRates(Gpu->Device, &util) == NVML_SUCCESS)
    {
        Gpu->GpuUsage = util.gpu / 100.0f;
        Gpu->BandwidthUsage = util.memory / 100.0f;
    }
    else
    {
        Gpu->Valid = FALSE;
        Gpu->GpuUsage = 0;
        Gpu->BandwidthUsage = 0;
    }

    if (Nvml.GetMemoryInfo(Gpu->Device, &memory) == NVML_SUCCESS)
    {
        Gpu->MemoryUsed = memory.used;
        Gpu->MemoryTotal = memory.total;
    }

    Gpu->PowerWatts = Nvml.GetPowerUsage(Gpu->Device, &value) == NVML_SUCCESS ? value / 1000.0f : 0;
    Gpu->TemperatureC = Nvml.GetTemperature(Gpu->Device, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS ? (FLOAT)value : 0;
    Gpu->FanSupported = Nvml.GetFanSpeed(Gpu->Device, &value) == NVML_SUCCESS;
    Gpu->FanPercent = Gpu->FanSupported ? value : 0;
    Gpu->SmClock = Nvml.GetClockInfo(Gpu->Device, NVML_CLOCK_SM, &value) == NVML_SUCCESS ? value : 0;
    Gpu->MemClock = Nvml.GetClockInfo(Gpu->Device, NVML_CLOCK_MEM, &value) == NVML_SUCCESS ? value : 0;

    PhAddItemCircularBuffer_FLOAT(&Gpu->History[MetricGpu], Gpu->GpuUsage);
    PhAddItemCircularBuffer_FLOAT(&Gpu->History[MetricMemory], (FLOAT)Gpu->MemoryUsed);
    PhAddItemCircularBuffer_FLOAT(&Gpu->History[MetricBandwidth], Gpu->BandwidthUsage);
    PhAddItemCircularBuffer_FLOAT(&Gpu->History[MetricPower], Gpu->PowerWatts);
    PhAddItemCircularBuffer_FLOAT(&Gpu->History[MetricTemperature], Gpu->TemperatureC);
}

// ---- formatting ------------------------------------------------------------

static FLOAT MetricMax(_In_ PTCC_GPU Gpu, _In_ ULONG Metric)
{
    switch (Metric)
    {
    case MetricMemory: return Gpu->MemoryTotal ? (FLOAT)Gpu->MemoryTotal : 1.0f;
    case MetricPower: return Gpu->PowerLimitWatts;
    case MetricTemperature: return 100.0f;
    default: return 1.0f;
    }
}

static PPH_STRING FormatMetric(_In_ ULONG Metric, _In_ FLOAT Value)
{
    PH_FORMAT format[2];

    switch (Metric)
    {
    case MetricMemory:
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
DEFINE_LABEL_Y(MetricMemory)
DEFINE_LABEL_Y(MetricBandwidth)
DEFINE_LABEL_Y(MetricPower)
DEFINE_LABEL_Y(MetricTemperature)

static PPH_GRAPH_LABEL_Y_FUNCTION LabelYFunctions[METRIC_COUNT] =
{
    LabelYMetricGpu, LabelYMetricMemory, LabelYMetricBandwidth, LabelYMetricPower, LabelYMetricTemperature
};

static PCWSTR MetricColorSetting(_In_ ULONG Metric)
{
    switch (Metric)
    {
    case MetricMemory: return SETTING_COLOR_PRIVATE;
    case MetricBandwidth: return SETTING_COLOR_PHYSICAL;
    case MetricPower: return SETTING_COLOR_POWER_USAGE;
    case MetricTemperature: return SETTING_COLOR_TEMPERATURE;
    default: return SETTING_COLOR_CPU_KERNEL;
    }
}

// ---- graphs ----------------------------------------------------------------

static VOID FillGraphData(_In_ PTCC_GPU Gpu, _In_ ULONG Metric, _Out_writes_(Count) PFLOAT Data, _In_ ULONG Count)
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
    PTCC_GPU gpu = Context;
    NMHDR* header = Parameter1;
    ULONG metric = ULONG_MAX;

    if (!gpu || !header)
        return TRUE;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        if (header->hwndFrom == gpu->GraphHandle[m])
            metric = m;
    }

    if (metric == ULONG_MAX)
        return TRUE;

    switch (header->code)
    {
    case GCN_GETDRAWINFO:
        {
            PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)header;
            PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;
            PPH_GRAPH_STATE state = &gpu->GraphState[metric];

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
            PPH_GRAPH_STATE state = &gpu->GraphState[metric];

            if (getTooltipText->Index < getTooltipText->TotalCount)
            {
                if (state->TooltipIndex != getTooltipText->Index)
                {
                    FLOAT value = PhGetItemCircularBuffer_FLOAT(&gpu->History[metric], getTooltipText->Index);
                    PH_FORMAT format[3];

                    PhInitFormatSR(&format[0], PH_AUTO_T(PH_STRING, FormatMetric(metric, value))->sr);
                    PhInitFormatC(&format[1], L'\n');
                    PhInitFormatSR(&format[2], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);

                    PhMoveReference(&state->TooltipText, PhFormat(format, RTL_NUMBER_OF(format), 0));
                }

                getTooltipText->Text = PhGetStringRef(state->TooltipText);
            }
        }
        break;
    }

    return TRUE;
}

static VOID CreateGraphs(_In_ PTCC_GPU Gpu)
{
    PH_GRAPH_CREATEPARAMS createParams;

    memset(&createParams, 0, sizeof(PH_GRAPH_CREATEPARAMS));
    createParams.Size = sizeof(PH_GRAPH_CREATEPARAMS);
    createParams.Callback = GraphMessageCallback;
    createParams.Context = Gpu;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        Gpu->GraphHandle[m] = PhCreateWindow(
            PH_GRAPH_CLASSNAME,
            NULL,
            WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS,
            0, 0, 0, 0,
            Gpu->Dialog,
            NULL,
            NULL,
            &createParams
            );
        Graph_SetTooltip(Gpu->GraphHandle[m], TRUE);
    }
}

static VOID LayoutGraphs(_In_ PTCC_GPU Gpu)
{
    RECT clientRect;
    RECT labelRect;
    RECT margin = Gpu->GraphMarginScaled;
    LONG padding = PhScaleToDisplay(GRAPH_PADDING, Gpu->DialogDpi);
    LONG graphWidth;
    LONG graphHeight;
    LONG y;
    HDWP defer;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        Gpu->GraphState[m].Valid = FALSE;
        Gpu->GraphState[m].TooltipIndex = ULONG_MAX;
    }

    PhGetClientRect(Gpu->Dialog, &clientRect);
    PhGetClientRect(GetDlgItem(Gpu->Dialog, IDC_LABEL0), &labelRect);
    graphWidth = clientRect.right - margin.left - margin.right;
    graphHeight = (clientRect.bottom - margin.top - margin.bottom - labelRect.bottom * METRIC_COUNT - padding * (METRIC_COUNT + 2)) / METRIC_COUNT;

    defer = BeginDeferWindowPos(METRIC_COUNT * 2);
    y = margin.top;

    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        BOOLEAN last = m == METRIC_COUNT - 1;

        defer = DeferWindowPos(defer, GetDlgItem(Gpu->Dialog, MetricLabelIds[m]), NULL, margin.left, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        y += labelRect.bottom + padding;
        defer = DeferWindowPos(defer, Gpu->GraphHandle[m], NULL, margin.left, y, graphWidth,
            last ? clientRect.bottom - margin.bottom - y : graphHeight, SWP_NOACTIVATE | SWP_NOZORDER);
        y += graphHeight + padding;
    }

    EndDeferWindowPos(defer);
}

static VOID SetLabelFormat(_In_ HWND Dialog, _In_ ULONG Id, _In_ PPH_STRING String)
{
    PhSetDialogItemText(Dialog, Id, PhGetString(String));
    PhDereferenceObject(String);
}

static VOID UpdatePanel(_In_ PTCC_GPU Gpu)
{
    PH_FORMAT format[2];

    if (!Gpu->Panel)
        return;

    PhInitFormatSize(&format[0], Gpu->MemoryUsed);
    SetLabelFormat(Gpu->Panel, IDC_MEMUSED_V, PhFormat(format, 1, 0));
    PhInitFormatSize(&format[0], Gpu->MemoryTotal);
    SetLabelFormat(Gpu->Panel, IDC_MEMTOTAL_V, PhFormat(format, 1, 0));
    SetLabelFormat(Gpu->Panel, IDC_POWER_V, FormatMetric(MetricPower, Gpu->PowerWatts));
    SetLabelFormat(Gpu->Panel, IDC_POWERLIMIT_V, FormatMetric(MetricPower, Gpu->PowerLimitWatts));

    PhInitFormatU(&format[0], Gpu->SmClock);
    PhInitFormatS(&format[1], L" MHz");
    SetLabelFormat(Gpu->Panel, IDC_SMCLOCK_V, PhFormat(format, 2, 0));
    PhInitFormatU(&format[0], Gpu->MemClock);
    SetLabelFormat(Gpu->Panel, IDC_MEMCLOCK_V, PhFormat(format, 2, 0));

    PhSetDialogItemText(Gpu->Panel, IDC_DRIVER_V, Gpu->DriverModel);

    if (Gpu->FanSupported)
    {
        PhInitFormatS(&format[0], L"Fan ");
        PhInitFormatU(&format[1], Gpu->FanPercent);
        SetLabelFormat(Gpu->Panel, IDC_FAN_V, PhFormat(format, 2, 0));
    }
    else
    {
        PhSetDialogItemText(Gpu->Panel, IDC_FAN_V, L"Passive");
    }
}

static VOID TickDialog(_In_ PTCC_GPU Gpu)
{
    for (ULONG m = 0; m < METRIC_COUNT; m++)
    {
        Gpu->GraphState[m].Valid = FALSE;
        Gpu->GraphState[m].TooltipIndex = ULONG_MAX;
        Graph_MoveGrid(Gpu->GraphHandle[m], 1);
        Graph_Draw(Gpu->GraphHandle[m]);
        Graph_UpdateTooltip(Gpu->GraphHandle[m]);
        InvalidateRect(Gpu->GraphHandle[m], NULL, FALSE);
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
    switch (WindowMessage)
    {
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
    PTCC_GPU gpu;

    if (WindowMessage == WM_INITDIALOG)
    {
        gpu = (PTCC_GPU)lParam;
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

            for (ULONG m = 0; m < METRIC_COUNT; m++)
                PhInitializeGraphState(&gpu->GraphState[m]);

            gpu->Dialog = WindowHandle;
            gpu->DialogDpi = PhGetWindowDpi(WindowHandle);
            PhSetWindowExStyle(WindowHandle, WS_EX_TRANSPARENT, 0);

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

            // "Tesla P40 (GPU 1, TCC)"
            PhInitFormatSR(&format[0], gpu->Name->sr);
            PhInitFormatS(&format[1], L"  •  NVML GPU ");
            PhInitFormatU(&format[2], gpu->Index);
            PhInitFormatS(&format[3], gpu->DriverModel[0] == L'T' ? L"  •  TCC" : L"");
            SetLabelFormat(WindowHandle, IDC_GPUNAME, PhFormat(format, 4, 0));

            gpu->Panel = PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_TCCGPU_PANEL), WindowHandle, PanelDialogProc, NULL);
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

// ---- sysinfo section -------------------------------------------------------

_Function_class_(PH_SYSINFO_SECTION_CALLBACK)
static BOOLEAN NTAPI SectionCallback(
    _In_ PPH_SYSINFO_SECTION Section,
    _In_ PH_SYSINFO_SECTION_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2
    )
{
    PTCC_GPU gpu = Section->Context;

    switch (Message)
    {
    case SysInfoDestroy:
        {
            if (gpu->Dialog)
            {
                for (ULONG m = 0; m < METRIC_COUNT; m++)
                {
                    PhDeleteGraphState(&gpu->GraphState[m]);
                    gpu->GraphHandle[m] = NULL;
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

            for (ULONG m = 0; m < METRIC_COUNT; m++)
            {
                if (gpu->GraphHandle[m])
                {
                    gpu->GraphState[m].Valid = FALSE;
                    gpu->GraphState[m].TooltipIndex = ULONG_MAX;
                    Graph_Draw(gpu->GraphHandle[m]);
                }
            }
        }
        return TRUE;
    case SysInfoCreateDialog:
        {
            PPH_SYSINFO_CREATE_DIALOG createDialog = Parameter1;

            createDialog->Instance = PluginInstance->DllBase;
            createDialog->Template = MAKEINTRESOURCE(IDD_TCCGPU_DIALOG);
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
            PH_FORMAT format[5];

            if (!getTooltipText)
                break;

            PhInitFormatSR(&format[0], PH_AUTO_T(PH_STRING, FormatMetric(MetricGpu, PhGetItemCircularBuffer_FLOAT(&gpu->History[MetricGpu], getTooltipText->Index)))->sr);
            PhInitFormatS(&format[1], L"  •  ");
            PhInitFormatSR(&format[2], PH_AUTO_T(PH_STRING, FormatMetric(MetricMemory, PhGetItemCircularBuffer_FLOAT(&gpu->History[MetricMemory], getTooltipText->Index)))->sr);
            PhInitFormatC(&format[3], L'\n');
            PhInitFormatSR(&format[4], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, getTooltipText->Index))->sr);

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

            PhInitFormatF(&format[0], gpu->GpuUsage * 100, 0);
            PhInitFormatS(&format[1], L"%\n");
            PhInitFormatSize(&format[2], gpu->MemoryUsed);
            PhInitFormatS(&format[3], L" / ");
            PhInitFormatSize(&format[4], gpu->MemoryTotal);
            drawPanel->SubTitle = PhFormat(format, 5, 64);
            drawPanel->SubTitleOverflow = PhFormat(format, 3, 64);
        }
        return TRUE;
    }

    return FALSE;
}

// ---- plugin callbacks ------------------------------------------------------

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    static CONST PH_STRINGREF extendedTools = PH_STRINGREF_INIT(L"ExtendedTools");

    // Follow ExtendedTools' Fahrenheit option when that plugin is loaded (its setting only exists then).
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

    if (NvmlLoad())
        NvmlDiscover();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    for (ULONG i = 0; i < GpuCount; i++)
        NvmlSample(Gpus[i]);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI SystemInformationInitializingCallback(_In_opt_ PVOID Parameter, _In_opt_ PVOID Context)
{
    PPH_PLUGIN_SYSINFO_POINTERS pointers = Parameter;

    if (!pointers)
        return;

    for (ULONG i = 0; i < GpuCount; i++)
    {
        PTCC_GPU gpu = Gpus[i];
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
        PH_SETTING_CREATE settings[] =
        {
            { IntegerSettingType, SETTING_NAME_SHOW_WDDM, L"0" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"NVIDIA TCC GPUs";
        info->Description = L"Shows NVIDIA GPUs in TCC/MCDM mode (e.g. Tesla P40) in System Information, using NVML.";
        info->Author = L"Alihkhawaher";
        info->Url = L"https://github.com/Alihkhawaher/systeminformer-plugins";

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackProcessProviderUpdatedEvent), ProcessesUpdatedCallback, NULL, &ProcessesUpdatedCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackSystemInformationInitializing), SystemInformationInitializingCallback, NULL, &SystemInformationInitializingCallbackRegistration);

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
