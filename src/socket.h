#ifndef _SOCKET_H
#define _SOCKET_H 1

#if defined(__linux__) || defined(__unix__)
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#else
#include <winsock2.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__linux__) || defined(__unix__)
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

#define SOCKET_SUCCESS 0

static inline int socket_init(void) {
#if defined(__linux__) || defined(__unix__)
    // No need to init on linux
#else
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return SOCKET_ERROR;

    if (LOBYTE(wsa.wVersion) != 2 || HIBYTE(wsa.wVersion) != 2) {
        WSACleanup();

        return SOCKET_ERROR;
    }
#endif

    return SOCKET_SUCCESS;
}

#define socket_create(family, type, protocol) socket(family, type, protocol)

static inline int socket_setasync(int fd) {
#if defined(__linux__) || defined(__unix__)
    return fcntl(fd, F_SETFL, O_NONBLOCK);
#else
    unsigned long opt = 1UL;

    return ioctlsocket(fd, FIONBIO, &opt);
#endif
}

static inline int socket_timeout(int fd, int time) {
#if defined(__linux__) || defined(__unix__)
    struct timeval timeout = {(long)time, (long)((time - (long)time) * 1e6)};
#else
    int timeout = time * 1e3;
#endif

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == SOCKET_ERROR)
        return SOCKET_ERROR;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == SOCKET_ERROR)
        return SOCKET_ERROR;

    return SOCKET_SUCCESS;
}

int socket_bind(int fd, const char *host, const char *port);

#define socket_listen(fd, backlog) listen(fd, backlog)

int socket_accept(int fd, char *host, char *port, int *ignore);

int socket_connect(int fd, const char *host, const char *port, int *ignore);

ssize_t socket_recv(int fd, void *buf, size_t len, int flags, int *ignore);

ssize_t socket_recvfrom(int fd, void *buf, size_t len, int flags, char *host, char *port, int *ignore);

ssize_t socket_send(int fd, void *buf, size_t len, int flags, int *ignore);

ssize_t socket_sendto(int fd, void *buf, size_t len, int flags, const char *host, const char *port, int *ignore);

static inline void socket_close(int fd) {
#if defined(__linux__) || defined(__unix__)
    close(fd);
#else
    closesocket(fd);
#endif
}

static inline void socket_clean(void) {
#if defined(__linux__) || defined(__unix__)
    // No need to clean on linux
#else
    WSACleanup();
#endif
}

#ifdef __cplusplus
}
#endif

#endif
