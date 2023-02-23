// m_proc_thread.c : implementation of the thread info built-in module.
//
// (c) Ulf Frisk, 2019-2023
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "pluginmanager.h"
#include "charutil.h"
#include "util.h"
#include "vmm.h"
#include "vmmdll.h"
#include "vmmwindef.h"

#define MTHREAD_INFOFILE_LENGTH  803ULL
#define MTHREAD_LINELENGTH       250ULL
#define MTHREAD_LINEHEADER       "   #    PID     TID          ETHREAD Status     WaitReason           Prio      ExitSt     StartAddress   InstructionPtr                 TEB          StackBase           StackPtr         StackLimit  CreateTime                 ExitTime"

#define MTHREAD_GET_STR_STATE(pe)           ((pe->bState < (sizeof(_KTHREAD_STATE_STR) / sizeof(LPCSTR))) ? _KTHREAD_STATE_STR[pe->bState] : "Unknown")
#define MTHREAD_GET_STR_WAIT_REASON(pe)     ((pe->bWaitReason < (sizeof(_KWAIT_REASON_STR) / sizeof(LPCSTR))) ? _KWAIT_REASON_STR[pe->bWaitReason] : "Unknown")

_Success_(return == 0)
NTSTATUS MThread_Read_ThreadInfo(_In_ VMM_HANDLE H, _In_ PVMM_MAP_THREADENTRY pe, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    CHAR sz[MTHREAD_INFOFILE_LENGTH + 1];
    CHAR szTimeCreate[32] = { 0 }, szTimeExit[32] = { 0 };
    Util_FileTime2String(pe->ftCreateTime, szTimeCreate);
    Util_FileTime2String(pe->ftExitTime, szTimeExit);
    snprintf(
        sz,
        MTHREAD_INFOFILE_LENGTH + 1,
        "PID:           %21i\n" \
        "TID:           %21i\n" \
        "ExitStatus:    %21x\n" \
        "State:         %21x %-7s\n" \
        "WaitReason:    %21x %-17s\n" \
        "SuspendCount:  %21x\n" \
        "Running:       %21x\n" \
        "Priority:      %21x\n" \
        "BasePriority:  %21x\n" \
        "ETHREAD:       %21llx\n" \
        "TEB:           %21llx\n" \
        "StartAddress:       %16llx\n" \
        "UserStackBase:      %16llx\n" \
        "UserStackLimit:     %16llx\n" \
        "KernelStackBase:    %16llx\n" \
        "KernelStackLimit:   %16llx\n" \
        "TrapFrame:          %16llx\n" \
        "StackPointer:       %16llx\n" \
        "InstructionPointer: %16llx\n" \
        "CreateTime:  %-23s\n" \
        "ExitTime:    %-23s\n",
        pe->dwPID,
        pe->dwTID,
        pe->dwExitStatus,
        pe->bState, MTHREAD_GET_STR_STATE(pe),
        pe->bWaitReason, MTHREAD_GET_STR_WAIT_REASON(pe),
        pe->bSuspendCount,
        pe->bRunning,
        pe->bPriority,
        pe->bBasePriority,
        pe->vaETHREAD,
        pe->vaTeb,
        pe->vaStartAddress,
        pe->vaStackBaseUser,
        pe->vaStackLimitUser,
        pe->vaStackBaseKernel,
        pe->vaStackLimitKernel,
        pe->vaTrapFrame,
        pe->vaRSP,
        pe->vaRIP,
        szTimeCreate,
        szTimeExit
    );
    return Util_VfsReadFile_FromPBYTE(sz, MTHREAD_INFOFILE_LENGTH, pb, cb, pcbRead, cbOffset);
}

