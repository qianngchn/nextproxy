#ifndef _SOCKET_H
#define _SOCKET_H 1

#if defined(__linux) || defined(__linux__) || defined(linux)
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#else
#include <winsock2.h>
#endif

#if defined(__linux) || defined(__linux__) || defined(linux)
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline int socket_init(void) {
#if defined(__linux) || defined(__linux__) || defined(linux)
    // No need to init on linux
#else
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
    if (LOBYTE(wsa.wVersion) != 2 || HIBYTE(wsa.wVersion) != 2) {
        WSACleanup();
        return -1;
    }
#endif
    return 0;
}

static inline int socket_create(int family, int type, int protocol) {
    return socket(family, type, protocol);
}

static inline int socket_setasync(int fd) {
#if defined(__linux) || defined(__linux__) || defined(linux)
    return fcntl(fd, F_SETFL, O_NONBLOCK);
#else
    unsigned long opt = 1UL;
    return ioctlsocket(fd, FIONBIO, &opt);
#endif
}

static inline int socket_timeout(int fd, int time) {
    int ret = 0;

#if defined(__linux) || defined(__linux__) || defined(linux)
    struct timeval timeout = {time / 1000, time % 1000};
#else
    int timeout = time;
#endif
    ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    return ret;
}

int socket_bind(int fd, const char *host, const char *port);

static inline int socket_listen(int fd, int backlog) {
    return listen(fd, backlog);
}

int socket_accept(int fd, char *host, char *port, int *retry);

int socket_connect(int fd, const char *host, const char *port, int *retry);

static inline ssize_t socket_recv(int fd, void *buf, size_t len, int flags, int *retry) {
    ssize_t length = recv(fd, buf, len, flags);

    if (retry != NULL) {
        int error = 0;
#if defined(__linux) || defined(__linux__) || defined(linux)
        error = errno;
        *retry = (error == EINTR || error == EWOULDBLOCK || error == EAGAIN);
#else
        error = WSAGetLastError();
        *retry = (error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSATRY_AGAIN);
#endif
    }

    return length;
}

ssize_t socket_recvfrom(int fd, void *buf, size_t len, int flags, char *host, char *port, int *retry);

static inline ssize_t socket_send(int fd, void *buf, size_t len, int flags, int *retry) {
#if defined(__linux) || defined(__linux__) || defined(linux)
    ssize_t length = send(fd, buf, len, flags | MSG_NOSIGNAL);
#else
    ssize_t length = send(fd, buf, len, flags);
#endif

    if (retry != NULL) {
        int error = 0;
#if defined(__linux) || defined(__linux__) || defined(linux)
        error = errno;
        *retry = (error == EINTR || error == EWOULDBLOCK || error == EAGAIN);
#else
        error = WSAGetLastError();
        *retry = (error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSATRY_AGAIN);
#endif
    }

    return length;
}

ssize_t socket_sendto(int fd, void *buf, size_t len, int flags, const char *host, const char *port, int *retry);

static inline void socket_close(int fd) {
#if defined(__linux) || defined(__linux__) || defined(linux)
    close(fd);
#else
    closesocket(fd);
#endif
}

static inline void socket_clean(void) {
#if defined(__linux) || defined(__linux__) || defined(linux)
    // No need to clean on linux
#else
    WSACleanup();
#endif
}

#ifdef __cplusplus
}
#endif

#endif
