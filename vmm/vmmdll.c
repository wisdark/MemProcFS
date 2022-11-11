// vmmdll.c : implementation of external exported library functions.
//
// (c) Ulf Frisk, 2018-2022
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "vmmdll.h"
#include "pluginmanager.h"
#include "charutil.h"
#include "util.h"
#include "pdb.h"
#include "pe.h"
#include "fc.h"
#include "statistics.h"
#include "version.h"
#include "vmm.h"
#include "vmmproc.h"
#include "vmmwin.h"
#include "vmmnet.h"
#include "vmmwinobj.h"
#include "vmmwinreg.h"
#include "mm_pfn.h"

// tags for external allocations:
#define OB_TAG_API_MAP_EAT              'EAT '
#define OB_TAG_API_MAP_HANDLE           'HND '
#define OB_TAG_API_MAP_HEAP             'HEAP'
#define OB_TAG_API_MAP_HEAP_ALLOC       'HEPA'
#define OB_TAG_API_MAP_IAT              'IAT '
#define OB_TAG_API_MAP_MODULE           'MOD '
#define OB_TAG_API_MAP_NET              'NET '
#define OB_TAG_API_MAP_PHYSMEM          'PMEM'
#define OB_TAG_API_MAP_POOL             'POOL'
#define OB_TAG_API_MAP_PTE              'PTE '
#define OB_TAG_API_MAP_SERVICES         'SVC '
#define OB_TAG_API_MAP_THREAD           'THRD'
#define OB_TAG_API_MAP_UNLOADEDMODULE   'UMOD'
#define OB_TAG_API_MAP_USER             'USER'
#define OB_TAG_API_MAP_VAD              'VAD '
#define OB_TAG_API_MAP_VAD_EX           'VADX'
#define OB_TAG_API_MODULE_FROM_NAME     'MODN'
#define OB_TAG_API_PROCESS_STRING       'PSTR'
#define OB_TAG_API_SEARCH               'SRCH'
#define OB_TAG_API_VFS_LIST_BLOB        'VFSB'

//-----------------------------------------------------------------------------
// INITIALIZATION FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

/*
* Close all VMM_HANDLE and clean up everything! No VMM_HANDLE will be valid
* after this function has been called.
*/
VOID VmmDllCore_CloseAll();

/*
* Close a VMM_HANDLE and clean up everything! The VMM_HANDLE will not be valid
* after this function has been called.
* -- H
*/
VOID VmmDllCore_Close(_In_opt_ _Post_ptr_invalid_ VMM_HANDLE H);

/*
* Initialize MemProcFS from user parameters. Upon success a VMM_HANDLE is returned.
* -- argc
* -- argv
* -- ppLcErrorInfo
* -- return
*/
_Success_(return != NULL)
VMM_HANDLE VmmDllCore_Initialize(_In_ DWORD argc, _In_ LPSTR argv[], _Out_opt_ PPLC_CONFIG_ERRORINFO ppLcErrorInfo);

EXPORTED_FUNCTION _Success_(return != NULL)
VMM_HANDLE VMMDLL_InitializeEx(_In_ DWORD argc, _In_ LPSTR argv[], _Out_opt_ PPLC_CONFIG_ERRORINFO ppLcErrorInfo)
{
    return VmmDllCore_Initialize(argc, argv, ppLcErrorInfo);
}

EXPORTED_FUNCTION _Success_(return != NULL)
VMM_HANDLE VMMDLL_Initialize(_In_ DWORD argc, _In_ LPSTR argv[])
{
    return VMMDLL_InitializeEx(argc, argv, NULL);
}

EXPORTED_FUNCTION
VOID VMMDLL_Close(_In_opt_ _Post_ptr_invalid_ VMM_HANDLE H)
{
    if(H && (H->magic == VMM_MAGIC)) {
        VmmDllCore_Close(H);
    }
}

EXPORTED_FUNCTION
VOID VMMDLL_CloseAll()
{
    VmmDllCore_CloseAll();
}



// ----------------------------------------------------------------------------
// Synchronization macro below. The VMM isn't thread safe so it's important to
// serialize access to it over the VMM LockMaster. This master lock is shared
// with internal VMM housekeeping functionality.
// ----------------------------------------------------------------------------

/*
* Verify that the supplied handle is valid and also check it out.
* This must be called by each external access which requires a VMM_HANDLE.
* Each successful VmmDllCore_HandleReserveExternal() call must be matched by
* a matched call to VmmDllCore_HandleReturnExternal() after completion.
* -- H
* -- return
*/
_Success_(return)
BOOL VmmDllCore_HandleReserveExternal(_In_opt_ VMM_HANDLE H);

/*
* Return a handle successfully reserved with a previous call to the function:
* VmmDllCore_HandleReserveExternal()
* -- H
*/
VOID VmmDllCore_HandleReturnExternal(_In_ VMM_HANDLE H);

#define CALL_IMPLEMENTATION_VMM(H, id, fn) {                                    \
    QWORD tm;                                                                   \
    BOOL result;                                                                \
    if(!VmmDllCore_HandleReserveExternal(H)) { return FALSE; }                  \
    tm = Statistics_CallStart(H);                                               \
    result = fn;                                                                \
    Statistics_CallEnd(H, id, tm);                                              \
    VmmDllCore_HandleReturnExternal(H);                                         \
    return result;                                                              \
}

#define CALL_IMPLEMENTATION_VMM_RETURN(H, id, RetTp, RetValFail, fn) {          \
    QWORD tm;                                                                   \
    RetTp retVal;                                                               \
    if(!VmmDllCore_HandleReserveExternal(H)) { return ((RetTp)RetValFail); }    \
    tm = Statistics_CallStart(H);                                               \
    retVal = fn;                                                                \
    Statistics_CallEnd(H, id, tm);                                              \
    VmmDllCore_HandleReturnExternal(H);                                         \
    return retVal;                                                              \
}

/*
* Query the size of memory allocated by the VMMDLL.
* -- pvMem
* -- return = number of bytes required to hold memory allocation.
*/
_Success_(return != 0)
SIZE_T VmmDllCore_MemSizeExternal(_In_ PVOID pvMem);

/*
* Free memory allocated by the VMMDLL.
* -- pvMem
*/
VOID VmmDllCore_MemFreeExternal(_Frees_ptr_opt_ PVOID pvMem);

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
PVOID VmmDllCore_MemAllocExternal(_In_ VMM_HANDLE H, _In_ DWORD tag, _In_ SIZE_T cb, _In_ SIZE_T cbHdr);

/*
* Query the size of memory allocated by the VMMDLL.
* -- pvMem
* -- return = number of bytes required to hold memory allocation.
*/
EXPORTED_FUNCTION _Success_(return != 0)
SIZE_T VMMDLL_MemSize(_In_ PVOID pvMem)
{
    return VmmDllCore_MemSizeExternal(pvMem);
}

/*
* Free memory allocated by the VMMDLL.
* -- pvMem
*/
EXPORTED_FUNCTION
VOID VMMDLL_MemFree(_Frees_ptr_opt_ PVOID pvMem)
{
    VmmDllCore_MemFreeExternal(pvMem);
}



//-----------------------------------------------------------------------------
// PLUGIN MANAGER FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

_Success_(return)
BOOL VMMDLL_InitializePlugins(_In_ VMM_HANDLE H)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_InitializePlugins,
        PluginManager_Initialize(H))
}



//-----------------------------------------------------------------------------
// CONFIGURATION SETTINGS BELOW:
//-----------------------------------------------------------------------------

#define VMMDLL_REFRESH_CHECK(fOption, mask)      (fOption & mask & 0x0000ffff00000000)

_Success_(return)
BOOL VMMDLL_ConfigGet_Impl(_In_ VMM_HANDLE H, _In_ ULONG64 fOption, _Out_ PULONG64 pqwValue)
{
    if(!fOption || !pqwValue) { return FALSE; }
    switch(fOption & 0xffffffff00000000) {
        case VMMDLL_OPT_CORE_SYSTEM:
            *pqwValue = H->vmm.tpSystem;
            return TRUE;
        case VMMDLL_OPT_CORE_MEMORYMODEL:
            *pqwValue = H->vmm.tpMemoryModel;
            return TRUE;
        case VMMDLL_OPT_CONFIG_VMM_VERSION_MAJOR:
            *pqwValue = VERSION_MAJOR;
            return TRUE;
        case VMMDLL_OPT_CONFIG_VMM_VERSION_MINOR:
            *pqwValue = VERSION_MINOR;
            return TRUE;
        case VMMDLL_OPT_CONFIG_VMM_VERSION_REVISION:
            *pqwValue = VERSION_REVISION;
            return TRUE;
        case VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED:
            *pqwValue = H->vmm.ThreadProcCache.fEnabled ? 1 : 0;
            return TRUE;
        case VMMDLL_OPT_CONFIG_IS_PAGING_ENABLED:
            *pqwValue = (H->vmm.flags & VMM_FLAG_NOPAGING) ? 0 : 1;
            return TRUE;
        case VMMDLL_OPT_CONFIG_TICK_PERIOD:
            *pqwValue = H->vmm.ThreadProcCache.cMs_TickPeriod;
            return TRUE;
        case VMMDLL_OPT_CONFIG_READCACHE_TICKS:
            *pqwValue = H->vmm.ThreadProcCache.cTick_MEM;
            return TRUE;
        case VMMDLL_OPT_CONFIG_TLBCACHE_TICKS:
            *pqwValue = H->vmm.ThreadProcCache.cTick_TLB;
            return TRUE;
        case VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL:
            *pqwValue = H->vmm.ThreadProcCache.cTick_Fast;
            return TRUE;
        case VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL:
            *pqwValue = H->vmm.ThreadProcCache.cTick_Medium;
            return TRUE;
        case VMMDLL_OPT_CONFIG_STATISTICS_FUNCTIONCALL:
            *pqwValue = Statistics_CallGetEnabled(H) ? 1 : 0;
            return TRUE;
        case VMMDLL_OPT_WIN_VERSION_MAJOR:
            *pqwValue = H->vmm.kernel.dwVersionMajor;
            return TRUE;
        case VMMDLL_OPT_WIN_VERSION_MINOR:
            *pqwValue = H->vmm.kernel.dwVersionMinor;
            return TRUE;
        case VMMDLL_OPT_WIN_VERSION_BUILD:
            *pqwValue = H->vmm.kernel.dwVersionBuild;
            return TRUE;
        case VMMDLL_OPT_WIN_SYSTEM_UNIQUE_ID:
            *pqwValue = H->vmm.dwSystemUniqueId;
            return TRUE;
        case VMMDLL_OPT_FORENSIC_MODE:
            *pqwValue = H->fc ? (BYTE)H->fc->db.tp : 0;
            return TRUE;
        // core options affecting both vmm.dll and pcileech.dll
        case VMMDLL_OPT_CORE_PRINTF_ENABLE:
            *pqwValue = H->cfg.fVerboseDll ? 1 : 0;
            return TRUE;
        case VMMDLL_OPT_CORE_VERBOSE:
            *pqwValue = H->cfg.fVerbose ? 1 : 0;
            return TRUE;
        case VMMDLL_OPT_CORE_VERBOSE_EXTRA:
            *pqwValue = H->cfg.fVerboseExtra ? 1 : 0;
            return TRUE;
        case VMMDLL_OPT_CORE_VERBOSE_EXTRA_TLP:
            *pqwValue = H->cfg.fVerboseExtraTlp ? 1 : 0;
            return TRUE;
        case VMMDLL_OPT_CORE_MAX_NATIVE_ADDRESS:
            *pqwValue = H->dev.paMax;
            return TRUE;
        default:
            // non-recognized option - possibly a device option to pass along to leechcore.dll
            return LcGetOption(H->hLC, fOption, pqwValue);
    }
}

_Success_(return)
BOOL VMMDLL_ConfigSet_Impl(_In_ VMM_HANDLE H, _In_ ULONG64 fOption, _In_ ULONG64 qwValue)
{
    if(!H || (H->magic != VMM_MAGIC)) { return FALSE; }
    // user-initiated refresh / cache flushes
    if((fOption & 0xffff000000000000) == 0x2001000000000000) {
        if(VMMDLL_REFRESH_CHECK(fOption, VMMDLL_OPT_REFRESH_FREQ_MEM)) {
            VmmProcRefresh_MEM(H);
            VmmProcRefresh_MEM(H);
            VmmProcRefresh_MEM(H);
        }
        if(VMMDLL_REFRESH_CHECK(fOption, VMMDLL_OPT_REFRESH_FREQ_MEM_PARTIAL)) {
            VmmProcRefresh_MEM(H);
        }
        if(VMMDLL_REFRESH_CHECK(fOption, VMMDLL_OPT_REFRESH_FREQ_TLB)) {
            VmmProcRefresh_TLB(H);
            VmmProcRefresh_TLB(H);
            VmmProcRefresh_TLB(H);
        }
        if(VMMDLL_REFRESH_CHECK(fOption, VMMDLL_OPT_REFRESH_FREQ_TLB_PARTIAL)) {
            VmmProcRefresh_TLB(H);
        }
        if(VMMDLL_REFRESH_CHECK(fOption, VMMDLL_OPT_REFRESH_FREQ_FAST)) {
            VmmProcRefresh_Fast(H);
        }
        if(VMMDLL_REFRESH_CHECK(fOption, VMMDLL_OPT_REFRESH_FREQ_MEDIUM)) {
            VmmProcRefresh_Medium(H);
        }
        if(VMMDLL_REFRESH_CHECK(fOption, VMMDLL_OPT_REFRESH_FREQ_SLOW)) {
            VmmProcRefresh_Slow(H);
        }
        return TRUE;
    }
    switch(fOption & 0xffffffff00000000) {
        case VMMDLL_OPT_CORE_PRINTF_ENABLE:
            LcSetOption(H->hLC, fOption, qwValue);
            H->cfg.fVerboseDll = qwValue ? TRUE : FALSE;
            VmmLog_LevelRefresh(H);
            PluginManager_Notify(H, VMMDLL_PLUGIN_NOTIFY_VERBOSITYCHANGE, NULL, 0);
            return TRUE;
        case VMMDLL_OPT_CORE_VERBOSE:
            LcSetOption(H->hLC, fOption, qwValue);
            H->cfg.fVerbose = qwValue ? TRUE : FALSE;
            VmmLog_LevelRefresh(H);
            PluginManager_Notify(H, VMMDLL_PLUGIN_NOTIFY_VERBOSITYCHANGE, NULL, 0);
            return TRUE;
        case VMMDLL_OPT_CORE_VERBOSE_EXTRA:
            LcSetOption(H->hLC, fOption, qwValue);
            H->cfg.fVerboseExtra = qwValue ? TRUE : FALSE;
            VmmLog_LevelRefresh(H);
            PluginManager_Notify(H, VMMDLL_PLUGIN_NOTIFY_VERBOSITYCHANGE, NULL, 0);
            return TRUE;
        case VMMDLL_OPT_CORE_VERBOSE_EXTRA_TLP:
            LcSetOption(H->hLC, fOption, qwValue);
            H->cfg.fVerboseExtraTlp = qwValue ? TRUE : FALSE;
            VmmLog_LevelRefresh(H);
            PluginManager_Notify(H, VMMDLL_PLUGIN_NOTIFY_VERBOSITYCHANGE, NULL, 0);
            return TRUE;
        case VMMDLL_OPT_CONFIG_IS_PAGING_ENABLED:
            H->vmm.flags = (H->vmm.flags & ~VMM_FLAG_NOPAGING) | (qwValue ? 0 : 1);
            return TRUE;
        case VMMDLL_OPT_CONFIG_TICK_PERIOD:
            H->vmm.ThreadProcCache.cMs_TickPeriod = (DWORD)qwValue;
            return TRUE;
        case VMMDLL_OPT_CONFIG_READCACHE_TICKS:
            H->vmm.ThreadProcCache.cTick_MEM = (DWORD)qwValue;
            return TRUE;
        case VMMDLL_OPT_CONFIG_TLBCACHE_TICKS:
            H->vmm.ThreadProcCache.cTick_TLB = (DWORD)qwValue;
            return TRUE;
        case VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL:
            H->vmm.ThreadProcCache.cTick_Fast = (DWORD)qwValue;
            return TRUE;
        case VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL:
            H->vmm.ThreadProcCache.cTick_Medium = (DWORD)qwValue;
            return TRUE;
        case VMMDLL_OPT_CONFIG_STATISTICS_FUNCTIONCALL:
            Statistics_CallSetEnabled(H, qwValue ? TRUE : FALSE);
            return TRUE;
        case VMMDLL_OPT_FORENSIC_MODE:
            return FcInitialize(H, (DWORD)qwValue, FALSE);
        default:
            // non-recognized option - possibly a device option to pass along to leechcore.dll
            return LcSetOption(H->hLC, fOption, qwValue);
    }
}

