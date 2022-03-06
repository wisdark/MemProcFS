// m_vmemd.c : implementation related to the vmemd native plugin module for the
// memory process file system.
//
// (c) Ulf Frisk, 2018-2022
// Author: Ulf Frisk, pcileech@frizk.net
//
#include <Windows.h>
#include <stdio.h>
#include <vmmdll.h>

VMMDLL_MEMORYMODEL_TP g_VMemD_TpMemoryModel = VMMDLL_MEMORYMODEL_NA;

#define UTIL_ASCIIFILENAME_ALLOW \
    "0000000000000000000000000000000011011111111111101111111111010100" \
    "1111111111111111111111111111011111111111111111111111111111110111" \
    "0000000000000000000000000000000000000000000000000000000000000000" \
    "0000000000000000000000000000000000000000000000000000000000000000"

/*
* Utility function to retrieve base address and the type of entry from a file name.
* -- wsz
* -- return
*/
_Success_(return)
BOOL VMemD_GetBaseAndTypeFromFileName(_In_ LPSTR usz, _Out_ PQWORD pva, _Out_ PBOOL pfVad)
{
    if((strlen(usz) < 15) || (usz[0] != '0') || (usz[1] != 'x')) { return FALSE; }
    *pva = strtoull(usz, NULL, 16);
    *pfVad = strstr(usz, ".vvmem") ? TRUE : FALSE;
    return TRUE;
}

VOID VMemD_Util_FileNameU(_Out_writes_(64) LPSTR uszOut, _In_ LPWSTR wsz)
{
    WCHAR ch;
    DWORD i = 0;
    while(wsz[i]) {
        if(wsz[i] == '\\') {
            wsz += i + 1ULL;
            i = 0;
            continue;
        }
        if(i == 62) {
            wsz += 1;
            continue;
        }
        i++;
    }
    i = 0;
    while((ch = wsz[i])) {
        uszOut[i] = ((ch < 128) && (UTIL_ASCIIFILENAME_ALLOW[ch] == '0')) ? '_' : ch;
        i++;
    }
    uszOut[i] = 0;
}

/*
* Utility function to efficiently search through an ordered array of data with
* a comparator function.
*/
PVOID VMemD_Util_qfind(_In_ PVOID pvFind, _In_ DWORD cMap, _In_ PVOID pvMap, _In_ DWORD cbEntry, _In_ int(*pfnCmp)(_In_ PVOID pvFind, _In_ PVOID pvEntry))
{
    int f;
    DWORD i, cbSearch, cbStep, cbMap;
    PBYTE pbMap = pvMap;
    if(!cMap || !cbEntry) { return NULL; }
    for(i = 1; ((cMap - 1) >> i); i++);
    cbMap = cMap * cbEntry;
    cbSearch = cbEntry * min(1UL << (i - 1), cMap - 1);
    cbStep = max(cbEntry, cbSearch >> 1);
    while(cbStep >= cbEntry) {
        f = pfnCmp(pvFind, pbMap + cbSearch);
        if(f < 0) {
            cbSearch -= cbStep;
        } else if(f > 0) {
            if(cbSearch + cbStep < cbMap) {
                cbSearch += cbStep;
            }
        } else {
            return pbMap + cbSearch;
        }
        cbStep = cbStep >> 1;
    }
    if(cbSearch < cbMap) {
        if(!pfnCmp(pvFind, pbMap + cbSearch)) {
            return pbMap + cbSearch;
        }
        if((cbSearch >= cbEntry) && !pfnCmp(pvFind, pbMap + cbSearch - cbEntry)) {
            return pbMap + cbSearch - cbEntry;
        }
    }
    return NULL;
}

/*
* Comparator function for VMemD_Util_qfind to serach entries in PTEMAP.
*/
int VMemD_ReadPte_CmpFind(_In_ QWORD vaFind, _In_ PVMMDLL_MAP_PTEENTRY pEntry)
{
    if(pEntry->vaBase > vaFind) { return -1; }
    if(pEntry->vaBase < vaFind) { return 1; }
    return 0;
}

