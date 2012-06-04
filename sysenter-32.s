.globl _libfakeroot_sysenter_landing

_libfakeroot_sysenter_landing:
    lea     8(%esp), %ecx   // args
    push    %ecx
    push    %eax            // call no
    call    _libfakeroot_sysenter_hook
    add     $8, %esp
    ret
