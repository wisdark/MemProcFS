// m_ldrmodules.c : implementation of the ldrmodules built-in module.
//
// (c) Ulf Frisk, 2018-2021
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "pluginmanager.h"
#include "vmm.h"
#include "vmmwin.h"
#include "charutil.h"
#include "util.h"
#include "pe.h"

#define LDRMODULES_CACHE_TP_EAT             1
#define LDRMODULES_CACHE_TP_IAT             2
#define LDRMODULES_NUM_CACHE                8
#define LDRMODULES_LINELENGTH_X86           107ULL
#define LDRMODULES_LINELENGTH_X64           123ULL
#define LDRMODULES_LINELENGTH_DIRECTORIES   54ULL
#define LDRMODULES_LINELENGTH_SECTIONS      70ULL
#define LDRMODULES_LINELENGTH_EAT           78ULL
#define LDRMODULES_LINELENGTH_IAT           128ULL

#define LDRMODULES_LINEHEADER_X86       "   #    PID    Pages Range Start-End      Description"
#define LDRMODULES_LINEHEADER_X64       "   #    PID    Pages      Range Start-End                 Description"

#define LDRMODULES_MAX_IATEAT               0x10000

typedef struct tdOBLDRMODULES_CACHE_ENTRY {
    OB ObHdr;
    DWORD dwHash;
    DWORD tp;
    DWORD cb;
    BYTE pb[0];
} OBLDRMODULES_CACHE_ENTRY, *POBLDRMODULES_CACHE_ENTRY;

/*
* Dynamically generate the file \<modulename>\export.txt
*/
_Success_(return == 0)
NTSTATUS LdrModules_ReadFile_EAT(_In_ PVMM_PROCESS pProcess, _In_ PVMMOB_MAP_EAT pEatMap, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    LPSTR sz;
    QWORD i, o = 0, cbMax, cStart, cEnd, cbLINELENGTH;
    PVMM_MAP_EATENTRY pe;
    cbLINELENGTH = LDRMODULES_LINELENGTH_EAT;
    cStart = (DWORD)(cbOffset / cbLINELENGTH);
    cEnd = (DWORD)min(pEatMap->cMap - 1, (cb + cbOffset + cbLINELENGTH - 1) / cbLINELENGTH);
    cbMax = 1 + (1 + cEnd - cStart) * cbLINELENGTH;
    if(!pEatMap->cMap || (cStart > pEatMap->cMap)) { return VMMDLL_STATUS_END_OF_FILE; }
    if(!(sz = LocalAlloc(LMEM_ZEROINIT, (SIZE_T)cbMax))) { return VMMDLL_STATUS_FILE_INVALID; }
    for(i = cStart; i <= cEnd; i++) {
        pe = pEatMap->pMap + i;
        o += Util_usnprintf_ln(
            sz + o,
            cbLINELENGTH,
            "%04x %5i%8x %016llx %s",
            (WORD)i,
            pe->dwOrdinal,
            (DWORD)(pe->vaFunction - pEatMap->vaModuleBase),
            pe->vaFunction,
            pe->uszFunction[0] ? pe->uszFunction : "---"
        );
    }
    nt = Util_VfsReadFile_FromPBYTE(sz, cbMax - 1, pb, cb, pcbRead, cbOffset - cStart * cbLINELENGTH);
    LocalFree(sz);
    return nt;
}

