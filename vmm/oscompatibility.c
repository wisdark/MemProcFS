// oscompatibility.c : VMM Windows/Linux compatibility layer.
//
// (c) Ulf Frisk, 2021-2023
// Author: Ulf Frisk, pcileech@frizk.net
//
#ifdef LINUX

#include "oscompatibility.h"
#include "util.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <linux/futex.h>

VMMFN_RtlDecompressBuffer OSCOMPAT_RtlDecompressBuffer;

// ----------------------------------------------------------------------------
// LocalAlloc/LocalFree BELOW:
// ----------------------------------------------------------------------------

HANDLE LocalAlloc(DWORD uFlags, SIZE_T uBytes)
{
    HANDLE h = malloc(uBytes);
    if(h && (uFlags & LMEM_ZEROINIT)) {
        memset(h, 0, uBytes);
    }
    return h;
}

VOID LocalFree(HANDLE hMem)
{
    free(hMem);
}

// ----------------------------------------------------------------------------
// LIBRARY FUNCTIONS BELOW:
// ----------------------------------------------------------------------------

FARPROC GetProcAddress(_In_opt_ HMODULE hModule, _In_ LPSTR lpProcName)
{
    if(!strcmp(lpProcName, "RtlDecompressBuffer")) {
        return OSCOMPAT_RtlDecompressBuffer;
    }
    if(hModule && ((SIZE_T)hModule & 0xfff)) {
        return dlsym(hModule, lpProcName);
    } 
    return NULL;
}

HMODULE LoadLibraryA(LPSTR lpFileName)
{
    if(!strcmp(lpFileName, "ntdll.dll")) {
        return (HMODULE)0x1000;      // FAKE HMODULE
    }
    if(lpFileName[0] == '/') {
        return dlopen(lpFileName, RTLD_NOW);
    }
    return 0;
}

BOOL FreeLibrary(_In_ HMODULE hLibModule)
{
    if((SIZE_T)hLibModule && ((SIZE_T)hLibModule > 0x10000)) {
        dlclose(hLibModule);
    }
    return TRUE;
}

DWORD GetModuleFileNameA(_In_opt_ HMODULE hModule, _Out_ LPSTR lpFilename, _In_ DWORD nSize)
{
    struct link_map *lm = NULL;
    if(hModule && ((SIZE_T)hModule & 0xfff)) {
        dlinfo(hModule, RTLD_DI_LINKMAP, &lm);
        if(lm) {
            strncpy(lpFilename, lm->l_name, nSize);
            lpFilename[nSize - 1] = 0;
            return strlen(lpFilename);
        }
    }
    return readlink("/proc/self/exe", lpFilename, nSize);
}

typedef struct tdMODULE_CB_INFO {
    LPCSTR lpModuleName;
    HMODULE hModule;
} MODULE_CB_INFO, *PMODULE_CB_INFO;

int GetModuleHandleA_CB(struct dl_phdr_info *info, size_t size, void *data)
{
    PMODULE_CB_INFO ctx = (PMODULE_CB_INFO)data;
    if(!ctx->lpModuleName && (info->dlpi_name[0] == 0)) {
        //Dl_info dl;
        //DWORD DEBUG1 = dladdr((void *)(info->dlpi_addr + info->dlpi_phdr[0].p_vaddr), &dl);
        ctx->hModule = (HMODULE)info->dlpi_addr;
        return 1;
    }
    if(ctx->lpModuleName && info->dlpi_name[0] && strstr(info->dlpi_name, ctx->lpModuleName)) {
        //Dl_info dl;
        //DWORD DEBUG1 = dladdr((void *)(info->dlpi_addr + info->dlpi_phdr[0].p_vaddr), &dl);
        ctx->hModule = (HMODULE)info->dlpi_addr;
        return 1;
    }
    return 0;
}

HMODULE GetModuleHandleA(_In_opt_ LPCSTR lpModuleName)
{
    MODULE_CB_INFO info = { 0 };
    info.lpModuleName = lpModuleName;
    dl_iterate_phdr(GetModuleHandleA_CB, (void*)&info);
    return info.hModule;
}