/*
* Comparator function for VMemD_Util_qfind to serach entries in VADMAP.
*/
int VMemD_ReadVad_CmpFind(_In_ QWORD vaFind, _In_ PVMMDLL_MAP_VADENTRY pEntry)
{
    if(pEntry->vaStart > vaFind) { return -1; }
    if(pEntry->vaStart < vaFind) { return 1; }
    return 0;
}

/*
* Read/Write virtual memory inside a memory map entry of PTE-type.
*/
NTSTATUS VMemD_ReadWritePte(_In_ DWORD dwPID, _In_ QWORD vaBase, _In_ BOOL fRead, _Out_writes_bytes_(*pcbReadWrite) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbReadWrite, _In_ ULONG64 cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    BOOL result;
    QWORD cbMax;
    DWORD cbPteMap = 0;
    PVMMDLL_MAP_PTE pPteMap = NULL;
    PVMMDLL_MAP_PTEENTRY pe = NULL;
    // read memory from "vmemd" directory file - "pte mapped"
    *pcbReadWrite = 0;
    result =
        VMMDLL_Map_GetPte(dwPID, NULL, &cbPteMap, FALSE) &&
        (pPteMap = LocalAlloc(0, cbPteMap)) &&
        VMMDLL_Map_GetPte(dwPID, pPteMap, &cbPteMap, FALSE) &&
        (pe = VMemD_Util_qfind((PVOID)vaBase, pPteMap->cMap, pPteMap->pMap, sizeof(VMMDLL_MAP_PTEENTRY), (int(*)(PVOID, PVOID))VMemD_ReadPte_CmpFind));
    if(!result) { goto fail; }
    if(pe->vaBase + (pe->cPages << 12) <= vaBase + cbOffset) {
        nt = VMMDLL_STATUS_END_OF_FILE;
        goto fail;
    }
    cbMax = min((pe->vaBase + (pe->cPages << 12)), (vaBase + cb + cbOffset)) - (vaBase + cbOffset);   // min(entry_top_addr, request_top_addr) - request_start_addr
    if(fRead) {
        result = VMMDLL_MemReadEx(dwPID, vaBase + cbOffset, pb, (DWORD)min(cb, cbMax), NULL, VMMDLL_FLAG_ZEROPAD_ON_FAIL);
        *pcbReadWrite = (DWORD)min(cb, cbMax);
        nt = (result && *pcbReadWrite) ? VMMDLL_STATUS_SUCCESS : VMMDLL_STATUS_END_OF_FILE;
    } else {
        VMMDLL_MemWrite(dwPID, vaBase + cbOffset, pb, (DWORD)min(cb, cbMax));
        *pcbReadWrite = cb;
        nt = VMMDLL_STATUS_SUCCESS;
    }
fail:
    LocalFree(pPteMap);
    return nt;
}

