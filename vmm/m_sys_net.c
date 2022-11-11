// m_sys_net.c : implementation related to the Sys/Net built-in module.
//
// The 'sys/net' module is responsible for displaying networking information
// in a 'netstat' like way at the path '/sys/net/'
//
// The module is a provider of forensic timelining information.
//
// (c) Ulf Frisk, 2020-2022
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "vmm.h"
#include "util.h"
#include "fc.h"

LPCSTR szMSYSNET_README =
"Information about the sys net module                                         \n" \
"====================================                                         \n" \
"The sys/net module tries to enumerate and list network connections in        \n" \
"Windows 7 and later (x64 only).                                              \n" \
"Future 32-bit and Windows XP/Vista support is less likely.                   \n" \
"---                                                                          \n" \
"Documentation: https://github.com/ufrisk/MemProcFS/wiki/FS_SysInfo_Network   \n";

// ----------------------------------------------------------------------------
// Net functionality below:
// Show information related to TCP/IP connectivity in the analyzed system.
// ----------------------------------------------------------------------------

#define MSYSNET_LINELENGTH                  128ULL
#define MSYSNET_LINELENGTH_VERBOSE          278ULL
#define MSYSNET_LINEHEADER                  "   #    PID Proto  State        Src                           Dst                          Process"
#define MSYSNET_LINEHEADER_VERBOSE          MSYSNET_LINEHEADER "              Time                     Object Address    Process Path"



VOID MSysNet_ReadLineCB(_In_ VMM_HANDLE H, _Inout_opt_ PVOID ctx, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVMM_MAP_NETENTRY pe, _Out_writes_(cbLineLength + 1) LPSTR szu8)
{
    PVMM_PROCESS pObProcess = VmmProcessGet(H, pe->dwPID);
    Util_usnprintf_ln(szu8, cbLineLength,
        "%04x%7i %s %s",
        ie,
        pe->dwPID,
        pe->uszText,
        pObProcess ? pObProcess->pObPersistent->uszNameLong : ""
    );
    Ob_DECREF(pObProcess);
}

VOID MSysNet_ReadLineVerboseCB(_In_ VMM_HANDLE H, _Inout_opt_ PVOID ctx, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVMM_MAP_NETENTRY pe, _Out_writes_(cbLineLength + 1) LPSTR szu8)
{
    CHAR szTime[24];
    PVMM_PROCESS pObProcess = VmmProcessGet(H, pe->dwPID);
    Util_FileTime2String(pe->ftTime, szTime);
    Util_usnprintf_ln(szu8, cbLineLength,
        "%04x%7i %s %-20s %s  %016llx  %s",
        ie,
        pe->dwPID,
        pe->uszText,
        pObProcess ? pObProcess->pObPersistent->uszNameLong : "",
        szTime,
        pe->vaObj,
        pObProcess ? pObProcess->pObPersistent->uszPathKernel : ""
    );
    Ob_DECREF(pObProcess);
}

NTSTATUS MSysNet_Read(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    PVMMOB_MAP_NET pObNetMap;
    if(!_stricmp(ctxP->uszPath, "readme.txt")) {
        return Util_VfsReadFile_FromStrA(szMSYSNET_README, pb, cb, pcbRead, cbOffset);
    }
    if(VmmMap_GetNet(H, &pObNetMap)) {
        if(!_stricmp(ctxP->uszPath, "netstat.txt")) {
            nt = Util_VfsLineFixed_Read(
                H, (UTIL_VFSLINEFIXED_PFN_CB)MSysNet_ReadLineCB, NULL, MSYSNET_LINELENGTH, MSYSNET_LINEHEADER,
                pObNetMap->pMap, pObNetMap->cMap, sizeof(VMM_MAP_NETENTRY),
                pb, cb, pcbRead, cbOffset
            );
        }
        if(!_stricmp(ctxP->uszPath, "netstat-v.txt")) {
            nt = Util_VfsLineFixed_Read(
                H, (UTIL_VFSLINEFIXED_PFN_CB)MSysNet_ReadLineVerboseCB, NULL, MSYSNET_LINELENGTH_VERBOSE, MSYSNET_LINEHEADER_VERBOSE,
                pObNetMap->pMap, pObNetMap->cMap, sizeof(VMM_MAP_NETENTRY),
                pb, cb, pcbRead, cbOffset
            );
        }
        Ob_DECREF(pObNetMap);
    }
    return nt;
}