// ----------------------------------------------------------------------------
// GENERAL HANDLES BELOW:
// ----------------------------------------------------------------------------

#define OSCOMPATIBILITY_HANDLE_INTERNAL         0x35d91cca
#define OSCOMPATIBILITY_HANDLE_TYPE_THREAD      2
#define OSCOMPATIBILITY_HANDLE_TYPE_EVENT       3

typedef struct tdHANDLE_INTERNAL {
    DWORD magic;
    DWORD type;
} HANDLE_INTERNAL, *PHANDLE_INTERNAL;

typedef struct tdHANDLE_INTERNAL_THREAD {
    DWORD magic;
    DWORD type;
    pthread_t thread;
} HANDLE_INTERNAL_THREAD, *PHANDLE_INTERNAL_THREAD;

BOOL CloseHandle(_In_ HANDLE hObject)
{
    PHANDLE_INTERNAL hi = (PHANDLE_INTERNAL)hObject;
    if(hi->magic != OSCOMPATIBILITY_HANDLE_INTERNAL) { return FALSE; }
    switch(hi->type) {
        case OSCOMPATIBILITY_HANDLE_TYPE_THREAD:
            pthread_join(((PHANDLE_INTERNAL_THREAD)hi)->thread, NULL);
            break;
        case OSCOMPATIBILITY_HANDLE_TYPE_EVENT:
            SetEvent(hObject);
            break;
        default:
            break;
    }
    LocalFree(hi);
    return TRUE;
}

QWORD GetTickCount64()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / (1000 * 1000);
}

BOOL QueryPerformanceFrequency(_Out_ LARGE_INTEGER *lpFrequency)
{
    *lpFrequency = 1000 * 1000;
    return TRUE;
}

BOOL QueryPerformanceCounter(_Out_ LARGE_INTEGER *lpPerformanceCount)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    *lpPerformanceCount = (ts.tv_sec * 1000 * 1000) + (ts.tv_nsec / 1000);  // uS resolution
    return TRUE;
}

HANDLE CreateThread(
    PVOID     lpThreadAttributes,
    SIZE_T    dwStackSize,
    PVOID     lpStartAddress,
    PVOID     lpParameter,
    DWORD     dwCreationFlags,
    PDWORD    lpThreadId
) {
    PHANDLE_INTERNAL_THREAD ph;
    pthread_t thread;
    int status;
    status = pthread_create(&thread, NULL, lpStartAddress, lpParameter);
    if(status) { return NULL;}
    ph = malloc(sizeof(HANDLE_INTERNAL_THREAD));
    if(!ph) { return NULL; }
    ph->magic = OSCOMPATIBILITY_HANDLE_INTERNAL;
    ph->type = OSCOMPATIBILITY_HANDLE_TYPE_THREAD;
    ph->thread = thread;
    return (HANDLE)ph;
}

BOOL GetExitCodeThread(_In_ HANDLE hThread, _Out_ LPDWORD lpExitCode)
{
    PHANDLE_INTERNAL_THREAD ph = (PHANDLE_INTERNAL_THREAD)hThread;
    *lpExitCode = 0;
    if((ph->magic != OSCOMPATIBILITY_HANDLE_INTERNAL) || (ph->type != OSCOMPATIBILITY_HANDLE_TYPE_THREAD)) { return FALSE; }
    return 0 == pthread_join(ph->thread, NULL);
}

VOID GetLocalTime(LPSYSTEMTIME lpSystemTime)
{
    time_t curtime;
    struct tm t = { 0 };
    curtime = time(NULL);
    localtime_r(&curtime, &t);
    lpSystemTime->wYear = t.tm_year;
    lpSystemTime->wMonth = t.tm_mon;
    lpSystemTime->wDayOfWeek = t.tm_wday;
    lpSystemTime->wDay = t.tm_mday;
    lpSystemTime->wHour = t.tm_hour;
    lpSystemTime->wMinute = t.tm_min;
    lpSystemTime->wSecond = t.tm_sec;
    lpSystemTime->wMilliseconds = 0;
}

