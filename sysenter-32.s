extern _libfakeroot_sysenter_hook
global _libfakeroot_sysenter_landing

_libfakeroot_sysenter_landing:
    lea     ecx, [esp+8]    ; args
    push    ecx
    push    eax             ; call no
    call    _libfakeroot_sysenter_hook
    add     esp, 8
    ret

; vim: set syn=nasm:
