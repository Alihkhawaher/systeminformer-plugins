/*
 * Tray icon helpers shared by this repo's plugins (header-only; build.ps1 adds common\ to the include path).
 *
 * A plugin registers its icons from GeneralCallbackTrayIconsInitializing with TrayRegister(); the user turns them
 * on under View > Tray icons. The update callback draws a 16x16 history graph (TrayDrawHistory) or a number
 * (TrayDrawText) and returns the tooltip text (the shell keeps 127 characters of it).
 *
 * SubId is what System Informer saves in its IconSettings to remember which icons are on, so it must stay the
 * same across restarts for the same GPU / disk.
 */

#pragma once

#include <phdk.h>

// Fills Count samples per line, newest first, already scaled to 0..1. Data2 is NULL for a one-line graph.
typedef VOID (*TRAY_FILL)(_In_opt_ PVOID Context, _Out_writes_(Count) PFLOAT Data1, _Out_writes_opt_(Count) PFLOAT Data2, _In_ ULONG Count);

static BOOLEAN TrayTransparency;

// The tray update thread has no auto pool: never use PH_AUTO / PhaXxx in an update callback. This appends a
// string and dereferences it.
static VOID TrayAppendOwned(_Inout_ PPH_STRING_BUILDER Builder, _In_opt_ _Post_invalid_ PPH_STRING String)
{
    if (String)
    {
        PhAppendStringBuilder(Builder, &String->sr);
        PhDereferenceObject(String);
    }
}

static GUID TrayGuid(_In_ CONST GUID* Base, _In_ ULONG SubId)
{
    GUID guid = *Base;

    guid.Data4[4] ^= (UCHAR)(SubId >> 24);
    guid.Data4[5] ^= (UCHAR)(SubId >> 16);
    guid.Data4[6] ^= (UCHAR)(SubId >> 8);
    guid.Data4[7] ^= (UCHAR)SubId;
    return guid;
}

// One registered icon: the plugin's update callback and context, and the mini info section shown when the user
// hovers or clicks the icon (System Informer's "hover info" window). Allocated once and never freed.
typedef struct _TRAY_ICON
{
    PPH_NF_ICON_UPDATE_CALLBACK Update;
    PVOID Context;
    PCWSTR Section;
} TRAY_ICON, *PTRAY_ICON;

_Function_class_(PH_NF_ICON_UPDATE_CALLBACK)
static VOID NTAPI TrayUpdateThunk(_In_ PPH_NF_ICON Icon, _Out_ PVOID* NewIconOrBitmap, _Out_ PULONG Flags, _Out_ PPH_STRING* NewText, _In_opt_ PVOID Context)
{
    PTRAY_ICON tray = Context;

    tray->Update(Icon, NewIconOrBitmap, Flags, NewText, tray->Context);
}

_Function_class_(PH_NF_ICON_MESSAGE_CALLBACK)
static BOOLEAN NTAPI TrayMessageThunk(_In_ PPH_NF_ICON Icon, _In_ ULONG_PTR WParam, _In_ ULONG_PTR LParam, _In_opt_ PVOID Context)
{
    PTRAY_ICON tray = Context;

    if (LOWORD(LParam) == PH_NF_MSG_SHOWMINIINFOSECTION && tray && tray->Section)
    {
        ((PPH_NF_MSG_SHOWMINIINFOSECTION_DATA)WParam)->SectionName = (PWSTR)tray->Section;
        return TRUE;
    }

    return FALSE;
}

// Text is kept by System Informer for the menu, and Section (a mini info section name created with
// TrayMiniInfoCreate) for the hover window: pass strings that live as long as the plugin (they are not freed).
// The update callback gets Context.
static VOID TrayRegister(
    _In_ PPH_TRAY_ICON_POINTERS Pointers,
    _In_ PPH_PLUGIN Plugin,
    _In_ CONST GUID* Base,
    _In_ ULONG SubId,
    _In_opt_ PVOID Context,
    _In_ PPH_STRING Text,
    _In_opt_ PCWSTR Section,
    _In_ BOOLEAN Available,
    _In_ PPH_NF_ICON_UPDATE_CALLBACK Update
    )
{
    PH_NF_ICON_REGISTRATION_DATA data;
    PTRAY_ICON tray = PhAllocateZero(sizeof(TRAY_ICON));

    TrayTransparency = !!PhGetIntegerSetting(SETTING_ICON_TRANSPARENCY_ENABLED);

    tray->Update = Update;
    tray->Context = Context;
    tray->Section = Section;
    data.UpdateCallback = TrayUpdateThunk;
    data.MessageCallback = TrayMessageThunk;
    Pointers->RegisterTrayIcon(Plugin, SubId, TrayGuid(Base, SubId), tray, Text->Buffer, Available ? 0 : PH_NF_ICON_UNAVAILABLE, &data);
}

// ---- mini info (hover window) list sections ----------------------------------------------------
// A section like System Informer's own "CPU" / "I/O": a header line and the processes sorted by one value.
// Callbacks run on the main thread (which has an auto pool). Fill a TRAY_MINIINFO once (static) and call
// TrayMiniInfoCreate for it from GeneralCallbackMiniInformationInitializing (which runs each time the window is
// created).