BOOL MSysNet_List(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _Inout_ PHANDLE pFileList)
{
    PVMMOB_MAP_NET pObNetMap;
    if(ctxP->uszPath[0]) { return FALSE; }
    VMMDLL_VfsList_AddFile(pFileList, "readme.txt", strlen(szMSYSNET_README), NULL);
    if(VmmMap_GetNet(H, &pObNetMap)) {
        VMMDLL_VfsList_AddFile(pFileList, "netstat.txt", UTIL_VFSLINEFIXED_LINECOUNT(H, pObNetMap->cMap) * MSYSNET_LINELENGTH, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "netstat-v.txt", UTIL_VFSLINEFIXED_LINECOUNT(H, pObNetMap->cMap) * MSYSNET_LINELENGTH_VERBOSE, NULL);
        Ob_DECREF(pObNetMap);
    }
    return TRUE;
}

VOID MSysNet_Timeline(
    _In_ VMM_HANDLE H,
    _In_opt_ PVOID ctxfc,
    _In_ HANDLE hTimeline,
    _In_ VOID(*pfnAddEntry)(_In_ VMM_HANDLE H, _In_ HANDLE hTimeline, _In_ QWORD ft, _In_ DWORD dwAction, _In_ DWORD dwPID, _In_ DWORD dwData32, _In_ QWORD qwData64, _In_ LPSTR uszText),
    _In_ VOID(*pfnEntryAddBySql)(_In_ VMM_HANDLE H, _In_ HANDLE hTimeline, _In_ DWORD cEntrySql, _In_ LPSTR *pszEntrySql)
) {
    DWORD i;
    PVMM_MAP_NETENTRY pe;
    PVMMOB_MAP_NET pObNetMap;
    if(VmmMap_GetNet(H, &pObNetMap)) {
        for(i = 0; i < pObNetMap->cMap; i++) {
            pe = pObNetMap->pMap + i;
            if(pe->ftTime && pe->uszText[0]) {
                pfnAddEntry(H, hTimeline, pe->ftTime, FC_TIMELINE_ACTION_CREATE, pe->dwPID, 0, pe->vaObj, pe->uszText);
            }
        }
        Ob_DECREF(pObNetMap);
    }
}

VOID MSysNet_FcLogJSON(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_CONTEXT ctxP, _In_ VOID(*pfnLogJSON)(_In_ VMM_HANDLE H, _In_ PVMMDLL_PLUGIN_FORENSIC_JSONDATA pData))
{
    PVMMDLL_PLUGIN_FORENSIC_JSONDATA pd;
    PVMMOB_MAP_NET pObNetMap = NULL;
    PVMM_MAP_NETENTRY pe;
    DWORD i;
    PVMM_PROCESS pObProcess = NULL;
    CHAR szTime[24], szu[MAX_PATH];
    if(ctxP->pProcess || !(pd = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMDLL_PLUGIN_FORENSIC_JSONDATA)))) { return; }
    pd->dwVersion = VMMDLL_PLUGIN_FORENSIC_JSONDATA_VERSION;
    pd->szjType = "net";
    if(VmmMap_GetNet(H, &pObNetMap)) {
        for(i = 0; i < pObNetMap->cMap; i++) {
            pe = pObNetMap->pMap + i;
            szu[0] = 0;
            if((pObProcess = VmmProcessGet(H, pe->dwPID))) {
                Util_FileTime2String(pe->ftTime, szTime);
                snprintf(szu, _countof(szu), "proc:[%s] time:[%s] path:[%s]",
                    pObProcess ? pObProcess->pObPersistent->uszNameLong : "",
                    szTime,
                    pObProcess ? pObProcess->pObPersistent->uszPathKernel : "");
                Ob_DECREF_NULL(&pObProcess);
            }
            pd->i = i;
            pd->dwPID = pe->dwPID;
            pd->vaObj = pe->vaObj;
            pd->usz[0] = pe->uszText;
            pd->usz[1] = szu;
            pfnLogJSON(H, pd);
        }
    }
    Ob_DECREF(pObNetMap);
    LocalFree(pd);
}

VOID M_SysNet_Initialize(_In_ VMM_HANDLE H, _Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if((pRI->tpSystem != VMM_SYSTEM_WINDOWS_X64) && (pRI->tpSystem != VMM_SYSTEM_WINDOWS_X86)) { return; }
    strcpy_s(pRI->reg_info.uszPathName, 128, "\\sys\\net");     // module name
    pRI->reg_info.fRootModule = TRUE;                           // module shows in root directory
    pRI->reg_fn.pfnList = MSysNet_List;                         // List function supported
    pRI->reg_fn.pfnRead = MSysNet_Read;                         // Read function supported
    pRI->reg_fnfc.pfnTimeline = MSysNet_Timeline;               // Timeline supported
    pRI->reg_fnfc.pfnLogJSON = MSysNet_FcLogJSON;               // JSON log function supported
    memcpy(pRI->reg_info.sTimelineNameShort, "Net", 4);
    strncpy_s(pRI->reg_info.uszTimelineFile, 32, "timeline_net", _TRUNCATE);
    pRI->pfnPluginManager_Register(H, pRI);
}