_Success_(return)
BOOL VMMDLL_ConfigGet(_In_ VMM_HANDLE H, _In_ ULONG64 fOption, _Out_ PULONG64 pqwValue)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_ConfigGet, VMMDLL_ConfigGet_Impl(H, fOption, pqwValue))
}

_Success_(return)
BOOL VMMDLL_ConfigSet(_In_ VMM_HANDLE H, _In_ ULONG64 fOption, _In_ ULONG64 qwValue)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_ConfigSet, VMMDLL_ConfigSet_Impl(H, fOption, qwValue))
}

//-----------------------------------------------------------------------------
// VFS - VIRTUAL FILE SYSTEM FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

_Success_(return)
BOOL VMMDLL_VfsList_Impl_ProcessRoot(_In_ VMM_HANDLE H, _In_ BOOL fNamePID, _Inout_ PHANDLE pFileList)
{
    PVMM_PROCESS pObProcess = NULL;
    CHAR uszBufferFileName[MAX_PATH];
    VMMDLL_VFS_FILELIST_EXINFO ExInfo = { 0 };
    while((pObProcess = VmmProcessGetNext(H, pObProcess, 0))) {
        if(fNamePID) {
            if(pObProcess->dwState) {
                sprintf_s(uszBufferFileName, MAX_PATH - 1, "%s-(%x)-%i", pObProcess->szName, pObProcess->dwState, pObProcess->dwPID);
            } else {
                sprintf_s(uszBufferFileName, MAX_PATH - 1, "%s-%i", pObProcess->szName, pObProcess->dwPID);
            }
        } else {
            sprintf_s(uszBufferFileName, MAX_PATH - 1, "%i", pObProcess->dwPID);
        }
        Util_VfsTimeStampFile(H, pObProcess, &ExInfo);
        VMMDLL_VfsList_AddDirectory(pFileList, uszBufferFileName, &ExInfo);
    }
    return TRUE;
}


BOOL VMMDLL_VfsList_Impl(_In_ VMM_HANDLE H, _In_ LPSTR uszPath, _Inout_ PHANDLE pFileList)
{
    BOOL result = FALSE;
    DWORD dwPID;
    LPSTR wszSubPath;
    PVMM_PROCESS pObProcess;
    if(!VMMDLL_VfsList_IsHandleValid(pFileList)) { return FALSE; }
    if(uszPath[0] == '\\') { uszPath++; }
    if(Util_VfsHelper_GetIdDir(uszPath, FALSE, &dwPID, &wszSubPath)) {
        if(!(pObProcess = VmmProcessGet(H, dwPID))) { return FALSE; }
        PluginManager_List(H, pObProcess, wszSubPath, pFileList);
        Ob_DECREF(pObProcess);
        return TRUE;
    }
    if(!_strnicmp(uszPath, "name", 4)) {
        if(strlen(uszPath) > 5) { return FALSE; }
        return VMMDLL_VfsList_Impl_ProcessRoot(H, TRUE, pFileList);
    }
    if(!_strnicmp(uszPath, "pid", 3)) {
        if(strlen(uszPath) > 4) { return FALSE; }
        return VMMDLL_VfsList_Impl_ProcessRoot(H, FALSE, pFileList);
    }
    PluginManager_List(H, NULL, uszPath, pFileList);
    return TRUE;
}

_Success_(return)
BOOL VMMDLL_VfsListU(_In_ VMM_HANDLE H, _In_ LPSTR uszPath, _Inout_ PVMMDLL_VFS_FILELIST2 pFileList)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_VfsList,
        VMMDLL_VfsList_Impl(H, uszPath, (PHANDLE)pFileList))
}

_Success_(return)
BOOL VMMDLL_VfsListW(_In_ VMM_HANDLE H, _In_ LPWSTR wszPath, _Inout_ PVMMDLL_VFS_FILELIST2 pFileList)
{
    LPSTR uszPath;
    BYTE pbBuffer[3 * MAX_PATH];
    if(!CharUtil_WtoU(wszPath, -1, pbBuffer, sizeof(pbBuffer), &uszPath, NULL, 0)) { return FALSE; }
    return VMMDLL_VfsListU(H, uszPath, pFileList);
}

typedef struct tdVMMDLL_VFS_FILELISTBLOB_CREATE_CONTEXT {
    POB_MAP pme;
    POB_STRMAP psm;
} VMMDLL_VFS_FILELISTBLOB_CREATE_CONTEXT, *PVMMDLL_VFS_FILELISTBLOB_CREATE_CONTEXT;

VOID VMMDLL_VfsListBlob_Impl_AddFile(_Inout_ HANDLE h, _In_ LPSTR uszName, _In_ ULONG64 cb, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    PVMMDLL_VFS_FILELISTBLOB_CREATE_CONTEXT ctx = (PVMMDLL_VFS_FILELISTBLOB_CREATE_CONTEXT)h;
    PVMMDLL_VFS_FILELISTBLOB_ENTRY pe = NULL;
    if(!(pe = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMDLL_VFS_FILELISTBLOB_ENTRY)))) { return; }
    if(!ObStrMap_PushPtrUU(ctx->psm, uszName, (LPSTR*)&pe->ouszName, NULL)) {
        LocalFree(pe);
        return;
    }
    if(pExInfo) {
        memcpy(&pe->ExInfo, pExInfo, sizeof(VMMDLL_VFS_FILELIST_EXINFO));
    }
    pe->cbFileSize = cb;
    ObMap_Push(ctx->pme, (QWORD)pe, pe);    // reference to pe overtaken by ctx->pme
}

VOID VMMDLL_VfsListBlob_Impl_AddDirectory(_Inout_ HANDLE h, _In_ LPSTR uszName, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    VMMDLL_VfsListBlob_Impl_AddFile(h, uszName, (ULONG64)-1, pExInfo);
}

_Success_(return != NULL)
PVMMDLL_VFS_FILELISTBLOB VMMDLL_VfsListBlob_Impl(_In_ VMM_HANDLE H, _In_ LPSTR uszPath)
{
    BOOL fResult = FALSE;
    VMMDLL_VFS_FILELISTBLOB_CREATE_CONTEXT ctx = { 0 };
    VMMDLL_VFS_FILELIST2 FL2;
    DWORD i = 0, cbStruct, cFileEntry, cbMultiText;
    PVMMDLL_VFS_FILELISTBLOB pFLB = NULL;
    PVMMDLL_VFS_FILELISTBLOB_ENTRY pe;
    // 1: init
    if(!(ctx.pme = ObMap_New(H, OB_MAP_FLAGS_OBJECT_LOCALFREE))) { goto fail; }
    if(!(ctx.psm = ObStrMap_New(H, OB_STRMAP_FLAGS_STR_ASSIGN_OFFSET))) { goto fail; }
    // 2: call
    FL2.dwVersion = VMMDLL_VFS_FILELIST_VERSION;
    FL2.pfnAddFile = VMMDLL_VfsListBlob_Impl_AddFile;
    FL2.pfnAddDirectory = VMMDLL_VfsListBlob_Impl_AddDirectory;
    FL2.h = &ctx;
    if(!VMMDLL_VfsList_Impl(H, uszPath, (PHANDLE)&FL2)) { goto fail; }
    // 3: assign result blob
    cFileEntry = ObMap_Size(ctx.pme);
    if(!ObStrMap_FinalizeBufferU(ctx.psm, 0, NULL, &cbMultiText)) { goto fail; }
    cbStruct = sizeof(VMMDLL_VFS_FILELISTBLOB) + cFileEntry * sizeof(VMMDLL_VFS_FILELISTBLOB_ENTRY) + cbMultiText;
    if(!(pFLB = VmmDllCore_MemAllocExternal(H, OB_TAG_API_VFS_LIST_BLOB, cbStruct, sizeof(VMMDLL_VFS_FILELISTBLOB)))) { goto fail; }  // VMMDLL_MemFree()
    pFLB->dwVersion = VMMDLL_VFS_FILELISTBLOB_VERSION;
    pFLB->cbStruct = cbStruct;
    pFLB->cFileEntry = cFileEntry;
    pFLB->uszMultiText = (LPSTR)((QWORD)pFLB + sizeof(VMMDLL_VFS_FILELISTBLOB) + cFileEntry * sizeof(VMMDLL_VFS_FILELISTBLOB_ENTRY));
    if(!ObStrMap_FinalizeBufferU(ctx.psm, cbMultiText, pFLB->uszMultiText, &pFLB->cbMultiText)) { goto fail; }
    for(i = 0; i < cFileEntry; i++) {
        pe = ObMap_GetByIndex(ctx.pme, i);
        if(!pe) { goto fail; }
        memcpy(pFLB->FileEntry + i, pe, sizeof(VMMDLL_VFS_FILELISTBLOB_ENTRY));
    }
    fResult = TRUE;
fail:
    Ob_DECREF(ctx.pme);
    Ob_DECREF(ctx.psm);
    if(!fResult) {
        VMMDLL_MemFree(pFLB); pFLB = NULL;
    }
    return pFLB;
}

_Success_(return != NULL)
PVMMDLL_VFS_FILELISTBLOB VMMDLL_VfsListBlobU(_In_ VMM_HANDLE H, _In_ LPSTR uszPath)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_VfsListBlob,
        PVMMDLL_VFS_FILELISTBLOB,
        NULL,
        VMMDLL_VfsListBlob_Impl(H, uszPath))
}

NTSTATUS VMMDLL_VfsRead_Impl(_In_ VMM_HANDLE H, _In_ LPSTR uszPath, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    DWORD dwPID;
    LPSTR uszSubPath;
    PVMM_PROCESS pObProcess;
    if(uszPath[0] == '\\') { uszPath++; }
    if(Util_VfsHelper_GetIdDir(uszPath, FALSE, &dwPID, &uszSubPath)) {
        if(!(pObProcess = VmmProcessGet(H, dwPID))) { return VMM_STATUS_FILE_INVALID; }
        nt = PluginManager_Read(H, pObProcess, uszSubPath, pb, cb, pcbRead, cbOffset);
        Ob_DECREF(pObProcess);
        return nt;
    }
    return PluginManager_Read(H, NULL, uszPath, pb, cb, pcbRead, cbOffset);
}

NTSTATUS VMMDLL_VfsReadU(_In_ VMM_HANDLE H, _In_ LPSTR uszFileName, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ ULONG64 cbOffset)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_VfsRead,
        NTSTATUS,
        VMMDLL_STATUS_UNSUCCESSFUL,
        VMMDLL_VfsRead_Impl(H, uszFileName, pb, cb, pcbRead, cbOffset))
}

NTSTATUS VMMDLL_VfsReadW(_In_ VMM_HANDLE H, _In_ LPWSTR wszFileName, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ ULONG64 cbOffset)
{
    LPSTR uszFileName;
    BYTE pbBuffer[3 * MAX_PATH];
    if(!CharUtil_WtoU(wszFileName, -1, pbBuffer, sizeof(pbBuffer), &uszFileName, NULL, 0)) { return VMM_STATUS_FILE_INVALID; }
    return VMMDLL_VfsReadU(H, uszFileName, pb, cb, pcbRead, cbOffset);
}

NTSTATUS VMMDLL_VfsWrite_Impl(_In_ VMM_HANDLE H, _In_ LPSTR uszPath, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    DWORD dwPID;
    LPSTR uszSubPath;
    PVMM_PROCESS pObProcess;
    if(uszPath[0] == '\\') { uszPath++; }
    if(Util_VfsHelper_GetIdDir(uszPath, FALSE, &dwPID, &uszSubPath)) {
        if(!(pObProcess = VmmProcessGet(H, dwPID))) { return VMM_STATUS_FILE_INVALID; }
        nt = PluginManager_Write(H, pObProcess, uszSubPath, pb, cb, pcbWrite, cbOffset);
        Ob_DECREF(pObProcess);
        return nt;
    }
    return PluginManager_Write(H, NULL, uszPath, pb, cb, pcbWrite, cbOffset);
}

NTSTATUS VMMDLL_VfsWriteU(_In_ VMM_HANDLE H, _In_ LPSTR uszFileName, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ ULONG64 cbOffset)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_VfsWrite,
        NTSTATUS,
        VMMDLL_STATUS_UNSUCCESSFUL,
        VMMDLL_VfsWrite_Impl(H, uszFileName, pb, cb, pcbWrite, cbOffset))
}