typedef struct _TRAY_MINIINFO
{
    PCWSTR Name;                                                    // unique, e.g. L"GPU: P40"
    PVOID Context;
    FLOAT (*ProcessValue)(_In_opt_ PVOID Context, _In_ PPH_PROCESS_ITEM Process);   // sort key, bigger first
    PPH_STRING (*FormatValue)(_In_opt_ PVOID Context, _In_ FLOAT Value);           // usage column
    PPH_STRING (*Header)(_In_opt_ PVOID Context);                                  // section title text
} TRAY_MINIINFO, *PTRAY_MINIINFO;

static int __cdecl TrayMiniInfoCompareProcess(_In_ void* Context, _In_ const void* A, _In_ const void* B)
{
    PTRAY_MINIINFO mi = Context;
    FLOAT a = mi->ProcessValue(mi->Context, (*(PPH_PROCESS_NODE*)A)->ProcessItem);
    FLOAT b = mi->ProcessValue(mi->Context, (*(PPH_PROCESS_NODE*)B)->ProcessItem);

    return a < b ? 1 : a > b ? -1 : 0;
}

static int __cdecl TrayMiniInfoCompareGroup(_In_ const void* A, _In_ const void* B)
{
    FLOAT a = *(PFLOAT)(*(PPH_MINIINFO_LIST_SECTION_SORT_DATA*)A)->UserData;
    FLOAT b = *(PFLOAT)(*(PPH_MINIINFO_LIST_SECTION_SORT_DATA*)B)->UserData;

    return a < b ? 1 : a > b ? -1 : 0;
}

_Function_class_(PH_MINIINFO_LIST_SECTION_CALLBACK)
static BOOLEAN NTAPI TrayMiniInfoCallback(_In_ PPH_MINIINFO_LIST_SECTION ListSection, _In_ PH_MINIINFO_LIST_SECTION_MESSAGE Message, _In_opt_ PVOID Parameter1, _In_opt_ PVOID Parameter2)
{
    PTRAY_MINIINFO mi = ListSection->Context;

    switch (Message)
    {
    case MiListSectionTick:
        {
            PPH_STRING text = mi->Header(mi->Context);

            ListSection->Section->Parameters->SetSectionText(ListSection->Section, text);
            PhDereferenceObject(text);
        }
        break;
    case MiListSectionSortProcessList:
        {
            PPH_MINIINFO_LIST_SECTION_SORT_LIST sortList = Parameter1;

            qsort_s(sortList->List->Items, sortList->List->Count, sizeof(PPH_PROCESS_NODE), TrayMiniInfoCompareProcess, mi);
        }
        return TRUE;
    case MiListSectionAssignSortData:
        {
            PPH_MINIINFO_LIST_SECTION_ASSIGN_SORT_DATA assign = Parameter1;
            PPH_LIST processes = assign->ProcessGroup->Processes;
            FLOAT sum = 0;

            for (ULONG i = 0; i < processes->Count; i++)
                sum += mi->ProcessValue(mi->Context, processes->Items[i]);

            *(PFLOAT)assign->SortData->UserData = sum;
        }
        return TRUE;
    case MiListSectionSortGroupList:
        {
            PPH_MINIINFO_LIST_SECTION_SORT_LIST sortList = Parameter1;

            qsort(sortList->List->Items, sortList->List->Count, sizeof(PPH_MINIINFO_LIST_SECTION_SORT_DATA), TrayMiniInfoCompareGroup);
        }
        return TRUE;
    case MiListSectionGetUsageText:
        {
            PPH_MINIINFO_LIST_SECTION_GET_USAGE_TEXT usage = Parameter1;

            PhMoveReference(&usage->Line1, mi->FormatValue(mi->Context, *(PFLOAT)usage->SortData->UserData));
        }
        return TRUE;
    }

    return FALSE;
}

static VOID TrayMiniInfoCreate(_In_ PPH_PLUGIN_MINIINFO_POINTERS Pointers, _In_ PTRAY_MINIINFO MiniInfo)
{
    PH_MINIINFO_LIST_SECTION section;

    memset(&section, 0, sizeof(PH_MINIINFO_LIST_SECTION));
    section.Context = MiniInfo;
    section.Callback = TrayMiniInfoCallback;
    Pointers->CreateListSection(MiniInfo->Name, 0, &section);
}

static VOID TrayFinish(_In_ PPH_GRAPH_DRAW_INFO DrawInfo, _In_ HDC Hdc, _In_ PVOID Bits, _In_ HBITMAP Bitmap, _In_ HBITMAP OldBitmap, _Out_ PVOID* NewIconOrBitmap, _Out_ PULONG Flags)
{
    if (TrayTransparency)
        PhBitmapSetAlpha(Bits, DrawInfo->Width, DrawInfo->Height);

    SelectBitmap(Hdc, OldBitmap);
    *NewIconOrBitmap = Bitmap;
    *Flags = PH_NF_UPDATE_IS_BITMAP;
}

