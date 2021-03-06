#include <string.h>
#include "socket.h"

int socket_bind(int fd, const char *host, const char *port) {
    int ret = 0; struct addrinfo temp, *hit = NULL, *list = NULL;

    memset(&temp, 0, sizeof(temp));
    temp.ai_flags = AI_PASSIVE;
    temp.ai_family = AF_UNSPEC;
    temp.ai_socktype = 0;
    temp.ai_protocol = 0;

    if (getaddrinfo(host, port, &temp, &list) != 0)
        return SOCKET_ERROR;

    for (hit = list; hit != NULL; hit = hit->ai_next) {
        ret = bind(fd, hit->ai_addr, hit->ai_addrlen);

        if (ret == SOCKET_SUCCESS) break;
    }

    freeaddrinfo(list);

    return ret;
}

int socket_accept(int fd, char *host, char *port, int *ignore) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(struct sockaddr_storage);
    int sock = accept(fd, (struct sockaddr *)&addr, &len);

    if (sock == INVALID_SOCKET && ignore != NULL) {
        int error = 0;

#if defined(__linux__) || defined(__unix__)
        error = errno;
        *ignore = (error == EINTR || error == EWOULDBLOCK || error == EPROTO || error == ECONNABORTED);
#else
        error = WSAGetLastError();
        *ignore = (error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAEPROTO || error == WSAECONNABORTED);
#endif
    }

    if (host != NULL || port != NULL)
        getnameinfo((struct sockaddr *)&addr, len, host, NI_MAXHOST, port, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);

    return sock;
}

int socket_connect(int fd, const char *host, const char *port, int *ignore) {
    int ret = 0, error = 0, retry = 0;
    struct addrinfo temp, *hit = NULL, *list = NULL;

    memset(&temp, 0, sizeof(temp));
    temp.ai_family = AF_UNSPEC;
    temp.ai_socktype = 0;
    temp.ai_protocol = 0;

    if (getaddrinfo(host, port, &temp, &list) != 0)
        return SOCKET_ERROR;

    for (hit = list; hit != NULL; hit = hit->ai_next) {
        ret = connect(fd, hit->ai_addr, hit->ai_addrlen);

#if defined(__linux__) || defined(__unix__)
        error = errno;
        retry = !(error == EINTR || error == EWOULDBLOCK || error == EINPROGRESS);
#else
        error = WSAGetLastError();
        retry = !(error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAEINPROGRESS);
#endif

        if (ignore != NULL) *ignore = !retry;
        if (error == 0 || retry == 0) break;
    }

    freeaddrinfo(list);

    return ret;
}

ssize_t socket_recv(int fd, void *buf, size_t len, int flags, int *ignore) {
    ssize_t length = recv(fd, buf, len, flags);

    if (ignore != NULL) {
        int error = 0;

#if defined(__linux__) || defined(__unix__)
        error = errno;
        *ignore = (error == EINTR || error == EWOULDBLOCK || error == EAGAIN);
#else
        error = WSAGetLastError();
        *ignore = (error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSATRY_AGAIN);
#endif
    }

    return length;
}

ssize_t socket_recvfrom(int fd, void *buf, size_t len, int flags, char *host, char *port, int *ignore) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(struct sockaddr_storage);
    ssize_t length = recvfrom(fd, buf, len, flags, (struct sockaddr *)&addr, &addrlen);

    if (ignore != NULL) {
        int error = 0;

#if defined(__linux__) || defined(__unix__)
        error = errno;
        *ignore = (error == EINTR || error == EWOULDBLOCK || error == EAGAIN);
#else
        error = WSAGetLastError();
        *ignore = (error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSATRY_AGAIN);
#endif
    }

    if (host != NULL || port != NULL)
        getnameinfo((struct sockaddr *)&addr, len, host, NI_MAXHOST, port, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);

    return length;
}

ssize_t socket_send(int fd, void *buf, size_t len, int flags, int *ignore) {
#if defined(__linux__) || defined(__unix__)
    ssize_t length = send(fd, buf, len, flags | MSG_NOSIGNAL);
#else
    ssize_t length = send(fd, buf, len, flags);
#endif

    if (ignore != NULL) {
        int error = 0;

#if defined(__linux__) || defined(__unix__)
        error = errno;
        *ignore = (error == EINTR || error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS);
#else
        error = WSAGetLastError();
        *ignore = (error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSATRY_AGAIN || error == WSAEINPROGRESS);
#endif
    }

    return length;
}

ssize_t socket_sendto(int fd, void *buf, size_t len, int flags, const char *host, const char *port, int *ignore) {
    ssize_t length = 0; int error = 0, retry = 0;
    struct addrinfo temp, *hit = NULL, *list = NULL;

    memset(&temp, 0, sizeof(temp));
    temp.ai_family = AF_UNSPEC;
    temp.ai_socktype = 0;
    temp.ai_protocol = 0;

    if (getaddrinfo(host, port, &temp, &list) != 0)
        return SOCKET_ERROR;

    for (hit = list; hit != NULL; hit = hit->ai_next) {
#if defined(__linux__) || defined(__unix__)
        length = sendto(fd, buf, len, flags | MSG_NOSIGNAL, hit->ai_addr, hit->ai_addrlen);
#else
        length = sendto(fd, buf, len, flags, hit->ai_addr, hit->ai_addrlen);
#endif

#if defined(__linux__) || defined(__unix__)
        error = errno;
        retry = !(error == EINTR || error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS);
#else
        error = WSAGetLastError();
        retry = !(error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSATRY_AGAIN || error == WSAEINPROGRESS);
#endif

        if (ignore != NULL) *ignore = !retry;
        if (error == 0 || retry == 0) break;
    }

    freeaddrinfo(list);

    return length;
}