NTSTATUS VMMDLL_VfsWriteW(_In_ VMM_HANDLE H, _In_ LPWSTR wszFileName, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ ULONG64 cbOffset)
{
    LPSTR uszFileName;
    BYTE pbBuffer[3 * MAX_PATH];
    if(!CharUtil_WtoU(wszFileName, -1, pbBuffer, sizeof(pbBuffer), &uszFileName, NULL, 0)) { return VMM_STATUS_FILE_INVALID; }
    return VMMDLL_VfsWriteU(H, uszFileName, pb, cb, pcbWrite, cbOffset);
}

EXPORTED_FUNCTION NTSTATUS VMMDLL_UtilVfsReadFile_FromPBYTE(_In_ PBYTE pbFile, _In_ ULONG64 cbFile, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ ULONG64 cbOffset)
{
    return Util_VfsReadFile_FromPBYTE(pbFile, cbFile, pb, cb, pcbRead, cbOffset);
}

EXPORTED_FUNCTION NTSTATUS VMMDLL_UtilVfsReadFile_FromQWORD(_In_ ULONG64 qwValue, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ ULONG64 cbOffset, _In_ BOOL fPrefix)
{
    return Util_VfsReadFile_FromQWORD(qwValue, pb, cb, pcbRead, cbOffset, fPrefix);
}

EXPORTED_FUNCTION NTSTATUS VMMDLL_UtilVfsReadFile_FromDWORD(_In_ DWORD dwValue, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ ULONG64 cbOffset, _In_ BOOL fPrefix)
{
    return Util_VfsReadFile_FromDWORD(dwValue, pb, cb, pcbRead, cbOffset, fPrefix);
}

EXPORTED_FUNCTION NTSTATUS VMMDLL_UtilVfsReadFile_FromBOOL(_In_ BOOL fValue, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ ULONG64 cbOffset)
{
    return Util_VfsReadFile_FromBOOL(fValue, pb, cb, pcbRead, cbOffset);
}

EXPORTED_FUNCTION NTSTATUS VMMDLL_UtilVfsWriteFile_BOOL(_Inout_ PBOOL pfTarget, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ ULONG64 cbOffset)
{
    return Util_VfsWriteFile_BOOL(pfTarget, pb, cb, pcbWrite, cbOffset);
}

EXPORTED_FUNCTION NTSTATUS VMMDLL_UtilVfsWriteFile_DWORD(_Inout_ PDWORD pdwTarget, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ ULONG64 cbOffset, _In_ DWORD dwMinAllow)
{
    return Util_VfsWriteFile_DWORD(pdwTarget, pb, cb, pcbWrite, cbOffset, dwMinAllow, 0);
}



//-----------------------------------------------------------------------------
// VMM CORE FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

DWORD VMMDLL_MemReadScatter_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_ PPMEM_SCATTER ppMEMs, _In_ DWORD cpMEMs, _In_ DWORD flags)
{
    DWORD i, cMEMs;
    PVMM_PROCESS pObProcess = NULL;
    if(dwPID == (DWORD)-1) {
        VmmReadScatterPhysical(H, ppMEMs, cpMEMs, flags);
    } else {
        pObProcess = VmmProcessGet(H, dwPID);
        if(!pObProcess) { return 0; }
        VmmReadScatterVirtual(H, pObProcess, ppMEMs, cpMEMs, flags);
        Ob_DECREF(pObProcess);
    }
    for(i = 0, cMEMs = 0; i < cpMEMs; i++) {
        if(ppMEMs[i]->f) {
            cMEMs++;
        }
    }
    return cMEMs;
}

DWORD VMMDLL_MemReadScatter(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_ PPMEM_SCATTER ppMEMs, _In_ DWORD cpMEMs, _In_ DWORD flags)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_MemReadScatter,
        DWORD,
        0,
        VMMDLL_MemReadScatter_Impl(H, dwPID, ppMEMs, cpMEMs, flags))
}

DWORD VMMDLL_MemWriteScatter_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_ PPMEM_SCATTER ppMEMs, _In_ DWORD cpMEMs)
{
    DWORD i, cMEMs;
    PVMM_PROCESS pObProcess = NULL;
    if(dwPID == (DWORD)-1) {
        VmmWriteScatterPhysical(H, ppMEMs, cpMEMs);
    } else {
        pObProcess = VmmProcessGet(H, dwPID);
        if(!pObProcess) { return 0; }
        VmmWriteScatterVirtual(H, pObProcess, ppMEMs, cpMEMs);
        Ob_DECREF(pObProcess);
    }
    for(i = 0, cMEMs = 0; i < cpMEMs; i++) {
        if(ppMEMs[i]->f) {
            cMEMs++;
        }
    }
    return cMEMs;
}

DWORD VMMDLL_MemWriteScatter(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_ PPMEM_SCATTER ppMEMs, _In_ DWORD cpMEMs)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_MemWriteScatter,
        DWORD,
        0,
        VMMDLL_MemWriteScatter_Impl(H, dwPID, ppMEMs, cpMEMs))
}

_Success_(return)
BOOL VMMDLL_MemReadEx_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwA, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb, _Out_opt_ PDWORD pcbReadOpt, _In_ ULONG64 flags)
{
    PVMM_PROCESS pObProcess = NULL;
    if(dwPID != (DWORD)-1) {
        pObProcess = VmmProcessGet(H, dwPID);
        if(!pObProcess) { return FALSE; }
    }
    VmmReadEx(H, pObProcess, qwA, pb, cb, pcbReadOpt, flags);
    Ob_DECREF(pObProcess);
    return TRUE;
}

_Success_(return)
BOOL VMMDLL_MemReadEx(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwA, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb, _Out_opt_ PDWORD pcbReadOpt, _In_ ULONG64 flags)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_MemReadEx,
        VMMDLL_MemReadEx_Impl(H, dwPID, qwA, pb, cb, pcbReadOpt, flags))
}

_Success_(return)
BOOL VMMDLL_MemRead(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwA, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb)
{
    DWORD dwRead;
    return VMMDLL_MemReadEx(H, dwPID, qwA, pb, cb, &dwRead, 0) && (dwRead == cb);
}

_Success_(return)
BOOL VMMDLL_MemReadPage(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwA, _Inout_bytecount_(4096) PBYTE pbPage)
{
    DWORD dwRead;
    return VMMDLL_MemReadEx(H, dwPID, qwA, pbPage, 4096, &dwRead, 0) && (dwRead == 4096);
}

_Success_(return)
BOOL VMMDLL_MemPrefetchPages_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_reads_(cPrefetchAddresses) PULONG64 pPrefetchAddresses, _In_ DWORD cPrefetchAddresses)
{
    DWORD i;
    BOOL result = FALSE;
    PVMM_PROCESS pObProcess = NULL;
    POB_SET pObSet_PrefetchAddresses = NULL;
    if(dwPID != (DWORD)-1) {
        pObProcess = VmmProcessGet(H, dwPID);
        if(!pObProcess) { goto fail; }
    }
    if(!(pObSet_PrefetchAddresses = ObSet_New(H))) { goto fail; }
    for(i = 0; i < cPrefetchAddresses; i++) {
        ObSet_Push(pObSet_PrefetchAddresses, pPrefetchAddresses[i] & ~0xfff);
    }
    VmmCachePrefetchPages(H, pObProcess, pObSet_PrefetchAddresses, 0);
    result = TRUE;
fail:
    Ob_DECREF(pObSet_PrefetchAddresses);
    Ob_DECREF(pObProcess);
    return result;
}

_Success_(return)
BOOL VMMDLL_MemPrefetchPages(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_reads_(cPrefetchAddresses) PULONG64 pPrefetchAddresses, _In_ DWORD cPrefetchAddresses)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_MemPrefetchPages,
        VMMDLL_MemPrefetchPages_Impl(H, dwPID, pPrefetchAddresses, cPrefetchAddresses))
}

_Success_(return)
BOOL VMMDLL_MemWrite_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwA, _In_reads_(cb) PBYTE pb, _In_ DWORD cb)
{
    BOOL result;
    PVMM_PROCESS pObProcess = NULL;
    if(dwPID != (DWORD)-1) {
        pObProcess = VmmProcessGet(H, dwPID);
        if(!pObProcess) { return FALSE; }
    }
    result = VmmWrite(H, pObProcess, qwA, pb, cb);
    Ob_DECREF(pObProcess);
    return result;
}

_Success_(return)
BOOL VMMDLL_MemWrite(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwA, _In_reads_(cb) PBYTE pb, _In_ DWORD cb)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_MemWrite,
        VMMDLL_MemWrite_Impl(H, dwPID, qwA, pb, cb))
}

_Success_(return)
BOOL VMMDLL_MemVirt2Phys_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwVA, _Out_ PULONG64 pqwPA)
{
    BOOL result;
    PVMM_PROCESS pObProcess = VmmProcessGet(H, dwPID);
    if(!pObProcess) { return FALSE; }
    result = VmmVirt2Phys(H, pObProcess, qwVA, pqwPA);
    Ob_DECREF(pObProcess);
    return result;
}

_Success_(return)
BOOL VMMDLL_MemVirt2Phys(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 qwVA, _Out_ PULONG64 pqwPA)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_MemVirt2Phys,
        VMMDLL_MemVirt2Phys_Impl(H, dwPID, qwVA, pqwPA))
}

_Success_(return)
BOOL VMMDLL_MemSearch_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_ PVMMDLL_MEM_SEARCH_CONTEXT ctx, _Out_opt_ PQWORD *ppva, _Out_opt_ PDWORD pcva)
{
    BOOL fResult = FALSE;
    POB_DATA pObData = NULL;
    PVMM_PROCESS pObProcess = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmSearch(H, pObProcess, (PVMM_MEMORY_SEARCH_CONTEXT)ctx, &pObData)) { goto fail; }
    if(pObData) {
        if(ppva) {
            if(!(*ppva = VmmDllCore_MemAllocExternal(H, OB_TAG_API_SEARCH, pObData->ObHdr.cbData, 0))) { goto fail; }    // VMMDLL_MemFree()
            memcpy(*ppva, pObData->pqw, pObData->ObHdr.cbData);
        }
        if(pcva) {
            *pcva = pObData->ObHdr.cbData / sizeof(QWORD);
        }
    }
    fResult = TRUE;
fail:
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObData);
    return fResult;
}

_Success_(return)
BOOL VMMDLL_MemSearch(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_ PVMMDLL_MEM_SEARCH_CONTEXT ctx, _Out_opt_ PQWORD *ppva, _Out_opt_ PDWORD pcva)
{
    if(pcva) { *pcva = 0; }
    if(ppva) { *ppva = NULL; }
    if(ctx->dwVersion != VMMDLL_MEM_SEARCH_VERSION) { return FALSE; }
    if(sizeof(VMMDLL_MEM_SEARCH_CONTEXT) != sizeof(VMM_MEMORY_SEARCH_CONTEXT)) { return FALSE; }
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_MemSearch,
        VMMDLL_MemSearch_Impl(H, dwPID, ctx, ppva, pcva))
}



//-----------------------------------------------------------------------------
// FORENSIC-MODE SPECIFIC FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

_Success_(return != 0)
SIZE_T VMMDLL_ForensicFileAppend_DoWork(_In_ VMM_HANDLE H, _In_ LPSTR uszFileName, _In_z_ _Printf_format_string_ LPSTR uszFormat, _In_ va_list arglist)
{
    CALL_IMPLEMENTATION_VMM_RETURN(H, STATISTICS_ID_VMMDLL_ForensicFileAppend, SIZE_T, 0, FcFileAppendEx(H, uszFileName, uszFormat, arglist))
}

/*
* Append text data to a memory-backed forensics file.
* All text should be UTF-8 encoded.
* -- H
* -- uszFileName
* -- uszFormat
* -- ..
* -- return = number of bytes appended (excluding terminating null).
*/
EXPORTED_FUNCTION _Success_(return != 0)
SIZE_T VMMDLL_ForensicFileAppend(
    _In_ VMM_HANDLE H,
    _In_ LPSTR uszFileName,
    _In_z_ _Printf_format_string_ LPSTR uszFormat,
    ...
) {
    SIZE_T ret;
    va_list arglist;
    va_start(arglist, uszFormat);
    ret = VMMDLL_ForensicFileAppend_DoWork(H, uszFileName, uszFormat, arglist);
    va_end(arglist);
    return ret;
}



//-----------------------------------------------------------------------------
// VMM PROCESS FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

