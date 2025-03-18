#include <uk/config.h>
#include <uk/syscall.h>
#include <sys/stat.h>
#include <uk/vfscore.h>

int __xstat(int ver, const char *path, struct stat *st) {
    return stat(path, st);
}

int __lxstat(int ver, const char *path, struct stat *st) {
    return lstat(path, st);
}

int __fxstat(int ver, int fd, struct stat *st) {
    return fstat(fd, st);
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *st, int flags) {
    return fstatat(dirfd, path, st, flags);
}

int __xmknod(int ver, const char *path, mode_t mode, dev_t *dev) {
    return mknod(path, mode, *dev);
} 