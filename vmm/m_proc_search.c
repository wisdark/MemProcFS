// m_proc_search.c : implementation of the virtual memory search built-in module.
//
// (c) Ulf Frisk, 2022
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "vmm.h"
#include "util.h"
#include "pluginmanager.h"

LPCSTR szSEARCH_README =
"Information about the search module                                          \n" \
"===================================                                          \n" \
"Write a hexascii sequence into search.txt and save to trigger a binary search\n" \
"in virtual address space for the data searched. The results once completed is\n" \
"shown in result.txt                                                          \n" \
"---                                                                          \n" \
"Before a search is initiated (by writing to search.txt) it is possible to add\n" \
"additional constraints to writeable files:                                   \n" \
"align.txt, addr-min.txt, addr-max.txt, search-skip-bitmask.txt.              \n" \
"---                                                                          \n" \
"An ongoing search may be cancelled by writing '1' to reset.txt.              \n" \
"Additional info is shown in status.txt.                                      \n";

typedef struct tdMOB_SEARCH_CONTEXT {
    OB ObHdr;
    DWORD dwPID;
    BOOL fActive;
    BOOL fCompleted;
    VMM_MEMORY_SEARCH_CONTEXT sctx;
    POB_DATA pObDataResult;
} MOB_SEARCH_CONTEXT, *PMOB_SEARCH_CONTEXT;

VOID MSearch_ContextUpdate(_In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _In_opt_ PMOB_SEARCH_CONTEXT ctxS)
{
    EnterCriticalSection(&ctxVmm->LockPlugin);
    if(!ctxS || !ObMap_Exists((POB_MAP)ctxP->ctxM, ctxS)) {
        Ob_DECREF(ObMap_RemoveByKey((POB_MAP)ctxP->ctxM, ctxP->dwPID));
        if(ctxS) { ObMap_Push((POB_MAP)ctxP->ctxM, ctxP->dwPID, ctxS); }
    }
    LeaveCriticalSection(&ctxVmm->LockPlugin);
}

VOID MSearch_ContextCleanup1_CB(PVOID pOb)
{
    ((PMOB_SEARCH_CONTEXT)pOb)->sctx.fAbortRequested = TRUE;
}

VOID MSearch_ContextCleanup_CB(PVOID pOb)
{
    Ob_DECREF(((PMOB_SEARCH_CONTEXT)pOb)->pObDataResult);
}

/*
* CALLER DECREF: return
*/
PMOB_SEARCH_CONTEXT MSearch_ContextGet(_In_ PVMMDLL_PLUGIN_CONTEXT ctxP)
{
    PMOB_SEARCH_CONTEXT pObCtx = NULL;
    EnterCriticalSection(&ctxVmm->LockPlugin);
    pObCtx = ObMap_GetByKey((POB_MAP)ctxP->ctxM, ctxP->dwPID);
    LeaveCriticalSection(&ctxVmm->LockPlugin);
    if(!pObCtx && (pObCtx = Ob_Alloc(OB_TAG_MOD_SEARCH_CTX, LMEM_ZEROINIT, sizeof(MOB_SEARCH_CONTEXT), MSearch_ContextCleanup_CB, MSearch_ContextCleanup1_CB))) {
        pObCtx->sctx.cSearch = 1;
        pObCtx->dwPID = ((PVMM_PROCESS)ctxP->pProcess)->dwPID;
        if(((PVMM_PROCESS)ctxP->pProcess)->fUserOnly) {
            pObCtx->sctx.vaMax = ctxVmm->f32 ? 0x7fffffff : 0x7fffffffffff;
        } else {
            pObCtx->sctx.vaMax = ctxVmm->f32 ? 0xffffffff : 0xffffffffffffffff;
        }
        pObCtx->sctx.search[0].cbAlign = 1;
    }
    return pObCtx;
}

/*
* Perform the memory search in an async worker thread
*/
DWORD WINAPI MSearch_PerformSeach_ThreadProc(_In_ PMOB_SEARCH_CONTEXT ctxS)
{
    PVMM_PROCESS pObProcess = NULL;
    if((pObProcess = VmmProcessGet(ctxS->dwPID))) {
        VmmSearch(pObProcess, &ctxS->sctx, &ctxS->pObDataResult);
    }
    ctxS->fCompleted = TRUE;
    ctxS->fActive = FALSE;
    Ob_DECREF(ctxS);
    Ob_DECREF(pObProcess);
    return 0;
}