_Success_(return)
BOOL VMMDLL_Map_GetPte_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ BOOL fIdentifyModules, _Out_ PVMMDLL_MAP_PTE *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    PVMM_PROCESS pObProcess = NULL;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_PTEENTRY peDst;
    PVMM_MAP_PTEENTRY peSrc;
    PVMMOB_MAP_PTE pObMapSrc = NULL;
    PVMMDLL_MAP_PTE pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_PTEENTRY) != sizeof(VMMDLL_MAP_PTEENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetPte(H, pObProcess, &pObMapSrc, fIdentifyModules)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->uszText);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_PTEENTRY);
    cbDst = sizeof(VMMDLL_MAP_PTE) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_PTE, cbDst, sizeof(VMMDLL_MAP_PTE)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_PTE_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszText, &peDst->uszText, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetPteU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ BOOL fIdentifyModules, _Out_ PVMMDLL_MAP_PTE *ppPteMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetPte, VMMDLL_Map_GetPte_Impl(H, dwPID, fIdentifyModules, ppPteMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetPteW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ BOOL fIdentifyModules, _Out_ PVMMDLL_MAP_PTE *ppPteMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetPte, VMMDLL_Map_GetPte_Impl(H, dwPID, fIdentifyModules, ppPteMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetVad_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ BOOL fIdentifyModules, _Out_ PVMMDLL_MAP_VAD *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    PVMM_PROCESS pObProcess = NULL;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_VADENTRY peDst;
    PVMM_MAP_VADENTRY peSrc;
    PVMMOB_MAP_VAD pObMapSrc = NULL;
    PVMMDLL_MAP_VAD pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_VADENTRY) != sizeof(VMMDLL_MAP_VADENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetVad(H, pObProcess, &pObMapSrc, (fIdentifyModules ? VMM_VADMAP_TP_FULL : VMM_VADMAP_TP_PARTIAL))) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->uszText);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_VADENTRY);
    cbDst = sizeof(VMMDLL_MAP_VAD) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_VAD, cbDst, sizeof(VMMDLL_MAP_VAD)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_VAD_VERSION;
    pMapDst->cPage = pObMapSrc->cPage;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszText, &peDst->uszText, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetVadU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ BOOL fIdentifyModules, _Out_ PVMMDLL_MAP_VAD *ppVadMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetVad, VMMDLL_Map_GetVad_Impl(H, dwPID, fIdentifyModules, ppVadMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetVadW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ BOOL fIdentifyModules, _Out_ PVMMDLL_MAP_VAD *ppVadMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetVad, VMMDLL_Map_GetVad_Impl(H, dwPID, fIdentifyModules, ppVadMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetVadEx_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ DWORD oPage, _In_ DWORD cPage, _Out_ PVMMDLL_MAP_VADEX *ppMapDst)
{
    BOOL fResult = FALSE;
    DWORD i, cbDst = 0, cbDstData;
    PVMMOB_MAP_VADEX pObMap = NULL;
    PVMM_PROCESS pObProcess = NULL;
    PVMMDLL_MAP_VADEX pMapDst = NULL;
    *ppMapDst = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetVadEx(H, pObProcess, &pObMap, VMM_VADMAP_TP_FULL, oPage, cPage)) { goto fail; }
    cbDstData = pObMap->cMap * sizeof(VMMDLL_MAP_VADEXENTRY);
    cbDst = sizeof(VMMDLL_MAP_VADEX) + cbDstData;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_VAD_EX, cbDst, sizeof(VMMDLL_MAP_VADEX)))) { goto fail; }    // VMMDLL_MemFree()
    pMapDst->dwVersion = VMMDLL_MAP_VADEX_VERSION;
    pMapDst->cMap = pObMap->cMap;
    memcpy(pMapDst->pMap, pObMap->pMap, cbDstData);
    for(i = 0; i < pObMap->cMap; i++) {
        pMapDst->pMap[i].vaVadBase = pObMap->pMap[i].peVad->vaStart;
    }
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMap);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return)
BOOL VMMDLL_Map_GetVadEx(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ DWORD oPage, _In_ DWORD cPage, _Out_ PVMMDLL_MAP_VADEX *ppVadExMap)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_Map_GetVadEx,
        VMMDLL_Map_GetVadEx_Impl(H, dwPID, oPage, cPage, ppVadExMap))
}

_Success_(return)
BOOL VMMDLL_Map_GetModule_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_MODULE *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    PVMM_PROCESS pObProcess = NULL;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_MODULEENTRY peDst;
    PVMM_MAP_MODULEENTRY peSrc;
    PVMMOB_MAP_MODULE pObMapSrc = NULL;
    PVMMDLL_MAP_MODULE pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_MODULEENTRY) != sizeof(VMMDLL_MAP_MODULEENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetModule(H, pObProcess, &pObMapSrc)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->uszText);
        ObStrMap_PushU(psmOb, peSrc->uszFullName);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_MODULEENTRY);
    cbDst = sizeof(VMMDLL_MAP_MODULE) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_MODULE, cbDst, sizeof(VMMDLL_MAP_MODULE)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_MODULE_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszText, &peDst->uszText, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszFullName, &peDst->uszFullName, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetModuleU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_MODULE *ppModuleMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetModule, VMMDLL_Map_GetModule_Impl(H, dwPID, ppModuleMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetModuleW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_MODULE *ppModuleMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetModule, VMMDLL_Map_GetModule_Impl(H, dwPID, ppModuleMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetModuleFromName_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_opt_ LPSTR uszModuleName, _Out_ PVMMDLL_MAP_MODULEENTRY *ppeDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    DWORD o = 0, cbDst = 0, cbDstStr, cbTMP;
    PVMMOB_MAP_MODULE pObMapSrc = NULL;
    PVMM_MAP_MODULEENTRY peSrc = NULL;
    POB_STRMAP psmOb = NULL;
    PBYTE pbMultiText;
    PVMMDLL_MAP_MODULEENTRY peDst = NULL;
    *ppeDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_MODULEENTRY) != sizeof(VMMDLL_MAP_MODULEENTRY)) { goto fail; }
    if(!VmmMap_GetModuleEntryEx(H, NULL, dwPID, uszModuleName, &pObMapSrc, &peSrc)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    ObStrMap_PushU(psmOb, peSrc->uszText);
    ObStrMap_PushU(psmOb, peSrc->uszFullName);
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDst = sizeof(VMMDLL_MAP_MODULEENTRY) + cbDstStr;
    if(!(peDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MODULE_FROM_NAME, cbDst, sizeof(VMMDLL_MAP_MODULEENTRY)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill entry:
    memcpy(peDst, peSrc, sizeof(VMMDLL_MAP_MODULEENTRY));
    // strmap below:
    f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszText, &peDst->uszText, NULL, fWideChar) &&
        ObStrMap_PushPtrUXUW(psmOb, peSrc->uszFullName, &peDst->uszFullName, NULL, fWideChar);
    if(!f) { goto fail; }
    pbMultiText = ((PBYTE)peDst) + sizeof(VMMDLL_MAP_MODULEENTRY);
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pbMultiText, &cbTMP, fWideChar);
    *ppeDst = peDst;
fail:
    if(ppeDst && !*ppeDst) { VMMDLL_MemFree(peDst); peDst = NULL; }
    Ob_DECREF(pObMapSrc);
    return *ppeDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetModuleFromNameU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_opt_ LPSTR uszModuleName, _Out_ PVMMDLL_MAP_MODULEENTRY *ppModuleMapEntry)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetModuleFromName, VMMDLL_Map_GetModuleFromName_Impl(H, dwPID, uszModuleName, ppModuleMapEntry, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetModuleFromNameW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_opt_ LPWSTR wszModuleName, _Out_ PVMMDLL_MAP_MODULEENTRY *ppModuleMapEntry)
{
    LPSTR uszModuleName = NULL;
    BYTE pbBuffer[MAX_PATH];
    if(wszModuleName) {
        if(!CharUtil_WtoU(wszModuleName, -1, pbBuffer, sizeof(pbBuffer), &uszModuleName, NULL, 0)) { return FALSE; }
    }
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetModuleFromName, VMMDLL_Map_GetModuleFromName_Impl(H, dwPID, uszModuleName, ppModuleMapEntry, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetUnloadedModule_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_UNLOADEDMODULE *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    PVMM_PROCESS pObProcess = NULL;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_UNLOADEDMODULEENTRY peDst;
    PVMM_MAP_UNLOADEDMODULEENTRY peSrc;
    PVMMOB_MAP_UNLOADEDMODULE pObMapSrc = NULL;
    PVMMDLL_MAP_UNLOADEDMODULE pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_UNLOADEDMODULEENTRY) != sizeof(VMMDLL_MAP_UNLOADEDMODULEENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetUnloadedModule(H, pObProcess, &pObMapSrc)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->uszText);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_UNLOADEDMODULEENTRY);
    cbDst = sizeof(VMMDLL_MAP_UNLOADEDMODULE) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_UNLOADEDMODULE, cbDst, sizeof(VMMDLL_MAP_UNLOADEDMODULE)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_UNLOADEDMODULE_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszText, &peDst->uszText, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetUnloadedModuleU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_UNLOADEDMODULE *ppUnloadedModuleMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetUnloadedModule, VMMDLL_Map_GetUnloadedModule_Impl(H, dwPID, ppUnloadedModuleMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetUnloadedModuleW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_UNLOADEDMODULE *ppUnloadedModuleMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetUnloadedModule, VMMDLL_Map_GetUnloadedModule_Impl(H, dwPID, ppUnloadedModuleMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetEAT_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName, _Out_ PVMMDLL_MAP_EAT *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    PVMM_PROCESS pObProcess = NULL;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMM_MAP_MODULEENTRY pModuleEntry = NULL;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_EATENTRY peDst;
    PVMM_MAP_EATENTRY peSrc;
    PVMMOB_MAP_EAT pObMapSrc = NULL;
    PVMMDLL_MAP_EAT pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_EATENTRY) != sizeof(VMMDLL_MAP_EATENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetModuleEntryEx(H, pObProcess, 0, uszModuleName, &pObModuleMap, &pModuleEntry)) { goto fail; }
    if(!VmmMap_GetEAT(H, pObProcess, pModuleEntry, &pObMapSrc)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->uszFunction);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_EATENTRY);
    cbDst = sizeof(VMMDLL_MAP_EAT) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_EAT, cbDst, sizeof(VMMDLL_MAP_EAT)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_EAT_VERSION;
    pMapDst->vaModuleBase = pObMapSrc->vaModuleBase;
    pMapDst->vaAddressOfFunctions = pObMapSrc->vaAddressOfFunctions;
    pMapDst->vaAddressOfNames = pObMapSrc->vaAddressOfNames;
    pMapDst->cNumberOfFunctions = pObMapSrc->cNumberOfFunctions;
    pMapDst->cNumberOfNames = pObMapSrc->cNumberOfNames;
    pMapDst->dwOrdinalBase = pObMapSrc->dwOrdinalBase;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszFunction, &peDst->uszFunction, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObModuleMap);
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return)
BOOL VMMDLL_Map_GetIAT_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName, _Out_ PVMMDLL_MAP_IAT *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    PVMM_PROCESS pObProcess = NULL;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMM_MAP_MODULEENTRY pModuleEntry = NULL;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_IATENTRY peDst;
    PVMM_MAP_IATENTRY peSrc;
    PVMMOB_MAP_IAT pObMapSrc = NULL;
    PVMMDLL_MAP_IAT pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_IATENTRY) != sizeof(VMMDLL_MAP_IATENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetModuleEntryEx(H, pObProcess, 0, uszModuleName, &pObModuleMap, &pModuleEntry)) { goto fail; }
    if(!VmmMap_GetIAT(H, pObProcess, pModuleEntry, &pObMapSrc)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->uszModule);
        ObStrMap_PushU(psmOb, peSrc->uszFunction);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_IATENTRY);
    cbDst = sizeof(VMMDLL_MAP_IAT) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_IAT, cbDst, sizeof(VMMDLL_MAP_IAT)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_IAT_VERSION;
    pMapDst->vaModuleBase = pObMapSrc->vaModuleBase;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszModule, &peDst->uszModule, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszFunction, &peDst->uszFunction, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObModuleMap);
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetEATU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR  uszModuleName, _Out_ PVMMDLL_MAP_EAT *ppEatMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetEAT, VMMDLL_Map_GetEAT_Impl(H, dwPID, uszModuleName, ppEatMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetEATW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPWSTR wszModuleName, _Out_ PVMMDLL_MAP_EAT *ppEatMap)
{
    LPSTR uszModuleName;
    BYTE pbBuffer[MAX_PATH];
    if(!CharUtil_WtoU(wszModuleName, -1, pbBuffer, sizeof(pbBuffer), &uszModuleName, NULL, 0)) { return FALSE; }
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetEAT, VMMDLL_Map_GetEAT_Impl(H, dwPID, uszModuleName, ppEatMap, TRUE))
}

_Success_(return) BOOL VMMDLL_Map_GetIATU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR  uszModuleName, _Out_ PVMMDLL_MAP_IAT *ppIatMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetIAT, VMMDLL_Map_GetIAT_Impl(H, dwPID, uszModuleName, ppIatMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetIATW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPWSTR wszModuleName, _Out_ PVMMDLL_MAP_IAT *ppIatMap)
{
    LPSTR uszModuleName;
    BYTE pbBuffer[MAX_PATH];
    if(!CharUtil_WtoU(wszModuleName, -1, pbBuffer, sizeof(pbBuffer), &uszModuleName, NULL, 0)) { return FALSE; }
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetIAT, VMMDLL_Map_GetIAT_Impl(H, dwPID, uszModuleName, ppIatMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetHeap_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_HEAP * ppDstMap)
{
    PVMMOB_MAP_HEAP pObMapSrc = NULL;
    PVMMDLL_MAP_HEAP pMapDst = NULL;
    PVMM_PROCESS pObProcess = NULL;
    DWORD cbData = 0;
    *ppDstMap = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetHeap(H, pObProcess, &pObMapSrc)) { goto fail; }
    cbData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_HEAPENTRY) + pObMapSrc->cSegments * sizeof(VMMDLL_MAP_HEAP_SEGMENTENTRY);
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_HEAP, sizeof(VMMDLL_MAP_HEAP) + cbData, sizeof(VMMDLL_MAP_HEAP)))) { goto fail; }      // VMMDLL_MemFree()
    pMapDst->dwVersion = VMMDLL_MAP_HEAP_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    pMapDst->cSegments = pObMapSrc->cSegments;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbData);
    pMapDst->pSegments = (PVMMDLL_MAP_HEAP_SEGMENTENTRY)(pMapDst->pMap + pMapDst->cMap);
    *ppDstMap = pMapDst;
fail:
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    return *ppDstMap ? TRUE : FALSE;
}

_Success_(return)
BOOL VMMDLL_Map_GetHeap(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_HEAP *ppHeapMap)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_Map_GetHeapEx,
        VMMDLL_Map_GetHeap_Impl(H, dwPID, ppHeapMap))
}

_Success_(return)
BOOL VMMDLL_Map_GetHeapAlloc_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ QWORD qwHeapNumOrAddress, _Out_ PVMMDLL_MAP_HEAPALLOC *ppDstMap)
{
    PVMMOB_MAP_HEAPALLOC pObMapSrc = NULL;
    PVMMDLL_MAP_HEAPALLOC pMapDst = NULL;
    PVMM_PROCESS pObProcess = NULL;
    DWORD cbData = 0;
    *ppDstMap = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetHeapAlloc(H, pObProcess, qwHeapNumOrAddress, &pObMapSrc)) { goto fail; }
    cbData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_HEAPALLOCENTRY);
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_HEAP_ALLOC, sizeof(VMMDLL_MAP_HEAPALLOC) + cbData, sizeof(VMMDLL_MAP_HEAPALLOC)))) { goto fail; }      // VMMDLL_MemFree()
    pMapDst->dwVersion = VMMDLL_MAP_HEAPALLOC_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbData);
    *ppDstMap = pMapDst;
fail:
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    return *ppDstMap ? TRUE : FALSE;
}

_Success_(return)
BOOL VMMDLL_Map_GetHeapAlloc(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ QWORD qwHeapNumOrAddress, _Out_ PVMMDLL_MAP_HEAPALLOC *ppHeapAllocMap)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_Map_GetHeapAllocEx,
        VMMDLL_Map_GetHeapAlloc_Impl(H, dwPID, qwHeapNumOrAddress, ppHeapAllocMap))
}

