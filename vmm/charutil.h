// charutil.h : definitions of various character/string utility functions.
//
// (c) Ulf Frisk, 2021
// Author: Ulf Frisk, pcileech@frizk.net
//
#ifndef __CHARUTIL_H__
#define __CHARUTIL_H__
#include "oscompatibility.h"

#define CHARUTIL_FLAG_NONE                      0x0000
#define CHARUTIL_FLAG_ALLOC                     0x0001
#define CHARUTIL_FLAG_TRUNCATE                  0x0002
#define CHARUTIL_FLAG_TRUNCATE_ONFAIL_NULLSTR   0x0006
#define CHARUTIL_FLAG_STR_BUFONLY               0x0008

/*
* Check whether a string is an ansi-string (only codepoints between 0-127).
* -- sz
* -- return
*/
BOOL CharUtil_IsAnsiA(_In_ LPCSTR sz);
BOOL CharUtil_IsAnsiW(_In_ LPCWSTR wsz);

/*
* Convert Ascii (0-255) or Wide (16-bit LE) string into a UTF-8 string.
* NB! wsz must NOT equal or overlap pbBuffer!
* CALLER LOCALFREE (if *pusz != pbBuffer): *pusz
* -- sz/wsz = the string to convert.
* -- cch = -1 for null-terminated string; or max number of chars (excl. null).
* -- pbBuffer = optional buffer to place the result in.
* -- cbBuffer
* -- pusz = if set to null: function calculate length only and return TRUE.
            result utf-8 string, either as (*pusz == pbBuffer) or LocalAlloc'ed
*           buffer that caller is responsible for free.
* -- pcbu = byte length (including terminating null) of utf-8 string.
* -- flags = CHARUTIL_FLAG_NONE, CHARUTIL_FLAG_ALLOC or CHARUTIL_FLAG_TRUNCATE
* -- return
*/
_Success_(return)
BOOL CharUtil_UtoU(
    _In_opt_ LPSTR usz,
    _In_ DWORD cch,
    _Maybenull_ _Writable_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ DWORD cbBuffer,
    _Out_opt_ LPSTR * pusz,
    _Out_opt_ PDWORD pcbu,
    _In_ DWORD flags
);

_Success_(return)
BOOL CharUtil_AtoU(
    _In_opt_ LPSTR sz,
    _In_ DWORD cch,
    _Maybenull_ _Writable_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ DWORD cbBuffer,
    _Out_opt_ LPSTR *pusz,
    _Out_opt_ PDWORD pcbu,
    _In_ DWORD flags
);

_Success_(return)
BOOL CharUtil_WtoU(
    _In_opt_ LPWSTR wsz,
    _In_ DWORD cch,
    _Maybenull_ _Writable_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ DWORD cbBuffer,
    _Out_opt_ LPSTR *pusz,
    _Out_opt_ PDWORD pcbu,
    _In_ DWORD flags
);

/*
* Convert UTF-8 string into a Windows Wide-Char string.
* Function support usz == pbBuffer - usz will then become overwritten.
* CALLER LOCALFREE (if *pusz != pbBuffer): *pusz
* -- usz = the string to convert.
* -- cch = -1 for null-terminated string; or max number of chars (excl. null).
* -- pbBuffer = optional buffer to place the result in.
* -- cbBuffer
* -- pusz = if set to null: function calculate length only and return TRUE.
            result wide-string, either as (*pwsz == pbBuffer) or LocalAlloc'ed
*           buffer that caller is responsible for free.
* -- pcbu = byte length (including terminating null) of wide-char string.
* -- flags = CHARUTIL_FLAG_NONE, CHARUTIL_FLAG_ALLOC or CHARUTIL_FLAG_TRUNCATE
* -- return
*/
_Success_(return)
BOOL CharUtil_UtoW(
    _In_opt_ LPSTR usz,
    _In_ DWORD cch,
    _Maybenull_ _Writable_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ DWORD cbBuffer,
    _Out_opt_ LPWSTR *pwsz,
    _Out_opt_ PDWORD pcbw,
    _In_ DWORD flags
);

