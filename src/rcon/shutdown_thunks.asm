; Native UDP RCON teardown thunks for Host_Shutdown / NET_Shutdown (x86).
;
; Preserve the entire entry state (including the original return value).
; These are teardown entries, not opportunities to guess a decompiler's
; inferred register arguments or to invoke a trampoline after unhooking it:
; save every register and flag, notify the plugin, restore, then continue at
; the saved original with the caller's untouched stack.

.686P
.model flat, C

EXTERN NativeUdpBeforeShutdown:PROC
EXTERN g_nativeUdpHostShutdown:DWORD
EXTERN g_nativeUdpNetShutdown:DWORD

SHUTDOWN_THUNK MACRO thunk, original
thunk PROC
    pushfd
    pushad
    call NativeUdpBeforeShutdown
    popad
    popfd
    jmp DWORD PTR [original]
thunk ENDP
ENDM

.code
SHUTDOWN_THUNK NativeUdpHookHostShutdown, g_nativeUdpHostShutdown
SHUTDOWN_THUNK NativeUdpHookNetShutdown, g_nativeUdpNetShutdown

END