_Success_(return)
BOOL VMMDLL_Map_GetThread_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_THREAD *ppMapDst)
{
    PVMMOB_MAP_THREAD pObMapSrc = NULL;
    PVMMDLL_MAP_THREAD pMapDst = NULL;
    PVMM_PROCESS pObProcess = NULL;
    DWORD cbData = 0;
    *ppMapDst = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetThread(H, pObProcess, &pObMapSrc)) { goto fail; }
    cbData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_THREADENTRY);
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_THREAD, sizeof(VMMDLL_MAP_THREAD) + cbData, sizeof(VMMDLL_MAP_THREAD)))) { goto fail; }      // VMMDLL_MemFree()
    pMapDst->dwVersion = VMMDLL_MAP_THREAD_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbData);
    *ppMapDst = pMapDst;
fail:
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    return *ppMapDst ? TRUE : FALSE;
}

EXPORTED_FUNCTION
_Success_(return) BOOL VMMDLL_Map_GetThread(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_THREAD *ppThreadMap)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_Map_GetThread,
        VMMDLL_Map_GetThread_Impl(H, dwPID, ppThreadMap))
}

_Success_(return)
BOOL VMMDLL_Map_GetHandle_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_HANDLE *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f;
    PVMMWIN_OBJECT_TYPE pOT;
    PVMM_PROCESS pObProcess = NULL;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_HANDLEENTRY peDst;
    PVMM_MAP_HANDLEENTRY peSrc;
    PVMMOB_MAP_HANDLE pObMapSrc = NULL;
    PVMMDLL_MAP_HANDLE pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_HANDLEENTRY) != sizeof(VMMDLL_MAP_HANDLEENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetHandle(H, pObProcess, &pObMapSrc, TRUE)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        pOT = VmmWin_ObjectTypeGet(H, (BYTE)peSrc->iType);
        ObStrMap_PushU(psmOb, (pOT ? pOT->usz : NULL));
        ObStrMap_PushU(psmOb, peSrc->uszText);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_HANDLEENTRY);
    cbDst = sizeof(VMMDLL_MAP_HANDLE) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_HANDLE, cbDst, sizeof(VMMDLL_MAP_HANDLE)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_HANDLE_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        pOT = VmmWin_ObjectTypeGet(H, (BYTE)peDst->iType);
        f = ObStrMap_PushPtrUXUW(psmOb, (pOT ? pOT->usz : NULL), &peDst->uszType, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszText, &peDst->uszText, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObProcess);
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetHandleU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_HANDLE *ppHandleMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetHandle, VMMDLL_Map_GetHandle_Impl(H, dwPID, ppHandleMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetHandleW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Out_ PVMMDLL_MAP_HANDLE *ppHandleMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetHandle, VMMDLL_Map_GetHandle_Impl(H, dwPID, ppHandleMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetPhysMem_Impl(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_PHYSMEM *ppMapDst)
{
    DWORD cbDst = 0, cbDstData = 0;
    PVMMOB_MAP_PHYSMEM pObMap = NULL;
    PVMMDLL_MAP_PHYSMEM pMapDst = NULL;
    *ppMapDst = NULL;
    if(!VmmMap_GetPhysMem(H, &pObMap)) { goto fail; }
    cbDstData = pObMap->cMap * sizeof(VMMDLL_MAP_PHYSMEMENTRY);
    cbDst = sizeof(VMMDLL_MAP_PHYSMEM) + cbDstData;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_PHYSMEM, cbDst, sizeof(VMMDLL_MAP_PHYSMEM)))) { goto fail; }      // VMMDLL_MemFree()
    pMapDst->dwVersion = VMMDLL_MAP_PHYSMEM_VERSION;
    pMapDst->cMap = pObMap->cMap;
    memcpy(pMapDst->pMap, pObMap->pMap, cbDstData);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObMap);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return)
BOOL VMMDLL_Map_GetPhysMem(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_PHYSMEM *ppPhysMemMap)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_Map_GetPhysMem,
        VMMDLL_Map_GetPhysMem_Impl(H, ppPhysMemMap))
}

_Success_(return)
BOOL VMMDLL_Map_GetPool_Impl(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_POOL *ppPoolMap, _In_ DWORD flags)
{
    DWORD cbDst = 0, cbDstData, cbDstDataMap, cbDstDataTag;
    PVMMDLL_MAP_POOL pMapDst = NULL;
    PVMMOB_MAP_POOL pObMap = NULL;
    if(!VmmMap_GetPool(H, &pObMap, (flags != VMMDLL_POOLMAP_FLAG_BIG))) { goto fail; }
    cbDstDataMap = pObMap->cMap * sizeof(VMMDLL_MAP_POOLENTRY);
    cbDstDataTag = pObMap->cTag * sizeof(VMMDLL_MAP_POOLENTRYTAG);
    cbDstData = cbDstDataMap + cbDstDataTag + pObMap->cMap * sizeof(DWORD);
    cbDst = sizeof(VMMDLL_MAP_POOL) + cbDstData;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_POOL, cbDst, sizeof(VMMDLL_MAP_POOL)))) { goto fail; }      // VMMDLL_MemFree()
    if(pMapDst) {
        ZeroMemory(pMapDst, sizeof(VMMDLL_MAP_POOL));
        pMapDst->dwVersion = VMMDLL_MAP_POOL_VERSION;
        pMapDst->cbTotal = cbDst;
        pMapDst->cMap = pObMap->cMap;
        memcpy(pMapDst->pMap, pObMap->pMap, cbDstData);
        // tag
        pMapDst->cTag = pObMap->cTag;
        pMapDst->pTag = (PVMMDLL_MAP_POOLENTRYTAG)(pMapDst->pMap + pMapDst->cMap);
        // tag index
        pMapDst->piTag2Map = (PDWORD)((QWORD)pMapDst->pTag + cbDstDataTag);
    }
    *ppPoolMap = pMapDst;
    Ob_DECREF(pObMap);
    return TRUE;
fail:
    *ppPoolMap = NULL;
    VMMDLL_MemFree(pMapDst);
    Ob_DECREF(pObMap);
    return FALSE;
}

_Success_(return)
BOOL VMMDLL_Map_GetPool(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_POOL* ppPoolMap, _In_ DWORD flags)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_Map_GetPool,
        VMMDLL_Map_GetPool_Impl(H, ppPoolMap, flags))
}

_Success_(return)
BOOL VMMDLL_Map_GetNet_Impl(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_NET *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_NETENTRY peDst;
    PVMM_MAP_NETENTRY peSrc;
    PVMMOB_MAP_NET pObMapSrc = NULL;
    PVMMDLL_MAP_NET pMapDst = NULL;
    POB_STRMAP psm = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_NETENTRY) != sizeof(VMMDLL_MAP_NETENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psm = ObStrMap_New(H, 0))) { goto fail; }
    if(!VmmMap_GetNet(H, &pObMapSrc)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psm, peSrc->Src.uszText);
        ObStrMap_PushU(psm, peSrc->Dst.uszText);
        ObStrMap_PushU(psm, peSrc->uszText);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psm, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_NETENTRY);
    cbDst = sizeof(VMMDLL_MAP_NET) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_NET, cbDst, sizeof(VMMDLL_MAP_NET)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_NET_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psm, peSrc->Src.uszText, &peDst->Src.uszText, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psm, peSrc->Dst.uszText, &peDst->Dst.uszText, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psm, peSrc->uszText, &peDst->uszText, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psm, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psm);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetNetU(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_NET *ppNetMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetNet, VMMDLL_Map_GetNet_Impl(H, ppNetMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetNetW(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_NET *ppNetMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetNet, VMMDLL_Map_GetNet_Impl(H, ppNetMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetUsers_Impl(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_USER *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_USERENTRY peDst;
    PVMM_MAP_USERENTRY peSrc;
    PVMMOB_MAP_USER pObMapSrc = NULL;
    PVMMDLL_MAP_USER pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!VmmMap_GetUser(H, &pObMapSrc)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->szSID);
        ObStrMap_PushU(psmOb, peSrc->uszText);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_USERENTRY);
    cbDst = sizeof(VMMDLL_MAP_USER) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_USER, cbDst, sizeof(VMMDLL_MAP_USER)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map [if required]:
    pMapDst->dwVersion = VMMDLL_MAP_USER_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    for(i = 0; i < pMapDst->cMap; i++) {
        peDst = pMapDst->pMap + i;
        peDst->vaRegHive = pObMapSrc->pMap[i].vaRegHive;
        // strmap below:
        for(i = 0; i < pMapDst->cMap; i++) {
            peSrc = pObMapSrc->pMap + i;
            peDst = pMapDst->pMap + i;
            f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszText, &peDst->uszText, NULL, fWideChar) &&
                ObStrMap_PushPtrUXUW(psmOb, peSrc->szSID, &peDst->uszSID, NULL, fWideChar);
            if(!f) { goto fail; }
        }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetUsersU(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_USER *ppUserMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetUsers, VMMDLL_Map_GetUsers_Impl(H, ppUserMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetUsersW(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_USER *ppUserMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetUsers, VMMDLL_Map_GetUsers_Impl(H, ppUserMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetServices_Impl(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_SERVICE *ppMapDst, _In_ BOOL fWideChar)
{
    BOOL f, fResult = FALSE;
    DWORD i, cbDst = 0, cbDstData, cbDstStr;
    PVMMDLL_MAP_SERVICEENTRY peDst;
    PVMM_MAP_SERVICEENTRY peSrc;
    PVMMOB_MAP_SERVICE pObMapSrc = NULL;
    PVMMDLL_MAP_SERVICE pMapDst = NULL;
    POB_STRMAP psmOb = NULL;
    *ppMapDst = NULL;
    // 0: sanity check:
    if(sizeof(VMM_MAP_SERVICEENTRY) != sizeof(VMMDLL_MAP_SERVICEENTRY)) { goto fail; }
    // 1: fetch map [and populate strings]:
    if(!(psmOb = ObStrMap_New(H, 0))) { goto fail; }
    if(!VmmMap_GetService(H, &pObMapSrc)) { goto fail; }
    for(i = 0; i < pObMapSrc->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        ObStrMap_PushU(psmOb, peSrc->uszServiceName);
        ObStrMap_PushU(psmOb, peSrc->uszDisplayName);
        ObStrMap_PushU(psmOb, peSrc->uszPath);
        ObStrMap_PushU(psmOb, peSrc->uszUserTp);
        ObStrMap_PushU(psmOb, peSrc->uszUserAcct);
        ObStrMap_PushU(psmOb, peSrc->uszImagePath);
    }
    // 2: byte count & alloc:
    if(!ObStrMap_FinalizeBufferXUW(psmOb, 0, NULL, &cbDstStr, fWideChar)) { goto fail; }
    cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_SERVICEENTRY);
    cbDst = sizeof(VMMDLL_MAP_SERVICE) + cbDstData + cbDstStr;
    if(!(pMapDst = VmmDllCore_MemAllocExternal(H, OB_TAG_API_MAP_SERVICES, cbDst, sizeof(VMMDLL_MAP_SERVICE)))) { goto fail; }    // VMMDLL_MemFree()
    // 3: fill map:
    pMapDst->dwVersion = VMMDLL_MAP_SERVICE_VERSION;
    pMapDst->cMap = pObMapSrc->cMap;
    memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    // strmap below:
    for(i = 0; i < pMapDst->cMap; i++) {
        peSrc = pObMapSrc->pMap + i;
        peDst = pMapDst->pMap + i;
        f = ObStrMap_PushPtrUXUW(psmOb, peSrc->uszServiceName, &peDst->uszServiceName, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszDisplayName, &peDst->uszDisplayName, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszPath, &peDst->uszPath, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszUserTp, &peDst->uszUserTp, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszUserAcct, &peDst->uszUserAcct, NULL, fWideChar) &&
            ObStrMap_PushPtrUXUW(psmOb, peSrc->uszImagePath, &peDst->uszImagePath, NULL, fWideChar);
        if(!f) { goto fail; }
    }
    pMapDst->pbMultiText = ((PBYTE)pMapDst->pMap) + cbDstData;
    ObStrMap_FinalizeBufferXUW(psmOb, cbDstStr, pMapDst->pbMultiText, &pMapDst->cbMultiText, fWideChar);
    *ppMapDst = pMapDst;
fail:
    if(pMapDst && !*ppMapDst) { VMMDLL_MemFree(pMapDst); pMapDst = NULL; }
    Ob_DECREF(pObMapSrc);
    Ob_DECREF(psmOb);
    return *ppMapDst ? TRUE : FALSE;
}

_Success_(return) BOOL VMMDLL_Map_GetServicesU(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_SERVICE *ppServiceMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetServices, VMMDLL_Map_GetServices_Impl(H, ppServiceMap, FALSE))
}

_Success_(return) BOOL VMMDLL_Map_GetServicesW(_In_ VMM_HANDLE H, _Out_ PVMMDLL_MAP_SERVICE *ppServiceMap)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Map_GetServices, VMMDLL_Map_GetServices_Impl(H, ppServiceMap, TRUE))
}

_Success_(return)
BOOL VMMDLL_Map_GetPfn_Impl(_In_ VMM_HANDLE H, _In_ DWORD pPfns[], _In_ DWORD cPfns, _Out_writes_bytes_opt_(*pcbMapDst) PVMMDLL_MAP_PFN pMapDst, _Inout_ PDWORD pcbMapDst)
{
    BOOL fResult = FALSE;
    POB_SET psObPfns = NULL;
    PMMPFNOB_MAP pObMapSrc = NULL;
    DWORD i, cbDst = 0, cbDstData;
    cbDstData = cPfns * sizeof(VMMDLL_MAP_PFNENTRY);
    cbDst = sizeof(VMMDLL_MAP_PFN) + cbDstData;
    if(pMapDst) {
        if(*pcbMapDst < cbDst) { goto fail; }
        if(!(psObPfns = ObSet_New(H))) { goto fail; }
        for(i = 0; i < cPfns; i++) {
            ObSet_Push(psObPfns, pPfns[i]);
        }
        if(!MmPfn_Map_GetPfnScatter(H, psObPfns, &pObMapSrc, TRUE)) { goto fail; }
        ZeroMemory(pMapDst, cbDst);
        pMapDst->dwVersion = VMMDLL_MAP_PFN_VERSION;
        pMapDst->cMap = pObMapSrc->cMap;
        cbDstData = pObMapSrc->cMap * sizeof(VMMDLL_MAP_PFNENTRY);
        cbDst = sizeof(VMMDLL_MAP_PFN) + cbDstData;
        memcpy(pMapDst->pMap, pObMapSrc->pMap, cbDstData);
    }
    fResult = TRUE;
fail:
    *pcbMapDst = cbDst;
    Ob_DECREF(psObPfns);
    Ob_DECREF(pObMapSrc);
    return fResult;
}

_Success_(return)
BOOL VMMDLL_Map_GetPfn(_In_ VMM_HANDLE H, _In_ DWORD pPfns[], _In_ DWORD cPfns, _Out_writes_bytes_opt_(*pcbPfnMap) PVMMDLL_MAP_PFN pPfnMap, _Inout_ PDWORD pcbPfnMap)
{
    CALL_IMPLEMENTATION_VMM(H,
        STATISTICS_ID_VMMDLL_Map_GetPfn,
        VMMDLL_Map_GetPfn_Impl(H, pPfns, cPfns, pPfnMap, pcbPfnMap))
}

_Success_(return)
BOOL VMMDLL_PidList_Impl(_In_ VMM_HANDLE H, _Out_writes_opt_(*pcPIDs) PDWORD pPIDs, _Inout_ PSIZE_T pcPIDs)
{
    VmmProcessListPIDs(H, pPIDs, pcPIDs, 0);
    return (*pcPIDs ? TRUE : FALSE);
}

_Success_(return)
BOOL VMMDLL_PidList(_In_ VMM_HANDLE H, _Out_writes_opt_(*pcPIDs) PDWORD pPIDs, _Inout_ PSIZE_T pcPIDs)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_PidList, VMMDLL_PidList_Impl(H, pPIDs, pcPIDs))
}

_Success_(return)
BOOL VMMDLL_PidGetFromName_Impl(_In_ VMM_HANDLE H, _In_ LPSTR szProcName, _Out_ PDWORD pdwPID)
{
    PVMM_PROCESS pObProcess = NULL;
    // 1: try locate process using long (full) name
    while((pObProcess = VmmProcessGetNext(H, pObProcess, 0))) {
        if(pObProcess->dwState) { continue; }
        if(!pObProcess->pObPersistent->uszNameLong || _stricmp(szProcName, pObProcess->pObPersistent->uszNameLong)) { continue; }
        *pdwPID = pObProcess->dwPID;
        Ob_DECREF(pObProcess);
        return TRUE;
    }
    // 2: try locate process using short (eprocess) name
    while((pObProcess = VmmProcessGetNext(H, pObProcess, 0))) {
        if(pObProcess->dwState) { continue; }
        if(_strnicmp(szProcName, pObProcess->szName, 15)) { continue; }
        *pdwPID = pObProcess->dwPID;
        Ob_DECREF(pObProcess);
        return TRUE;
    }
    return FALSE;
}

_Success_(return)
BOOL VMMDLL_PidGetFromName(_In_ VMM_HANDLE H, _In_ LPSTR szProcName, _Out_ PDWORD pdwPID)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_PidGetFromName,
        VMMDLL_PidGetFromName_Impl(H, szProcName, pdwPID))
}