VOID MThread_ReadLineCB(_In_ VMM_HANDLE H, _Inout_opt_ PVOID ctx, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVMM_MAP_THREADENTRY pe, _Out_writes_(cbLineLength + 1) LPSTR szu8)
{
    CHAR szTimeCreate[24], szTimeExit[24];
    Util_FileTime2String(pe->ftCreateTime, szTimeCreate);
    Util_FileTime2String(pe->ftExitTime, szTimeExit);
    Util_usnprintf_ln(szu8, cbLineLength,
        "%04x%7i%8i %16llx %1x %-7s %2i %-17s %2x %2x %2x %8x %16llx %16llx -- %16llx : %16llx > %16llx > %16llx [%s :: %s]",
        ie,
        pe->dwPID,
        pe->dwTID,
        pe->vaETHREAD,
        pe->bState,
        MTHREAD_GET_STR_STATE(pe),
        pe->bWaitReason,
        MTHREAD_GET_STR_WAIT_REASON(pe),
        pe->bRunning,
        pe->bBasePriority,
        pe->bPriority,
        pe->dwExitStatus,
        pe->vaStartAddress,
        pe->vaRIP,
        pe->vaTeb,
        pe->vaStackBaseUser,
        pe->vaRSP,
        pe->vaStackLimitUser,
        szTimeCreate,
        szTimeExit
    );
}

/*
* Read : function as specified by the module manager. The module manager will
* call into this callback function whenever a read shall occur from a "file".
* -- H
* -- ctxP
* -- pb
* -- cb
* -- pcbRead
* -- cbOffset
* -- return
*/
_Success_(return == 0)
NTSTATUS MThread_Read(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    PVMMOB_MAP_THREAD pObThreadMap = NULL;
    CHAR uszThreadName[16 + 1];
    LPSTR uszSubPath;
    DWORD dwTID;
    PVMM_MAP_THREADENTRY pe;
    if(!VmmMap_GetThread(H, ctxP->pProcess, &pObThreadMap)) { return VMMDLL_STATUS_FILE_INVALID; }
    // module root - thread info file
    if(!_stricmp(ctxP->uszPath, "threads.txt")) {
        nt = Util_VfsLineFixed_Read(
            H, (UTIL_VFSLINEFIXED_PFN_CB)MThread_ReadLineCB, NULL, MTHREAD_LINELENGTH, MTHREAD_LINEHEADER,
            pObThreadMap->pMap, pObThreadMap->cMap, sizeof(VMM_MAP_THREADENTRY),
            pb, cb, pcbRead, cbOffset
        );
        goto finish;
    }
    // individual thread file
    uszSubPath = CharUtil_PathSplitFirst(ctxP->uszPath, uszThreadName, _countof(uszThreadName));
    if(uszSubPath && (dwTID = (DWORD)Util_GetNumericA(ctxP->uszPath)) && (pe = VmmMap_GetThreadEntry(H, pObThreadMap, dwTID))) {
        if(!_stricmp(uszSubPath, "info.txt")) {
            nt = MThread_Read_ThreadInfo(H, pe, pb, cb, pcbRead, cbOffset);
            goto finish;
        }
        // individual thread files backed by user-mode memory below:
        if(!_stricmp(uszSubPath, "teb")) {
            nt = VmmReadAsFile(H, (PVMM_PROCESS)ctxP->pProcess, pe->vaTeb, 0x1000, pb, cb, pcbRead, cbOffset);
            goto finish;
        }
        if(!_stricmp(uszSubPath, "stack")) {
            nt = VmmReadAsFile(H, (PVMM_PROCESS)ctxP->pProcess, pe->vaStackLimitUser, pe->vaStackBaseUser - pe->vaStackLimitUser, pb, cb, pcbRead, cbOffset);
            goto finish;
        }
        // individual thread files backed by kernel memory below:
        if(!_stricmp(uszSubPath, "ethread")) {
            nt = VmmReadAsFile(H, PVMM_PROCESS_SYSTEM, pe->vaETHREAD, H->vmm.offset.ETHREAD.oMax, pb, cb, pcbRead, cbOffset);
            goto finish;
        }
        if(!_stricmp(uszSubPath, "kstack")) {
            nt = VmmReadAsFile(H, PVMM_PROCESS_SYSTEM, pe->vaStackLimitKernel, pe->vaStackBaseKernel - pe->vaStackLimitKernel, pb, cb, pcbRead, cbOffset);
            goto finish;
        }
    }
finish:
    Ob_DECREF(pObThreadMap);
    return nt;
}

