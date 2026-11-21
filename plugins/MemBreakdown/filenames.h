/*
 * MemBreakdown file names (filenames.c): kernel file key -> NT path, from an ETW file-name rundown.
 */

#pragma once

typedef struct _MB_FILE_NAME
{
    ULONG_PTR Key;
    PWSTR Name;     // NT path, e.g. \Device\HarddiskVolume3\...
} MB_FILE_NAME, *PMB_FILE_NAME;

typedef struct _MB_FILE_NAMES
{
    PMB_FILE_NAME Names;    // sorted by Key
    ULONG Count;
    ULONG Events;           // FileIo events seen (for diagnostics)
} MB_FILE_NAMES, *PMB_FILE_NAMES;

// Runs a private kernel trace session for RunMs, stops it and collects the file names it reported.
// Returns a Win32 error code (ERROR_SUCCESS when the session ran). Needs admin.
ULONG MbCollectFileNames(
    _Out_ PMB_FILE_NAMES Names,
    _In_ ULONG RunMs
    );

PCWSTR MbFindFileName(
    _In_ PMB_FILE_NAMES Names,
    _In_ ULONG_PTR Key
    );

VOID MbFreeFileNames(
    _Inout_ PMB_FILE_NAMES Names
    );
