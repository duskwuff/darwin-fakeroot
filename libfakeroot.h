#include <stdint.h>
#include <stddef.h>

typedef uintptr_t cpuword_t;

typedef struct {
    cpuword_t low;
    cpuword_t high;
} syscall_return_t;

int libfakeroot_patch_func(const char *name);