/*
* Write : function as specified by the module manager. The module manager will
* call into this callback function whenever a write shall occur from a "file".
* -- H
* -- ctxP
* -- pb
* -- cb
* -- pcbWrite
* -- cbOffset
* -- return
*/
NTSTATUS MThread_Write(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    PVMMOB_MAP_THREAD pObThreadMap = NULL;
    CHAR uszThreadName[16 + 1];
    LPSTR uszSubPath;
    DWORD dwTID;
    PVMM_MAP_THREADENTRY pe;
    if(!VmmMap_GetThread(H, ctxP->pProcess, &pObThreadMap)) { return VMMDLL_STATUS_FILE_INVALID; }
    // individual thread file
    uszSubPath = CharUtil_PathSplitFirst(ctxP->uszPath, uszThreadName, sizeof(uszThreadName));
    if(uszSubPath && (dwTID = (DWORD)Util_GetNumericA(ctxP->uszPath)) && (pe = VmmMap_GetThreadEntry(H, pObThreadMap, dwTID))) {
        // individual thread files backed by user-mode memory below:
        if(!_stricmp(uszSubPath, "teb")) {
            nt = VmmWriteAsFile(H, (PVMM_PROCESS)ctxP->pProcess, pe->vaTeb, 0x1000, pb, cb, pcbWrite, cbOffset);
            goto finish;
        }
        if(!_stricmp(uszSubPath, "stack")) {
            nt = VmmWriteAsFile(H, (PVMM_PROCESS)ctxP->pProcess, pe->vaStackLimitUser, pe->vaStackBaseUser - pe->vaStackLimitUser, pb, cb, pcbWrite, cbOffset);
            goto finish;
        }
        // individual thread files backed by kernel memory below:
        if(!_stricmp(uszSubPath, "ethread")) {
            nt = VmmWriteAsFile(H, PVMM_PROCESS_SYSTEM, pe->vaETHREAD, H->vmm.offset.ETHREAD.oMax, pb, cb, pcbWrite, cbOffset);
            goto finish;
        }
        if(!_stricmp(uszSubPath, "kstack")) {
            nt = VmmWriteAsFile(H, PVMM_PROCESS_SYSTEM, pe->vaStackLimitKernel, pe->vaStackBaseKernel - pe->vaStackLimitKernel, pb, cb, pcbWrite, cbOffset);
            goto finish;
        }
    }
finish:
    Ob_DECREF(pObThreadMap);
    return nt;
}

/*
* Set file timestamp into the ExInfo struct.
* -- pThreadEntry
* -- pExInfo
*/
VOID MThread_List_TimeStampFile(_In_ PVMM_MAP_THREADENTRY pThreadEntry, _Out_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    pExInfo->dwVersion = VMMDLL_VFS_FILELIST_EXINFO_VERSION;
    pExInfo->qwCreationTime = pThreadEntry->ftCreateTime;
    pExInfo->qwLastWriteTime = pThreadEntry->ftExitTime;
    if(!pExInfo->qwLastWriteTime) {
        pExInfo->qwLastWriteTime = pExInfo->qwCreationTime;
    }
}

