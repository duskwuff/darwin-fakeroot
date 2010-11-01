extern _libfakeroot_sysenter_hook
global _libfakeroot_sysenter_landing

_libfakeroot_sysenter_landing:
    enter   0x40, 0
    mov     [rbp-0x40], rdi ; copy args to buffer
    mov     [rbp-0x38], rsi
    mov     [rbp-0x30], rdx
    mov     [rbp-0x28], rcx
    mov     [rbp-0x20], r8
    mov     [rbp-0x18], r9 
    mov     r8, [rbp+0x10]  ; copy the stack arguments
    mov     r9, [rbp+0x18]
    mov     [rbp-0x10], r8
    mov     [rbp-0x08], r9

    mov     rdi, rax        ; callno
    lea     rsi, [rbp-0x40] ; argv

    call    _libfakeroot_sysenter_hook
    leave
    ret

; vim: set syn=nasm:
