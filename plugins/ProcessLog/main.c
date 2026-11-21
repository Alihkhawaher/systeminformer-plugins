/*
 * ProcessLog - System Informer plugin: a log of every process that ran (start, exit, duration, parent, command
 * line, user, exit code), including processes too short-lived for System Informer's 1 s refresh to see.
 * Tools > Process log opens the list.
 */

#include "processlog.h"

#define MENU_ID_SHOW_LOG 1

PPH_PLUGIN PluginInstance = NULL;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainMenuInitializingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Parameter);
    UNREFERENCED_PARAMETER(Context);

    PlStartCapture();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Parameter);
    UNREFERENCED_PARAMETER(Context);

    PlStopCapture();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;

    UNREFERENCED_PARAMETER(Context);

    if (!menuInfo || menuInfo->u.MainMenu.SubMenuIndex != PH_MENU_ITEM_LOCATION_TOOLS)
        return;

    PhInsertEMenuItem(menuInfo->Menu, PhPluginCreateEMenuItem(PluginInstance, 0, MENU_ID_SHOW_LOG, L"Process &log", NULL), ULONG_MAX);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MenuItemCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;

    UNREFERENCED_PARAMETER(Context);

    if (menuItem && menuItem->Id == MENU_ID_SHOW_LOG)
        PlShowWindow();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    static ULONG count = 0;

    UNREFERENCED_PARAMETER(Parameter);
    UNREFERENCED_PARAMETER(Context);

    if (++count % 10 == 0) // status file for checking the capture without opening the window
        PlWriteStatusFile();
}

LOGICAL DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _Reserved_ PVOID Reserved
    )
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
    {
        PPH_PLUGIN_INFORMATION info;
        PH_SETTING_CREATE settings[] =
        {
            { IntegerPairSettingType, SETTING_NAME_WINDOW_POSITION, L"0,0" },
            { ScalableIntegerPairSettingType, SETTING_NAME_WINDOW_SIZE, L"@96|1100,600" },
            { StringSettingType, SETTING_NAME_COLUMNS, L"" },
            { IntegerSettingType, SETTING_NAME_MAX_FILE_MB, L"20" },     // 32 MB
            { IntegerSettingType, SETTING_NAME_MAX_ENTRIES, L"c350" },   // 50000
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Process log";
        info->Description = L"Logs every process start and exit from ETW kernel events, including processes that live only milliseconds. Tools > Process log.";
        info->Author = L"Alihkhawaher";
        info->Url = L"https://github.com/Alihkhawaher/systeminformer-plugins";

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackUnload), UnloadCallback, NULL, &PluginUnloadCallbackRegistration);
        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackMenuItem), MenuItemCallback, NULL, &PluginMenuItemCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackMainMenuInitializing), MainMenuInitializingCallback, NULL, &MainMenuInitializingCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackProcessProviderUpdatedEvent), ProcessesUpdatedCallback, NULL, &ProcessesUpdatedCallbackRegistration);

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
