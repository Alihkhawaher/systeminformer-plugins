/*
 * SMART straight from the drive, the way CrystalDiskInfo does it (documented disk IOCTLs, no driver):
 *   NVMe: IOCTL_STORAGE_QUERY_PROPERTY, protocol-specific NVMe log page 02h (SMART / Health Information).
 *   SATA: SMART_RCV_DRIVE_DATA, READ ATTRIBUTES (D0h) + READ THRESHOLDS (D1h).
 *   USB:  through the enclosure's bridge with IOCTL_SCSI_PASS_THROUGH: SATA disks via SAT ATA PASS-THROUGH,
 *         NVMe disks via the JMicron / Realtek / ASMedia command, sent ONLY to a bridge known to understand it
 *         (VID:PID lists from smartmontools' drivedb.h). Only read commands are ever sent.
 * Ported from the remote support agent's health check (health.ps1).
 */

#include "diskmon.h"
#include <stdarg.h>
#include <setupapi.h>
#include <cfgmgr32.h>

// {53F56307-B6BF-11D0-94F2-00A0C91EFB8B}
static CONST GUID DiskInterfaceGuid = { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

static HANDLE OpenDisk(_In_ ULONG Disk, _In_ ULONG Access)
{
    WCHAR path[32];

    _snwprintf_s(path, RTL_NUMBER_OF(path), _TRUNCATE, L"\\\\.\\PhysicalDrive%lu", Disk);
    return CreateFileW(path, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static VOID SetText(_Out_writes_(Count) PWSTR Buffer, _In_ SIZE_T Count, _In_ PCWSTR Format, ...)
{
    va_list args;

    va_start(args, Format);
    _vsnwprintf_s(Buffer, Count, _TRUNCATE, Format, args);
    va_end(args);
}

VOID HealthAddProblem(
    _Inout_updates_(Count) PWSTR Buffer,
    _In_ SIZE_T Count,
    _Inout_ SMART_STATUS* Status,
    _In_ SMART_STATUS Level,
    _In_ PCWSTR Format,
    ...
    )
{
    WCHAR text[160];
    va_list args;

    va_start(args, Format);
    _vsnwprintf_s(text, RTL_NUMBER_OF(text), _TRUNCATE, Format, args);
    va_end(args);

    if (Buffer[0])
        wcsncat_s(Buffer, Count, L", ", _TRUNCATE);
    wcsncat_s(Buffer, Count, text, _TRUNCATE);

    if (*Status < Level)
        *Status = Level;
}

// ---- raw reads ---------------------------------------------------------------------------------

static BOOLEAN ReadNvmeHealthLog(_In_ ULONG Disk, _Out_writes_(512) PUCHAR Log)
{
    static CONST ULONG accesses[] = { GENERIC_READ | GENERIC_WRITE, 0 };
    static CONST STORAGE_PROPERTY_ID properties[] = { StorageDeviceProtocolSpecificProperty, StorageAdapterProtocolSpecificProperty };
    UCHAR buffer[sizeof(STORAGE_PROPERTY_QUERY) - 1 + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 4096];

    for (ULONG a = 0; a < RTL_NUMBER_OF(accesses); a++)
    {
        HANDLE handle = OpenDisk(Disk, accesses[a]);

        if (handle == INVALID_HANDLE_VALUE)
            continue;

        for (ULONG p = 0; p < RTL_NUMBER_OF(properties); p++)
        {
            PSTORAGE_PROPERTY_QUERY query = (PSTORAGE_PROPERTY_QUERY)buffer;
            PSTORAGE_PROTOCOL_SPECIFIC_DATA protocol = (PSTORAGE_PROTOCOL_SPECIFIC_DATA)query->AdditionalParameters;
            PSTORAGE_PROTOCOL_DATA_DESCRIPTOR descriptor = (PSTORAGE_PROTOCOL_DATA_DESCRIPTOR)buffer;
            DWORD returned = 0;

            memset(buffer, 0, sizeof(buffer));
            query->PropertyId = properties[p];
            query->QueryType = PropertyStandardQuery;
            protocol->ProtocolType = ProtocolTypeNvme;
            protocol->DataType = NVMeDataTypeLogPage;
            protocol->ProtocolDataRequestValue = 2; // SMART / Health Information
            protocol->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
            protocol->ProtocolDataLength = 512;

            if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, buffer, sizeof(buffer), buffer, sizeof(buffer), &returned, NULL))
            {
                protocol = &descriptor->ProtocolSpecificData;

                if (descriptor->Version == sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) &&
                    protocol->ProtocolDataOffset >= sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) &&
                    protocol->ProtocolDataLength >= 512 &&
                    FIELD_OFFSET(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData) + protocol->ProtocolDataOffset + 512 <= returned)
                {
                    memcpy(Log, (PUCHAR)protocol + protocol->ProtocolDataOffset, 512);
                    CloseHandle(handle);
                    return TRUE;
                }
            }
        }

        CloseHandle(handle);
    }

    return FALSE;
}

