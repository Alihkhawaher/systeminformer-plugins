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

// Text is kept by System Informer for the menu: pass a string that lives as long as the plugin (it is not freed).
static VOID TrayRegister(
    _In_ PPH_TRAY_ICON_POINTERS Pointers,
    _In_ PPH_PLUGIN Plugin,
    _In_ CONST GUID* Base,
    _In_ ULONG SubId,
    _In_opt_ PVOID Context,
    _In_ PPH_STRING Text,
    _In_ BOOLEAN Available,
    _In_ PPH_NF_ICON_UPDATE_CALLBACK Update
    )
{
    PH_NF_ICON_REGISTRATION_DATA data;

    TrayTransparency = !!PhGetIntegerSetting(SETTING_ICON_TRANSPARENCY_ENABLED);

    data.UpdateCallback = Update;
    data.MessageCallback = NULL;
    Pointers->RegisterTrayIcon(Plugin, SubId, TrayGuid(Base, SubId), Context, Text->Buffer, Available ? 0 : PH_NF_ICON_UNAVAILABLE, &data);
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
