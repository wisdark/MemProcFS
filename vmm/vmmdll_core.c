// vmmdll_core.c : implementation of core library functionality which mainly
//      consists of library initialization and cleanup/close functionality.
//
// (c) Ulf Frisk, 2022
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "vmm.h"
#include "vmmdll.h"
#include "vmmlog.h"
#include "vmmproc.h"
#include "vmmwork.h"
#include "ob/ob.h"
#include "ob/ob_tag.h"
#include "charutil.h"
#include "util.h"
#include "fc.h"
#include "statistics.h"
#include "version.h"

//-----------------------------------------------------------------------------
// INITIALIZATION AND CLOSE FUNCTIONALITY BELOW:
// 
// Initialize and Close functionality is put behind a single shared global lock.
//-----------------------------------------------------------------------------

// globals below:
#define VMM_HANDLE_MAX_COUNT            32
static POB_MAP g_VMMDLL_ALLOCMAP_EXT    = NULL;
static SRWLOCK g_VMMDLL_CORE_LOCK_SRW   = SRWLOCK_INIT;
static POB_MAP g_VMMDLL_CORE_ALLHANDLE  = NULL;
static DWORD g_VMMDLL_CORE_HANDLE_COUNT = 0;
static VMM_HANDLE g_VMMDLL_CORE_HANDLES[VMM_HANDLE_MAX_COUNT] = { 0 };

// forward declarations below:
VOID VmmDllCore_MemLeakFindExternal(_In_ VMM_HANDLE H);

/*
* Verify that the supplied handle is valid and also check it out.
* This must be called by each external access which requires a VMM_HANDLE.
* Each successful VmmDllCore_HandleReserveExternal() call must be matched by
* a matched call to VmmDllCore_HandleReturnExternal() after completion.
* -- H
* -- return
*/
_Success_(return)
BOOL VmmDllCore_HandleReserveExternal(_In_opt_ VMM_HANDLE H)
{
    DWORD i = 0;
    BOOL fResult = FALSE;
    if(!H || ((SIZE_T)H < 0x10000)) { return FALSE;}
    AcquireSRWLockShared(&g_VMMDLL_CORE_LOCK_SRW);
    for(i = 0; i < g_VMMDLL_CORE_HANDLE_COUNT; i++) {
        if(g_VMMDLL_CORE_HANDLES[i] == H) {
            InterlockedIncrement(&H->cThreadExternal);
            fResult = (H->magic == VMM_MAGIC) && !H->fAbort;
            break;
        }
    }
    ReleaseSRWLockShared(&g_VMMDLL_CORE_LOCK_SRW);
    return fResult;
}

/*
* Return a handle successfully reserved with a previous call to the function:
* VmmDllCore_HandleReserveExternal()
* -- H
*/
VOID VmmDllCore_HandleReturnExternal(_In_ VMM_HANDLE H)
{
    InterlockedDecrement(&H->cThreadExternal);
}

/*
* Remove a handle from the external handle array.
* NB! Function is to be called behind exclusive lock g_VMMDLL_CORE_LOCK_SRW.
* -- H
*/
VOID VmmDllCore_HandleRemove(_In_ VMM_HANDLE H)
{
    DWORD i;
    if(H && (H->magic == VMM_MAGIC)) {
        for(i = 0; i < g_VMMDLL_CORE_HANDLE_COUNT; i++) {
            if(g_VMMDLL_CORE_HANDLES[i] == H) {
                g_VMMDLL_CORE_HANDLE_COUNT--;
                if(i < g_VMMDLL_CORE_HANDLE_COUNT) {
                    g_VMMDLL_CORE_HANDLES[i] = g_VMMDLL_CORE_HANDLES[g_VMMDLL_CORE_HANDLE_COUNT];
                    g_VMMDLL_CORE_HANDLES[g_VMMDLL_CORE_HANDLE_COUNT] = NULL;
                } else {
                    g_VMMDLL_CORE_HANDLES[i] = NULL;
                }
                break;
            }
        }
    }
}

/*
* Add a new handle to the external handle array.
* NB! Function is to be called behind exclusive lock g_VMMDLL_CORE_LOCK_SRW.
* -- H
*/
_Success_(return)
BOOL VmmDllCore_HandleAdd(_In_ VMM_HANDLE H)
{
    if(g_VMMDLL_CORE_HANDLE_COUNT < VMM_HANDLE_MAX_COUNT) {
        g_VMMDLL_CORE_HANDLES[g_VMMDLL_CORE_HANDLE_COUNT] = H;
        g_VMMDLL_CORE_HANDLE_COUNT++;
        return TRUE;
    }
    return FALSE;
}