/*
* Convert UTF-8, Ascii (0-255) or Wide (16-bit LE) string into a JSON string.
* Function support sz/usz/wsz == pbBuffer - sz/usz/wsz will then become overwritten.
* CALLER LOCALFREE (if *pjsz != pbBuffer): *pjsz
* -- sz/usz/wsz = the string to convert.
* -- cch = -1 for null-terminated string; or max number of chars (excl. null).
* -- pbBuffer = optional buffer to place the result in.
* -- cbBuffer
* -- pjsz = if set to null: function calculate length only and return TRUE.
            result utf-8 string, either as (*pjsz == pbBuffer) or LocalAlloc'ed
*           buffer that caller is responsible for free.
* -- pcbj = byte length (including terminating null) of utf-8 string.
* -- flags = CHARUTIL_FLAG_NONE, CHARUTIL_FLAG_ALLOC or CHARUTIL_FLAG_TRUNCATE
* -- return
*/
_Success_(return)
BOOL CharUtil_UtoJ(
    _In_opt_ LPSTR usz,
    _In_ DWORD cch,
    _Maybenull_ _Writable_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ DWORD cbBuffer,
    _Out_opt_ LPSTR *pjsz,
    _Out_opt_ PDWORD pcbj,
    _In_ DWORD flags
);

_Success_(return)
BOOL CharUtil_AtoJ(
    _In_opt_ LPSTR sz,
    _In_ DWORD cch,
    _Maybenull_ _Writable_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ DWORD cbBuffer,
    _Out_opt_ LPSTR *pjsz,
    _Out_opt_ PDWORD pcbj,
    _In_ DWORD flags
);

_Success_(return)
BOOL CharUtil_WtoJ(
    _In_opt_ LPWSTR wsz,
    _In_ DWORD cch,
    _Maybenull_ _Writable_bytes_(cbBuffer) PBYTE pbBuffer,
    _In_ DWORD cbBuffer,
    _Out_opt_ LPSTR *pjsz,
    _Out_opt_ PDWORD pcbj,
    _In_ DWORD flags
);

/*
* Hash a string quickly using the ROT13 algorithm either to a 64-bit or 32-bit number.
* -- sz/usz/wsz = the string to hash
* -- fUpper
* -- return
*/
DWORD CharUtil_Hash32U(_In_ LPCSTR usz, _In_ BOOL fUpper);
DWORD CharUtil_Hash32A(_In_ LPCSTR sz, _In_ BOOL fUpper);
DWORD CharUtil_Hash32W(_In_ LPCWSTR wsz, _In_ BOOL fUpper);
QWORD CharUtil_Hash64U(_In_ LPCSTR usz, _In_ BOOL fUpper);
QWORD CharUtil_Hash64A(_In_ LPCSTR sz, _In_ BOOL fUpper);
QWORD CharUtil_Hash64W(_In_ LPCWSTR wsz, _In_ BOOL fUpper);

/*
* Hash a name string in a way that is supported by the file system.
* NB! this is not the same hash as the Windows registry uses.
* -- usz/sz/wsz
* -- iSuffix
* -- return
*/
DWORD CharUtil_HashNameFsU(_In_ LPCSTR usz, _In_opt_ DWORD iSuffix);
DWORD CharUtil_HashNameFsA(_In_ LPCSTR sz, _In_opt_ DWORD iSuffix);
DWORD CharUtil_HashNameFsW(_In_ LPCWSTR wsz, _In_opt_ DWORD iSuffix);

/*
* Hash a path string in a way that is supported by the file system.
* NB! this is not the same hash as the Windows registry uses.
* -- usz/sz/wsz
* -- iSuffix
* -- return
*/
QWORD CharUtil_HashPathFsU(_In_ LPCSTR usz);
QWORD CharUtil_HashPathFsA(_In_ LPCSTR sz);
QWORD CharUtil_HashPathFsW(_In_ LPCWSTR wsz);