/*
* Dynamically generate the file \<modulename>\import.txt
*/
_Success_(return == 0)
NTSTATUS LdrModules_ReadFile_IAT(_In_ PVMM_PROCESS pProcess, _In_ PVMMOB_MAP_IAT pIatMap, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    LPSTR sz;
    QWORD i, o = 0, cbMax, cStart, cEnd, cbLINELENGTH;
    PVMM_MAP_IATENTRY pe;
    cbLINELENGTH = LDRMODULES_LINELENGTH_IAT;
    cStart = (DWORD)(cbOffset / cbLINELENGTH);
    cEnd = (DWORD)min(pIatMap->cMap - 1, (cb + cbOffset + cbLINELENGTH - 1) / cbLINELENGTH);
    cbMax = 1 + (1 + cEnd - cStart) * cbLINELENGTH;
    if(!pIatMap->cMap || (cStart > pIatMap->cMap)) { return VMMDLL_STATUS_END_OF_FILE; }
    if(!(sz = LocalAlloc(LMEM_ZEROINIT, (SIZE_T)cbMax))) { return VMMDLL_STATUS_FILE_INVALID; }
    for(i = cStart; i <= cEnd; i++) {
        pe = pIatMap->pMap + i;
        o += Util_usnprintf_ln(
            sz + o,
            cbLINELENGTH,
            "%04x %016llx %-40.40s %s",     // 128 bytes (chars) / line (function)
            (WORD)i,
            pe->vaFunction,
            pe->uszFunction,
            pe->uszModule
        );
    }
    nt = Util_VfsReadFile_FromPBYTE(sz, cbMax - 1, pb, cb, pcbRead, cbOffset - cStart * cbLINELENGTH);
    LocalFree(sz);
    return nt;
}

/*
* Dynamically generate the file \<modulename>\directories
*/
_Success_(return == 0)
NTSTATUS LdrModules_ReadFile_Directories(_In_ PVMM_PROCESS pProcess, _In_ QWORD vaModuleBase, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt;
    LPSTR sz;
    QWORD i, o = 0, cbMax, cStart, cEnd, cbLINELENGTH;
    IMAGE_DATA_DIRECTORY Directory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
    if(!PE_DirectoryGetAll(pProcess, vaModuleBase, NULL, Directory)) { return VMMDLL_STATUS_FILE_INVALID; }
    cbLINELENGTH = LDRMODULES_LINELENGTH_DIRECTORIES;
    cStart = (DWORD)(cbOffset / cbLINELENGTH);
    cEnd = (DWORD)min(16 - 1, (cb + cbOffset + cbLINELENGTH - 1) / cbLINELENGTH);
    cbMax = 1 + (1 + cEnd - cStart) * cbLINELENGTH;
    if(cStart > 16) { return VMMDLL_STATUS_END_OF_FILE; }
    if(!(sz = LocalAlloc(LMEM_ZEROINIT, (SIZE_T)cbMax))) { return VMMDLL_STATUS_FILE_INVALID; }
    for(i = cStart; i <= min(15, cEnd); i++) {
        o += Util_usnprintf_ln(
            sz + o,
            cbLINELENGTH,
            "%x %-16.16s %016llx %08x %08x",
            (DWORD)i,
            PE_DATA_DIRECTORIES[i],
            vaModuleBase + Directory[i].VirtualAddress,
            Directory[i].VirtualAddress,
            Directory[i].Size
        );
    }
    nt = Util_VfsReadFile_FromPBYTE(sz, cbMax - 1, pb, cb, pcbRead, cbOffset - cStart * cbLINELENGTH);
    LocalFree(sz);
    return nt;
}