static BOOLEAN ReadAtaSmart(_In_ HANDLE Handle, _In_ UCHAR Feature, _Out_writes_(512) PUCHAR Data)
{
    SENDCMDINPARAMS input;
    UCHAR output[sizeof(SENDCMDOUTPARAMS) - 1 + 512];
    PSENDCMDOUTPARAMS result = (PSENDCMDOUTPARAMS)output;
    DWORD returned = 0;

    memset(&input, 0, sizeof(input));
    memset(output, 0, sizeof(output));
    input.cBufferSize = 512;
    input.irDriveRegs.bFeaturesReg = Feature;
    input.irDriveRegs.bSectorCountReg = 1;
    input.irDriveRegs.bSectorNumberReg = 1;
    input.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    input.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    input.irDriveRegs.bDriveHeadReg = 0xA0;
    input.irDriveRegs.bCommandReg = SMART_CMD;

    if (!DeviceIoControl(Handle, SMART_RCV_DRIVE_DATA, &input, sizeof(input) - 1, output, sizeof(output), &returned, NULL))
        return FALSE;
    if (result->DriverStatus.bDriverError)
        return FALSE;

    memcpy(Data, result->bBuffer, 512);
    return TRUE;
}

// One SCSI command through the USB bridge. DataIn: SCSI_IOCTL_DATA_IN / _OUT / _UNSPECIFIED.
static BOOLEAN Scsi(_In_ HANDLE Handle, _In_reads_(CdbLength) PUCHAR Cdb, _In_ UCHAR CdbLength, _In_ UCHAR DataIn,
    _Inout_updates_opt_(Length) PUCHAR Data, _In_ ULONG Length)
{
    typedef struct _SPT_REQUEST
    {
        SCSI_PASS_THROUGH Spt;
        UCHAR Sense[32];
        UCHAR Buffer[512];
    } SPT_REQUEST;
    SPT_REQUEST request;
    DWORD returned = 0;

    if (Length > sizeof(request.Buffer))
        return FALSE;

    memset(&request, 0, sizeof(request));
    request.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    request.Spt.CdbLength = CdbLength;
    request.Spt.SenseInfoLength = sizeof(request.Sense);
    request.Spt.DataIn = DataIn;
    request.Spt.DataTransferLength = Length;
    request.Spt.TimeOutValue = 10;
    request.Spt.DataBufferOffset = FIELD_OFFSET(SPT_REQUEST, Buffer);
    request.Spt.SenseInfoOffset = FIELD_OFFSET(SPT_REQUEST, Sense);
    memcpy(request.Spt.Cdb, Cdb, CdbLength);

    if (DataIn == SCSI_IOCTL_DATA_OUT && Data)
        memcpy(request.Buffer, Data, Length);

    if (!DeviceIoControl(Handle, IOCTL_SCSI_PASS_THROUGH, &request, sizeof(request), &request, sizeof(request), &returned, NULL))
        return FALSE;
    if (request.Spt.ScsiStatus != 0)
        return FALSE;

    if (DataIn == SCSI_IOCTL_DATA_IN && Data)
        memcpy(Data, request.Buffer, Length);

    return TRUE;
}

// SATA disk in a USB enclosure: SAT ATA PASS-THROUGH(16), then (12). Only a sector with a valid checksum counts.
static BOOLEAN ReadSatSmart(_In_ HANDLE Handle, _In_ UCHAR Feature, _Out_writes_(512) PUCHAR Data)
{
    UCHAR cdb16[16] = { 0x85, 0x08, 0x0E, 0, Feature, 0, 1, 0, 0, 0, 0x4F, 0, 0xC2, 0xA0, 0xB0, 0 };
    UCHAR cdb12[12] = { 0xA1, 0x08, 0x0E, Feature, 1, 0, 0x4F, 0xC2, 0xA0, 0xB0, 0, 0 };

    for (ULONG i = 0; i < 2; i++)
    {
        BOOLEAN ok = i == 0 ? Scsi(Handle, cdb16, 16, SCSI_IOCTL_DATA_IN, Data, 512) : Scsi(Handle, cdb12, 12, SCSI_IOCTL_DATA_IN, Data, 512);
        UCHAR sum = 0;

        if (!ok)
            continue;

        for (ULONG b = 0; b < 512; b++)
            sum += Data[b];

        if (sum == 0 && (Data[0] || Data[1]))
            return TRUE;
    }

    return FALSE;
}