_Success_(return)
BOOL VMMDLL_ProcessGetInformation_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_opt_ PVMMDLL_PROCESS_INFORMATION pInfo, _In_ PSIZE_T pcbProcessInfo)
{
    PVMM_PROCESS pObProcess = NULL;
    if(!pcbProcessInfo) { return FALSE; }
    if(!pInfo) {
        *pcbProcessInfo = sizeof(VMMDLL_PROCESS_INFORMATION);
        return TRUE;
    }
    if(*pcbProcessInfo < sizeof(VMMDLL_PROCESS_INFORMATION)) { return FALSE; }
    if(pInfo->magic != VMMDLL_PROCESS_INFORMATION_MAGIC) { return FALSE; }
    if(pInfo->wVersion != VMMDLL_PROCESS_INFORMATION_VERSION) { return FALSE; }
    if(!(pObProcess = VmmProcessGetEx(H, NULL, dwPID, VMM_FLAG_PROCESS_TOKEN))) { return FALSE; }
    ZeroMemory(pInfo, sizeof(VMMDLL_PROCESS_INFORMATION_MAGIC));
    // set general parameters
    pInfo->magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    pInfo->wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
    pInfo->wSize = sizeof(VMMDLL_PROCESS_INFORMATION);
    pInfo->tpMemoryModel = (VMMDLL_MEMORYMODEL_TP)H->vmm.tpMemoryModel;
    pInfo->tpSystem = (VMMDLL_SYSTEM_TP)H->vmm.tpSystem;
    pInfo->fUserOnly = pObProcess->fUserOnly;
    pInfo->dwPID = dwPID;
    pInfo->dwPPID = pObProcess->dwPPID;
    pInfo->dwState = pObProcess->dwState;
    pInfo->paDTB = pObProcess->paDTB;
    pInfo->paDTB_UserOpt = pObProcess->paDTB_UserOpt;
    memcpy(pInfo->szName, pObProcess->szName, sizeof(pInfo->szName));
    strncpy_s(pInfo->szNameLong, sizeof(pInfo->szNameLong), pObProcess->pObPersistent->uszNameLong, _TRUNCATE);
    // set operating system specific parameters
    if((H->vmm.tpSystem == VMM_SYSTEM_WINDOWS_X64) || (H->vmm.tpSystem == VMM_SYSTEM_WINDOWS_X86)) {
        if(H->vmm.tpSystem == VMM_SYSTEM_WINDOWS_X64) {
            pInfo->win.fWow64 = pObProcess->win.fWow64;
            pInfo->win.vaPEB32 = pObProcess->win.vaPEB32;
        }
        pInfo->win.vaEPROCESS = pObProcess->win.EPROCESS.va;
        pInfo->win.vaPEB = pObProcess->win.vaPEB;
        pInfo->win.qwLUID = pObProcess->win.TOKEN.qwLUID;
        pInfo->win.dwSessionId = pObProcess->win.TOKEN.dwSessionId;
        if(pObProcess->win.TOKEN.szSID) {
            strncpy_s(pInfo->win.szSID, sizeof(pInfo->win.szSID), pObProcess->win.TOKEN.szSID, _TRUNCATE);
        }
        pInfo->win.IntegrityLevel = (VMMDLL_PROCESS_INTEGRITY_LEVEL)pObProcess->win.TOKEN.IntegrityLevel;
    }
    Ob_DECREF(pObProcess);
    return TRUE;
}

_Success_(return)
BOOL VMMDLL_ProcessGetInformation(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _Inout_opt_ PVMMDLL_PROCESS_INFORMATION pProcessInformation, _In_ PSIZE_T pcbProcessInformation)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_ProcessGetInformation, VMMDLL_ProcessGetInformation_Impl(H, dwPID, pProcessInformation, pcbProcessInformation))
}

BOOL VMMDLL_ProcessGetInformationString_Impl_CallbackCriteria(_In_ VMM_HANDLE H, _In_ PVMM_PROCESS pProcess, _In_ PVOID ctx)
{
    return !pProcess->pObPersistent->UserProcessParams.fProcessed;
}

VOID VMMDLL_ProcessGetInformationString_Impl_CallbackAction(_In_ VMM_HANDLE H, _In_ PVMM_PROCESS pProcess, _In_ PVOID ctx)
{
    VmmWin_UserProcessParameters_Get(H, pProcess);
}

_Success_(return != NULL)
LPSTR VMMDLL_ProcessGetInformationString_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ DWORD fOptionString)
{
    SIZE_T csz;
    LPSTR sz = NULL, szStrDup = NULL;
    PVMM_PROCESS pObProcess = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { return FALSE; }
    switch(fOptionString) {
        case VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_USER_IMAGE:
        case VMMDLL_PROCESS_INFORMATION_OPT_STRING_CMDLINE:
            if(!pObProcess->pObPersistent->UserProcessParams.fProcessed) {
                VmmWork_ProcessActionForeachParallel_Void(H, 0, NULL, VMMDLL_ProcessGetInformationString_Impl_CallbackCriteria, VMMDLL_ProcessGetInformationString_Impl_CallbackAction);
            }
    }
    switch(fOptionString) {
        case VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_KERNEL:
            sz = pObProcess->pObPersistent->uszPathKernel;
            break;
        case VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_USER_IMAGE:
            sz = pObProcess->pObPersistent->UserProcessParams.uszImagePathName;
            break;
        case VMMDLL_PROCESS_INFORMATION_OPT_STRING_CMDLINE:
            sz = pObProcess->pObPersistent->UserProcessParams.uszCommandLine;
            break;
    }
    if(sz) {
        csz = strlen(sz);
        if((szStrDup = VmmDllCore_MemAllocExternal(H, OB_TAG_API_PROCESS_STRING, csz + 1, 0))) {
            strncpy_s(szStrDup, csz + 1, sz, _TRUNCATE);
        }
    }
    Ob_DECREF(pObProcess);
    return szStrDup;
}

_Success_(return != NULL)
LPSTR VMMDLL_ProcessGetInformationString(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ DWORD fOptionString)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_ProcessGetInformationString,
        LPSTR,
        NULL,
        VMMDLL_ProcessGetInformationString_Impl(H, dwPID, fOptionString))
}

_Success_(return)
BOOL VMMDLL_ProcessGet_Sections_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModule, _Out_writes_opt_(cSections) PIMAGE_SECTION_HEADER pSections, _In_ DWORD cSections, _Out_ PDWORD pcSections)
{
    BOOL fResult = FALSE;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PVMM_PROCESS pObProcess = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetModuleEntryEx(H, pObProcess, 0, uszModule, &pObModuleMap, &pModule)) { goto fail; }
    *pcSections = PE_SectionGetNumberOf(H, pObProcess, pModule->vaBase);
    if(pSections) {
        if(cSections != *pcSections) { goto fail; }
        if(!PE_SectionGetAll(H, pObProcess, pModule->vaBase, cSections, pSections)) { goto fail; }
    }
    fResult = TRUE;
fail:
    Ob_DECREF(pObModuleMap);
    Ob_DECREF(pObProcess);
    return fResult;
}

_Success_(return)
BOOL VMMDLL_ProcessGet_Directories_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModule, _Out_writes_(16) PIMAGE_DATA_DIRECTORY pDataDirectories)
{
    BOOL fResult = FALSE;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PVMM_PROCESS pObProcess = NULL;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    // fetch requested module
    if(!VmmMap_GetModuleEntryEx(H, pObProcess, 0, uszModule, &pObModuleMap, &pModule)) { goto fail; }
    // data directories
    if(!PE_DirectoryGetAll(H, pObProcess, pModule->vaBase, NULL, pDataDirectories)) { goto fail; }
    fResult = TRUE;
fail:
    Ob_DECREF(pObModuleMap);
    Ob_DECREF(pObProcess);
    return fResult;
}

_Success_(return)
BOOL VMMDLL_ProcessGetDirectoriesU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModule, _Out_writes_(16) PIMAGE_DATA_DIRECTORY pDataDirectories)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_ProcessGetDirectories, VMMDLL_ProcessGet_Directories_Impl(H, dwPID, uszModule, pDataDirectories))
}

_Success_(return)
BOOL VMMDLL_ProcessGetDirectoriesW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPWSTR wszModule, _Out_writes_(16) PIMAGE_DATA_DIRECTORY pDataDirectories)
{
    LPSTR uszModule;
    BYTE pbBuffer[MAX_PATH];
    if(!CharUtil_WtoU(wszModule, -1, pbBuffer, sizeof(pbBuffer), &uszModule, NULL, 0)) { return FALSE; }
    return VMMDLL_ProcessGetDirectoriesU(H, dwPID, uszModule, pDataDirectories);
}

_Success_(return)
BOOL VMMDLL_ProcessGetSectionsU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR  uszModule, _Out_writes_opt_(cSections) PIMAGE_SECTION_HEADER pSections, _In_ DWORD cSections, _Out_ PDWORD pcSections)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_ProcessGetSections,
        VMMDLL_ProcessGet_Sections_Impl(H, dwPID, uszModule, pSections, cSections, pcSections))
}

_Success_(return)
BOOL VMMDLL_ProcessGetSectionsW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPWSTR wszModule, _Out_writes_opt_(cSections) PIMAGE_SECTION_HEADER pSections, _In_ DWORD cSections, _Out_ PDWORD pcSections)
{
    LPSTR uszModule;
    BYTE pbBuffer[MAX_PATH];
    if(!CharUtil_WtoU(wszModule, -1, pbBuffer, sizeof(pbBuffer), &uszModule, NULL, 0)) { return FALSE; }
    return VMMDLL_ProcessGetSectionsU(H, dwPID, uszModule, pSections, cSections, pcSections);
}

ULONG64 VMMDLL_ProcessGetModuleBase_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName)
{
    QWORD vaModuleBase = 0;
    PVMM_MAP_MODULEENTRY peModule;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    if(VmmMap_GetModuleEntryEx(H, NULL, dwPID, uszModuleName, &pObModuleMap, &peModule)) {
        vaModuleBase = peModule->vaBase;
        Ob_DECREF(pObModuleMap);
    }
    return vaModuleBase;
}

ULONG64 VMMDLL_ProcessGetModuleBaseU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_ProcessGetModuleBase,
        ULONG64,
        0,
        VMMDLL_ProcessGetModuleBase_Impl(H, dwPID, uszModuleName))
}

ULONG64 VMMDLL_ProcessGetModuleBaseW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPWSTR wszModuleName)
{
    LPSTR uszModuleName;
    BYTE pbBuffer[MAX_PATH];
    if(!CharUtil_WtoU(wszModuleName, -1, pbBuffer, sizeof(pbBuffer), &uszModuleName, NULL, 0)) { return FALSE; }
    return VMMDLL_ProcessGetModuleBaseU(H, dwPID, uszModuleName);
}

ULONG64 VMMDLL_ProcessGetProcAddress_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName, _In_ LPSTR szFunctionName)
{
    PVMM_PROCESS pObProcess = NULL;
    PVMMOB_MAP_EAT pObEatMap = NULL;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMM_MAP_MODULEENTRY peModule;
    QWORD va = 0;
    DWORD i;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { goto fail; }
    if(!VmmMap_GetModuleEntryEx(H, NULL, dwPID, uszModuleName, &pObModuleMap, &peModule)) { goto fail; }
    if(!VmmMap_GetEAT(H, pObProcess, peModule, &pObEatMap)) { goto fail; }
    if(!VmmMap_GetEATEntryIndexU(H, pObEatMap, szFunctionName, &i)) { goto fail; }
    va = pObEatMap->pMap[i].vaFunction;
fail:
    Ob_DECREF(pObEatMap);
    Ob_DECREF(pObModuleMap);
    Ob_DECREF(pObProcess);
    return va;
}