/*
* Dynamically generate the file \<modulename>\sections
*/
_Success_(return == 0)
NTSTATUS LdrModules_ReadFile_Sections(_In_ PVMM_PROCESS pProcess, _In_ QWORD vaModuleBase, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    LPSTR sz;
    QWORD i, o = 0, cbMax, cStart, cEnd, cbLINELENGTH;
    DWORD cSections;
    PIMAGE_SECTION_HEADER pSections = NULL;
    cSections = PE_SectionGetNumberOf(pProcess, vaModuleBase);
    cbLINELENGTH = LDRMODULES_LINELENGTH_SECTIONS;
    cStart = (DWORD)(cbOffset / cbLINELENGTH);
    cEnd = (DWORD)min(cSections - 1, (cb + cbOffset + cbLINELENGTH - 1) / cbLINELENGTH);
    cbMax = 1 + (1 + cEnd - cStart) * cbLINELENGTH;
    if(!cSections || (cStart > cSections)) { return VMMDLL_STATUS_END_OF_FILE; }
    if(!(sz = LocalAlloc(LMEM_ZEROINIT, (SIZE_T)cbMax))) { goto fail; }
    if(!(pSections = LocalAlloc(LMEM_ZEROINIT, cSections * sizeof(IMAGE_SECTION_HEADER)))) { goto fail; }
    if(!PE_SectionGetAll(pProcess, vaModuleBase, cSections, pSections)) { goto fail; }
    for(i = cStart; i <= cEnd; i++) {
        o += Util_usnprintf_ln(
            sz + o,
            cbLINELENGTH,
            "%02x %-8.8s  %016llx %08x %08x %c%c%c %08x %08x",
            (DWORD)i,
            pSections[i].Name,
            vaModuleBase + pSections[i].VirtualAddress,
            pSections[i].VirtualAddress,
            pSections[i].Misc.VirtualSize,
            (pSections[i].Characteristics & IMAGE_SCN_MEM_READ) ? 'r' : '-',
            (pSections[i].Characteristics & IMAGE_SCN_MEM_WRITE) ? 'w' : '-',
            (pSections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ? 'x' : '-',
            pSections[i].PointerToRawData,
            pSections[i].SizeOfRawData
        );
    }
    nt = Util_VfsReadFile_FromPBYTE(sz, cbMax - 1, pb, cb, pcbRead, cbOffset - cStart * cbLINELENGTH);
fail:
    LocalFree(pSections);
    LocalFree(sz);
    return nt;
}

/*
* Dynamically generate the file \modules.txt.
*/
VOID LdrModules_ModuleReadLine_Callback(_In_ PVMM_PROCESS pProcess, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVMM_MAP_MODULEENTRY pe, _Out_writes_(cbLineLength + 1) LPSTR usz)
{
    Util_usnprintf_ln(usz, cbLineLength,
        ctxVmm->f32 ? "%04x%7i %8x %08x-%08x %s %s" : "%04x%7i %8x %016llx-%016llx %s %s",
        ie,
        pProcess->dwPID,
        pe->cbImageSize >> 12,
        pe->vaBase,
        pe->vaBase + pe->cbImageSize - 1,
        pe->fWoW64 ? "32" : "  ",
        pe->uszText + pe->cbuText - min(65, pe->cbuText)
    );
}

/*
* Dynamically generate the file \unloaded_modules.txt.
*/
VOID LdrModules_UnloadedReadLine_Callback(_In_ PVMM_PROCESS pProcess, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVMM_MAP_UNLOADEDMODULEENTRY pe, _Out_writes_(cbLineLength + 1) LPSTR usz)
{
    Util_usnprintf_ln(usz, cbLineLength,
        ctxVmm->f32 ? "%04x%7i %8x %08x-%08x %s %s" : "%04x%7i %8x %016llx-%016llx %s %s",
        ie,
        pProcess->dwPID,
        pe->cbImageSize >> 12,
        pe->vaBase,
        pe->vaBase + pe->cbImageSize - 1,
        pe->fWoW64 ? "32" : "  ",
        pe->uszText
    );
}

/*
* Helper write function - Write to the requested data directory file.
*/
VOID LdrModules_Write_DirectoriesD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPSTR uszDirectory, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    DWORD i;
    IMAGE_DATA_DIRECTORY Directory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
    *pcbWrite = 0;
    if(PE_DirectoryGetAll(pProcess, pModule->vaBase, NULL, Directory)) {
        for(i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; i++) {
            if(!_strnicmp((LPSTR)PE_DATA_DIRECTORIES[i], uszDirectory, 0)) {
                VmmWriteAsFile(pProcess, pModule->vaBase + Directory[i].VirtualAddress, Directory[i].Size, pb, cb, pcbWrite, cbOffset);
            }
        }
    }
}