/*
* Read/Write virtual memory inside a memory map entry of VAD-type.
*/
NTSTATUS VMemD_ReadWriteVad(_In_ DWORD dwPID, _In_ QWORD vaBase, _In_ BOOL fRead, _Out_writes_bytes_(*pcbReadWrite) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbReadWrite, _In_ ULONG64 cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    BOOL result;
    QWORD cbMax;
    DWORD cbVadMap = 0;
    PVMMDLL_MAP_VAD pVadMap = NULL;
    PVMMDLL_MAP_VADENTRY pe = NULL;
    // read memory from "vmemd" directory file - "pte mapped"
    *pcbReadWrite = 0;
    result =
        VMMDLL_Map_GetVad(dwPID, NULL, &cbVadMap, FALSE) &&
        (pVadMap = LocalAlloc(0, cbVadMap)) &&
        VMMDLL_Map_GetVad(dwPID, pVadMap, &cbVadMap, FALSE) &&
        (pe = VMemD_Util_qfind((PVOID)vaBase, pVadMap->cMap, pVadMap->pMap, sizeof(VMMDLL_MAP_VADENTRY), (int(*)(PVOID, PVOID))VMemD_ReadVad_CmpFind));
    if(!result) { goto fail; }
    if(pe->vaEnd <= vaBase + cbOffset) {
        nt = VMMDLL_STATUS_END_OF_FILE;
        goto fail;
    }
    cbMax = min(pe->vaEnd + 1, (vaBase + cb + cbOffset)) - (vaBase + cbOffset);   // min(entry_top_addr, request_top_addr) - request_start_addr
    if(fRead) {
        result = VMMDLL_MemReadEx(dwPID, vaBase + cbOffset, pb, (DWORD)min(cb, cbMax), NULL, VMMDLL_FLAG_ZEROPAD_ON_FAIL);
        *pcbReadWrite = (DWORD)min(cb, cbMax);
        nt = (result && *pcbReadWrite) ? VMMDLL_STATUS_SUCCESS : VMMDLL_STATUS_END_OF_FILE;
    } else {
        VMMDLL_MemWrite(dwPID, vaBase + cbOffset, pb, (DWORD)min(cb, cbMax));
        *pcbReadWrite = cb;
        nt = VMMDLL_STATUS_SUCCESS;
    }
fail:
    LocalFree(pVadMap);
    return nt;
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
NTSTATUS VMemD_Read(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Out_ LPVOID pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ ULONG64 cbOffset)
{
    BOOL fVad;
    QWORD vaBase;
    if(!VMemD_GetBaseAndTypeFromFileName(ctx->uszPath, &vaBase, &fVad)) { return VMMDLL_STATUS_FILE_INVALID; }
    return fVad ?
        VMemD_ReadWriteVad(ctx->dwPID, vaBase, TRUE, pb, cb, pcbRead, cbOffset) :
        VMemD_ReadWritePte(ctx->dwPID, vaBase, TRUE, pb, cb, pcbRead, cbOffset);
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
NTSTATUS VMemD_WritePte(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _In_ LPVOID pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ ULONG64 cbOffset)
{
    BOOL fVad;
    QWORD vaBase;
    if(!VMemD_GetBaseAndTypeFromFileName(ctx->uszPath, &vaBase, &fVad)) { return VMMDLL_STATUS_FILE_INVALID; }
    return fVad ?
        VMemD_ReadWriteVad(ctx->dwPID, vaBase, FALSE, pb, cb, pcbWrite, cbOffset) :
        VMemD_ReadWritePte(ctx->dwPID, vaBase, FALSE, pb, cb, pcbWrite, cbOffset);
}

/*
* List : function as specified by the module manager. The module manager will
* call into this callback function whenever a list directory shall occur from
* the given module.
* -- ctx
* -- pFileList
* -- return
*/
BOOL VMemD_List(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Inout_ PHANDLE pFileList)
{
    BOOL f, fResult = FALSE;
    DWORD iVad, iPte, cbPteMap = 0, cbVadMap = 0;
    PVMMDLL_MAP_PTE pPteMap = NULL;
    PVMMDLL_MAP_PTEENTRY pPte = NULL;
    PVMMDLL_MAP_VAD pVadMap = NULL;
    PVMMDLL_MAP_VADENTRY pVad = NULL;
    WCHAR wszBufferFileName[MAX_PATH] = { 0 };
    CHAR uszInfo[64] = { 0 };
    // Retrieve mandatory memory map based on hardware page tables.
    f = VMMDLL_Map_GetPte(ctx->dwPID, NULL, &cbPteMap, TRUE) &&
        (pPteMap = LocalAlloc(0, cbPteMap));
    f = f && VMMDLL_Map_GetPte(ctx->dwPID, pPteMap, &cbPteMap, TRUE);
    if(!f) { goto fail; }
    // Retrieve optional memory map based on virtual address descriptors (VADs).
    f = VMMDLL_Map_GetVad(ctx->dwPID, NULL, &cbVadMap, TRUE) &&
        (pVadMap = LocalAlloc(0, cbVadMap)) &&
        VMMDLL_Map_GetVad(ctx->dwPID, pVadMap, &cbVadMap, TRUE);
    // Display VadMap entries in the file system (if any)
    for(iVad = 0; (f && (iVad < pVadMap->cMap)); iVad++) {
        pVad = pVadMap->pMap + iVad;
        VMemD_Util_FileNameU(uszInfo, pVad->wszText);
        if(g_VMemD_TpMemoryModel == VMMDLL_MEMORYMODEL_X64) {
            swprintf_s(
                wszBufferFileName,
                MAX_PATH - 1,
                L"0x%016llx%s%S.vvmem",
                pVad->vaStart,
                uszInfo[0] ? L"-" : L"",
                uszInfo
            );
        } else if((g_VMemD_TpMemoryModel == VMMDLL_MEMORYMODEL_X86) || (g_VMemD_TpMemoryModel == VMMDLL_MEMORYMODEL_X86PAE)) {
            swprintf_s(
                wszBufferFileName,
                MAX_PATH - 1,
                L"0x%08lx%s%S.vvmem",
                (DWORD)pVad->vaStart,
                uszInfo[0] ? L"-" : L"",
                uszInfo
            );
        }
        VMMDLL_VfsList_AddFileW(pFileList, wszBufferFileName, pVad->vaEnd + 1 - pVad->vaStart, NULL);
    }
    // Display PteMap entries in the file system unless already part of Vad
    for(iPte = 0, iVad = 0; iPte < pPteMap->cMap; iPte++) {
        pPte = pPteMap->pMap + iPte;
        if(pVadMap) {
            while((iVad < pVadMap->cMap) && (pVadMap->pMap[iVad].vaEnd < pPte->vaBase) && ++iVad);
            if((iVad < pVadMap->cMap) && (pVadMap->pMap[iVad].vaStart <= pPte->vaBase) && (pVadMap->pMap[iVad].vaEnd >= pPte->vaBase)) { continue; }
        }
        VMemD_Util_FileNameU(uszInfo, pPte->wszText);
        if(g_VMemD_TpMemoryModel == VMMDLL_MEMORYMODEL_X64) {
            swprintf_s(
                wszBufferFileName,
                MAX_PATH - 1,
                L"0x%016llx%s%S.vmem",
                pPte->vaBase,
                uszInfo[0] ? L"-" : L"",
                uszInfo
            );
        } else if((g_VMemD_TpMemoryModel == VMMDLL_MEMORYMODEL_X86) || (g_VMemD_TpMemoryModel == VMMDLL_MEMORYMODEL_X86PAE)) {
            swprintf_s(
                wszBufferFileName,
                MAX_PATH - 1,
                L"0x%08lx%s%S.vmem",
                (DWORD)pPte->vaBase,
                uszInfo[0] ? L"-" : L"",
                uszInfo
            );
        }
        VMMDLL_VfsList_AddFileW(pFileList, wszBufferFileName, pPte->cPages << 12, NULL);
    }
    fResult = TRUE;
fail:
    LocalFree(pPteMap);
    LocalFree(pVadMap);
    return fResult;
}

/*
* Initialization function for the vmemd native plugin module.
* It's important that the function is exported in the DLL and that it is
* declared exactly as below. The plugin manager will call into this function
* after the DLL is loaded. The DLL then must fill the appropriate information
* into the supplied struct and call the pfnPluginManager_Register function to
* register itself with the plugin manager.
* -- pRegInfo
*/
__declspec(dllexport)
VOID InitializeVmmPlugin(_In_ PVMMDLL_PLUGIN_REGINFO pRegInfo)
{
    if((pRegInfo->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRegInfo->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    // Ensure that the plugin support the memory model that is used. The plugin
    // currently supports the 64-bit x64 and 32-bit x86 and x86-pae memory models.
    if(!((pRegInfo->tpMemoryModel == VMMDLL_MEMORYMODEL_X64) || (pRegInfo->tpMemoryModel == VMMDLL_MEMORYMODEL_X86) || (pRegInfo->tpMemoryModel == VMMDLL_MEMORYMODEL_X86PAE))) { return; }
    g_VMemD_TpMemoryModel = pRegInfo->tpMemoryModel;
    strcpy_s(pRegInfo->reg_info.uszPathName, 128, "\\vmemd");   // module name - 'vmemd'.
    pRegInfo->reg_info.fProcessModule = TRUE;                   // module shows in process directory.
    pRegInfo->reg_fn.pfnList = VMemD_List;                      // List function supported.
    pRegInfo->reg_fn.pfnRead = VMemD_Read;                      // Read function supported.
    pRegInfo->reg_fn.pfnWrite = VMemD_WritePte;                 // Write function supported.
    pRegInfo->pfnPluginManager_Register(pRegInfo);              // Register with the plugin manager.
}