ULONG64 VMMDLL_ProcessGetProcAddressU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName, _In_ LPSTR szFunctionName)
{
    CALL_IMPLEMENTATION_VMM_RETURN(
        H,
        STATISTICS_ID_VMMDLL_ProcessGetProcAddress,
        ULONG64,
        0,
        VMMDLL_ProcessGetProcAddress_Impl(H, dwPID, uszModuleName, szFunctionName))
}

ULONG64 VMMDLL_ProcessGetProcAddressW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPWSTR wszModuleName, _In_ LPSTR szFunctionName)
{
    LPSTR uszModuleName;
    BYTE pbBuffer[MAX_PATH];
    if(!CharUtil_WtoU(wszModuleName, -1, pbBuffer, sizeof(pbBuffer), &uszModuleName, NULL, 0)) { return FALSE; }
    return VMMDLL_ProcessGetProcAddressU(H, dwPID, uszModuleName, szFunctionName);
}



//-----------------------------------------------------------------------------
// LOGGING FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

_Success_(return)
BOOL VMMDLL_LogEx2_Impl(_In_ VMM_HANDLE H, _In_opt_ VMMDLL_MODULE_ID MID, _In_ VMMDLL_LOGLEVEL dwLogLevel, _In_z_ _Printf_format_string_ LPSTR uszFormat, va_list arglist)
{
    if(MID & 0x80000000) {
        if((MID < VMMDLL_MID_MAIN) && (MID > VMMDLL_MID_PYTHON)) {
            return FALSE;
        }
    }
    VmmLogEx2(H, (DWORD)MID, (VMMLOG_LEVEL)dwLogLevel, uszFormat, arglist);
    return TRUE;
}

_Success_(return)
BOOL VMMDLL_LogEx2(_In_ VMM_HANDLE H, _In_opt_ VMMDLL_MODULE_ID MID, _In_ VMMDLL_LOGLEVEL dwLogLevel, _In_z_ _Printf_format_string_ LPSTR uszFormat, va_list arglist)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_Log, VMMDLL_LogEx2_Impl(H, MID, dwLogLevel, uszFormat, arglist))
}

VOID VMMDLL_LogEx(_In_ VMM_HANDLE H, _In_opt_ VMMDLL_MODULE_ID MID, _In_ VMMDLL_LOGLEVEL dwLogLevel, _In_z_ _Printf_format_string_ LPSTR uszFormat, va_list arglist)
{
    VMMDLL_LogEx2(H, MID, dwLogLevel, uszFormat, arglist);
}

VOID VMMDLL_Log(_In_ VMM_HANDLE H, _In_opt_ VMMDLL_MODULE_ID MID, _In_ VMMDLL_LOGLEVEL dwLogLevel, _In_z_ _Printf_format_string_ LPSTR uszFormat, ...)
{
    va_list arglist;
    va_start(arglist, uszFormat);
    VMMDLL_LogEx2(H, MID, dwLogLevel, uszFormat, arglist);
    va_end(arglist);
}



//-----------------------------------------------------------------------------
// WINDOWS SPECIFIC REGISTRY FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

/*
* Retrieve information about the registry hives in the target system.
* -- pHives = buffer of cHives * sizeof(VMMDLL_REGISTRY_HIVE_INFORMATION) to receive information about all hives. NULL to receive # hives in pcHives.
* -- cHives
* -- pcHives = if pHives == NULL: # total hives. if pHives: # read hives.
* -- return
*/
_Success_(return)
BOOL VMMDLL_WinReg_HiveList_Impl(_In_ VMM_HANDLE H, _Out_writes_(cHives) PVMMDLL_REGISTRY_HIVE_INFORMATION pHives, _In_ DWORD cHives, _Out_ PDWORD pcHives)
{
    BOOL fResult = TRUE;
    POB_REGISTRY_HIVE pObHive = NULL;
    if(!pHives) {
        *pcHives = VmmWinReg_HiveCount(H);
        goto cleanup;
    }
    *pcHives = 0;
    while((pObHive = VmmWinReg_HiveGetNext(H, pObHive))) {
        if(*pcHives == cHives) {
            fResult = FALSE;
            goto cleanup;
        }
        memcpy(pHives + (*pcHives), pObHive, sizeof(VMMDLL_REGISTRY_HIVE_INFORMATION));
        pHives[*pcHives].magic = VMMDLL_REGISTRY_HIVE_INFORMATION_MAGIC;
        pHives[*pcHives].wVersion = VMMDLL_REGISTRY_HIVE_INFORMATION_VERSION;
        pHives[*pcHives].wSize = sizeof(VMMDLL_REGISTRY_HIVE_INFORMATION);
        *pcHives += 1;
    }
cleanup:
    Ob_DECREF(pObHive);
    return fResult;
}

_Success_(return)
BOOL VMMDLL_WinReg_HiveList(_In_ VMM_HANDLE H, _Out_writes_(cHives) PVMMDLL_REGISTRY_HIVE_INFORMATION pHives, _In_ DWORD cHives, _Out_ PDWORD pcHives)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_WinRegHive_List,
        VMMDLL_WinReg_HiveList_Impl(H, pHives, cHives, pcHives))
}

/*
* Read a contigious arbitrary amount of registry hive memory and report the
* number of bytes read in pcbRead.
* NB! Address space does not include regf registry hive file header!
* -- vaCMHive
* -- ra
* -- pb
* -- cb
* -- pcbRead
* -- flags = flags as in VMMDLL_FLAG_*
*/
_Success_(return)
BOOL VMMDLL_WinReg_HiveReadEx_Impl(_In_ VMM_HANDLE H, _In_ ULONG64 vaCMHive, _In_ DWORD ra, _Out_ PBYTE pb, _In_ DWORD cb, _Out_opt_ PDWORD pcbReadOpt, _In_ ULONG64 flags)
{
    POB_REGISTRY_HIVE pObHive = VmmWinReg_HiveGetByAddress(H, vaCMHive);
    if(!pObHive) { return FALSE; }
    VmmWinReg_HiveReadEx(H, pObHive, ra, pb, cb, pcbReadOpt, flags);
    Ob_DECREF(pObHive);
    return TRUE;
}

_Success_(return)
BOOL VMMDLL_WinReg_HiveReadEx(_In_ VMM_HANDLE H, _In_ ULONG64 vaCMHive, _In_ DWORD ra, _Out_ PBYTE pb, _In_ DWORD cb, _Out_opt_ PDWORD pcbReadOpt, _In_ ULONG64 flags)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_WinRegHive_ReadEx,
        VMMDLL_WinReg_HiveReadEx_Impl(H, vaCMHive, ra, pb, cb, pcbReadOpt, flags))
}

/*
* Write a virtually contigious arbitrary amount of memory to a registry hive.
* NB! Address space does not include regf registry hive file header!
* -- vaCMHive
* -- ra
* -- pb
* -- cb
* -- return = TRUE on success, FALSE on partial or zero write.
*/
_Success_(return)
BOOL VMMDLL_WinReg_HiveWrite_Impl(_In_ VMM_HANDLE H, _In_ ULONG64 vaCMHive, _In_ DWORD ra, _In_ PBYTE pb, _In_ DWORD cb)
{
    BOOL f;
    POB_REGISTRY_HIVE pObHive = VmmWinReg_HiveGetByAddress(H, vaCMHive);
    if(!pObHive) { return FALSE; }
    f = VmmWinReg_HiveWrite(H, pObHive, ra, pb, cb);
    Ob_DECREF(pObHive);
    return f;
}

_Success_(return)
BOOL VMMDLL_WinReg_HiveWrite(_In_ VMM_HANDLE H, _In_ ULONG64 vaCMHive, _In_ DWORD ra, _In_ PBYTE pb, _In_ DWORD cb)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_WinRegHive_Write,
        VMMDLL_WinReg_HiveWrite_Impl(H, vaCMHive, ra, pb, cb))
}

_Success_(return)
BOOL VMMDLL_WinReg_EnumKeyEx_Impl(_In_ VMM_HANDLE H, _In_opt_ LPSTR uszFullPathKey, _In_opt_ LPWSTR wszFullPathKey, _In_ DWORD dwIndex, _Out_writes_opt_(cbName) PBYTE pbName, _In_ DWORD cbName, _Out_ PDWORD pcchName, _Out_opt_ PFILETIME lpftLastWriteTime)
{
    BOOL f;
    VMM_REGISTRY_KEY_INFO KeyInfo = { 0 };
    CHAR uszPathKey[MAX_PATH];
    POB_REGISTRY_HIVE pObHive = NULL;
    POB_REGISTRY_KEY pObKey = NULL, pObSubKey = NULL;
    POB_MAP pmObSubKeys = NULL;
    BYTE pbBuffer[2 * MAX_PATH];
    *pcchName = 0;
    if(wszFullPathKey) {
        if(!CharUtil_WtoU(wszFullPathKey, -1, pbBuffer, sizeof(pbBuffer), &uszFullPathKey, NULL, 0)) { return FALSE; }
    }
    if(!uszFullPathKey) { return FALSE; }
    if(pbName && !cbName) {
        if(lpftLastWriteTime) { *(PQWORD)lpftLastWriteTime = 0; }
        return FALSE;
    }
    f = VmmWinReg_PathHiveGetByFullPath(H, uszFullPathKey, &pObHive, uszPathKey) &&
        (pObKey = VmmWinReg_KeyGetByPath(H, pObHive, uszPathKey));
    if(f) {
        if(f && (dwIndex == (DWORD)-1)) {
            // actual key
            VmmWinReg_KeyInfo(pObHive, pObKey, &KeyInfo);
        } else {
            // subkeys
            f = (pmObSubKeys = VmmWinReg_KeyList(H, pObHive, pObKey)) &&
                (pObSubKey = ObMap_GetByIndex(pmObSubKeys, dwIndex));
            if(f) { VmmWinReg_KeyInfo(pObHive, pObSubKey, &KeyInfo); }
        }
    }
    if(wszFullPathKey) {
        f = f && CharUtil_UtoW(KeyInfo.uszName, -1, pbName, cbName, NULL, pcchName, (pbName ? CHARUTIL_FLAG_STR_BUFONLY : 0));
        *pcchName = *pcchName >> 1;
    } else {
        f = f && CharUtil_UtoU(KeyInfo.uszName, -1, pbName, cbName, NULL, pcchName, (pbName ? CHARUTIL_FLAG_STR_BUFONLY : 0));
    }
    if(lpftLastWriteTime) { *(PQWORD)lpftLastWriteTime = KeyInfo.ftLastWrite; }
    Ob_DECREF(pObSubKey);
    Ob_DECREF(pmObSubKeys);
    Ob_DECREF(pObKey);
    Ob_DECREF(pObHive);
    return f;
}

_Success_(return)
BOOL VMMDLL_WinReg_EnumValue_Impl(_In_ VMM_HANDLE H, _In_opt_ LPSTR uszFullPathKey, _In_opt_ LPWSTR wszFullPathKey, _In_ DWORD dwIndex, _Out_writes_opt_(cbName) PBYTE pbName, _In_ DWORD cbName, _Out_ PDWORD pcchName, _Out_opt_ PDWORD lpType, _Out_writes_opt_(*lpcbData) PBYTE lpData, _Inout_opt_ PDWORD lpcbData)
{
    BOOL f;
    VMM_REGISTRY_VALUE_INFO ValueInfo = { 0 };
    CHAR uszPathKey[MAX_PATH];
    POB_REGISTRY_HIVE pObHive = NULL;
    POB_REGISTRY_KEY pObKey = NULL;
    POB_MAP pmObValues = NULL;
    POB_REGISTRY_VALUE pObValue = NULL;
    BYTE pbBuffer[2 * MAX_PATH];
    *pcchName = 0;
    if(wszFullPathKey) {
        if(!CharUtil_WtoU(wszFullPathKey, -1, pbBuffer, sizeof(pbBuffer), &uszFullPathKey, NULL, 0)) { return FALSE; }
    }
    if(!uszFullPathKey) { return FALSE; }
    if((pbName && !cbName) || (lpData && (!lpcbData || !*lpcbData))) {
        if(lpType) { *lpType = 0; }
        if(lpcbData) { *lpcbData = 0; }
        return FALSE;
    }
    f = VmmWinReg_PathHiveGetByFullPath(H, uszFullPathKey, &pObHive, uszPathKey) &&
        (pObKey = VmmWinReg_KeyGetByPath(H, pObHive, uszPathKey)) &&
        (pmObValues = VmmWinReg_KeyValueList(H, pObHive, pObKey)) &&
        (pObValue = ObMap_GetByIndex(pmObValues, dwIndex));
    if(f) {
        VmmWinReg_ValueInfo(pObHive, pObValue, &ValueInfo);
        if(wszFullPathKey) {
            f = CharUtil_UtoW(ValueInfo.uszName, -1, pbName, cbName, NULL, pcchName, (pbName ? CHARUTIL_FLAG_STR_BUFONLY : 0));
            *pcchName = *pcchName >> 1;
        } else {
            CharUtil_UtoU(ValueInfo.uszName, -1, pbName, cbName, NULL, pcchName, (pbName ? CHARUTIL_FLAG_STR_BUFONLY : 0));
        }
    }
    if(lpType) { *lpType = ValueInfo.dwType; }
    if(f && lpData) {
        f = VmmWinReg_ValueQuery4(H, pObHive, pObValue, NULL, lpData, *lpcbData, lpcbData);
    } else if(lpcbData) {
        *lpcbData = ValueInfo.cbData;
    }
    Ob_DECREF(pObValue);
    Ob_DECREF(pObKey);
    Ob_DECREF(pmObValues);
    Ob_DECREF(pObHive);
    return f;
}

_Success_(return) BOOL VMMDLL_WinReg_EnumKeyExU(_In_ VMM_HANDLE H, _In_ LPSTR uszFullPathKey, _In_ DWORD dwIndex, _Out_writes_opt_(*lpcchName) LPSTR lpName, _Inout_ LPDWORD lpcchName, _Out_opt_ PFILETIME lpftLastWriteTime)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_WinReg_EnumValueW, VMMDLL_WinReg_EnumKeyEx_Impl(H, uszFullPathKey, NULL, dwIndex, (PBYTE)lpName, *lpcchName, lpcchName, lpftLastWriteTime))
}

