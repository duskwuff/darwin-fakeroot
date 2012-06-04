#include "communicate.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <pthread.h>

static pthread_mutex_t comm_lockout;

int init_commfd(const char *sockpath)
{
    if(pthread_mutex_init(&comm_lockout, NULL) != 0)
        return -1;

    int sock = socket(PF_LOCAL, SOCK_STREAM, PF_UNSPEC);
    if(sock < 0)
        return -1;

    fcntl(sock, F_SETFD, 1); // close-on-exec

    {
        struct sockaddr_un uaddr;
        strncpy(uaddr.sun_path, sockpath, sizeof(uaddr.sun_path));
        uaddr.sun_family = PF_LOCAL;
        uaddr.sun_len = SUN_LEN(&uaddr);

        if(connect(sock, (struct sockaddr *) &uaddr, sizeof(uaddr)) < 0)
            return -1;
    }

    return sock;
}


int set_owner(int fd, dev_t dev, ino_t ino, uid_t uid, gid_t gid)
{
    if(pthread_mutex_lock(&comm_lockout) != 0) {
        perror("pthread_mutex_lock");
        return -1;
    }

    struct comm_pkt pkt = {
        .action = SET_OWNER,
        .dev = dev,
        .ino = ino,
        .uid = uid,
        .gid = gid,
    };

    int res = send(fd, &pkt, sizeof(pkt), 0);

    pthread_mutex_unlock(&comm_lockout);

    if(res != sizeof(pkt)) {
        perror("send");
        return -1;
    }

    return 0;
}


int get_owner(int fd, dev_t dev, ino_t ino, uid_t *uid, gid_t *gid)
{
    if(pthread_mutex_lock(&comm_lockout) != 0) {
        perror("pthread_mutex_lock");
        return -1;
    }

    struct comm_pkt pkt = {
        .action = GET_OWNER,
        .dev = dev,
        .ino = ino,
    };

    if(send(fd, &pkt, sizeof(pkt), 0) != sizeof(pkt)) {
        pthread_mutex_unlock(&comm_lockout);
        perror("send");
        return -1;
    }

    if(recv(fd, &pkt, sizeof(pkt), 0) != sizeof(pkt)) {
        pthread_mutex_unlock(&comm_lockout);
        perror("recv");
        return -1;
    }

    pthread_mutex_unlock(&comm_lockout);

    if(pkt.known) {
        if(uid) *uid = pkt.uid;
        if(gid) *gid = pkt.gid;
    }

    return pkt.known;
}