// NVMe behind a JMicron JMS583/JMS581. NEVER send to other bridges: there byte 1 = 0x80 means "reset".
static BOOLEAN ReadJmicronNvmeLog(_In_ HANDLE Handle, _Out_writes_(512) PUCHAR Log)
{
    UCHAR cmd[512] = { 0 };
    UCHAR reply[512];
    UCHAR cdb[12] = { 0xA1, 0x80, 0, 0x00, 0x02, 0x00, 0, 0, 0, 0, 0, 0 };

    *(PULONG)&cmd[0] = 0x454D564E;                  // "NVME"
    *(PULONG)&cmd[8] = 0x02;                        // Get Log Page
    *(PULONG)&cmd[12] = 0xFFFFFFFF;                 // whole controller
    *(PULONG)&cmd[48] = 0x02 | ((512 / 4 - 1) << 16); // log 02h, 128 dwords

    if (!Scsi(Handle, cdb, 12, SCSI_IOCTL_DATA_OUT, cmd, 512))
        return FALSE;
    cdb[1] = 0x82;
    if (!Scsi(Handle, cdb, 12, SCSI_IOCTL_DATA_IN, Log, 512))
        return FALSE;
    cdb[1] = 0x8F;
    if (!Scsi(Handle, cdb, 12, SCSI_IOCTL_DATA_IN, reply, 512))
        return FALSE;

    return *(PULONG)&reply[0] == 0x454D564E && (*(PULONG)&reply[20] >> 17) == 0;
}

static BOOLEAN ReadRealtekNvmeLog(_In_ HANDLE Handle, _Out_writes_(512) PUCHAR Log)
{
    UCHAR cdb[16] = { 0xE4, 0x00, 0x02, 0x02, 0x02, 0 };
    return Scsi(Handle, cdb, 16, SCSI_IOCTL_DATA_IN, Log, 512);
}

static BOOLEAN ReadAsmediaNvmeLog(_In_ HANDLE Handle, _Out_writes_(512) PUCHAR Log)
{
    UCHAR cdb[16] = { 0xE6, 0x02, 0, 0x02, 0, 0, 0x00, 0x7F, 0 };
    return Scsi(Handle, cdb, 16, SCSI_IOCTL_DATA_IN, Log, 512);
}

// A bridge can "succeed" with zeros or junk: only a plausible health log is believed.
static BOOLEAN NvmeLogPlausible(_In_reads_(512) PUCHAR Log)
{
    USHORT kelvin = *(PUSHORT)&Log[1];
    BOOLEAN any = FALSE;

    for (ULONG i = 0; i < 512 && !any; i++)
        any = Log[i] != 0;

    return any && kelvin >= 233 && kelvin <= 400 && Log[3] <= 100 && Log[4] <= 100;
}

// "VID:PID" of the USB device the disk hangs off (walks up to 4 parents), or FALSE.
static BOOLEAN GetUsbBridgeId(_In_ ULONG Disk, _Out_ PUSHORT Vid, _Out_ PUSHORT Pid)
{
    HDEVINFO info = SetupDiGetClassDevsW(&DiskInterfaceGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA interfaceData = { sizeof(SP_DEVICE_INTERFACE_DATA) };
    BOOLEAN found = FALSE;

    if (info == INVALID_HANDLE_VALUE)
        return FALSE;

    for (ULONG i = 0; !found && SetupDiEnumDeviceInterfaces(info, NULL, &DiskInterfaceGuid, i, &interfaceData); i++)
    {
        UCHAR detailBuffer[1024];
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)detailBuffer;
        SP_DEVINFO_DATA devinfo = { sizeof(SP_DEVINFO_DATA) };
        STORAGE_DEVICE_NUMBER number;
        DWORD returned;
        HANDLE handle;

        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, sizeof(detailBuffer), NULL, &devinfo))
            continue;

        handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        if (DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &number, sizeof(number), &returned, NULL) &&
            number.DeviceNumber == Disk)
        {
            DEVINST node = devinfo.DevInst;

            for (ULONG level = 0; level < 5 && !found; level++)
            {
                WCHAR id[MAX_DEVICE_ID_LEN];
                unsigned int vid, pid;

                if (CM_Get_Device_IDW(node, id, RTL_NUMBER_OF(id), 0) == CR_SUCCESS &&
                    swscanf_s(id, L"USB\\VID_%4x&PID_%4x", &vid, &pid) == 2)
                {
                    *Vid = (USHORT)vid;
                    *Pid = (USHORT)pid;
                    found = TRUE;
                }
                else if (CM_Get_Parent(&node, node, 0) != CR_SUCCESS)
                {
                    break;
                }
            }

            CloseHandle(handle);
            break;
        }

        CloseHandle(handle);
    }

    SetupDiDestroyDeviceInfoList(info);
    return found;
}

