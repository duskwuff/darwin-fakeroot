#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>

int init_commfd(const char *socket_path);
int get_owner(int fd, dev_t dev, ino_t ino, uid_t *uid, gid_t *gid);
int set_owner(int fd, dev_t dev, ino_t ino, uid_t uid, gid_t gid);

#define GET_OWNER   0x1000
#define SET_OWNER   0x1001

// Make sure that this structure is laid out the same way on 32- and 64-bit!
struct comm_pkt {
    uint64_t cookie;
    uint64_t action, known;
    uint64_t dev, ino, uid, gid;
};

