#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#include <db.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/event.h>

#include "communicate.h"

#define fatal(msg) do { perror(msg); exit(1); } while(0)

#define _STR(x) #x
#define STR(x) _STR(x)

DB *nodeData;

struct dbKey {
    dev_t dev;
    ino_t ino;
};

struct dbVal {
    uid_t uid;
    gid_t gid;
};

void process_pkt(struct comm_pkt *pkt)
{
    struct dbKey key = {
        .dev = pkt->dev,
        .ino = pkt->ino,
    };

    DBT dkey = {
        .data = &key,
        .size = sizeof(key),
    };

    int result;

    switch(pkt->action) {
        case GET_OWNER:
        {
            DBT dval;

            result = nodeData->get(nodeData, &dkey, &dval, 0);
            if(result == 0) {
                struct dbVal *val = dval.data;

                pkt->uid = val->uid;
                pkt->gid = val->gid;
                pkt->known = 1;
            } else if(result == 1) {
                pkt->known = 0;
            } else {
                perror("db->get");
                pkt->known = 0;
            }
        }
            break;

        case SET_OWNER:
        {
            DBT dval;

            result = nodeData->get(nodeData, &dkey, &dval, 0);
            if(result == 1) {
                struct dbVal val = {
                    .uid = pkt->uid,
                    .gid = pkt->gid,
                };
                dval.data = &val;
                dval.size = sizeof(val);
                result = nodeData->put(nodeData, &dkey, &dval, 0);
                if(result != 0) {
                    perror("db->put");
                    break;
                }
            } else {
                struct dbVal *val = dval.data;
                val->uid = pkt->uid;
                val->gid = pkt->gid;
            }
        }
            break;

        default:
            fprintf(stderr, "Unknown action %x\n", (int) pkt->action);
    }
}

//////////////////////////////////////////////////////////////////////////////

static struct option cmdLineOpts[] = {
    { "help",       no_argument,        NULL,   'h' },
    { "version",    no_argument,        NULL,   'v' },
    { "libpath",    required_argument,  NULL,   'l' },
    { "persist",    required_argument,  NULL,   'p' },
    { NULL, 0, NULL, 0},
};

static int exit_flag = 0;
static int lsock = -1;
static char sockpath[256] = "";
static const char *persistPath = NULL,
                  *libPath = STR(LIBINSTALLPATH) "/libfakeroot.dylib";

void cleanup(void)
{
    if(sockpath[0] && unlink(sockpath) < 0)
        perror("unlink");

    if(nodeData) {
        nodeData->close(nodeData);
    }
}

void sigint(int sig)
{
    exit_flag = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "Usage: fakeroot [options] cmd [args...]\n"
            "\n"
            "Options:\n"
            "    -h,  --help            Print this message\n"
            "    -v,  --version         Version information\n"
            "    -l,  --libpath=[path]  Use alternate libfakeroot.dylib\n"
            "    -p,  --persist=[path]  Save/load ownership to file\n"
           );
    exit(1);
}

int main(int argc, char **argv, char **envp)
{
    int ch;

    int had_posixly_correct = 0;
    if(getenv("POSIXLY_CORRECT")) {
        had_posixly_correct = 1;
    } else {
        putenv("POSIXLY_CORRECT=1");
    }

    while((ch = getopt_long(argc, argv, "hvl:p:", cmdLineOpts, NULL)) != -1) {
        switch(ch) {
            case 'h':
                usage();
                break;

            case 'v':
                fprintf(stderr,
                        "fakeroot darwin " STR(VERSION) "\n"
                        );
                break;

            case 'l':
                if(optarg[0] == '/') {
                    libPath = optarg;
                } else {
                    char buf[PATH_MAX];
                    getcwd(buf, sizeof(buf));
                    strlcat(buf, "/", sizeof(buf));
                    strlcat(buf, optarg, sizeof(buf));
                    libPath = strdup(buf);
                }
                break;

            case 'p':
                persistPath = optarg;
                break;

            default:
                usage();
        }
    }

    if(!had_posixly_correct)
        unsetenv("POSIXLY_CORRECT");

    argc -= optind;
    argv += optind;
    
    if(argc < 1)
        usage();

    lsock = socket(PF_LOCAL, SOCK_STREAM, PF_UNSPEC);
    if(lsock < 0)
        fatal("socket");

    nodeData = dbopen(persistPath, O_RDWR | O_CREAT, 0666, DB_HASH, NULL);

    snprintf(sockpath, sizeof(sockpath), "/tmp/fakeroot.%d.sock", getpid());

    {
        struct sockaddr_un uaddr;
        strncpy(uaddr.sun_path, sockpath, sizeof(uaddr.sun_path));
        uaddr.sun_family = PF_LOCAL;
        uaddr.sun_len = SUN_LEN(&uaddr);

        if(bind(lsock, (struct sockaddr *) &uaddr, sizeof(uaddr)) < 0)
            fatal("bind");
    }

    atexit(cleanup);

    if(listen(lsock, 4) < 0)
        fatal("listen");

    int kq = kqueue();
    if(kq < 0)
        fatal("kqueue");

    {
        struct kevent ev = {
            .ident  = lsock,
            .filter = EVFILT_READ,
            .flags  = EV_ADD,
        };
            
        if(kevent(kq, &ev, 1, NULL, 0, NULL) < 0)
            fatal("kevent (add lsock)");
    }

    signal(SIGINT, sigint);

    if(fork() == 0) {
        setenv("FAKEROOT_SOCKET", sockpath, 1);
        setenv("DYLD_INSERT_LIBRARIES", libPath, 1);

        execvp(argv[0], argv);

        // oops?
        perror("exec");
        kill(getppid(), SIGINT);
        exit(1);
    }

    int connections = 0;

    while(!exit_flag) {
        struct kevent events[16];
        int nevent = kevent(kq, NULL, 0, events, 16, NULL);
        if(nevent < 0 && errno != EINTR)
            fatal("kevent (waiting)");

        for(int i = 0; i < nevent; i++) {
            if(events[i].filter != EVFILT_READ)
                continue; // wtf?

            if(events[i].ident == lsock) {
                int csock = accept(lsock, NULL, NULL);
                if(csock < 0) {
                    perror("accept");
                    continue;
                }

                struct kevent ev = {
                    .ident  = csock,
                    .filter = EVFILT_READ,
                    .flags  = EV_ADD,
                };

                if(kevent(kq, &ev, 1, NULL, 0, NULL) < 0)
                    fatal("kevent (add csock)");

                connections++;
            } else {
                // must be a connected sock!
                if(events[i].flags & EV_EOF) { // socket closed!
                    close(events[i].ident);
                    connections--;
                    continue;
                }

                struct comm_pkt pkt;

                if(recv(events[i].ident, &pkt, sizeof(pkt), 0) != sizeof(pkt)) {
                    close(events[i].ident);
                    connections--;
                    continue;
                }

                process_pkt(&pkt);

                if(send(events[i].ident, &pkt, sizeof(pkt), 0) != sizeof(pkt)) {
                    close(events[i].ident);
                    connections--;
                }
            }
        }

        if(connections == 0)
            break;
    }

    exit(0);
}