typedef enum _BRIDGE_KIND { BridgeNone, BridgeJmicron, BridgeRealtek, BridgeAsmedia } BRIDGE_KIND;

// smartmontools drivedb.h sntjmicron / sntrealtek / sntasmedia entries (2026-10). MaySat = may hold a SATA disk.
static BRIDGE_KIND LookupBridge(_In_ USHORT Vid, _In_ USHORT Pid, _Out_ PBOOLEAN MaySat)
{
    static CONST struct { USHORT Vid; USHORT Pid; BRIDGE_KIND Kind; BOOLEAN Sat; } table[] =
    {
        { 0x152D, 0x0583, BridgeJmicron, FALSE }, { 0x152D, 0xA583, BridgeJmicron, FALSE }, { 0x30DE, 0x1000, BridgeJmicron, FALSE },
        { 0x152D, 0xB581, BridgeJmicron, TRUE },
        { 0x0BDA, 0x9210, BridgeRealtek, FALSE }, { 0x1DE1, 0xE101, BridgeRealtek, FALSE },
        { 0x0BDA, 0x9211, BridgeRealtek, TRUE }, { 0x2EB9, 0x9211, BridgeRealtek, TRUE }, { 0x0BDA, 0x9220, BridgeRealtek, TRUE },
        { 0x174C, 0x2362, BridgeAsmedia, FALSE }, { 0x174C, 0x2364, BridgeAsmedia, FALSE }, { 0x0411, 0x039D, BridgeAsmedia, FALSE },
        { 0x04E8, 0x4001, BridgeAsmedia, FALSE }, { 0x04E8, 0x61FB, BridgeAsmedia, FALSE }, { 0x04E8, 0x61FD, BridgeAsmedia, FALSE },
        { 0x0781, 0x55AE, BridgeAsmedia, FALSE }, { 0x0B05, 0x1932, BridgeAsmedia, FALSE }, { 0x0BC2, 0xAA1A, BridgeAsmedia, FALSE },
        { 0x1058, 0x266E, BridgeAsmedia, FALSE }, { 0x13FE, 0x6500, BridgeAsmedia, FALSE },
    };

    *MaySat = TRUE;

    for (ULONG i = 0; i < RTL_NUMBER_OF(table); i++)
    {
        if (table[i].Vid == Vid && table[i].Pid == Pid)
        {
            *MaySat = table[i].Sat;
            return table[i].Kind;
        }
    }

    return BridgeNone;
}

// ---- parsing -----------------------------------------------------------------------------------

