#include "libfakeroot.h"

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "communicate.h"

void cthread_set_errno_self(int error); // Libc SPI

static int uid = 0, euid = 0, gid = 0, egid = 0;

static int comm_fd;

static const char *insert_environ[16];

static const char *strip_environ[] = {
    "DYLD_INSERT_LIBRARIES",
    "FAKEROOT_SOCKET",
    "FAKEROOT_STATE",
    NULL
};


static void try_patch(const char *func)
{
    int res = libfakeroot_patch_func(func);
#ifdef DEBUG
    if(res != 0) {
        if(strchr(func, '$')) return; // fewer crazy aliases on 64-bit
        fprintf(stderr, "failed to patch %s - err = %d\n", func, res);
    }
#else
    (void) res;
#endif
}


const char *patch_funcs[] = {
    "getuid", "getgid", "geteuid", "getegid",
    "setuid", "setgid", "seteuid", "setegid",
    "setreuid", "setregid", "issetugid",

    "execve",

    "open", "open$UNIX2003", "open$NOCANCEL", "open$NOCANCEL$UNIX2003",

    "mkdir",
    "symlink",

    "stat",  "fstat",  "lstat",
    "stat64", "fstat64", "lstat64",

/*
    "access", 
*/

    "chown", "fchown", "lchown",

/*
    "getattrlist", "fgetattrlist", "getattrlist$UNIX2003",
    "setattrlist", "fsetattrlist", "setattrlist$UNIX2003",
    "getdirentriesattr",
*/

    // We override these to make our control FD "play dead"
    "close", "close$UNIX2003",
    "dup2",

/*  No prototypes available:

    DEPRECATED
        "statv", "fstatv", "lstatv", <- deprecated anyway!

    ACL SUPPORT
        "stat_extended", "fstat_extended", "lstat_extended",
        "stat64_extended", "fstat64_extended", "lstat64_extended",
        "open_extended", "mkdir_extended", "access_extended",
        "chmod_extended", "fchmod_extended",
*/
};

static char env_dyld_string[512], env_sock_string[512];

void libfakeroot_init(void)
{
    const char *fakeroot_socket = getenv("FAKEROOT_SOCKET");
    if(!fakeroot_socket) {
        fprintf(stderr, "Missing FAKEROOT_SOCKET... can't continue\n");
        abort();
    }

    comm_fd = init_commfd(fakeroot_socket);
    if(!comm_fd) {
        perror("init_commfd");
        abort();
    }

    snprintf(env_dyld_string, sizeof(env_dyld_string),
            "DYLD_INSERT_LIBRARIES=%s", getenv("DYLD_INSERT_LIBRARIES"));
    snprintf(env_sock_string, sizeof(env_sock_string),
            "FAKEROOT_SOCKET=%s", fakeroot_socket);

    insert_environ[0] = env_dyld_string;
    insert_environ[1] = env_sock_string;
    insert_environ[2] = NULL;

    const char *old_state = getenv("FAKEROOT_STATE");
    if(old_state)
        sscanf(old_state, "%d:%d:%d:%d", &uid, &gid, &euid, &egid);

    for(int i = 0; patch_funcs[i]; i++)
        try_patch(patch_funcs[i]);

#ifdef DEBUG
    // mainly to get stdio initialization out of our hair
    printf("fakeroot initialized in pid %d\n", getpid());
#endif
}


static cpuword_t do_execve(void *stack[])
{
    const char *path = (const char *) stack[0];
    const char ** argv = (const char **) stack[1];
    const char ** envp = (const char **) stack[2];

    const char *envbuf[512];
    char statebuf[128];

    int envptr = 0;

    for(int i = 0; i < 512; i++) {
        if(!envp[i]) break;
        for(int j = 0; strip_environ[j]; j++) {
            const char *ent = strip_environ[j];
            int len = strlen(ent);
            if(memcmp(envp[i], ent, len) == 0 && ent[len] == '=') {
                goto skip;
            }
        }
        envbuf[envptr++] = envp[i];
skip: {}
    }

    for(int i = 0; insert_environ[i]; i++) {
        envbuf[envptr++] = insert_environ[i];
    }

    snprintf(statebuf, sizeof(statebuf),
            "FAKEROOT_STATE=%d:%d:%d:%d", uid, gid, euid, egid);
    envbuf[envptr++] = statebuf;

    envbuf[envptr++] = NULL;

    syscall(SYS_execve, path, argv, envbuf);
    return errno; // no such thing as a successful return
}