/*
* Write : function as specified by the module manager. The module manager will
* call into this callback function whenever a write shall occur from a "file".
* -- ctx
* -- pb
* -- cb
* -- pcbWrite
* -- cbOffset
* -- return
*/
NTSTATUS MSearch_Write(_In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_SUCCESS;
    PMOB_SEARCH_CONTEXT pObCtx = NULL;
    BOOL fReset;
    DWORD dw;
    QWORD qw;
    BYTE pbSearchBuffer[32];
    *pcbWrite = cb;
    if(!(pObCtx = MSearch_ContextGet(ctxP))) { return VMMDLL_STATUS_FILE_INVALID; }
    if(!_stricmp(ctxP->uszPath, "reset.txt")) {
        fReset = FALSE;
        nt = Util_VfsWriteFile_BOOL(&fReset, pb, cb, pcbWrite, cbOffset);
        if(fReset) {
            // removal via context update will clear up objects and also
            // cancel / abort any running tasks via the object refcount.
            MSearch_ContextUpdate(ctxP, NULL);
        }
    }
    if(!pObCtx->fActive && !pObCtx->fCompleted) {
        if(!_stricmp(ctxP->uszPath, "align.txt")) {
            dw = pObCtx->sctx.search[0].cbAlign;
            nt = Util_VfsWriteFile_DWORD(&dw, pb, cb, pcbWrite, cbOffset + 5, 1, 2048);
            if((dw != pObCtx->sctx.search[0].cbAlign) && (0 == (dw & (dw - 1)))) {
                if(dw == 0) { dw = 1; }
                // update (if ok) within critical section
                EnterCriticalSection(&ctxVmm->LockPlugin);
                if(!pObCtx->fActive && !pObCtx->fCompleted) {
                    pObCtx->sctx.search[0].cbAlign = dw;
                    MSearch_ContextUpdate(ctxP, pObCtx);
                }
                LeaveCriticalSection(&ctxVmm->LockPlugin);
            }
        }
        if(!_stricmp(ctxP->uszPath, "addr-max.txt")) {
            qw = pObCtx->sctx.vaMax;
            nt = Util_VfsWriteFile_QWORD(&qw, pb, cb, pcbWrite, cbOffset + (ctxVmm->f32 ? 8 : 0), 1, 0);
            qw = (qw - 1) | 0xfff;
            if((qw != pObCtx->sctx.vaMax)) {
                // update (if ok) within critical section
                EnterCriticalSection(&ctxVmm->LockPlugin);
                if(!pObCtx->fActive && !pObCtx->fCompleted) {
                    pObCtx->sctx.vaMax = qw;
                    MSearch_ContextUpdate(ctxP, pObCtx);
                }
                LeaveCriticalSection(&ctxVmm->LockPlugin);
            }
        }
        if(!_stricmp(ctxP->uszPath, "addr-min.txt")) {
            qw = pObCtx->sctx.vaMin;
            nt = Util_VfsWriteFile_QWORD(&qw, pb, cb, pcbWrite, cbOffset + (ctxVmm->f32 ? 8 : 0), 0, 0);
            qw = qw & ~0xfff;
            if((qw != pObCtx->sctx.vaMin)) {
                // update (if ok) within critical section
                EnterCriticalSection(&ctxVmm->LockPlugin);
                if(!pObCtx->fActive && !pObCtx->fCompleted) {
                    pObCtx->sctx.vaMin = qw;
                    MSearch_ContextUpdate(ctxP, pObCtx);
                }
                LeaveCriticalSection(&ctxVmm->LockPlugin);
            }
        }
        if(!_stricmp(ctxP->uszPath, "search-skip-bitmask.txt")) {
            memcpy(pbSearchBuffer, pObCtx->sctx.search[0].pbSkipMask, 32);
            nt = Util_VfsWriteFile_HEXASCII(pbSearchBuffer, 32, pb, cb, pcbWrite, cbOffset);
            if(*pcbWrite) {
                // update (if ok) within critical section
                EnterCriticalSection(&ctxVmm->LockPlugin);
                if(!pObCtx->fActive && !pObCtx->fCompleted) {
                    pObCtx->sctx.search[0].cb = max(pObCtx->sctx.search[0].cb, (*pcbWrite + 1) >> 1);
                    memcpy(pObCtx->sctx.search[0].pbSkipMask, pbSearchBuffer, 32);
                    MSearch_ContextUpdate(ctxP, pObCtx);
                }
                LeaveCriticalSection(&ctxVmm->LockPlugin);
            }
        }
        if(!_stricmp(ctxP->uszPath, "search.txt")) {
            memcpy(pbSearchBuffer, pObCtx->sctx.search[0].pb, 32);
            nt = Util_VfsWriteFile_HEXASCII(pbSearchBuffer, 32, pb, cb, pcbWrite, cbOffset);
            if(*pcbWrite) {
                // update (if ok) within critical section
                EnterCriticalSection(&ctxVmm->LockPlugin);
                if(!pObCtx->fActive && !pObCtx->fCompleted) {
                    pObCtx->sctx.search[0].cb = (*pcbWrite + 1) >> 1;
                    memcpy(pObCtx->sctx.search[0].pb, pbSearchBuffer, 32);
                    MSearch_ContextUpdate(ctxP, pObCtx);
                    // start search by queuing the search onto a work item
                    // in a separate thread. also increase refcount since
                    // worker thread is responsible for its own DECREF.
                    pObCtx->sctx.fAbortRequested = FALSE;
                    pObCtx->fActive = TRUE;
                    VmmWork((LPTHREAD_START_ROUTINE)MSearch_PerformSeach_ThreadProc, Ob_INCREF(pObCtx), NULL);
                }
                LeaveCriticalSection(&ctxVmm->LockPlugin);
            }
        }
    }
    Ob_DECREF(pObCtx);
    return nt;
}