static PCWSTR AtaName(_In_ UCHAR Id)
{
    switch (Id)
    {
    case 1: return L"Raw Read Error Rate";
    case 2: return L"Throughput Performance";
    case 3: return L"Spin-Up Time";
    case 4: return L"Start/Stop Count";
    case 5: return L"Reallocated Sectors Count";
    case 7: return L"Seek Error Rate";
    case 8: return L"Seek Time Performance";
    case 9: return L"Power-On Hours";
    case 10: return L"Spin Retry Count";
    case 11: return L"Recalibration Retries";
    case 12: return L"Power Cycle Count";
    case 13: return L"Soft Read Error Rate";
    case 160: return L"Uncorrectable Sector Count (vendor)";
    case 161: return L"Valid Spare Blocks (vendor)";
    case 163: return L"Initial Invalid Blocks (vendor)";
    case 164: return L"Total Erase Count (vendor)";
    case 165: return L"Maximum Erase Count (vendor)";
    case 166: return L"Minimum Erase Count (vendor)";
    case 167: return L"Average Erase Count (vendor)";
    case 168: return L"SATA PHY Error Count";
    case 169: return L"Remaining Life (vendor)";
    case 170: return L"Available Reserved Space";
    case 171: return L"Program Fail Count";
    case 172: return L"Erase Fail Count";
    case 173: return L"Wear Leveling Count";
    case 174: return L"Unexpected Power Loss Count";
    case 175: return L"Program Fail Count (Chip)";
    case 176: return L"Erase Fail Count (Chip)";
    case 177: return L"Wear Leveling Count";
    case 178: return L"Used Reserved Block Count (Chip)";
    case 179: return L"Used Reserved Block Count (Total)";
    case 180: return L"Unused Reserved Block Count (Total)";
    case 181: return L"Program Fail Count (Total)";
    case 182: return L"Erase Fail Count (Total)";
    case 183: return L"Runtime Bad Block / SATA Downshift";
    case 184: return L"End-to-End Error";
    case 187: return L"Reported Uncorrectable Errors";
    case 188: return L"Command Timeout";
    case 189: return L"High Fly Writes";
    case 190: return L"Airflow Temperature";
    case 191: return L"G-Sense Error Rate";
    case 192: return L"Power-off Retract Count";
    case 193: return L"Load Cycle Count";
    case 194: return L"Temperature";
    case 195: return L"Hardware ECC Recovered";
    case 196: return L"Reallocation Event Count";
    case 197: return L"Current Pending Sector Count";
    case 198: return L"Uncorrectable Sector Count";
    case 199: return L"UltraDMA CRC Error Count";
    case 200: return L"Write Error Rate";
    case 201: return L"Soft Read Error Rate";
    case 202: return L"Percent Lifetime Remaining (vendor)";
    case 206: return L"Flying Height";
    case 220: return L"Disk Shift";
    case 222: return L"Loaded Hours";
    case 223: return L"Load/Unload Retry Count";
    case 224: return L"Load Friction";
    case 225: return L"Load/Unload Cycle Count";
    case 226: return L"Load-in Time";
    case 230: return L"Drive Life Protection Status (vendor)";
    case 231: return L"SSD Life Left (vendor)";
    case 232: return L"Endurance Remaining (vendor)";
    case 233: return L"Media Wearout Indicator (vendor)";
    case 234: return L"Average Erase Count (vendor)";
    case 235: return L"Good Block Count (vendor)";
    case 240: return L"Head Flying Hours (vendor)";
    case 241: return L"Total LBAs Written";
    case 242: return L"Total LBAs Read";
    case 243: return L"Total LBAs Written Expanded";
    case 244: return L"Total LBAs Read Expanded";
    case 246: return L"Total Host Sector Writes (vendor)";
    case 247: return L"Host Program Page Count (vendor)";
    case 248: return L"Background Program Page Count (vendor)";
    case 249: return L"NAND Writes 1GiB (vendor)";
    case 250: return L"Read Error Retry Rate";
    case 251: return L"Minimum Spares Remaining (vendor)";
    case 252: return L"Newly Added Bad Flash Block (vendor)";
    case 254: return L"Free Fall Protection";
    default: return L"Vendor specific";
    }
}

// Counts that mean the surface / flash is going: any non-zero raw value is a warning.
static PCWSTR AtaWatchName(_In_ UCHAR Id)
{
    switch (Id)
    {
    case 5: return L"reallocated sectors";
    case 197: return L"pending sectors";
    case 198: return L"uncorrectable sectors";
    default: return NULL;
    }
}

static PSMART_ROW AddRow(_Inout_ PDISK_SMART Smart)
{
    PSMART_ROW row;

    if (Smart->RowCount >= SMART_MAX_ROWS)
        return NULL;

    row = &Smart->Rows[Smart->RowCount++];
    memset(row, 0, sizeof(SMART_ROW));
    row->Status = SmartGood;
    return row;
}

static VOID AddNvmeRow(_Inout_ PDISK_SMART Smart, _In_ PCWSTR Name, _In_ SMART_STATUS Status, _In_ PCWSTR Format, ...)
{
    PSMART_ROW row = AddRow(Smart);
    va_list args;

    if (!row)
        return;

    wcsncpy_s(row->Name, RTL_NUMBER_OF(row->Name), Name, _TRUNCATE);
    va_start(args, Format);
    _vsnwprintf_s(row->Raw, RTL_NUMBER_OF(row->Raw), _TRUNCATE, Format, args);
    va_end(args);
    row->Status = Status;
}

static ULONG64 U64(_In_reads_(8) PUCHAR P) { return *(ULONG64 UNALIGNED*)P; }
static ULONG U32(_In_reads_(4) PUCHAR P) { return *(ULONG UNALIGNED*)P; }