static int create_or_open(int *created, int call,
        const char *path, int oflag, mode_t mode)
{
    // No O_CREAT? Easy.
    if(!(oflag & O_CREAT)) {
        *created = 0;
        return syscall(call, path, oflag, mode);
    }

    int result;
retry:
    // Try force-creating the file first...
    result = syscall(call, path, oflag | O_EXCL, mode);
    if(result >= 0) {
        // Success! Created.
        *created = 1;
        return result;
    }

    if(errno != EEXIST) {
        // Not an error we expected. Throw it.
        return -1;
    }

    if(oflag & O_EXCL) {
        // This error would have been passed on to the user anyway.
        // Let 'em have it.
        *created = 0;
        return -1;
    }

    // Try just opening the file?
    result = syscall(call, path, oflag & ~O_CREAT, mode);
    if(result >= 0) {
        // Success! Opened.
        *created = 0;
        return result;
    }

    if(errno != ENOENT)
        return -1; // meh.

    // Weirdness... someone must have managed to delete this file
    // between our two open() calls.
    //
    // Try again and see if we can win the race.
    goto retry;
}


syscall_return_t libfakeroot_sysenter_hook(cpuword_t callno, cpuword_t *stack)
{
    int realCall = callno & 0xffff;
    cpuword_t error = 0, result = 0, result2 = 0;

    switch(realCall) {

        case SYS_getuid: result = uid; break;
        case SYS_getgid: result = gid; break;
        case SYS_geteuid: result = euid; break;
        case SYS_getegid: result = egid; break;

        case SYS_setuid: uid = euid = stack[0]; break;
        case SYS_setgid: gid = egid = stack[0]; break;
        case SYS_seteuid: euid = stack[0]; break;
        case SYS_setegid: egid = stack[0]; break;

        case SYS_setreuid: uid = stack[0]; euid = stack[1]; break;
        case SYS_setregid: gid = stack[0]; egid = stack[1]; break;

        case SYS_issetugid:
            // we're kind of setugid no matter what. Meh.
            result = 1;
            break;

        case SYS_execve:
            error = do_execve((void **) stack);
            break;

        case SYS_open:
        case SYS_open_nocancel:
        {
            const char *path = (const char *) stack[0];
            int oflag = (int) stack[1];
            mode_t mode = (mode_t) stack[2];

            int created;
            int fd = create_or_open(&created, realCall, path, oflag, mode);

            if(fd < 0) {
                error = errno;
                break;
            }

            result = fd;

            if(created) {
                // Get the inode
                struct stat64 sbuf;
                int sres = syscall(SYS_fstat64, fd, &sbuf);
                if(sres < 0) {
                    perror("fstat failed after open()");
                    break;
                }

                set_owner(comm_fd, sbuf.st_dev, sbuf.st_ino, euid, egid);
            }
        }
            break;

        case SYS_mkdir:
        {
            const char *path = (const char *) stack[0];
            mode_t mode = (mode_t) stack[1];

            result = syscall(realCall, path, mode);
            if((int) result < 0) {
                error = errno;
                break;
            }

            // Get the inode
            struct stat64 sbuf;
            int sres = syscall(SYS_stat64, path, &sbuf);
            if(sres < 0) {
                perror("stat failed after mkdir()");
                break;
            }

            set_owner(comm_fd, sbuf.st_dev, sbuf.st_ino, euid, egid);
        }
            break;

        case SYS_symlink:
        {
            const char *target = (const char *) stack[0],
                  *linkname = (const char *) stack[1];

            result = syscall(realCall, target, linkname);
            if((int) result < 0) {
                error = errno;
                break;
            }

            // Get the inode
            struct stat64 sbuf;
            int sres = syscall(SYS_lstat64, linkname, &sbuf);
            if(sres < 0) {
                perror("stat failed after symlink()");
                break;
            }

            set_owner(comm_fd, sbuf.st_dev, sbuf.st_ino, euid, egid);
        }
            break;

/*
        case SYS_access:
        {
            struct stat sbuf;
            result = syscall(SYS_stat, stack[0], &sbuf);
            if((int) result < 0) {
                error = errno;
                break;
            }

            // Get the "real" owner
            if(get_owner(comm_fd, sbuf->st_dev, sbuf->st_ino,
                        &sbuf.st_uid, &sbuf.st_gid) < 0) {
                error = EIO;
                break;
            }

            int emode = sbuf.st_mode & 7;
            if(sbuf.st_gid == gid)
                emode |= (sbuf.st_mode >> 3) & 7;
            if(sbuf.st_uid == uid)
                emode |= (sbuf.st_mode >> 6) & 7;

            if( ( (stack[1] & R_OK) && !(emode & 4) ) || 
                ( (stack[1] & W_OK) && !(emode & 2) ) || 
                ( (stack[1] & X_OK) && !(emode & 1) ) ) {
                error = EACCES;
            }
        }
            break;
*/

        case SYS_stat:
        case SYS_lstat:
        case SYS_fstat:
        {
            struct stat *sbuf = (struct stat *) stack[1];
            result = syscall(realCall, stack[0], sbuf);
            if((int) result < 0) {
                error = errno;
                break;
            }

            if(get_owner(comm_fd, sbuf->st_dev, sbuf->st_ino,
                        &(sbuf->st_uid), &(sbuf->st_gid)) < 0) {
                error = EIO;
            }
        }
            break;


        case SYS_stat64:
        case SYS_lstat64:
        case SYS_fstat64:
        {
            struct stat64 *sbuf = (struct stat64 *) stack[1];
            result = syscall(realCall, stack[0], sbuf);
            if((int) result < 0) {
                error = errno;
                break;
            }

            // Stat buffer now contains dev/inode, so we can use it
            // to get the "real" owner
            if(get_owner(comm_fd, sbuf->st_dev, sbuf->st_ino,
                        &(sbuf->st_uid), &(sbuf->st_gid)) < 0) {
                error = EIO;
            }
        }
            break;


        case SYS_chown:
        case SYS_fchown:
        case SYS_lchown:
        {
            // OK, what are we actually pretending to chown here?
            struct stat64 sbuf;
            int statCall;
            switch(realCall) {
                case SYS_chown:  statCall = SYS_stat64; break;
                case SYS_fchown: statCall = SYS_fstat64; break;
                case SYS_lchown: statCall = SYS_lstat64; break;
            }
            result = syscall(statCall, stack[0], &sbuf);

            // ...and if this is EPERM, you're now thoroughly confused!
            if((int) result != 0) {
                error = errno;
                break;
            }

            set_owner(comm_fd, sbuf.st_dev, sbuf.st_ino, stack[1], stack[2]);
        }
            break;


        case SYS_close:
        case SYS_close_nocancel:
            // Hide comm_fd - part I
            if(stack[0] == comm_fd)
                error = EBADF;
            else {
                result = syscall(realCall, stack[0]);
                if(result < 0) error = errno;
            }
            break;


        case SYS_dup2:
            // Hide comm_fd - part II
            if(stack[0] == comm_fd || stack[1] == comm_fd)
                error = EBADF;
            else {
                result = syscall(realCall, stack[0], stack[1]);
                if(result < 0) error = errno;
            }
            break;


        default:
        {
#ifdef DEBUG
            printf("sysenter hook: unexpected callno = %d|%d, "
                   "stack = %lx %lx %lx %lx %lx %lx %lx %lx\n",
                   (int) (callno >> 16),
                   (int) (callno & 0xffff),
                   stack[0], stack[1], stack[2], stack[3],
                   stack[4], stack[5], stack[6], stack[7]
                  );
            error = ENOSYS;
#else
            // Fake it...
            result = syscall(realCall,
                    stack[0], stack[1], stack[2], stack[3],
                    stack[4], stack[5], stack[6], stack[7]);
            if((int) result == -1)
                error = errno;
#endif
        }
    }

    if(error) {
        cthread_set_errno_self(error);
        return (syscall_return_t) {-1, -1};
    } else {
        return (syscall_return_t) {result, result2};
    }
}