/*
* Close a VMM_HANDLE and clean up everything! The VMM_HANDLE will not be valid
* after this function has been called. Function call may take some time since
* it's dependent on thread-stoppage (which may take time) to do a clean cleanup.
* The strategy is:
*   (1) disable external calls (set magic and abort flag)
*   (2) wait for worker threads to exit (done on abort) when completed no
*       threads except this one should access the handle.
*   (3) shut down Forensic > Vmm > LeechCore > Threading > Log
* NB! Function is to be called behind exclusive lock g_VMMDLL_CORE_LOCK_SRW.
* -- H = a VMM_HANDLE fully or partially initialized
*/
VOID VmmDllCore_CloseHandle(_In_opt_ _Post_ptr_invalid_ VMM_HANDLE H)
{
    QWORD tc, tcStart;
    if(H) {
        // 1: Remove handle from external allow-list. This will stop external
        //    API calls using the handle.
        VmmDllCore_HandleRemove(H);
        // 2: Set the abort flag. This will cause internal threading shutdown.
        H->fAbort = TRUE;
        H->magic = 0;
        // 3: Abort work multithreading & forensic database queries (to speed up termination)
        VmmWork_Interrupt(H);
        FcInterrupt(H);
        // 4: Wait for multi-threading to shut down
        tcStart = GetTickCount64();
        while(H->cThreadExternal) {
            tc = GetTickCount64();
            if((tc - tcStart) > 30000) {
                tcStart = GetTickCount64();
                VmmLog(H, MID_CORE, LOGLEVEL_1_CRITICAL, "Shutdown waiting for long running external thread (%i).", H->cThreadExternal);
                VmmWork_Interrupt(H);
                FcInterrupt(H);
            }
            SwitchToThread();
        }
        while(H->cThreadInternal) {
            tc = GetTickCount64();
            if((tc - tcStart) > 30000) {
                tcStart = GetTickCount64();
                VmmLog(H, MID_CORE, LOGLEVEL_1_CRITICAL, "Shutdown waiting for long running internal thread (%i).", H->cThreadExternal);
                VmmWork_Interrupt(H);
                FcInterrupt(H);
            }
            SwitchToThread();
        }
        // 5: Close forensic sub-system.
        FcClose(H);
        // Close vmm sub-system.
        VmmClose(H);
        // Close leechcore
        LcClose(H->hLC);
        // Close work (multi-threading)
        VmmWork_Close(H);
        // Warn external (api-user) memory leaks
        VmmDllCore_MemLeakFindExternal(H);
        // Close logging (last)
        Statistics_CallSetEnabled(H, FALSE);
        VmmLog_Close(H);
        LocalFree(H);
    }
}

/*
* Close a VMM_HANDLE and clean up everything! The VMM_HANDLE will not be valid
* after this function has been called.
* -- H
*/
VOID VmmDllCore_Close(_In_opt_ _Post_ptr_invalid_ VMM_HANDLE H)
{
    AcquireSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
    VmmDllCore_CloseHandle(H);
    ReleaseSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
}

/*
* Close all VMM_HANDLE and clean up everything! No VMM_HANDLE will be valid
* after this function has been called.
*/
VOID VmmDllCore_CloseAll()
{
    VMM_HANDLE H;
    AcquireSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
    while((H = g_VMMDLL_CORE_HANDLES[0])) {
        VmmDllCore_CloseHandle(H);
    }
    ReleaseSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
}