static VOID ParseNvmeLog(_In_reads_(512) PUCHAR B, _Inout_ PDISK_SMART Smart)
{
    static CONST struct { UCHAR Bit; PCWSTR Text; SMART_STATUS Status; } flags[] =
    {
        { 0x01, L"available spare below threshold", SmartFail },
        { 0x02, L"temperature outside its limits", SmartWarn },
        { 0x04, L"reliability degraded by media or internal errors", SmartFail },
        { 0x08, L"drive switched to read-only", SmartFail },
        { 0x10, L"volatile memory backup failed", SmartWarn },
        { 0x20, L"persistent memory region read-only", SmartFail },
    };
    ULONG64 unitsRead = U64(&B[32]);
    ULONG64 unitsWritten = U64(&B[48]);

    Smart->Kind = 1;
    Smart->CriticalWarning = B[0];
    Smart->TemperatureC = (LONG)*(PUSHORT)&B[1] - 273;
    Smart->WearPercent = B[5];
    Smart->BytesRead = unitsRead * 512000;      // units of 1000 x 512 bytes
    Smart->BytesWritten = unitsWritten * 512000;
    Smart->PowerCycles = U64(&B[112]);
    Smart->PowerOnHours = U64(&B[128]);
    Smart->MediaErrors = U64(&B[160]);
    Smart->CriticalTemperatureMinutes = U32(&B[196]);

    AddNvmeRow(Smart, L"Critical warning", B[0] ? SmartFail : SmartGood, L"0x%02X", B[0]);
    for (ULONG i = 0; i < RTL_NUMBER_OF(flags); i++)
    {
        if (B[0] & flags[i].Bit)
            AddNvmeRow(Smart, L"  drive reports", flags[i].Status, L"%s", flags[i].Text);
    }
    AddNvmeRow(Smart, L"Temperature", Smart->TemperatureC >= 75 ? SmartFail : Smart->TemperatureC >= 65 ? SmartWarn : SmartGood, L"%ld °C", Smart->TemperatureC);
    AddNvmeRow(Smart, L"Available spare", B[3] < B[4] ? SmartFail : SmartGood, L"%u %% (threshold %u %%)", B[3], B[4]);
    AddNvmeRow(Smart, L"Percentage used", B[5] >= 95 ? SmartFail : B[5] >= 80 ? SmartWarn : SmartGood, L"%u %%", B[5]);
    AddNvmeRow(Smart, L"Data units read", SmartGood, L"%I64u (%.2f TB)", unitsRead, unitsRead * 512000 / 1e12);
    AddNvmeRow(Smart, L"Data units written", SmartGood, L"%I64u (%.2f TB)", unitsWritten, unitsWritten * 512000 / 1e12);
    AddNvmeRow(Smart, L"Host read commands", SmartGood, L"%I64u", U64(&B[64]));
    AddNvmeRow(Smart, L"Host write commands", SmartGood, L"%I64u", U64(&B[80]));
    AddNvmeRow(Smart, L"Controller busy time", SmartGood, L"%I64u min", U64(&B[96]));
    AddNvmeRow(Smart, L"Power cycles", SmartGood, L"%I64u", Smart->PowerCycles);
    AddNvmeRow(Smart, L"Power-on hours", SmartGood, L"%I64u", Smart->PowerOnHours);
    AddNvmeRow(Smart, L"Unsafe shutdowns", SmartGood, L"%I64u", U64(&B[144]));
    AddNvmeRow(Smart, L"Media and data integrity errors", Smart->MediaErrors ? SmartWarn : SmartGood, L"%I64u", Smart->MediaErrors);
    AddNvmeRow(Smart, L"Error log entries", SmartGood, L"%I64u", U64(&B[176]));
    AddNvmeRow(Smart, L"Warning temperature time", SmartGood, L"%lu min", U32(&B[192]));
    AddNvmeRow(Smart, L"Critical temperature time", Smart->CriticalTemperatureMinutes ? SmartWarn : SmartGood, L"%lu min", Smart->CriticalTemperatureMinutes);

    for (ULONG i = 0; i < 8; i++)
    {
        USHORT k = *(PUSHORT)&B[200 + 2 * i];
        WCHAR name[32];

        if (k)
        {
            _snwprintf_s(name, RTL_NUMBER_OF(name), _TRUNCATE, L"Temperature sensor %lu", i + 1);
            AddNvmeRow(Smart, name, SmartGood, L"%ld °C", (LONG)k - 273);
        }
    }

    AddNvmeRow(Smart, L"Thermal throttle 1 (count / time)", SmartGood, L"%lu / %lu s", U32(&B[216]), U32(&B[224]));
    AddNvmeRow(Smart, L"Thermal throttle 2 (count / time)", SmartGood, L"%lu / %lu s", U32(&B[220]), U32(&B[228]));
}

