.globl _libfakeroot_sysenter_landing

_libfakeroot_sysenter_landing:
    enter   $0x40, $0
    mov     %rdi, -0x40(%rbp)   // copy args to buffer
    mov     %rsi, -0x38(%rbp)
    mov     %rdx, -0x30(%rbp)
    mov     %rcx, -0x28(%rbp)
    mov     %r8, -0x20(%rbp)
    mov     %r9, -0x18(%rbp)
    mov     0x10(%rbp), %r8     // copy the stack arguments
    mov     0x18(%rbp), %r9
    mov     %r8, -0x10(%rbp)
    mov     %r9, -0x08(%rbp)

    mov     %rax, %rdi          // callno
    lea     -0x40(%rbp), %rsi   // argv

    call    _libfakeroot_sysenter_hook
    leave
    ret