/*
* Helper write function - Write to the requested section header file.
*/
VOID LdrModules_Write_SectionsD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPSTR uszSection, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    IMAGE_SECTION_HEADER SectionHeader;
    if(!PE_SectionGetFromName(pProcess, pModule->vaBase, uszSection, &SectionHeader)) { *pcbWrite = 0;  return; }
    VmmWriteAsFile(pProcess, pModule->vaBase + SectionHeader.VirtualAddress, SectionHeader.Misc.VirtualSize, pb, cb, pcbWrite, cbOffset);
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
NTSTATUS LdrModules_Write(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    CHAR uszModuleName[MAX_PATH];
    LPSTR uszModuleSubPath;
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    *pcbWrite = 0;
    uszModuleSubPath = CharUtil_PathSplitFirst(ctx->uszPath, uszModuleName, sizeof(uszModuleName));
    if(uszModuleName[0] && uszModuleSubPath[0] && VmmMap_GetModuleEntryEx((PVMM_PROCESS)ctx->pProcess, 0, uszModuleName, &pObModuleMap, &pModule)) {
        if(!_stricmp(uszModuleSubPath, "pefile.dll")) {
            PE_FileRaw_Write(pProcess, pModule->vaBase, pb, cb, pcbWrite, (DWORD)cbOffset);
        }
        if(!_strnicmp(uszModuleSubPath, "sectionsd\\", 10)) {
            LdrModules_Write_SectionsD(pProcess, pModule, uszModuleSubPath + 10, pb, cb, pcbWrite, cbOffset);
        }
        if(!_strnicmp(uszModuleSubPath, "directoriesd\\", 13)) {
            LdrModules_Write_DirectoriesD(pProcess, pModule, uszModuleSubPath + 13, pb, cb, pcbWrite, cbOffset);
        }
    }
    Ob_DECREF(pObModuleMap);
    return VMM_STATUS_SUCCESS;
}

/*
* Helper read function - Read the requested data directory file.
*/
NTSTATUS LdrModules_Read_DirectoriesD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPSTR uszDirectory, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    DWORD i;
    IMAGE_DATA_DIRECTORY Directory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
    if(PE_DirectoryGetAll(pProcess, pModule->vaBase, NULL, Directory)) {
        for(i = 0; i < 16; i++) {
            if(!_stricmp(uszDirectory, PE_DATA_DIRECTORIES[i])) {
                return VmmReadAsFile(pProcess, pModule->vaBase + Directory[i].VirtualAddress, Directory[i].Size, pb, cb, pcbRead, cbOffset);
            }
        }
    }
    return VMMDLL_STATUS_FILE_INVALID;
}

/*
* Helper read function - Read the requested section header file.
*/
NTSTATUS LdrModules_Read_SectionsD(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPSTR uszSection, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    IMAGE_SECTION_HEADER SectionHeader;
    if(!PE_SectionGetFromName(pProcess, pModule->vaBase, uszSection, &SectionHeader)) { return VMMDLL_STATUS_FILE_INVALID; }
    return VmmReadAsFile(pProcess, pModule->vaBase + SectionHeader.VirtualAddress, SectionHeader.Misc.VirtualSize, pb, cb, pcbRead, cbOffset);
}