_Success_(return == 0)
NTSTATUS MSearch_ReadStatus(_In_ PMOB_SEARCH_CONTEXT ctxS, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    CHAR *szStatus, szBuffer[256];
    if(ctxS->fActive) {
        szStatus = "RUNNING";
    } else if(ctxS->fCompleted) {
        szStatus = "COMPLETED";
    } else {
        szStatus = "NOT_STARTED";
    }
    snprintf(
        szBuffer,
        sizeof(szBuffer),
        "Status:          %s\n" \
        "Search size:     %i\n" \
        "Search align:    %i\n" \
        "Min address:     0x%llx\n" \
        "Max address:     0x%llx\n" \
        "Current address: 0x%llx\n" \
        "Bytes read:      0x%llx\n" \
        "Search hits:     %i\n",
        szStatus,
        ctxS->sctx.search[0].cb,
        ctxS->sctx.search[0].cbAlign,
        ctxS->sctx.vaMin,
        ctxS->sctx.vaMax,
        ctxS->sctx.vaCurrent,
        ctxS->sctx.cbReadTotal,
        ctxS->sctx.cResult
    );
    if(pb) {
        return Util_VfsReadFile_FromPBYTE(szBuffer, strlen(szBuffer), pb, cb, pcbRead, cbOffset);
    } else {
        *pcbRead = (DWORD)strlen(szBuffer);
        return VMMDLL_STATUS_SUCCESS;
    }
}

VOID MSearch_ReadLine_CB(_Inout_opt_ PVOID ctx, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PQWORD pe, _Out_writes_(cbLineLength + 1) LPSTR szu8)
{
    Util_usnprintf_ln(szu8, cbLineLength, ctxVmm->f32 ? "%08x" : "%016llx", *pe);
}

/*
* Read : function as specified by the module manager. The module manager will
* call into this callback function whenever a read shall occur from a "file".
* -- ctx
* -- pb
* -- cb
* -- pcbRead
* -- cbOffset
* -- return
*/
NTSTATUS MSearch_Read(_In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    PMOB_SEARCH_CONTEXT pObCtx = NULL;
    if(!(pObCtx = MSearch_ContextGet(ctxP))) { return VMMDLL_STATUS_FILE_INVALID; }
    if(!_stricmp(ctxP->uszPath, "readme.txt")) {
        nt = Util_VfsReadFile_FromStrA(szSEARCH_README, pb, cb, pcbRead, cbOffset);
    } else if(!_stricmp(ctxP->uszPath, "addr-max.txt")) {
        nt = ctxVmm->f32 ?
            Util_VfsReadFile_FromDWORD((DWORD)pObCtx->sctx.vaMax, pb, cb, pcbRead, cbOffset, FALSE) :
            Util_VfsReadFile_FromQWORD((QWORD)pObCtx->sctx.vaMax, pb, cb, pcbRead, cbOffset, FALSE);
    } else if(!_stricmp(ctxP->uszPath, "addr-min.txt")) {
        nt = ctxVmm->f32 ?
            Util_VfsReadFile_FromDWORD((DWORD)pObCtx->sctx.vaMin, pb, cb, pcbRead, cbOffset, FALSE) :
            Util_VfsReadFile_FromQWORD((QWORD)pObCtx->sctx.vaMin, pb, cb, pcbRead, cbOffset, FALSE);
    } else if(!_stricmp(ctxP->uszPath, "align.txt")) {
        nt = Util_VfsReadFile_FromDWORD(pObCtx->sctx.search[0].cbAlign, pb, cb, pcbRead, cbOffset + 5, FALSE);
    } else if(!_stricmp(ctxP->uszPath, "reset.txt")) {
        nt = Util_VfsReadFile_FromBOOL(FALSE, pb, cb, pcbRead, cbOffset);
    } else if(!_stricmp(ctxP->uszPath, "result.txt")) {
        nt = VMMDLL_STATUS_END_OF_FILE;
        if(pObCtx->pObDataResult) {
            nt = Util_VfsLineFixed_Read(
                (UTIL_VFSLINEFIXED_PFN_CB)MSearch_ReadLine_CB, NULL, ctxVmm->f32 ? 9 : 17, NULL,
                pObCtx->pObDataResult->pqw, pObCtx->pObDataResult->ObHdr.cbData / sizeof(QWORD), sizeof(QWORD),
                pb, cb, pcbRead, cbOffset
            );
        }
    } else if(!_stricmp(ctxP->uszPath, "search.txt")) {
        nt = Util_VfsReadFile_FromHEXASCII(pObCtx->sctx.search[0].pb, pObCtx->sctx.search[0].cb, pb, cb, pcbRead, cbOffset);
    } else if(!_stricmp(ctxP->uszPath, "search-skip-bitmask.txt")) {
        nt = Util_VfsReadFile_FromHEXASCII(pObCtx->sctx.search[0].pbSkipMask, pObCtx->sctx.search[0].cb, pb, cb, pcbRead, cbOffset);
    } else if(!_stricmp(ctxP->uszPath, "status.txt")) {
        nt = MSearch_ReadStatus(pObCtx, pb, cb, pcbRead, cbOffset);
    }
    Ob_DECREF(pObCtx);
    return nt;
}