VOID GetSystemTimeAsFileTime(PFILETIME lpSystemTimeAsFileTime)
{
    *lpSystemTimeAsFileTime = (time(NULL) * 10000000) + 116444736000000000;
}

HANDLE FindFirstFileA(LPSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    DWORD i;
    DIR *hDir;
    CHAR szDirName[MAX_PATH] = { 0 };
    strcpy_s(lpFindFileData->__cExtension, 5, lpFileName + strlen(lpFileName) - 3);
    strcpy_s(szDirName, MAX_PATH - 1, lpFileName);
    for(i = strlen(szDirName) - 1; i > 0; i--) {
        if(szDirName[i] == '/') {
            szDirName[i] = 0;
            break;
        }
    }
    hDir = opendir(szDirName);
    if(!hDir) { return NULL; }
    return FindNextFileA((HANDLE)hDir, lpFindFileData) ? (HANDLE)hDir : INVALID_HANDLE_VALUE;
}

BOOL FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    DIR *hDir = (DIR*)hFindFile;
    struct dirent *dir;
    char* sz;
    if(!hDir) { return FALSE; }
    while ((dir = readdir(hDir)) != NULL) {
        sz = dir->d_name;
        if((strlen(sz) > 3) && !strcasecmp(sz + strlen(sz) - 3, lpFindFileData->__cExtension)) {
            strcpy_s(lpFindFileData->cFileName, MAX_PATH, sz);
            return TRUE;
        }
    }
    closedir(hDir);
    return FALSE;
}

DWORD InterlockedAdd(DWORD *Addend, DWORD Value)
{
    return __sync_add_and_fetch(Addend, Value);
}

BOOL FileTimeToSystemTime(_In_ PFILETIME lpFileTime, _Out_ PSYSTEMTIME pSystemTime)
{
    time_t tm = 0;
    struct tm t = { 0 };
    if(*lpFileTime >= 116444736000000000ULL) {
        tm = (*lpFileTime - 116444736000000000ULL) / 10000000ULL;
    }
    gmtime_r(&tm, &t);
    pSystemTime->wYear = 1900 + t.tm_year;
    pSystemTime->wMonth = 1 + t.tm_mon;
    pSystemTime->wDayOfWeek = t.tm_wday;
    pSystemTime->wDay = t.tm_mday;
    pSystemTime->wHour = t.tm_hour;
    pSystemTime->wMinute = t.tm_min;
    pSystemTime->wSecond = t.tm_sec;
    pSystemTime->wMilliseconds = (*lpFileTime / 10000) % 1000;
    return TRUE;
}



// ----------------------------------------------------------------------------
// SID functionality below:
// ----------------------------------------------------------------------------

_Success_(return)
BOOL ConvertStringSidToSidA(_In_opt_ LPSTR szSID, _Outptr_ PSID *ppSID)
{
    BYTE c = 0;
    DWORD i, csz;
    PBYTE pbSID = NULL;
    if(!szSID || !ppSID) { return FALSE; }
    if(strncmp(szSID, "S-1-", 4)) { return FALSE; }
    szSID += 4;
    csz = (DWORD)strlen(szSID);
    for(i = 0; i < csz; i++) {
        if(szSID[i] == '-') {
            if(szSID[i - 1] == '-') { return FALSE; }
            c++;
        }
    }
    if((c == 0) || (c > SID_MAX_SUB_AUTHORITIES) || (szSID[csz - 1] == '-')) { return FALSE; }
    if(!(pbSID = LocalAlloc(0, 8 + c * sizeof(DWORD)))) { return FALSE; }
    *ppSID = (PSID)pbSID;
    *(PQWORD)pbSID = _byteswap_uint64(strtoull(szSID, NULL, 10));
    pbSID[0] = 1;
    pbSID[1] = c;
    pbSID += sizeof(QWORD);
    while(TRUE) {
        while(TRUE) {
            szSID += 1;
            if(szSID[0] == 0) { return TRUE; }
            if(szSID[0] == '-') { szSID += 1; break; }
        }
        *(PDWORD)pbSID = strtoul(szSID, NULL, 10);
        pbSID += sizeof(DWORD);
    }
}