_Success_(return) BOOL VMMDLL_WinReg_EnumKeyExW(_In_ VMM_HANDLE H, _In_ LPWSTR wszFullPathKey, _In_ DWORD dwIndex, _Out_writes_opt_(*lpcchName) LPWSTR lpName, _Inout_ LPDWORD lpcchName, _Out_opt_ PFILETIME lpftLastWriteTime)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_WinReg_EnumValueW, VMMDLL_WinReg_EnumKeyEx_Impl(H, NULL, wszFullPathKey, dwIndex, (PBYTE)lpName, *lpcchName << 1, lpcchName, lpftLastWriteTime))
}

_Success_(return) BOOL VMMDLL_WinReg_EnumValueU(_In_ VMM_HANDLE H, _In_ LPSTR uszFullPathKey, _In_ DWORD dwIndex, _Out_writes_opt_(*lpcchValueName) LPSTR lpValueName, _Inout_ LPDWORD lpcchValueName, _Out_opt_ LPDWORD lpType, _Out_writes_opt_(*lpcbData) LPBYTE lpData, _Inout_opt_ LPDWORD lpcbData)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_WinReg_EnumValueW, VMMDLL_WinReg_EnumValue_Impl(H, uszFullPathKey, NULL, dwIndex, (PBYTE)lpValueName, *lpcchValueName, lpcchValueName, lpType, lpData, lpcbData))
}

_Success_(return) BOOL VMMDLL_WinReg_EnumValueW(_In_ VMM_HANDLE H, _In_ LPWSTR wszFullPathKey, _In_ DWORD dwIndex, _Out_writes_opt_(*lpcchValueName) LPWSTR lpValueName, _Inout_ LPDWORD lpcchValueName, _Out_opt_ LPDWORD lpType, _Out_writes_opt_(*lpcbData) LPBYTE lpData, _Inout_opt_ LPDWORD lpcbData)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_WinReg_EnumValueW, VMMDLL_WinReg_EnumValue_Impl(H, NULL, wszFullPathKey, dwIndex, (PBYTE)lpValueName, *lpcchValueName << 1, lpcchValueName, lpType, lpData, lpcbData))
}

_Success_(return) BOOL VMMDLL_WinReg_QueryValueExU(_In_ VMM_HANDLE H, _In_ LPSTR uszFullPathKeyValue, _Out_opt_ LPDWORD lpType, _Out_writes_opt_(*lpcbData) LPBYTE lpData, _When_(lpData == NULL, _Out_opt_) _When_(lpData != NULL, _Inout_opt_) LPDWORD lpcbData)
{
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_WinReg_QueryValueEx, VmmWinReg_ValueQuery2(H, uszFullPathKeyValue, lpType, lpData, lpcbData ? *lpcbData : 0, lpcbData))
}

_Success_(return) BOOL VMMDLL_WinReg_QueryValueExW(_In_ VMM_HANDLE H, _In_ LPWSTR wszFullPathKeyValue, _Out_opt_ LPDWORD lpType, _Out_writes_opt_(*lpcbData) LPBYTE lpData, _When_(lpData == NULL, _Out_opt_) _When_(lpData != NULL, _Inout_opt_) LPDWORD lpcbData)
{
    LPSTR uszFullPathKeyValue;
    BYTE pbBuffer[2 * MAX_PATH];
    if(!CharUtil_WtoU(wszFullPathKeyValue, -1, pbBuffer, sizeof(pbBuffer), &uszFullPathKeyValue, NULL, 0)) { return FALSE; }
    CALL_IMPLEMENTATION_VMM(H, STATISTICS_ID_VMMDLL_WinReg_QueryValueEx, VmmWinReg_ValueQuery2(H, uszFullPathKeyValue, lpType, lpData, lpcbData ? *lpcbData : 0, lpcbData))
}



//-----------------------------------------------------------------------------
// WINDOWS SPECIFIC UTILITY FUNCTIONS BELOW:
//-----------------------------------------------------------------------------

_Success_(return)
BOOL VMMDLL_WinGetThunkInfoIAT_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName, _In_ LPSTR szImportModuleName, _In_ LPSTR szImportFunctionName, _Out_ PVMMDLL_WIN_THUNKINFO_IAT pThunkInfoIAT)
{
    BOOL f = FALSE;
    QWORD vaModuleBase;
    PVMM_PROCESS pObProcess = NULL;
    f = (sizeof(VMMDLL_WIN_THUNKINFO_IAT) == sizeof(PE_THUNKINFO_IAT)) &&
        (pObProcess = VmmProcessGet(H, dwPID)) &&
        (vaModuleBase = VMMDLL_ProcessGetModuleBase_Impl(H, dwPID, uszModuleName)) &&
        PE_GetThunkInfoIAT(H, pObProcess, vaModuleBase, szImportModuleName, szImportFunctionName, (PPE_THUNKINFO_IAT)pThunkInfoIAT);
    Ob_DECREF(pObProcess);
    return f;
}

_Success_(return)
BOOL VMMDLL_WinGetThunkInfoIATU(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPSTR uszModuleName, _In_ LPSTR szImportModuleName, _In_ LPSTR szImportFunctionName, _Out_ PVMMDLL_WIN_THUNKINFO_IAT pThunkInfoIAT)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_WinGetThunkIAT,
        VMMDLL_WinGetThunkInfoIAT_Impl(H, dwPID, uszModuleName, szImportModuleName, szImportFunctionName, pThunkInfoIAT))
}

_Success_(return)
BOOL VMMDLL_WinGetThunkInfoIATW(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ LPWSTR wszModuleName, _In_ LPSTR szImportModuleName, _In_ LPSTR szImportFunctionName, _Out_ PVMMDLL_WIN_THUNKINFO_IAT pThunkInfoIAT)
{
    LPSTR uszModuleName;
    BYTE pbBuffer[MAX_PATH];
    if(!CharUtil_WtoU(wszModuleName, -1, pbBuffer, sizeof(pbBuffer), &uszModuleName, NULL, 0)) { return FALSE; }
    return VMMDLL_WinGetThunkInfoIATU(H, dwPID, uszModuleName, szImportModuleName, szImportFunctionName, pThunkInfoIAT);
}



//-----------------------------------------------------------------------------
// WINDOWS SPECIFIC DEBUGGING / SYMBOL FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

_Success_(return)
BOOL VMMDLL_PdbLoad_Impl(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 vaModuleBase, _Out_writes_(MAX_PATH) LPSTR szModuleName)
{
    BOOL fResult;
    PDB_HANDLE hPdb;
    PVMM_PROCESS pObProcess;
    if(!(pObProcess = VmmProcessGet(H, dwPID))) { return FALSE; }
    fResult =
        (hPdb = PDB_GetHandleFromModuleAddress(H, pObProcess, vaModuleBase)) &&
        PDB_LoadEnsure(H, hPdb) &&
        PDB_GetModuleInfo(H, hPdb, szModuleName, NULL, NULL);
    Ob_DECREF(pObProcess);
    return fResult;
}

_Success_(return)
BOOL VMMDLL_PdbLoad(_In_ VMM_HANDLE H, _In_ DWORD dwPID, _In_ ULONG64 vaModuleBase, _Out_writes_(MAX_PATH) LPSTR szModuleName)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_PdbLoad,
        VMMDLL_PdbLoad_Impl(H, dwPID, vaModuleBase, szModuleName))
}

_Success_(return)
BOOL VMMDLL_PdbSymbolName_Impl(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ QWORD cbSymbolAddressOrOffset, _Out_writes_(MAX_PATH) LPSTR szSymbolName, _Out_opt_ PDWORD pdwSymbolDisplacement)
{
    DWORD cbPdbModuleSize = 0;
    QWORD vaPdbModuleBase = 0;
    PDB_HANDLE hPdb = PDB_GetHandleFromModuleName(H, szModule);
    if(PDB_GetModuleInfo(H, hPdb, NULL, &vaPdbModuleBase, &cbPdbModuleSize)) {
        if((vaPdbModuleBase <= cbSymbolAddressOrOffset) && (vaPdbModuleBase + cbPdbModuleSize >= cbSymbolAddressOrOffset)) {
            cbSymbolAddressOrOffset -= vaPdbModuleBase;     // cbSymbolAddressOrOffset is absolute address
        }
    }
    return PDB_GetSymbolFromOffset(H, hPdb, (DWORD)cbSymbolAddressOrOffset, szSymbolName, pdwSymbolDisplacement);
}

_Success_(return)
BOOL VMMDLL_PdbSymbolName(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ QWORD cbSymbolAddressOrOffset, _Out_writes_(MAX_PATH) LPSTR szSymbolName, _Out_opt_ PDWORD pdwSymbolDisplacement)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_PdbSymbolName,
        VMMDLL_PdbSymbolName_Impl(H, szModule, cbSymbolAddressOrOffset, szSymbolName, pdwSymbolDisplacement))
}

_Success_(return)
BOOL VMMDLL_PdbSymbolAddress_Impl(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ LPSTR szSymbolName, _Out_ PULONG64 pvaSymbolAddress)
{
    PDB_HANDLE hPdb = (strcmp(szModule, "nt") && strcmp(szModule, "ntoskrnl")) ? PDB_GetHandleFromModuleName(H, szModule) : PDB_HANDLE_KERNEL;
    return PDB_GetSymbolAddress(H, hPdb, szSymbolName, pvaSymbolAddress);
}

_Success_(return)
BOOL VMMDLL_PdbSymbolAddress(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ LPSTR szSymbolName, _Out_ PULONG64 pvaSymbolAddress)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_PdbSymbolAddress,
        VMMDLL_PdbSymbolAddress_Impl(H, szModule, szSymbolName, pvaSymbolAddress))
}

_Success_(return)
BOOL VMMDLL_PdbTypeSize_Impl(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ LPSTR szTypeName, _Out_ PDWORD pcbTypeSize)
{
    PDB_HANDLE hPdb = (strcmp(szModule, "nt") && strcmp(szModule, "ntoskrnl")) ? PDB_GetHandleFromModuleName(H, szModule) : PDB_HANDLE_KERNEL;
    return PDB_GetTypeSize(H, hPdb, szTypeName, pcbTypeSize);
}

_Success_(return)
BOOL VMMDLL_PdbTypeSize(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ LPSTR szTypeName, _Out_ PDWORD pcbTypeSize)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_PdbTypeSize,
        VMMDLL_PdbTypeSize_Impl(H, szModule, szTypeName, pcbTypeSize))
}

_Success_(return)
BOOL VMMDLL_PdbTypeChildOffset_Impl(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ LPSTR uszTypeName, _In_ LPSTR uszTypeChildName, _Out_ PDWORD pcbTypeChildOffset)
{
    PDB_HANDLE hPdb = (strcmp(szModule, "nt") && strcmp(szModule, "ntoskrnl")) ? PDB_GetHandleFromModuleName(H, szModule) : PDB_HANDLE_KERNEL;
    return PDB_GetTypeChildOffset(H, hPdb, uszTypeName, uszTypeChildName, pcbTypeChildOffset);
}

_Success_(return)
BOOL VMMDLL_PdbTypeChildOffset(_In_ VMM_HANDLE H, _In_ LPSTR szModule, _In_ LPSTR uszTypeName, _In_ LPSTR uszTypeChildName, _Out_ PDWORD pcbTypeChildOffset)
{
    CALL_IMPLEMENTATION_VMM(
        H,
        STATISTICS_ID_VMMDLL_PdbTypeChildOffset,
        VMMDLL_PdbTypeChildOffset_Impl(H, szModule, uszTypeName, uszTypeChildName, pcbTypeChildOffset))
}




//-----------------------------------------------------------------------------
// VMM UTIL FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

_Success_(return)
BOOL VMMDLL_UtilFillHexAscii(_In_reads_opt_(cb) PBYTE pb, _In_ DWORD cb, _In_ DWORD cbInitialOffset, _Out_writes_opt_(*pcsz) LPSTR sz, _Inout_ PDWORD pcsz)
{
    return Util_FillHexAscii(pb, cb, cbInitialOffset, sz, pcsz);
}




//-----------------------------------------------------------------------------
// INTERNAL USE ONLY HELPER FUNCTIONS BELOW:
//-----------------------------------------------------------------------------
VOID VMMDLL_VfsList_AddFile(_In_ HANDLE pFileList, _In_ LPSTR uszName, _In_ ULONG64 cb, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    ((PVMMDLL_VFS_FILELIST2)pFileList)->pfnAddFile(((PVMMDLL_VFS_FILELIST2)pFileList)->h, uszName, cb, pExInfo);
}
VOID VMMDLL_VfsList_AddDirectory(_In_ HANDLE pFileList, _In_ LPSTR uszName, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    ((PVMMDLL_VFS_FILELIST2)pFileList)->pfnAddDirectory(((PVMMDLL_VFS_FILELIST2)pFileList)->h, uszName, pExInfo);
}

/*
* Helper functions for callbacks into the VMM_VFS_FILELIST structure.
*/
VOID VMMDLL_VfsList_AddFileW(_In_ HANDLE pFileList, _In_ LPWSTR wszName, _In_ ULONG64 cb, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    LPSTR uszName;
    BYTE pbBuffer[3 * MAX_PATH];
    if(!CharUtil_WtoU(wszName, -1, pbBuffer, sizeof(pbBuffer), &uszName, NULL, CHARUTIL_FLAG_TRUNCATE)) { return; }
    ((PVMMDLL_VFS_FILELIST2)pFileList)->pfnAddFile(((PVMMDLL_VFS_FILELIST2)pFileList)->h, uszName, cb, pExInfo);
}

VOID VMMDLL_VfsList_AddDirectoryW(_In_ HANDLE pFileList, _In_ LPWSTR wszName, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    LPSTR uszName;
    BYTE pbBuffer[3 * MAX_PATH];
    if(!CharUtil_WtoU(wszName, -1, pbBuffer, sizeof(pbBuffer), &uszName, NULL, CHARUTIL_FLAG_TRUNCATE)) { return; }
    ((PVMMDLL_VFS_FILELIST2)pFileList)->pfnAddDirectory(((PVMMDLL_VFS_FILELIST2)pFileList)->h, uszName, pExInfo);
}

BOOL VMMDLL_VfsList_IsHandleValid(_In_ HANDLE pFileList)
{
    return ((PVMMDLL_VFS_FILELIST2)pFileList)->dwVersion == VMMDLL_VFS_FILELIST_VERSION;
}