/*
* List : function as specified by the module manager. The module manager will
* call into this callback function whenever a list directory shall occur from
* the given module.
* -- ctx
* -- pFileList
* -- return
*/
BOOL MSearch_List(_In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Inout_ PHANDLE pFileList)
{
    DWORD cbResult = 0;
    PMOB_SEARCH_CONTEXT pObCtx = NULL;
    if(ctxP->uszPath[0]) { return FALSE; }
    if(!(pObCtx = MSearch_ContextGet(ctxP))) { return FALSE; }
    VMMDLL_VfsList_AddFile(pFileList, "addr-max.txt", ctxVmm->f32 ? 8 : 16, NULL);
    VMMDLL_VfsList_AddFile(pFileList, "addr-min.txt", ctxVmm->f32 ? 8 : 16, NULL);
    VMMDLL_VfsList_AddFile(pFileList, "align.txt", 3, NULL);
    VMMDLL_VfsList_AddFile(pFileList, "readme.txt", strlen(szSEARCH_README), NULL);
    VMMDLL_VfsList_AddFile(pFileList, "reset.txt", 1, NULL);
    cbResult = pObCtx->pObDataResult ? ((ctxVmm->f32 ? 9ULL : 17ULL) * pObCtx->pObDataResult->ObHdr.cbData / sizeof(QWORD)) : 0;
    VMMDLL_VfsList_AddFile(pFileList, "result.txt", cbResult, NULL);
    VMMDLL_VfsList_AddFile(pFileList, "search.txt", pObCtx->sctx.search[0].cb * 2ULL, NULL);
    VMMDLL_VfsList_AddFile(pFileList, "search-skip-bitmask.txt", pObCtx->sctx.search[0].cb * 2ULL, NULL);
    cbResult = 0;
    MSearch_ReadStatus(pObCtx, NULL, 0, &cbResult, 0);
    VMMDLL_VfsList_AddFile(pFileList, "status.txt", cbResult, NULL);
    Ob_DECREF(pObCtx);
    return TRUE;
}

VOID MSearch_Close(_In_ PVMMDLL_PLUGIN_CONTEXT ctxP)
{
    Ob_DECREF(ctxP->ctxM);
}

/*
* Initialization function. The module manager shall call into this function
* when the module shall be initialized. If the module wish to initialize it
* shall call the supplied pfnPluginManager_Register function.
* NB! the module does not have to register itself - for example if the target
* operating system or architecture is unsupported.
* -- pRI
*/
VOID M_Search_Initialize(_Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if(!(pRI->reg_info.ctxM = (PVMMDLL_PLUGIN_INTERNAL_CONTEXT)ObMap_New(OB_MAP_FLAGS_OBJECT_OB))) { return; }
    pRI->reg_fn.pfnList = MSearch_List;                             // List function supported
    pRI->reg_fn.pfnRead = MSearch_Read;                             // Read function supported
    pRI->reg_fn.pfnWrite = MSearch_Write;                           // Write function supported
    pRI->reg_fn.pfnClose = MSearch_Close;                           // Close function supported
    // register process plugin
    strcpy_s(pRI->reg_info.uszPathName, 128, "\\search");
    pRI->reg_info.fRootModule = FALSE;
    pRI->reg_info.fProcessModule = TRUE;
    pRI->pfnPluginManager_Register(pRI);
}