/*
* Linux compatible function of WIN32 API function ConvertSidToStringSidA()
* CALLER LocalFree: *pszSid
* -- pSID
* -- pszSID
* -- return
*/
_Success_(return)
BOOL ConvertSidToStringSidA(_In_opt_ PSID pSID, _Outptr_ LPSTR *pszSid)
{
    PBYTE pbSID = (PBYTE)pSID;
    DWORD dwVersion, c, o, cbSID;
    QWORD qwAuthority;
    LPSTR szSID;
    if(!pSID) { return FALSE; }
    dwVersion = pbSID[0];
    if(dwVersion != 1) { return FALSE; }
    c = pbSID[1];
    if((c == 0) || (c > SID_MAX_SUB_AUTHORITIES)) { return FALSE; }
    qwAuthority = _byteswap_uint64(*(PQWORD)(pbSID)) & 0x0000ffffffffffff;
    cbSID = 64 + c * 12;
    if(!(szSID = LocalAlloc(0, cbSID))) { return FALSE; }
    o = snprintf(szSID, cbSID, "S-1-%llu", qwAuthority);
    pbSID += 8;
    while(c) {
        o += snprintf(szSID + o, cbSID - o, "-%u", *(PDWORD)pbSID);
        pbSID += 4;
        c--;
    }
    *pszSid = szSID;
    return TRUE;
}

/*
* Linux compatible function of WIN32 API function ConvertSidToStringSidA()
* -- pSID
* -- return
*/
_Success_(return)
BOOL IsValidSid(_In_opt_ PSID pSID)
{
    LPSTR szSID = NULL;
    BOOL fResult = ConvertSidToStringSidA(pSID, &szSID);
    LocalFree(szSID);
    return fResult;
}



// ----------------------------------------------------------------------------
// CRITICAL_SECTION functionality below:
// ----------------------------------------------------------------------------

VOID InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    memset(lpCriticalSection, 0, sizeof(CRITICAL_SECTION));
    pthread_mutexattr_init(&lpCriticalSection->mta);
    pthread_mutexattr_settype(&lpCriticalSection->mta, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lpCriticalSection->mutex, &lpCriticalSection->mta);
}

BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount)
{
    InitializeCriticalSection(lpCriticalSection);
    return TRUE;
}

VOID DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    pthread_mutex_destroy(&lpCriticalSection->mutex);
    memset(lpCriticalSection, 0, sizeof(CRITICAL_SECTION));
}

VOID EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    pthread_mutex_lock(&lpCriticalSection->mutex);
}

VOID LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    pthread_mutex_unlock(&lpCriticalSection->mutex);
}



// ----------------------------------------------------------------------------
// SRWLock functionality below:
// ----------------------------------------------------------------------------

