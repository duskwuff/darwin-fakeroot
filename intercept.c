#include "libfakeroot.h"

#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include <mach/error.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/vm_map.h>

void libfakeroot_sysenter_landing(void);

int libfakeroot_patch_func(const char *name)
{
    // get the call stub
    uint8_t *fptr = dlsym(RTLD_DEFAULT, name);
    if(!fptr) return ESRCH;

    // Check signature
#if defined(__i386__)
    if(fptr[0]  != 0xb8 ||  // mov -> eax
       fptr[5]  != 0xe8 ||  // call
       fptr[26] != 0xc3     // ret
      ) return EINVAL;
#elif defined(__x86_64__)
    if(fptr[0]  != 0xb8 ||  // mov -> rax
       fptr[8]  != 0x0f ||  // syscall (first byte)
       fptr[17] != 0xc3     // ret
      ) return EINVAL;
#endif

    mach_error_t err;

    err = vm_protect(mach_task_self(),
            (vm_address_t) fptr, 32, false,
            VM_PROT_READ | VM_PROT_WRITE);

    if(err) return EFAULT;

#if defined(__i386__)
    {
        void *target = libfakeroot_sysenter_landing;
        void *base = &fptr[5] + 5;
        *((uintptr_t *) &fptr[6]) = target - base;
    }
#elif defined(__x86_64__)
    {
        fptr[5] = 0x49; // mov imm64 -> r11
        fptr[6] = 0xbb;
        *((void **) &fptr[7]) = libfakeroot_sysenter_landing;
        fptr[15] = 0x41; // jmp *r11
        fptr[16] = 0xff;
        fptr[17] = 0xe3;
    }
#endif

    err = vm_protect(mach_task_self(),
            (vm_address_t) fptr, 32, false,
            VM_PROT_READ | VM_PROT_EXECUTE);

    if(err) return EFAULT;

    return 0;
}