/*
* Print the help. This requires a partially initialized VMM_HANDLE.
* -- H
*/
VOID VmmDllCore_PrintHelp(_In_ VMM_HANDLE H)
{
    vmmprintf(H,
        "                                                                               \n" \
        " THE MEMORY PROCESS FILE SYSTEM v%i.%i.%i COMMAND LINE REFERENCE:              \n" \
        " The Memory Process File System may be used in stand-alone mode with support   \n" \
        " for memory dump files, local memory via rekall winpmem driver or together with\n" \
        " PCILeech if pcileech.dll is placed in the application directory. For infor-   \n" \
        " mation about PCILeech please consult the separate PCILeech documentation.     \n" \
        " -----                                                                         \n" \
        " The Memory Process File System (c) 2018-2021 Ulf Frisk                        \n" \
        " License: GNU Affero General Public License v3.0                               \n" \
        " Contact information: pcileech@frizk.net                                       \n" \
        " The Memory Process File System: https://github.com/ufrisk/MemProcFS           \n" \
        " LeechCore:                      https://github.com/ufrisk/LeechCore           \n" \
        " PCILeech:                       https://github.com/ufrisk/pcileech            \n" \
        " -----                                                                         \n" \
        " The recommended way to use the Memory Process File System is to specify the   \n" \
        " memory acquisition device in the -device option and possibly more options.    \n" \
        " Example 1: MemProcFS.exe -device c:\\temp\\memdump-win10x64.pmem              \n" \
        " Example 2: MemProcFS.exe -device c:\\temp\\memdump-winXPx86.dumpit -v -vv     \n" \
        " Example 3: MemProcFS.exe -device FPGA                                         \n" \
        " Example 4: MemProcFS.exe -device PMEM://c:\\temp\\winpmem_x64.sys             \n" \
        " The Memory Process File System may also be started the memory dump file name  \n" \
        " as the only option. This allows to make file extensions associated so that    \n" \
        " they may be opened by double-clicking on them. This mode allows no options.   \n" \
        " Example 4: MemProcFS.exe c:\\dumps\\memdump-win7x64.dumpit                    \n" \
        " -----                                                                         \n" \
        " Valid options:                                                                \n" \
        "   -device : select memory acquisition device or memory dump file to use.      \n" \
        "          Valid options: <any device supported by the leechcore library>       \n" \
        "          such as, but not limited to: <memory_dump_file>, PMEM, FPGA          \n" \
        "          ---                                                                  \n" \
        "          <memory_dump_file> = memory dump file name optionally including path.\n" \
        "          PMEM = use winpmem 'winpmem_64.sys' to acquire live memory.          \n" \
        "          PMEM://c:\\path\\to\\winpmem_64.sys = path to winpmem driver.        \n" \
        "          ---                                                                  \n" \
        "          Please see https://github.com/ufrisk/LeechCore for additional info.  \n" \
        "   -remote : connect to a remote host running the LeechAgent. Please see the   \n" \
        "          LeechCore documentation for more information.                        \n" \
        "   -v   : verbose option. Additional information is displayed in the output.   \n" \
        "          Option has no value. Example: -v                                     \n" \
        "   -vv  : extra verbose option. More detailed additional information is shown  \n" \
        "          in output. Option has no value. Example: -vv                         \n" \
        "   -vvv : super verbose option. Show all data transferred such as PCIe TLPs.   \n" \
        "          Option has no value. Example: -vvv                                   \n" \
        "   -logfile : specify an optional log file.                                    \n" \
        "   -loglevel : specify the log verbosity level as a comma-separated list.      \n" \
        "          Please consult https://github.com/ufrisk/MemProcFS/wiki for details. \n" \
        "          example: -loglevel 4,f:5,f:VMM:6                                     \n" \
        "   -cr3 : base address of kernel/process page table (PML4) / CR3 CPU register. \n" \
        "   -max : memory max address, valid range: 0x0 .. 0xffffffffffffffff           \n" \
        "          default: auto-detect (max supported by device / target system).      \n" \
        "   -memmap-str : specify a physical memory map in parameter agrument text.     \n" \
        "   -memmap : specify a physical memory map given in a file or specify 'auto'.  \n" \
        "          example: -memmap c:\\temp\\my_custom_memory_map.txt                  \n" \
        "          example: -memmap auto                                                \n" \
        "   -pagefile0..9 : specify page file / swap file. By default pagefile have     \n" \
        "          index 0 - example: -pagefile0 pagefile.sys while swapfile have       \n" \
        "          index 1 - example: -pagefile1 swapfile.sys                           \n" \
        "   -pythonpath : specify the path to a python 3 installation for Windows.      \n" \
        "          The path given should be to the directory that contain: python.dll   \n" \
        "          Example: -pythonpath \"C:\\Program Files\\Python37\"                 \n" \
        "   -disable-python : prevent/disable the python plugin sub-system from loading.\n" \
        "          Example: -disable-python                                             \n" \
        "   -disable-symbolserver : disable any integrations with the Microsoft Symbol  \n" \
        "          Server used by the debugging .pdb symbol subsystem. Functionality    \n" \
        "          will be limited if this is activated. Example: -disable-symbolserver \n" \
        "   -disable-symbols : disable symbol lookups from .pdb files.                  \n" \
        "          Example: -disable-symbols                                            \n" \
        "   -disable-infodb : disable the infodb and any symbol lookups via it.         \n" \
        "          Example: -disable-infodb                                             \n" \
        "   -mount : drive letter/path to mount The Memory Process File system at.      \n" \
        "          default: M   Example: -mount Q                                       \n" \
        "   -norefresh : disable automatic cache and processes refreshes even when      \n" \
        "          running against a live memory target - such as PCIe FPGA or live     \n" \
        "          driver acquired memory. This is not recommended. Example: -norefresh \n" \
        "   -waitinitialize : wait debugging .pdb symbol subsystem to fully start before\n" \
        "          mounting file system and fully starting MemProcFS.                   \n" \
        "   -userinteract = allow vmm.dll to, on the console, query the user for        \n" \
        "          information such as, but not limited to, leechcore device options.   \n" \
        "          Default: user interaction = disabled.                                \n" \
        "   -forensic : start a forensic scan of the physical memory immediately after  \n" \
        "          startup if possible. Allowed parameter values range from 0-4.        \n" \
        "          Note! forensic mode is not available for live memory.                \n" \
        "          0 = not enabled (default value)                                      \n" \
        "          1 = forensic mode with in-memory sqlite database.                    \n" \
        "          2 = forensic mode with temp sqlite database deleted upon exit.       \n" \
        "          3 = forensic mode with temp sqlite database remaining upon exit.     \n" \
        "          4 = forensic mode with static named sqlite database (vmm.sqlite3).   \n" \
        "          default: 0  Example -forensic 4                                      \n" \
        "                                                                               \n",
        VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION
    );
}