static int futex(uint32_t *uaddr, int futex_op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
{
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

VOID InitializeSRWLock(PSRWLOCK SRWLock)
{
    ZeroMemory(SRWLock, sizeof(SRWLOCK));
}

BOOL AcquireSRWLockExclusive_Try(_Inout_ PSRWLOCK SRWLock)
{
    DWORD dwZero = 0;
    __sync_fetch_and_add_4(&SRWLock->c, 1);
    if(atomic_compare_exchange_strong(&SRWLock->xchg, &dwZero, 1)) {
        return TRUE;
    }
    __sync_sub_and_fetch_4(&SRWLock->c, 1);
    return FALSE;
}

VOID AcquireSRWLockExclusive(_Inout_ PSRWLOCK SRWLock)
{
    DWORD dwZero;
    __sync_fetch_and_add_4(&SRWLock->c, 1);
    while(TRUE) {
        dwZero = 0;
        if(atomic_compare_exchange_strong(&SRWLock->xchg, &dwZero, 1)) {
            return;
        }
        futex(&SRWLock->xchg, FUTEX_WAIT, 1, NULL, NULL, 0);
    }
}

_Success_(return)
BOOL AcquireSRWLockExclusive_Timeout(_Inout_ PSRWLOCK SRWLock, _In_ DWORD dwMilliseconds)
{
    DWORD dwZero;
    struct timespec ts;
    __sync_fetch_and_add_4(&SRWLock->c, 1);
    while(TRUE) {
        dwZero = 0;
        if(atomic_compare_exchange_strong(&SRWLock->xchg, &dwZero, 1)) {
            return TRUE;
        }
        if((dwMilliseconds != 0) && (dwMilliseconds != 0xffffffff)) {
            ts.tv_sec = dwMilliseconds / 1000;
            ts.tv_nsec = (dwMilliseconds % 1000) * 1000 * 1000;
            if((-1 == futex(&SRWLock->xchg, FUTEX_WAIT, 1, &ts, NULL, 0)) && (errno != EAGAIN)) {
                __sync_sub_and_fetch_4(&SRWLock->c, 1);
                return FALSE;
            }
        } else {
            if((-1 == futex(&SRWLock->xchg, FUTEX_WAIT, 1, NULL, NULL, 0)) && (errno != EAGAIN)) {
                __sync_sub_and_fetch_4(&SRWLock->c, 1);
                return FALSE;
            }
        }
    }
}

VOID ReleaseSRWLockExclusive(_Inout_ PSRWLOCK SRWLock)
{
    DWORD dwOne = 1;
    if(atomic_compare_exchange_strong(&SRWLock->xchg, &dwOne, 0)) {
        if(__sync_sub_and_fetch_4(&SRWLock->c, 1)) {
            futex(&SRWLock->xchg, FUTEX_WAKE, 1, NULL, NULL, 0);
        }
    }
}

// ----------------------------------------------------------------------------
// EVENT functionality below:
// ----------------------------------------------------------------------------

typedef struct tdHANDLE_INTERNAL_EVENT2 {
    DWORD magic;
    DWORD type;
    BOOL fEventManualReset;
    SRWLOCK SRWLock;
} HANDLE_INTERNAL_EVENT2, *PHANDLE_INTERNAL_EVENT2;

// function is limited and not thread-safe, but use case in leechcore is single-threaded
DWORD WaitForSingleObject(_In_ HANDLE hHandle, _In_ DWORD dwMilliseconds)
{
    PHANDLE_INTERNAL_EVENT2 ph = (PHANDLE_INTERNAL_EVENT2)hHandle;
    BOOL fResult;
    if((ph->magic != OSCOMPATIBILITY_HANDLE_INTERNAL) || (ph->type != OSCOMPATIBILITY_HANDLE_TYPE_EVENT)) { return 0xffffffff; }
    if(!AcquireSRWLockExclusive_Timeout(&ph->SRWLock, dwMilliseconds)) {
        return 0xffffffff;  // timeout
    }
    if(ph->fEventManualReset) {
        ReleaseSRWLockExclusive(&ph->SRWLock);
    }
    return 0;
}

DWORD WaitForMultipleObjectsAll(_In_ DWORD nCount, HANDLE *lpHandles, _In_ DWORD dwMilliseconds)
{
    DWORD i;
    BOOL fAll = FALSE;
    PHANDLE_INTERNAL_EVENT2 ph;
    // 1: verify handle validity
    for(i = 0; i < nCount; i++) {
        ph = *(PHANDLE_INTERNAL_EVENT2 *)(lpHandles + i);
        if((ph->magic != OSCOMPATIBILITY_HANDLE_INTERNAL) || (ph->type != OSCOMPATIBILITY_HANDLE_TYPE_EVENT)) {
            return 0xffffffff;
        }
    }
    // 2: wait for all objects
    while(!fAll) {
        fAll = TRUE;
        for(i = 0; i < nCount; i++) {
            ph = *(PHANDLE_INTERNAL_EVENT2 *)(lpHandles + i);
            if(!AcquireSRWLockExclusive_Try(&ph->SRWLock)) {
                if(!AcquireSRWLockExclusive_Timeout(&ph->SRWLock, dwMilliseconds)) {
                    return 0xffffffff;  // timeout
                }
                fAll = FALSE;
            }
            ReleaseSRWLockExclusive(&ph->SRWLock);
        }
    }
    return 0;
}

DWORD WaitForMultipleObjectsSingle(_In_ DWORD nCount, HANDLE *lpHandles, _In_ DWORD dwMilliseconds)
{
    DWORD i;
    PHANDLE_INTERNAL_EVENT2 ph;
    // 1: verify handle validity
    for(i = 0; i < nCount; i++) {
        ph = *(PHANDLE_INTERNAL_EVENT2*)(lpHandles + i);
        if((ph->magic != OSCOMPATIBILITY_HANDLE_INTERNAL) || (ph->type != OSCOMPATIBILITY_HANDLE_TYPE_EVENT)) {
            return 0xffffffff;
        }
    }
    // 2: try find single available object - or else sleep and try again
    while(TRUE) {
        for(i = 0; i < nCount; i++) {
            ph = *(PHANDLE_INTERNAL_EVENT2*)(lpHandles + i);
            if(AcquireSRWLockExclusive_Try(&ph->SRWLock)) {
                if(ph->fEventManualReset) {
                    ReleaseSRWLockExclusive(&ph->SRWLock);
                }
                return i;
            }
        }
        Sleep(5);
    }
}

DWORD WaitForMultipleObjects(_In_ DWORD nCount, HANDLE *lpHandles, _In_ BOOL bWaitAll, _In_ DWORD dwMilliseconds)
{
    return bWaitAll ?
        WaitForMultipleObjectsAll(nCount, lpHandles, dwMilliseconds) :
        WaitForMultipleObjectsSingle(nCount, lpHandles, dwMilliseconds);

}

BOOL SetEvent(_In_ HANDLE hEventIngestPhys)
{
    PHANDLE_INTERNAL_EVENT2 ph = (PHANDLE_INTERNAL_EVENT2)hEventIngestPhys;
    if((ph->magic != OSCOMPATIBILITY_HANDLE_INTERNAL) || (ph->type != OSCOMPATIBILITY_HANDLE_TYPE_EVENT)) { return FALSE; }
    ReleaseSRWLockExclusive(&ph->SRWLock);
    return TRUE;
}

BOOL ResetEvent(_In_ HANDLE hEventIngestPhys)
{
    PHANDLE_INTERNAL_EVENT2 ph = (PHANDLE_INTERNAL_EVENT2)hEventIngestPhys;
    if((ph->magic != OSCOMPATIBILITY_HANDLE_INTERNAL) || (ph->type != OSCOMPATIBILITY_HANDLE_TYPE_EVENT)) { return FALSE; }
    return AcquireSRWLockExclusive_Try(&ph->SRWLock);
}

HANDLE CreateEvent(_In_opt_ PVOID lpEventAttributes, _In_ BOOL bManualReset, _In_ BOOL bInitialState, _In_opt_ PVOID lpName)
{
    PHANDLE_INTERNAL_EVENT2 ph;
    ph = malloc(sizeof(HANDLE_INTERNAL_EVENT2));
    ZeroMemory(ph, sizeof(HANDLE_INTERNAL_EVENT2));
    ph->magic = OSCOMPATIBILITY_HANDLE_INTERNAL;
    ph->type = OSCOMPATIBILITY_HANDLE_TYPE_EVENT;
    ph->fEventManualReset = bManualReset;
    if(bInitialState) {
        SetEvent((HANDLE)ph);
    } else {
        ResetEvent((HANDLE)ph);
    }
    return (HANDLE)ph;
}

// ----------------------------------------------------------------------------
// SLIST functionality below:
// ----------------------------------------------------------------------------

VOID InitializeSListHead(PSLIST_HEADER ListHead)
{
    ZeroMemory(ListHead, sizeof(SLIST_HEADER));
}

USHORT QueryDepthSList(PSLIST_HEADER ListHead)
{
    return ListHead->c;
}

PSLIST_ENTRY InterlockedPopEntrySList(_Inout_ PSLIST_HEADER ListHead)
{
    PSLIST_ENTRY e = NULL;
    AcquireSRWLockExclusive(&ListHead->LockSRW);
    if(ListHead->c) {
        ListHead->c--;
        if((e = ListHead->Next)) {
            ListHead->Next = ListHead->Next->Next;
            e->Next = NULL;
        }
    }
    ReleaseSRWLockExclusive(&ListHead->LockSRW);
    return e;
}


PSLIST_ENTRY InterlockedPushEntrySList(_Inout_ PSLIST_HEADER ListHead, _Inout_ PSLIST_ENTRY ListEntry)
{
    PSLIST_ENTRY e = NULL;
    AcquireSRWLockExclusive(&ListHead->LockSRW);
    ListHead->c++;
    e = ListHead->Next;
    ListEntry->Next = e;
    ListHead->Next = ListEntry;
    ReleaseSRWLockExclusive(&ListHead->LockSRW); 
    return e;
}

// ----------------------------------------------------------------------------
// VARIOUS FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

/*
* Linux implementation of ntdll!RtlDecompressBuffer for COMPRESS_ALGORITHM_XPRESS:
* Dynamically load libMSCompression.so (if it exists) and use it. If library does
* not exist then fail gracefully (i.e. don't support XPRESS decompress).
* https://github.com/coderforlife/ms-compress   (License: GPLv3)
*/
NTSTATUS OSCOMPAT_RtlDecompressBuffer(USHORT CompressionFormat, PUCHAR UncompressedBuffer, ULONG  UncompressedBufferSize, PUCHAR CompressedBuffer, ULONG  CompressedBufferSize, PULONG FinalUncompressedSize)
{
    int rc;
    void* lib_mscompress;
    SIZE_T cbOut;
    static BOOL fFirst = TRUE;
    static SRWLOCK LockSRW = SRWLOCK_INIT;
    static int(*pfn_xpress_decompress)(PBYTE pbIn, SIZE_T cbIn, PBYTE pbOut, SIZE_T *pcbOut) = NULL;
    if(CompressionFormat != 3) { return VMM_STATUS_UNSUCCESSFUL; } // 3 == COMPRESS_ALGORITHM_XPRESS
    if(fFirst) {
        AcquireSRWLockExclusive(&LockSRW);
        if(fFirst) {
            fFirst = FALSE;
            lib_mscompress = dlopen("libMSCompression.so", RTLD_NOW);
            if(lib_mscompress) {
                pfn_xpress_decompress = (int(*)(PBYTE,SIZE_T,PBYTE,SIZE_T*))dlsym(lib_mscompress, "xpress_decompress");
            }
        }
        ReleaseSRWLockExclusive(&LockSRW);
    }
    *FinalUncompressedSize = 0;
    if(pfn_xpress_decompress) {
        cbOut = UncompressedBufferSize;
        rc = pfn_xpress_decompress(CompressedBuffer, CompressedBufferSize, UncompressedBuffer, &cbOut);
        if(rc == 0) {
            *FinalUncompressedSize = cbOut;
            return VMM_STATUS_SUCCESS;
        }
    }
    return VMM_STATUS_UNSUCCESSFUL;
}

errno_t tmpnam_s(char *_Buffer, ssize_t _Size)
{
    if(_Size < 32) { return -1; }
    snprintf(_Buffer, _Size, "/tmp/vmm-%x%x", (uint32_t)((uint64_t)_Buffer >> 12), rand());
    return 0;
}

int _vscprintf(_In_z_ _Printf_format_string_ char const *const _Format, va_list _ArgList)
{
    char *sz = NULL;
    int len = vasprintf(&sz, _Format, _ArgList);
    free(sz);
    return len;
}

#endif /* LINUX */