NTSTATUS LdrModules_Read_ModuleSubFile(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _In_ PVMM_MAP_MODULEENTRY pModule, _In_ LPSTR uszPath, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    PVMMOB_MAP_EAT pObEatMap = NULL;
    PVMMOB_MAP_IAT pObIatMap = NULL;
    POBLDRMODULES_CACHE_ENTRY pObCacheEntry = NULL;
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    if(!_stricmp(uszPath, "base.txt")) {
        return Util_VfsReadFile_FromQWORD(pModule->vaBase, pb, cb, pcbRead, cbOffset, FALSE);
    }
    if(!_stricmp(uszPath, "entry.txt")) {
        return Util_VfsReadFile_FromQWORD(pModule->vaEntry, pb, cb, pcbRead, cbOffset, FALSE);
    }
    if(!_stricmp(uszPath, "fullname.txt")) {
        return Util_VfsReadFile_FromPBYTE(pModule->uszFullName, strlen(pModule->uszFullName), pb, cb, pcbRead, cbOffset);
    }
    if(!_stricmp(uszPath, "size.txt")) {
        return Util_VfsReadFile_FromDWORD(pModule->cbImageSize, pb, cb, pcbRead, cbOffset, FALSE);
    }
    if(!_stricmp(uszPath, "directories.txt")) {
        return LdrModules_ReadFile_Directories(pProcess, pModule->vaBase, pb, cb, pcbRead, cbOffset);
    }
    if(!_stricmp(uszPath, "export.txt")) {
        if(VmmMap_GetEAT(pProcess, pModule, &pObEatMap)) {
            nt = LdrModules_ReadFile_EAT(pProcess, pObEatMap, pb, cb, pcbRead, cbOffset);
            Ob_DECREF(pObEatMap);
        }
        return nt;
    }
    if(!_stricmp(uszPath, "import.txt")) {
        if(VmmMap_GetIAT(pProcess, pModule, &pObIatMap)) {
            nt = LdrModules_ReadFile_IAT(pProcess, pObIatMap, pb, cb, pcbRead, cbOffset);
            Ob_DECREF(pObIatMap);
        }
        return nt;
    }
    if(!_stricmp(uszPath, "pefile.dll")) {
        return PE_FileRaw_Read(pProcess, pModule->vaBase, pb, cb, pcbRead, (DWORD)cbOffset) ? VMMDLL_STATUS_SUCCESS : VMMDLL_STATUS_FILE_INVALID;
    }
    if(!_stricmp(uszPath, "sections.txt")) {
        return LdrModules_ReadFile_Sections(pProcess, pModule->vaBase, pb, cb, pcbRead, cbOffset);
    }
    if(!_strnicmp(uszPath, "sectionsd\\", 10)) {
        return LdrModules_Read_SectionsD(pProcess, pModule, uszPath + 10, pb, cb, pcbRead, cbOffset);
    }
    if(!_strnicmp(uszPath, "directoriesd\\", 13)) {
        return LdrModules_Read_DirectoriesD(pProcess, pModule, uszPath + 13, pb, cb, pcbRead, cbOffset);
    }
    return VMMDLL_STATUS_FILE_INVALID;
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
NTSTATUS LdrModules_Read(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    CHAR uszModuleName[MAX_PATH];
    LPSTR uszModuleSubPath;
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMMOB_MAP_UNLOADEDMODULE pObUnloadedModuleMap = NULL;
    if(!_stricmp(ctx->uszPath, "modules.txt")) {
        if(VmmMap_GetModule((PVMM_PROCESS)ctx->pProcess, &pObModuleMap)) {
            nt = Util_VfsLineFixed_Read(
                (UTIL_VFSLINEFIXED_PFN_CB)LdrModules_ModuleReadLine_Callback, ctx->pProcess,
                (ctxVmm->f32 ? LDRMODULES_LINELENGTH_X86 : LDRMODULES_LINELENGTH_X64),
                (ctxVmm->f32 ? LDRMODULES_LINEHEADER_X86 : LDRMODULES_LINEHEADER_X64),
                pObModuleMap->pMap, pObModuleMap->cMap, sizeof(VMM_MAP_MODULEENTRY),
                pb, cb, pcbRead, cbOffset
            );
            Ob_DECREF(pObModuleMap);
        }
        return nt;
    }
    if(!_stricmp(ctx->uszPath, "unloaded_modules.txt")) {
        if(VmmMap_GetUnloadedModule((PVMM_PROCESS)ctx->pProcess, &pObUnloadedModuleMap)) {
            nt = Util_VfsLineFixed_Read(
                (UTIL_VFSLINEFIXED_PFN_CB)LdrModules_UnloadedReadLine_Callback, ctx->pProcess,
                (ctxVmm->f32 ? LDRMODULES_LINELENGTH_X86 : LDRMODULES_LINELENGTH_X64),
                (ctxVmm->f32 ? LDRMODULES_LINEHEADER_X86 : LDRMODULES_LINEHEADER_X64),
                pObUnloadedModuleMap->pMap, pObUnloadedModuleMap->cMap, sizeof(VMM_MAP_UNLOADEDMODULEENTRY),
                pb, cb, pcbRead, cbOffset
            );
            Ob_DECREF(pObUnloadedModuleMap);
        }
        return nt;
    }
    uszModuleSubPath = CharUtil_PathSplitFirst(ctx->uszPath, uszModuleName, sizeof(uszModuleName));
    *pcbRead = 0;
    if(uszModuleName[0] && uszModuleSubPath[0] && VmmMap_GetModuleEntryEx((PVMM_PROCESS)ctx->pProcess, 0, uszModuleName, &pObModuleMap, &pModule)) {
        nt = LdrModules_Read_ModuleSubFile(ctx, pModule, uszModuleSubPath, pb, cb, pcbRead, cbOffset);
        Ob_DECREF(pObModuleMap);
        return nt;
    }
    Ob_DECREF(pObModuleMap);
    return VMMDLL_STATUS_FILE_INVALID;
}

/*
* List : function as specified by the module manager. The module manager will
* call into this callback function whenever a list directory shall occur from
* the given module.
* -- ctx
* -- pFileList
* -- return
*/
BOOL LdrModules_List(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Inout_ PHANDLE pFileList)
{
    DWORD c, i, cbLine;
    CHAR szSectionName[9] = { 0 };
    CHAR uszPath1[MAX_PATH];
    LPSTR uszPath2;
    PVMMOB_MAP_MODULE pObModuleMap = NULL;
    PVMM_MAP_MODULEENTRY pModule = NULL;
    PIMAGE_SECTION_HEADER pSections = NULL;
    IMAGE_DATA_DIRECTORY pDataDirectories[16];
    PVMMOB_MAP_UNLOADEDMODULE pObUnloadedModuleMap = NULL;
    PVMM_PROCESS pProcess = (PVMM_PROCESS)ctx->pProcess;
    if(!VmmMap_GetModule(pProcess, &pObModuleMap)) { goto fail; }
    // modules root directory -> add directory per DLL
    if(!ctx->uszPath[0]) {
        for(i = 0; i < pObModuleMap->cMap; i++) {
            VMMDLL_VfsList_AddDirectory(pFileList, pObModuleMap->pMap[i].uszText, NULL);
        }
        cbLine = ctxVmm->f32 ? LDRMODULES_LINELENGTH_X86 : LDRMODULES_LINELENGTH_X64;
        VMMDLL_VfsList_AddFile(pFileList, "modules.txt", UTIL_VFSLINEFIXED_LINECOUNT(pObModuleMap->cMap) * cbLine, NULL);
        if(VmmMap_GetUnloadedModule(pProcess, &pObUnloadedModuleMap)) {
            VMMDLL_VfsList_AddFile(pFileList, "unloaded_modules.txt", UTIL_VFSLINEFIXED_LINECOUNT(pObUnloadedModuleMap->cMap) * cbLine, NULL);
            Ob_DECREF_NULL(&pObUnloadedModuleMap);
        }
        goto success;
    }
    // individual module directory -> list files
    uszPath2 = CharUtil_PathSplitFirst(ctx->uszPath, uszPath1, sizeof(uszPath1));
    if(!(pModule = VmmMap_GetModuleEntry(pObModuleMap, uszPath1))) { goto fail; }
    // module-specific 'root' directory
    if(!uszPath2[0]) {
        VMMDLL_VfsList_AddFile(pFileList, "base.txt", 16, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "entry.txt", 16, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "fullname.txt",  strlen(pModule->uszFullName), NULL);
        VMMDLL_VfsList_AddFile(pFileList, "size.txt", 8, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "directories.txt", IMAGE_NUMBEROF_DIRECTORY_ENTRIES * LDRMODULES_LINELENGTH_DIRECTORIES, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "export.txt", pModule->cEAT * LDRMODULES_LINELENGTH_EAT, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "import.txt", pModule->cIAT * LDRMODULES_LINELENGTH_IAT, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "sections.txt", pModule->cSection * LDRMODULES_LINELENGTH_SECTIONS, NULL);
        VMMDLL_VfsList_AddFile(pFileList, "pefile.dll", pModule->cbFileSizeRaw, NULL);
        VMMDLL_VfsList_AddDirectory(pFileList, "sectionsd", NULL);
        VMMDLL_VfsList_AddDirectory(pFileList, "directoriesd", NULL);
        goto success;
    }
    // module-specific 'sectiond' directory
    if(uszPath2[0] && !_stricmp(uszPath2, "sectionsd")) {
        c = PE_SectionGetNumberOf(pProcess, pModule->vaBase);
        if(!(pSections = LocalAlloc(0, c * sizeof(IMAGE_SECTION_HEADER)))) { goto fail; }
        if(!PE_SectionGetAll(pProcess, pModule->vaBase, c, pSections)) { goto fail; }
        for(i = 0; i < c; i++) {
            if(pSections[i].Name[0]) {
                memcpy(szSectionName, pSections[i].Name, 8);
            } else {
                snprintf(szSectionName, 9, "%02x", i);
            }
            VMMDLL_VfsList_AddFile(pFileList, szSectionName, pSections[i].Misc.VirtualSize, NULL);
        }
        LocalFree(pSections);
        goto success;
    }
    // module-specific 'directoriesd' directory
    if(uszPath2[0] && !_stricmp(uszPath2, "directoriesd")) {
        ZeroMemory(pDataDirectories, IMAGE_NUMBEROF_DIRECTORY_ENTRIES * sizeof(IMAGE_DATA_DIRECTORY));
        if(PE_DirectoryGetAll(pProcess, pModule->vaBase, NULL, pDataDirectories)) {
            for(i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; i++) {
                VMMDLL_VfsList_AddFile(pFileList, (LPSTR)PE_DATA_DIRECTORIES[i], pDataDirectories[i].Size, NULL);
            }
        }
        goto success;
    }
fail:
    Ob_DECREF(pObModuleMap);
    return FALSE;
success:
    Ob_DECREF(pObModuleMap);
    return TRUE;
}

/*
* Initialization function. The module manager shall call into this function
* when the module shall be initialized. If the module wish to initialize it
* shall call the supplied pfnPluginManager_Register function.
* NB! the module does not have to register itself - for example if the target
* operating system or architecture is unsupported.
* -- pPluginRegInfo
*/
VOID M_LdrModules_Initialize(_Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if((pRI->tpSystem != VMM_SYSTEM_WINDOWS_X64) && (pRI->tpSystem != VMM_SYSTEM_WINDOWS_X86)) { return; }
    strcpy_s(pRI->reg_info.uszPathName, 128, "\\modules");           // module name
    pRI->reg_info.fProcessModule = TRUE;                             // module shows in process directory
    pRI->reg_fn.pfnList = LdrModules_List;                           // List function supported
    pRI->reg_fn.pfnRead = LdrModules_Read;                           // Read function supported
    if(ctxMain->dev.fWritable) {
        pRI->reg_fn.pfnWrite = LdrModules_Write;                     // Write function supported
    }
    pRI->pfnPluginManager_Register(pRI);
}