/*
* Convert a string into a file name compatible string by replacing illegal
* characters with '_'. Also optionally add a suffix between 1-9 and fix
* upper-case letters. If insufficient space the result will be truncated.
* -- uszDst
* -- cbuDst
* -- uszSrc
* -- iSuffix
* -- fUpper
* -- return = number of bytes written (including terminating NULL).
*/
_Success_(return != 0)
DWORD CharUtil_FixFsNameU(
    _Out_writes_(cbuDst) LPSTR uszDst,
    _In_ DWORD cbuDst,
    _In_ LPCSTR uszSrc,
    _In_opt_ DWORD iSuffix,
    _In_ BOOL fUpper
);

/*
* Convert a string into a file name compatible string by replacing illegal
* characters with '_'. Also optionally add a suffix between 1-9 and fix
* upper-case letters. One of [usz, sz, wsz] must be valid.
* -- uszOut
* -- usz
* -- sz
* -- wsz
* -- cwsz
* -- cch = number of bytes/wchars in usz/sz/wsz or _TRUNCATE
* -- iSuffix
* -- fUpper
* -- return = number of bytes written (including terminating NULL).
*/
_Success_(return != 0)
DWORD CharUtil_FixFsName(
    _Out_writes_(2*MAX_PATH) LPSTR uszOut,
    _In_opt_ LPCSTR usz,
    _In_opt_ LPCSTR sz,
    _In_opt_ LPCWSTR wsz,
    _In_ DWORD cch,
    _In_opt_ DWORD iSuffix,
    _In_ BOOL fUpper
);

/*
* Split a "path" string into two at the first slash/backslash character.
* The 1st string is returned in the pusz1 caller-allocated buffer. The
* remainder is returned as return data (is a sub-string of wsz). If no
* 2nd string is found null-terminator character is returned (NB! not as NULL).
* -- usz = utf-8/ascii string to split.
* -- usz1 = buffer to receive result.
* -- cbu1 = byte length of usz1 buffer
* -- return = remainder of split string.
*/
LPSTR CharUtil_PathSplitFirst(_In_ LPSTR usz, _Out_writes_(cbu1) LPSTR usz1, _In_ DWORD cbu1);

/*
* Return the sub-string after the first (back)slash character in usz.
* If no (back)slash is found original string is returned. The returned data
* must not be free'd and is only valid as long as the usz parameter is valid.
* -- usz = utf-8 or ascii string.
* -- return
*/
LPSTR CharUtil_PathSplitNext(_In_ LPSTR usz);

/*
* Return the sub-string after the last (back)slash character in usz.
* If no (back)slash is found original string is returned. The returned data
* must not be free'd and is only valid as long as the usz parameter is valid.
* -- usz = utf-8 or ascii string.
* -- return
*/
LPSTR CharUtil_PathSplitLast(_In_ LPSTR usz);

/*
* Split the string usz into two at the last (back)slash which is removed.
* Ex: usz: XXX/YYY/ZZZ/AAA -> uszPath: XXX/YYY/ZZZ + return: AAA
* -- usz = utf-8 or ascii string.
* -- uszPath = buffer to receive result.
* -- cbuPath = byte length of uszPath buffer
* -- return
*/
LPSTR CharUtil_PathSplitLastEx(_In_ LPSTR usz, _Out_writes_(cbuPath) LPSTR uszPath, _In_ DWORD cbuPath);

/*
* Checks if a string ends with a certain substring.
* -- usz
* -- uszEndsWith
* -- fCaseInsensitive
* -- return
*/
BOOL CharUtil_StrEndsWith(_In_opt_ LPSTR usz, _In_opt_ LPSTR uszEndsWith, _In_ BOOL fCaseInsensitive);

/*
* Compare a wide-char string to a utf-8 string.
* -- wsz
* -- usz
* -- return = 0 if equals, -1/1 otherwise.
*/
int CharUtil_CmpWU(_In_opt_ LPWSTR wsz, _In_opt_ LPSTR usz, _In_ BOOL fCaseInsensitive);

#endif /* __CHARUTIL_H__ */