static VOID ParseAtaSmart(_In_reads_(512) PUCHAR A, _In_reads_opt_(512) PUCHAR T, _Inout_ PDISK_SMART Smart)
{
    Smart->Kind = 2;

    for (ULONG i = 0; i < 30; i++)
    {
        PUCHAR attribute = &A[2 + 12 * i];
        UCHAR id = attribute[0];
        UCHAR current = attribute[3];
        UCHAR worst = attribute[4];
        UCHAR threshold = 0;
        ULONG64 raw = 0;
        ULONG low;
        PSMART_ROW row;
        PCWSTR watch;

        if (!id)
            continue;

        if (T)
        {
            for (ULONG j = 0; j < 30; j++)
            {
                if (T[2 + 12 * j] == id)
                {
                    threshold = T[2 + 12 * j + 1];
                    break;
                }
            }
        }

        for (LONG k = 5; k >= 0; k--)
            raw = raw * 256 + attribute[5 + k];
        low = (ULONG)(raw & 0xFFFFFFFF);

        if (!(row = AddRow(Smart)))
            break;

        _snwprintf_s(row->Id, RTL_NUMBER_OF(row->Id), _TRUNCATE, L"%02X", id);
        wcsncpy_s(row->Name, RTL_NUMBER_OF(row->Name), AtaName(id), _TRUNCATE);
        _snwprintf_s(row->Current, RTL_NUMBER_OF(row->Current), _TRUNCATE, L"%u", current);
        _snwprintf_s(row->Worst, RTL_NUMBER_OF(row->Worst), _TRUNCATE, L"%u", worst);
        _snwprintf_s(row->Threshold, RTL_NUMBER_OF(row->Threshold), _TRUNCATE, L"%u", threshold);
        _snwprintf_s(row->Raw, RTL_NUMBER_OF(row->Raw), _TRUNCATE, L"%I64u", raw);

        watch = AtaWatchName(id);
        if (threshold > 0 && current <= threshold)
            row->Status = SmartFail;
        else if (watch && low > 0)
            row->Status = SmartWarn;

        switch (id)
        {
        case 9: Smart->PowerOnHours = low; break;
        case 12: Smart->PowerCycles = low; break;
        case 194:
        case 190:
            if (Smart->TemperatureC == LONG_MIN && (raw & 0xFF) > 0 && (raw & 0xFF) < 125)
                Smart->TemperatureC = (LONG)(raw & 0xFF);
            break;
        case 169: case 202: case 231: case 232: case 233:
            // "life left" style normalised values: 100 = new.
            if (Smart->WearPercent < 0 && current <= 100)
                Smart->WearPercent = 100 - current;
            break;
        case 241: Smart->BytesWritten = raw * 512; break;
        case 242: Smart->BytesRead = raw * 512; break;
        }
    }
}

// Health rules from the remote support agent: the drive's own SMART decides.
VOID SmartEvaluate(
    _In_ PDISK_SMART Smart,
    _Inout_updates_(ProblemsCount) PWSTR Problems,
    _In_ SIZE_T ProblemsCount,
    _Inout_ SMART_STATUS* Status
    )
{
    if (Smart->WearPercent >= 95)
        HealthAddProblem(Problems, ProblemsCount, Status, SmartFail, L"wear %ld%%", Smart->WearPercent);
    else if (Smart->WearPercent >= 80)
        HealthAddProblem(Problems, ProblemsCount, Status, SmartWarn, L"wear %ld%%", Smart->WearPercent);

    if (Smart->TemperatureC != LONG_MIN)
    {
        if (Smart->TemperatureC >= 75)
            HealthAddProblem(Problems, ProblemsCount, Status, SmartFail, L"temperature %ld °C", Smart->TemperatureC);
        else if (Smart->TemperatureC >= 65)
            HealthAddProblem(Problems, ProblemsCount, Status, SmartWarn, L"temperature %ld °C", Smart->TemperatureC);
    }

    if (Smart->Kind == 1)
    {
        for (ULONG i = 0; i < Smart->RowCount; i++)
        {
            if (wcscmp(Smart->Rows[i].Name, L"  drive reports") == 0)
                HealthAddProblem(Problems, ProblemsCount, Status, Smart->Rows[i].Status, L"drive reports %s", Smart->Rows[i].Raw);
        }

        if (Smart->MediaErrors > 0)
            HealthAddProblem(Problems, ProblemsCount, Status, SmartWarn, L"%I64u media errors", Smart->MediaErrors);
        if (Smart->CriticalTemperatureMinutes > 0)
            HealthAddProblem(Problems, ProblemsCount, Status, SmartWarn, L"%lu min above critical temperature", Smart->CriticalTemperatureMinutes);
    }
    else if (Smart->Kind == 2)
    {
        for (ULONG i = 0; i < Smart->RowCount; i++)
        {
            PSMART_ROW row = &Smart->Rows[i];
            ULONG id = wcstoul(row->Id, NULL, 16);
            PCWSTR watch = AtaWatchName((UCHAR)id);

            if (row->Status == SmartFail)
                HealthAddProblem(Problems, ProblemsCount, Status, SmartFail, L"%s at failure threshold", row->Name);
            else if (row->Status == SmartWarn && watch)
                HealthAddProblem(Problems, ProblemsCount, Status, SmartWarn, L"%s %s", row->Raw, watch);
        }
    }
}

