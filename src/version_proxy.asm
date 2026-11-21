; Jump stubs that forward every version.dll export to the real System32 version.dll.
; The function pointers are filled in by InitVersionProxy() in dllmain.cpp.

EXTERN p_GetFileVersionInfoA:QWORD
EXTERN p_GetFileVersionInfoByHandle:QWORD
EXTERN p_GetFileVersionInfoExA:QWORD
EXTERN p_GetFileVersionInfoExW:QWORD
EXTERN p_GetFileVersionInfoSizeA:QWORD
EXTERN p_GetFileVersionInfoSizeExA:QWORD
EXTERN p_GetFileVersionInfoSizeExW:QWORD
EXTERN p_GetFileVersionInfoSizeW:QWORD
EXTERN p_GetFileVersionInfoW:QWORD
EXTERN p_VerFindFileA:QWORD
EXTERN p_VerFindFileW:QWORD
EXTERN p_VerInstallFileA:QWORD
EXTERN p_VerInstallFileW:QWORD
EXTERN p_VerLanguageNameA:QWORD
EXTERN p_VerLanguageNameW:QWORD
EXTERN p_VerQueryValueA:QWORD
EXTERN p_VerQueryValueW:QWORD

.CODE

STUB MACRO name
name&_stub PROC
    jmp QWORD PTR [p_&name]
name&_stub ENDP
ENDM

STUB GetFileVersionInfoA
STUB GetFileVersionInfoByHandle
STUB GetFileVersionInfoExA
STUB GetFileVersionInfoExW
STUB GetFileVersionInfoSizeA
STUB GetFileVersionInfoSizeExA
STUB GetFileVersionInfoSizeExW
STUB GetFileVersionInfoSizeW
STUB GetFileVersionInfoW
STUB VerFindFileA
STUB VerFindFileW
STUB VerInstallFileA
STUB VerInstallFileW
STUB VerLanguageNameA
STUB VerLanguageNameW
STUB VerQueryValueA
STUB VerQueryValueW

END