/*
* Initialize command line config settings in H->cfg and H->dev.
* Upon failure the VMM_HANDLE will be partially intiialized. This is important
* since the '-printf' command line option is required to print info on-screen.
* It's recommended to put the '-printf' option as the first argument!
* -- H = a cleared fresh VMM_HANDLE not yet fully initialized.
* -- argc
* -- argv
* -- return
*/
_Success_(return)
BOOL VmmDllCore_InitializeConfig(_In_ VMM_HANDLE H, _In_ DWORD argc, _In_ char *argv[])
{
    char *argv2[3];
    DWORD i = 0, iPageFile;
    if((argc == 2) && argv[1][0] && (argv[1][0] != '-')) {
        // click to open -> only 1 argument ...
        argv2[0] = argv[0];
        argv2[1] = "-device";
        argv2[2] = argv[1];
        return VmmDllCore_InitializeConfig(H, 3, argv2);
    }
    while(i < argc) {
        if(0 == _stricmp(argv[i], "")) {
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-printf")) {
            H->cfg.fVerboseDll = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-userinteract")) {
            H->cfg.fUserInteract = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-v")) {
            H->cfg.fVerbose = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-vv")) {
            H->cfg.fVerboseExtra = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-vvv")) {
            H->cfg.fVerboseExtraTlp = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-disable-symbolserver")) {
            H->cfg.fDisableSymbolServerOnStartup = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-disable-symbols")) {
            H->cfg.fDisableSymbolServerOnStartup = TRUE;
            H->cfg.fDisableSymbols = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-disable-infodb")) {
            H->cfg.fDisableInfoDB = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-disable-python")) {
            H->cfg.fDisablePython = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-norefresh")) {
            H->cfg.fDisableBackgroundRefresh = TRUE;
            i++;
            continue;
        } else if(0 == _stricmp(argv[i], "-waitinitialize")) {
            H->cfg.fWaitInitialize = TRUE;
            i++;
            continue;
        } else if(i + 1 >= argc) {
            return FALSE;
        } else if(0 == _stricmp(argv[i], "-cr3")) {
            H->cfg.paCR3 = Util_GetNumericA(argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-forensic")) {
            H->cfg.tpForensicMode = (DWORD)Util_GetNumericA(argv[i + 1]);
            if(H->cfg.tpForensicMode > FC_DATABASE_TYPE_MAX) { return FALSE; }
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-max")) {
            H->dev.paMax = Util_GetNumericA(argv[i + 1]);
            i += 2;
            continue;
        } else if((0 == _stricmp(argv[i], "-device")) || (0 == strcmp(argv[i], "-z"))) {
            strcpy_s(H->dev.szDevice, MAX_PATH, argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-remote")) {
            strcpy_s(H->dev.szRemote, MAX_PATH, argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-memmap")) {
            strcpy_s(H->cfg.szMemMap, MAX_PATH, argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-memmap-str")) {
            strcpy_s(H->cfg.szMemMapStr, _countof(H->cfg.szMemMapStr), argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-pythonpath")) {
            strcpy_s(H->cfg.szPythonPath, MAX_PATH, argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-mount")) {
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-logfile")) {
            strcpy_s(H->cfg.szLogFile, MAX_PATH, argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _stricmp(argv[i], "-loglevel")) {
            strcpy_s(H->cfg.szLogLevel, MAX_PATH, argv[i + 1]);
            i += 2;
            continue;
        } else if(0 == _strnicmp(argv[i], "-pagefile", 9)) {
            iPageFile = argv[i][9] - '0';
            if(iPageFile < 10) {
                strcpy_s(H->cfg.szPageFile[iPageFile], MAX_PATH, argv[i + 1]);
            }
            i += 2;
            continue;
        } else {
            return FALSE;
        }
    }
    if(H->dev.paMax && (H->dev.paMax < 0x00100000)) { return FALSE; }
    if(!H->dev.paMax && (H->cfg.szMemMap[0] || H->cfg.szMemMapStr[0])) {
        // disable memory auto-detect when memmap is specified
        H->dev.paMax = -1;
    }
    H->cfg.fFileInfoHeader = TRUE;
    H->cfg.fVerbose = H->cfg.fVerbose && H->cfg.fVerboseDll;
    H->cfg.fVerboseExtra = H->cfg.fVerboseExtra && H->cfg.fVerboseDll;
    H->cfg.fVerboseExtraTlp = H->cfg.fVerboseExtraTlp && H->cfg.fVerboseDll;
    H->dev.dwVersion = LC_CONFIG_VERSION;
    H->dev.dwPrintfVerbosity |= H->cfg.fVerboseDll ? LC_CONFIG_PRINTF_ENABLED : 0;
    H->dev.dwPrintfVerbosity |= H->cfg.fVerbose ? LC_CONFIG_PRINTF_V : 0;
    H->dev.dwPrintfVerbosity |= H->cfg.fVerboseExtra ? LC_CONFIG_PRINTF_VV : 0;
    H->dev.dwPrintfVerbosity |= H->cfg.fVerboseExtraTlp ? LC_CONFIG_PRINTF_VVV : 0;
    return (H->dev.szDevice[0] != 0);
}

/*
* Initialize memory map auto - i.e. retrieve it from the registry and load it into LeechCore.
* -- H
* -- return
*/
_Success_(return)
BOOL VmmDllCore_InitializeMemMapAuto(_In_ VMM_HANDLE H)
{
    BOOL fResult = FALSE;
    DWORD i, cbMemMap = 0;
    LPSTR szMemMap = NULL;
    PVMMOB_MAP_PHYSMEM pObMap = NULL;
    if(!VmmMap_GetPhysMem(H, &pObMap)) { goto fail; }
    if(!(szMemMap = LocalAlloc(LMEM_ZEROINIT, 0x01000000))) { goto fail; }
    for(i = 0; i < pObMap->cMap; i++) {
        cbMemMap += snprintf(szMemMap + cbMemMap, 0x01000000 - cbMemMap - 1, "%016llx %016llx\n", pObMap->pMap[i].pa, pObMap->pMap[i].pa + pObMap->pMap[i].cb - 1);
    }
    fResult =
        LcCommand(H->hLC, LC_CMD_MEMMAP_SET, cbMemMap, (PBYTE)szMemMap, NULL, NULL) &&
        LcGetOption(H->hLC, LC_OPT_CORE_ADDR_MAX, &H->dev.paMax);
fail:
    Ob_DECREF(pObMap);
    LocalFree(szMemMap);
    return fResult;
}

#ifdef _WIN32

/*
* Request user input. This is done upon a request from LeechCore. User input is
* only requested in interactive user contexts.
* -- H = partially initialized VMM_HANDLE.
* -- argc
* -- argv
* -- return
*/
_Success_(return != NULL)
VMM_HANDLE VmmDllCore_InitializeRequestUserInput(_In_ _Post_ptr_invalid_ VMM_HANDLE H, _In_ DWORD argc, _In_ LPSTR argv[])
{
    LPSTR szProto;
    DWORD i, cbRead = 0;
    CHAR szInput[33] = { 0 };
    CHAR szDevice[MAX_PATH] = { 0 };
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);     // must not be closed.
    // 1: read input
    vmmprintf(H, "\n?> ");
    ReadConsoleA(hStdIn, szInput, 32, &cbRead, NULL);
    for(i = 0; i < _countof(szInput); i++) {
        if((szInput[i] == '\r') || (szInput[i] == '\n')) { szInput[i] = 0; }
    }
    cbRead = (DWORD)strlen(szInput);
    if(!cbRead) { return NULL; }
    // 2: clear "userinput" option and update "device" option
    for(i = 0; i < argc; i++) {
        if(0 == _stricmp(argv[i], "-userinteract")) {
            argv[i] = "";
        }
        if((i + 1 < argc) && ((0 == _stricmp(argv[i], "-device")) || (0 == strcmp(argv[i], "-z")))) {
            szProto = strstr(argv[i + 1], "://");
            snprintf(
                szDevice,
                MAX_PATH - 1,
                "%s%s%sid=%s",
                argv[i + 1],
                szProto ? "" : "://",
                szProto && szProto[3] ? "," : "",
                szInput);
            argv[i + 1] = szDevice;
        }
    }
    // 3: try re-initialize with new user input.
    //    (and close earlier partially initialized handle).
    AcquireSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
    VmmDllCore_CloseHandle(H);
    ReleaseSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
    return VMMDLL_InitializeEx(argc, argv, NULL);
}

#endif /* _WIN32 */

/*
* Initialize MemProcFS from user parameters. Upon success a VMM_HANDLE is returned.
* The returned VMM_HANDLE will not yet be in any required external info maps.
* -- argc
* -- argv
* -- ppLcErrorInfo
* -- return
*/
_Success_(return != NULL)
VMM_HANDLE VmmDllCore_Initialize(_In_ DWORD argc, _In_ LPSTR argv[], _Out_opt_ PPLC_CONFIG_ERRORINFO ppLcErrorInfo)
{
    VMM_HANDLE H = NULL;
    FILE *hFile = NULL;
    BOOL f;
    DWORD cbMemMap = 0;
    PBYTE pbMemMap = NULL;
    PLC_CONFIG_ERRORINFO pLcErrorInfo = NULL;
    LPSTR uszUserText;
    BYTE pbBuffer[3 * MAX_PATH];
    AcquireSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
    if(ppLcErrorInfo) { *ppLcErrorInfo = NULL; }
    if(!g_VMMDLL_ALLOCMAP_EXT) {
        // allocate a shared global alloc map once at 1st init. (ok to "leak" this).
        if(!(g_VMMDLL_ALLOCMAP_EXT = ObMap_New(NULL, OB_MAP_FLAGS_OBJECT_OB))) { goto fail; }
    }
    if(!(H = LocalAlloc(LMEM_ZEROINIT, sizeof(struct tdVMM_HANDLE)))) { goto fail; }
    H->magic = VMM_MAGIC;
    // 1: initialize configuration from command line
    //    after config initialization call vmmprintf should work regardless of
    //    success/fail.
    if(!VmmDllCore_InitializeConfig(H, (DWORD)argc, argv)) {
        VmmDllCore_PrintHelp(H);
        goto fail;
    }
    // 9: upon success add handle to external allow-list.
    if(!VmmDllCore_HandleAdd(H)) {
        vmmprintf(H, "MemProcFS: Failed to add handle to external allow-list (max %i concurrent tasks allowed).\n", g_VMMDLL_CORE_HANDLE_COUNT);
        goto fail;
    }
    // 2: initialize LeechCore memory acquisition device
    if(!(H->hLC = LcCreateEx(&H->dev, &pLcErrorInfo))) {
#ifdef _WIN32
        if(pLcErrorInfo && (pLcErrorInfo->dwVersion == LC_CONFIG_ERRORINFO_VERSION)) {
            if(pLcErrorInfo->cwszUserText && CharUtil_WtoU(pLcErrorInfo->wszUserText, -1, pbBuffer, sizeof(pbBuffer), &uszUserText, NULL, 0)) {
                vmmprintf(H, "MESSAGE FROM MEMORY ACQUISITION DEVICE:\n=======================================\n%s\n", uszUserText);
            }
            if(H->cfg.fUserInteract && pLcErrorInfo->fUserInputRequest) {
                LcMemFree(pLcErrorInfo);
                // the request user input function will force a re-initialization upon
                // success and free/discard the earlier partially initialized handle.
                ReleaseSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
                return VmmDllCore_InitializeRequestUserInput(H, argc, argv);
            }
        }
#endif /* _WIN32 */
        vmmprintf(H, "MemProcFS: Failed to connect to memory acquisition device.\n");
        goto fail;
    }
    // 3: initialize/(refresh) the logging sub-system
    VmmLog_LevelRefresh(H);
    // 4: Set LeechCore MemMap (if exists and not auto - i.e. from file)
    if(H->cfg.szMemMap[0] && _stricmp(H->cfg.szMemMap, "auto")) {
        f = (pbMemMap = LocalAlloc(LMEM_ZEROINIT, 0x01000000)) &&
            !fopen_s(&hFile, H->cfg.szMemMap, "rb") && hFile &&
            (cbMemMap = (DWORD)fread(pbMemMap, 1, 0x01000000, hFile)) && (cbMemMap < 0x01000000) &&
            LcCommand(H->hLC, LC_CMD_MEMMAP_SET, cbMemMap, pbMemMap, NULL, NULL) &&
            LcGetOption(H->hLC, LC_OPT_CORE_ADDR_MAX, &H->dev.paMax);
        LocalFree(pbMemMap);
        if(hFile) { fclose(hFile); }
        if(!f) {
            vmmprintf(H, "MemProcFS: Failed to load initial memory map from: '%s'.\n", H->cfg.szMemMap);
            goto fail;
        }
    }
    if(H->cfg.szMemMapStr[0]) {
        f = LcCommand(H->hLC, LC_CMD_MEMMAP_SET, (DWORD)strlen(H->cfg.szMemMapStr), H->cfg.szMemMapStr, NULL, NULL) &&
            LcGetOption(H->hLC, LC_OPT_CORE_ADDR_MAX, &H->dev.paMax);
        if(!f) {
            vmmprintf(H, "MemProcFS: Failed to load command line argument memory map.\n");
            goto fail;
        }
    }
    // 5: initialize work (multi-threading sub-system).
    if(!VmmWork_Initialize(H)) {
        vmmprintf(H, "MemProcFS: Failed to initialize work multi-threading.\n");
        goto fail;
    }
    // 6: device context (H->dev) is initialized from here onwards - device functionality is working!
    //    try initialize vmm subsystem.
    if(!VmmProcInitialize(H)) {
        vmmprintf(H, "MOUNT: INFO: PROC file system not mounted.\n");
        goto fail;
    }
    // 7: vmm context (H->vmm) is initialized from here onwards - vmm functionality is working!
    //    set LeechCore MemMap (if auto).
    if(H->cfg.szMemMap[0] && !_stricmp(H->cfg.szMemMap, "auto")) {
        if(!VmmDllCore_InitializeMemMapAuto(H)) {
            vmmprintf(H, "MemProcFS: Failed to load initial memory map from: '%s'.\n", H->cfg.szMemMap);
            goto fail;
        }
    }
    // 8: initialize forensic mode (if set by user parameter).
    if(H->cfg.tpForensicMode) {
        if(!FcInitialize(H, H->cfg.tpForensicMode, FALSE)) {
            if(H->dev.fVolatile) {
                vmmprintf(H, "MemProcFS: Failed to initialize forensic mode - volatile (live) memory not supported - please use memory dump!\n");
            } else {
                vmmprintf(H, "MemProcFS: Failed to initialize forensic mode.\n");
            }
            goto fail;
        }
    }
    ReleaseSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
    return H;
fail:
    if(ppLcErrorInfo) {
        *ppLcErrorInfo = pLcErrorInfo;
    } else {
        LcMemFree(pLcErrorInfo);
    }
    VmmDllCore_CloseHandle(H);
    ReleaseSRWLockExclusive(&g_VMMDLL_CORE_LOCK_SRW);
    return NULL;
}



//-----------------------------------------------------------------------------
// EXTERNAL MEMORY ALLOCATION / DEALLOCATION FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

typedef struct tdVMMDLLCORE_MEMLEAKEXTERNAL_CONTEXT {
    VMM_HANDLE H;
    DWORD c;
} VMMDLLCORE_MEMLEAKEXTERNAL_CONTEXT, *PVMMDLLCORE_MEMLEAKEXTERNAL_CONTEXT;

VOID VmmDllCore_MemLeakFindExternal_MapFilterCB(_In_ QWORD k, _In_ POB v, _In_ PVMMDLLCORE_MEMLEAKEXTERNAL_CONTEXT ctx)
{
    if((v->H != ctx->H) || (ctx->c >= 10)) { return; }
    ctx->c++;
    VmmLog(ctx->H, MID_API, LOGLEVEL_2_WARNING, "MEMORY NOT DEALLOCATED AT CLOSE: va=0x%llx size=0x%x tag=%c%c%c%c", (QWORD)v + sizeof(OB), v->cbData, v->_tagCh[3], v->_tagCh[2], v->_tagCh[1], v->_tagCh[0]);
    if(ctx->c == 10) {
        VmmLog(ctx->H, MID_API, LOGLEVEL_2_WARNING, "MEMORY NOT DEALLOCATED AT CLOSE: FIRST %i ENTRIES SHOWN - WARNING MUTED!", ctx->c);
    }
}

/*
* Warn/Log potential user memory leaks at handle close.
* This is done by walking the external handle map.
* -- H
*/
VOID VmmDllCore_MemLeakFindExternal(_In_ VMM_HANDLE H)
{
    VMMDLLCORE_MEMLEAKEXTERNAL_CONTEXT ctxFilter = { 0 };
    ctxFilter.H = H;
    if(VmmLogIsActive(H, MID_API, LOGLEVEL_2_WARNING)) {
        ObMap_Filter(g_VMMDLL_ALLOCMAP_EXT, &ctxFilter, (OB_MAP_FILTER_PFN)VmmDllCore_MemLeakFindExternal_MapFilterCB);
    }
}

/*
* Query the size of memory allocated by the VMMDLL.
* -- pvMem
* -- return = number of bytes required to hold memory allocation.
*/
_Success_(return != 0)
SIZE_T VmmDllCore_MemSizeExternal(_In_ PVOID pvMem)
{
    POB pObMem;
    if(ObMap_ExistsKey(g_VMMDLL_ALLOCMAP_EXT, (QWORD)pvMem)) {
        pObMem = (POB)((SIZE_T)pvMem - sizeof(OB));
        if((pObMem->_magic2 == OB_HEADER_MAGIC) && (pObMem->_magic1 == OB_HEADER_MAGIC)) {
            return pObMem->cbData;
        }
    }
    return 0;
}

/*
* Free memory allocated by the VMMDLL.
* -- pvMem
*/
VOID VmmDllCore_MemFreeExternal(_Frees_ptr_opt_ PVOID pvMem)
{
    POB pObMem;
    if((pObMem = ObMap_RemoveByKey(g_VMMDLL_ALLOCMAP_EXT, (QWORD)pvMem))) {
        Ob_DECREF(pObMem);
    }
}

/*
* Allocate "external" memory to be free'd only by VMMDLL_MemFree // VmmDllCore_MemFreeExternal.
* CALLER VMMDLL_MemFree(return)
* -- H
* -- tag = tag identifying the type of object.
* -- cb = total size to allocate (not guaranteed to be zero-filled).
* -- cbHdr = size of header (guaranteed to be zero-filled).
* -- return
*/
_Success_(return != NULL)
PVOID VmmDllCore_MemAllocExternal(_In_ VMM_HANDLE H, _In_ DWORD tag, _In_ SIZE_T cb, _In_ SIZE_T cbHdr)
{
    POB_DATA pObData;
    if((cb > 0x40000000) || (cb < cbHdr)) { return NULL; }
    if(!(pObData = (POB_DATA)Ob_AllocEx(H, tag, 0, cb + sizeof(OB), NULL, NULL))) { return NULL; }
    ZeroMemory(pObData->pb, cbHdr);
    ObMap_Push(g_VMMDLL_ALLOCMAP_EXT, (QWORD)pObData + sizeof(OB), pObData);    // map will INCREF on success
    pObData = Ob_DECREF(pObData);
    return pObData ? pObData->pb : NULL;
}