// ---- entry point -------------------------------------------------------------------------------

VOID SmartRead(
    _In_ ULONG DiskNumber,
    _In_ STORAGE_BUS_TYPE BusType,
    _Out_ PDISK_SMART Smart
    )
{
    UCHAR data[512];
    UCHAR thresholds[512];
    HANDLE handle;

    memset(Smart, 0, sizeof(DISK_SMART));
    Smart->TemperatureC = LONG_MIN;
    Smart->PowerOnHours = ULLONG_MAX;
    Smart->PowerCycles = ULLONG_MAX;
    Smart->WearPercent = -1;
    Smart->BytesWritten = ULLONG_MAX;
    Smart->BytesRead = ULLONG_MAX;

    if (BusType == BusTypeUsb)
    {
        USHORT vid = 0, pid = 0;
        BOOLEAN maySat = TRUE;
        BRIDGE_KIND bridge = BridgeNone;
        BOOLEAN haveId = GetUsbBridgeId(DiskNumber, &vid, &pid);

        if (haveId)
            bridge = LookupBridge(vid, pid, &maySat);

        handle = OpenDisk(DiskNumber, GENERIC_READ | GENERIC_WRITE);

        if (handle == INVALID_HANDLE_VALUE)
        {
            SetText(Smart->Note, RTL_NUMBER_OF(Smart->Note), L"cannot open the disk (error %lu)", GetLastError());
            return;
        }

        if (bridge != BridgeNone)
        {
            BOOLEAN ok = bridge == BridgeJmicron ? ReadJmicronNvmeLog(handle, data) :
                bridge == BridgeRealtek ? ReadRealtekNvmeLog(handle, data) : ReadAsmediaNvmeLog(handle, data);

            if (ok && NvmeLogPlausible(data))
            {
                ParseNvmeLog(data, Smart);
                SetText(Smart->Source, RTL_NUMBER_OF(Smart->Source), L"NVMe health log through USB bridge %04X:%04X", vid, pid);
                CloseHandle(handle);
                return;
            }
        }

        if (maySat && ReadSatSmart(handle, 0xD0, data))
        {
            BOOLEAN haveThresholds = ReadSatSmart(handle, 0xD1, thresholds);

            ParseAtaSmart(data, haveThresholds ? thresholds : NULL, Smart);
            SetText(Smart->Source, RTL_NUMBER_OF(Smart->Source), L"ATA SMART through USB bridge %04X:%04X (SAT)", vid, pid);
            CloseHandle(handle);
            return;
        }

        CloseHandle(handle);

        if (haveId)
            SetText(Smart->Note, RTL_NUMBER_OF(Smart->Note), L"SMART not exposed through USB bridge %04X:%04X", vid, pid);
        else
            SetText(Smart->Note, RTL_NUMBER_OF(Smart->Note), L"SMART not exposed through the USB bridge");
        return;
    }

    if (BusType == BusTypeNvme || BusType == BusTypeRAID)
    {
        if (ReadNvmeHealthLog(DiskNumber, data))
        {
            ParseNvmeLog(data, Smart);
            SetText(Smart->Source, RTL_NUMBER_OF(Smart->Source), L"NVMe SMART / Health log");
            return;
        }

        if (BusType == BusTypeNvme)
        {
            SetText(Smart->Note, RTL_NUMBER_OF(Smart->Note), L"could not read the NVMe health log (error %lu)", GetLastError());
            return;
        }
    }

    if (BusType == BusTypeSata || BusType == BusTypeAta || BusType == BusTypeRAID)
    {
        handle = OpenDisk(DiskNumber, GENERIC_READ | GENERIC_WRITE);

        if (handle != INVALID_HANDLE_VALUE)
        {
            if (ReadAtaSmart(handle, 0xD0, data))
            {
                BOOLEAN haveThresholds = ReadAtaSmart(handle, 0xD1, thresholds);

                ParseAtaSmart(data, haveThresholds ? thresholds : NULL, Smart);
                SetText(Smart->Source, RTL_NUMBER_OF(Smart->Source), L"ATA SMART attributes");
                CloseHandle(handle);
                return;
            }

            CloseHandle(handle);
        }

        SetText(Smart->Note, RTL_NUMBER_OF(Smart->Note), L"could not read ATA SMART (error %lu)", GetLastError());
        return;
    }

    if (!Smart->Note[0])
        SetText(Smart->Note, RTL_NUMBER_OF(Smart->Note), L"SMART not read on this bus type");
}