// One line, or two stacked lines (Data1 at the bottom, Data2 on top of it) when Color2 != CLR_NONE.
static VOID TrayDrawHistory(
    _In_ PPH_NF_ICON Icon,
    _In_ COLORREF Color1,
    _In_ COLORREF Color2,
    _In_ TRAY_FILL Fill,
    _In_opt_ PVOID Context,
    _Out_ PVOID* NewIconOrBitmap,
    _Out_ PULONG Flags
    )
{
    PH_GRAPH_DRAW_INFO drawInfo;
    HBITMAP bitmap;
    HBITMAP oldBitmap;
    PVOID bits;
    HDC hdc;
    ULONG count;
    PFLOAT data1;
    PFLOAT data2 = NULL;
    BOOLEAN twoLines = Color2 != CLR_NONE;

    memset(&drawInfo, 0, sizeof(PH_GRAPH_DRAW_INFO));
    drawInfo.Width = 16;
    drawInfo.Height = 16;
    drawInfo.Flags = twoLines ? PH_GRAPH_USE_LINE_2 : 0;
    drawInfo.Step = 2;
    drawInfo.BackColor = RGB(0x00, 0x00, 0x00);

    Icon->Pointers->BeginBitmap(&drawInfo.Width, &drawInfo.Height, &bitmap, &bits, &hdc, &oldBitmap);

    count = drawInfo.Width / 2 + 1;
    data1 = PhAllocateZero(count * sizeof(FLOAT));
    if (twoLines)
        data2 = PhAllocateZero(count * sizeof(FLOAT));

    Fill(Context, data1, data2, count);

    drawInfo.LineDataCount = count;
    drawInfo.LineData1 = data1;
    drawInfo.LineData2 = data2;
    drawInfo.LineColor1 = Color1;
    drawInfo.LineBackColor1 = PhHalveColorBrightness(Color1);
    if (twoLines)
    {
        drawInfo.LineColor2 = Color2;
        drawInfo.LineBackColor2 = PhHalveColorBrightness(Color2);
    }

    PhDrawGraphDirect(hdc, bits, &drawInfo);
    TrayFinish(&drawInfo, hdc, bits, bitmap, oldBitmap, NewIconOrBitmap, Flags);

    if (data2)
        PhFree(data2);
    PhFree(data1);
}

// A number ("45") in the icon (no plugin uses it yet), like System Informer's "CPU usage (text)".
static VOID TrayDrawText(
    _In_ PPH_NF_ICON Icon,
    _In_ FLOAT Value,
    _In_ COLORREF Color,
    _Out_ PVOID* NewIconOrBitmap,
    _Out_ PULONG Flags
    )
{
    PH_GRAPH_DRAW_INFO drawInfo;
    HBITMAP bitmap;
    HBITMAP oldBitmap;
    PVOID bits;
    HDC hdc;
    PH_FORMAT format[1];
    PPH_STRING text;

    memset(&drawInfo, 0, sizeof(PH_GRAPH_DRAW_INFO));
    drawInfo.Width = 16;
    drawInfo.Height = 16;
    drawInfo.BackColor = RGB(0x00, 0x00, 0x00);

    Icon->Pointers->BeginBitmap(&drawInfo.Width, &drawInfo.Height, &bitmap, &bits, &hdc, &oldBitmap);

    PhInitFormatF(&format[0], Value, 0);
    text = PhFormat(format, 1, 0);
    drawInfo.TextFont = PhNfGetTrayIconFont(0);
    drawInfo.TextColor = Color;
    PhDrawTrayIconText(hdc, bits, &drawInfo, &text->sr);
    PhDereferenceObject(text);

    TrayFinish(&drawInfo, hdc, bits, bitmap, oldBitmap, NewIconOrBitmap, Flags);
}

// Copies Count samples of a buffer into Data, divided by Max and clamped to 0..1.
static VOID TrayScale(_In_ PPH_CIRCULAR_BUFFER_FLOAT Buffer, _In_ FLOAT Max, _Out_writes_(Count) PFLOAT Data, _In_ ULONG Count)
{
    ULONG available = Buffer->Count < Count ? Buffer->Count : Count;

    if (Max <= 0)
        Max = 1;

    for (ULONG i = 0; i < Count; i++)
    {
        FLOAT v = i < available ? PhGetItemCircularBuffer_FLOAT(Buffer, i) / Max : 0;
        Data[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
}

// Largest value over the first Count samples (with Buffer2 added when given), at least Floor.
static FLOAT TrayPeak(_In_ PPH_CIRCULAR_BUFFER_FLOAT Buffer1, _In_opt_ PPH_CIRCULAR_BUFFER_FLOAT Buffer2, _In_ ULONG Count, _In_ FLOAT Floor)
{
    ULONG available = Buffer1->Count < Count ? Buffer1->Count : Count;
    FLOAT max = Floor;

    for (ULONG i = 0; i < available; i++)
    {
        FLOAT v = PhGetItemCircularBuffer_FLOAT(Buffer1, i) + (Buffer2 ? PhGetItemCircularBuffer_FLOAT(Buffer2, i) : 0);
        if (v > max)
            max = v;
    }

    return max;
}