/*
* List : function as specified by the module manager. The module manager will
* call into this callback function whenever a list directory shall occur from
* the given module.
* -- H
* -- ctxP
* -- pFileList
* -- return
*/
BOOL MThread_List(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Inout_ PHANDLE pFileList)
{
    DWORD i, dwTID, cbStack;
    CHAR uszBuffer[32] = { 0 };
    PVMMOB_MAP_THREAD pObThreadMap = NULL;
    PVMM_MAP_THREADENTRY pe;
    VMMDLL_VFS_FILELIST_EXINFO ExInfo = { 0 };
    if(!VmmMap_GetThread(H, ctxP->pProcess, &pObThreadMap)) { goto fail; }
    // module root - list thread map
    if(!ctxP->uszPath[0]) {
        for(i = 0; i < pObThreadMap->cMap; i++) {
            pe = pObThreadMap->pMap + i;
            MThread_List_TimeStampFile(pe, &ExInfo);
            _snprintf_s(uszBuffer, _countof(uszBuffer), 32, "%i", pe->dwTID);
            VMMDLL_VfsList_AddDirectory(pFileList, uszBuffer, &ExInfo);
        }
        VMMDLL_VfsList_AddFile(pFileList, "threads.txt", UTIL_VFSLINEFIXED_LINECOUNT(H, pObThreadMap->cMap) * MTHREAD_LINELENGTH, NULL);
        Ob_DECREF_NULL(&pObThreadMap);
        return TRUE;
    }
    // specific thread
    if(!(dwTID = (DWORD)Util_GetNumericA(ctxP->uszPath))) { goto fail; }
    if(!(pe = VmmMap_GetThreadEntry(H, pObThreadMap, dwTID))) { goto fail; }
    MThread_List_TimeStampFile(pe, &ExInfo);
    VMMDLL_VfsList_AddFile(pFileList, "info.txt", MTHREAD_INFOFILE_LENGTH, &ExInfo);
    VMMDLL_VfsList_AddFile(pFileList, "ethread", H->vmm.offset.ETHREAD.oMax, &ExInfo);
    if(pe->vaTeb) {
        VMMDLL_VfsList_AddFile(pFileList, "teb", 0x1000, &ExInfo);
    }
    if(pe->vaStackBaseUser && pe->vaStackLimitUser && (pe->vaStackLimitUser < pe->vaStackBaseUser)) {
        cbStack = (DWORD)(pe->vaStackBaseUser - pe->vaStackLimitUser);
        VMMDLL_VfsList_AddFile(pFileList, "stack", cbStack, &ExInfo);
    }
    if(pe->vaStackBaseKernel && pe->vaStackLimitKernel && (pe->vaStackLimitKernel < pe->vaStackBaseKernel)) {
        cbStack = (DWORD)(pe->vaStackBaseKernel - pe->vaStackLimitKernel);
        VMMDLL_VfsList_AddFile(pFileList, "kstack", cbStack, &ExInfo);
    }
fail:
    Ob_DECREF_NULL(&pObThreadMap);
    return TRUE;
}

/*
* Initialization function. The module manager shall call into this function
* when the module shall be initialized. If the module wish to initialize it
* shall call the supplied pfnPluginManager_Register function.
* NB! the module does not have to register itself - for example if the target
* operating system or architecture is unsupported.
* -- H
* -- pRI
*/
VOID M_ProcThread_Initialize(_In_ VMM_HANDLE H, _Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if(!((pRI->tpSystem == VMM_SYSTEM_WINDOWS_X64) || (pRI->tpSystem == VMM_SYSTEM_WINDOWS_X86))) { return; }
    strcpy_s(pRI->reg_info.uszPathName, 128, "\\threads");              // module name
    pRI->reg_info.fRootModule = FALSE;                                  // module shows in root directory
    pRI->reg_info.fProcessModule = TRUE;                                // module shows in process directory
    pRI->reg_fn.pfnList = MThread_List;                                 // List function supported
    pRI->reg_fn.pfnRead = MThread_Read;                                 // Read function supported
    pRI->reg_fn.pfnWrite = MThread_Write;                               // Write function supported
    pRI->pfnPluginManager_Register(H, pRI);
}
