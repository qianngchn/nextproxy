#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <regex.h>
#include "event.h"
#include "socket.h"

#if defined(__linux) || defined(__linux__) || defined(linux)
#include <netinet/tcp.h>
#else
#include <ws2tcpip.h>
#endif

#define BUFF_SIZE (1024 >> 1)
#define MAX_HEADER_SIZE (1024 << 2)
#define MAX_DATA_SIZE (1024 << 10)
#define MAX_CLIENTS FD_SETSIZE

typedef struct proxy {
    int client;
    int remote;
    int tunnel;
    char header[MAX_HEADER_SIZE];
    ssize_t header_size;
    char data[MAX_DATA_SIZE];
    ssize_t data_sid;
    ssize_t data_rid;
    struct proxy *next;
} PROXY;

static PROXY list = {0, 0, 0, {0}, 0, {0}, 0, 0, NULL};
static int clients = 0;
static int relay_mode = 0;
static int ipv6_mode = 0;
static int local_tcp = INVALID_SOCKET;

static int debug_flag = 0;
static int logger_flag = 0;
static FILE *logger_file = NULL;

static void print_log(const char *format, ...) {
    if (debug_flag || logger_flag) {
        char buff[128];
        char message[128];
        time_t now = time(NULL);
        va_list ap;
        strftime(buff, sizeof(buff), "%m-%d %H:%M:%S", localtime(&now));
        va_start(ap, format);
        vsnprintf(message, sizeof(message), format, ap);
        va_end(ap);
        if (debug_flag)
            printf("[%s] %s\n", buff, message);
        if (logger_flag && logger_file != NULL)
            fprintf(logger_file, "[%s] %s\n", buff, message);
    }
}

static int match_regex(const char *text, const char *pattern, const int index, char *result) {
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED | REG_ICASE | REG_NEWLINE) == 0) {
        regmatch_t match[regex.re_nsub + 1];
        if (regexec(&regex, text, regex.re_nsub + 1, match, 0) == 0) {
            if (result != NULL) {
                int start = match[index].rm_so, end = match[index].rm_eo;
                strncpy(result, text + start, end - start);
                result[end - start] = 0;
            }
            regfree(&regex);
            return 0;
        }
    }
    regfree(&regex);
    return 1;
}

static inline void set_socket(int fd) {
    socket_setasync(fd);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
    opt = MAX_DATA_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&opt, sizeof(opt));
    opt = MAX_DATA_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&opt, sizeof(opt));
}

static inline void set_nodelay(int fd, int opt) {
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt));
}

static inline void set_cork(int fd, int opt) {
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, (char *)&opt, sizeof(opt));
}

static PROXY *add_proxy(PROXY *head) {
    PROXY *cur = (PROXY *)malloc(sizeof(PROXY));
    cur->client = INVALID_SOCKET;
    cur->remote = INVALID_SOCKET;
    cur->tunnel = 0;
    *(cur->header) = 0;
    cur->header_size = 0;
    *(cur->data) = 0;
    cur->data_sid = 0;
    cur->data_rid = 0;
    cur->next = head->next;
    head->next = cur;
    return cur;
}

static void remove_proxy(int fd, PROXY *head) {
    PROXY *prev = head;
    PROXY *cur = prev->next;
    while (cur != NULL) {
        if (cur->client == fd || cur->remote == fd) {
            PROXY *temp = cur;
            prev->next = cur->next;
            cur = cur->next;
            if (temp->client != INVALID_SOCKET) socket_close(temp->client);
            if (temp->remote != INVALID_SOCKET) socket_close(temp->remote);
            free(temp); temp = NULL;
            break;
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
}

static void clean_proxy(PROXY *head) {
    while (head->next != NULL) {
        PROXY *cur = head->next;
        head->next = cur->next;
        if (cur->client != INVALID_SOCKET) socket_close(cur->client);
        if (cur->remote != INVALID_SOCKET) socket_close(cur->remote);
        free(cur); cur = NULL;
    }
}

static void handle_header(char *buff, char *host, char *port) {
    // todo: relay to sock5 or shadowsocks header
    int i = 0, j = 0, k = 0;
    while (*(buff + i) != ' ') i++;
    if (strncmp(buff, "GET", 3) == 0) {
        j = i + 8;
        while (*(buff + j) != '/') j++;
        for (k = i + 8; k < j; k++)
            if (*(buff + k) == ':') break;
        if (k != j) {
            strncpy(host, buff + i + 8, k - i - 8);
            strncpy(port, buff + k + 1, j - k - 1);
        } else {
            strncpy(host, buff + i + 8, j - i - 8);
            strcpy(port, "80");
        }
        sprintf(buff, "GET %s", buff + j);
    } else if (strncmp(buff, "CONNECT", 7) == 0) {
        j = i + 1;
        while (*(buff + j) != ' ') j++;
        for (k = i + 1; k < j; k++)
            if (*(buff + k) == ':') break;
        if (k != j) {
            strncpy(host, buff + i + 1, k - i - 1);
            strncpy(port, buff + k + 1, j - k - 1);
        } else {
            strncpy(host, buff + i + 1, j - i - 1);
            strcpy(port, "443");
        }
        sprintf(buff, "HTTP/1.1 200 Tunnel established\n\n");
    }
}

static void handle_data(char * buff) {
    // todo: handle socks5 or shadowsocks data
}

static void remote_tcp_cb(int fd, const int event, void *data) {
    print_log("fd: %d, event: %d, callback: %s, start", fd, event, __FUNCTION__);
    ssize_t len = 0; int error = 0, retry = 0;
    PROXY *head = (PROXY *)&list;
    PROXY *cur = head->next;
    while (cur != NULL) {
        if (cur->remote == fd) break;
        cur = cur->next;
    }
    switch (event) {
        case EVENT_IO_READ:
            len = socket_recv(cur->remote, cur->data + cur->data_rid, MAX_DATA_SIZE - cur->data_rid, 0, &retry);
            printf("%s", cur->header);
            printf("%ld %d %d\n", len, error, retry);
            if (retry == 0) {
                if (len > 0 && len < MAX_DATA_SIZE) {
                    cur->data_rid += len;
                    event_io_start(cur->client, EVENT_IO_WRITE);
                } else {
                    if (len == MAX_DATA_SIZE)
                        print_log("http data too large: %d", len);
                    print_log("close remote socket: %d", cur->remote);
                    event_io_remove(cur->remote, EVENT_IO_READ);
                    event_io_remove(cur->remote, EVENT_IO_WRITE);
                    event_io_stop(cur->client, EVENT_IO_WRITE);
                    socket_close(cur->remote);
                    cur->remote = INVALID_SOCKET;
                    *(cur->data) = 0;
                    cur->data_sid = 0;
                    cur->data_rid = 0;
                }
            }
            break;
        case EVENT_IO_WRITE:
            len = socket_send(cur->remote, cur->header, cur->header_size, 0, &retry);
            printf("%s", cur->header);
            printf("%ld %d %d\n", len, error, retry);
            if (retry == 0) {
                if (len >= 0) {
                    if (len == cur->header_size) {
                        event_io_stop(cur->remote, EVENT_IO_WRITE);
                        *(cur->header) = 0;
                        cur->header_size = 0;
                    }
                } else {
                    print_log("close remote socket: %d", cur->remote);
                    event_io_remove(cur->remote, EVENT_IO_READ);
                    event_io_remove(cur->remote, EVENT_IO_WRITE);
                    event_io_start(cur->client, EVENT_IO_READ);
                    socket_close(cur->remote);
                    cur->remote = INVALID_SOCKET;
                    *(cur->header) = 0;
                    cur->header_size = 0;
                }
            }
            break;
    }
    print_log("fd: %d, event: %d, callback: %s, exit", fd, event, __FUNCTION__);
}

static void client_tcp_cb(int fd, const int event, void *data) {
    print_log("fd: %d, event: %d, callback: %s, start", fd, event, __FUNCTION__);
    ssize_t len = 0; int error = 0, retry = 0;
    PROXY *head = (PROXY *)&list;
    PROXY *cur = head->next;
    while (cur != NULL) {
        if (cur->client == fd) break;
        cur = cur->next;
    }
    switch (event) {
        case EVENT_IO_READ:
            len = socket_recv(cur->client, cur->header, MAX_HEADER_SIZE, 0, &retry);
            printf("%ld %d %d\n", len, error, retry);
            if (retry == 0) {
                if (len > 0 && len < MAX_HEADER_SIZE) {
                    *(cur->header + len) = 0;
                    if (cur->remote == INVALID_SOCKET) {
                        int sock = 0;
                        if (ipv6_mode == 0)
                            sock = socket_create(AF_INET, SOCK_STREAM, 0);
                        else
                            sock = socket_create(AF_INET6, SOCK_STREAM, 0);
                        if (sock > 0) {
                            cur->remote = sock;
                            set_socket(cur->remote);
                            event_io_add(cur->remote, EVENT_IO_READ, head, remote_tcp_cb);
                            event_io_add(cur->remote, EVENT_IO_WRITE, head, remote_tcp_cb);
                        }
                    }
                    if (cur->tunnel == 0) {
                        char host[BUFF_SIZE] = {0}, port[BUFF_SIZE] = {0};
                        handle_header(cur->header, host, port);
                        cur->header_size = strlen(cur->header);
                        if (strcmp(port, "443") == 0) {
                            cur->tunnel = 1;
                            socket_send(cur->client, cur->header, strlen(cur->header), 0, NULL);
                            *(cur->header) = 0;
                            cur->header_size = 0;
                        }
                        if (cur->remote != INVALID_SOCKET && socket_connect(cur->remote, host, port, NULL) == 0) {
                            print_log("connect to %s:%s, using socket: %d", host, port, cur->remote);
                            if (cur->tunnel == 0) {
                                event_io_start(cur->remote, EVENT_IO_READ);
                                event_io_start(cur->remote, EVENT_IO_WRITE);
                            }
                        } else {
                            print_log("connect remote socket error: %d", cur->remote);
                            event_io_remove(cur->remote, EVENT_IO_READ);
                            event_io_remove(cur->remote, EVENT_IO_WRITE);
                            socket_close(cur->remote);
                            cur->remote = INVALID_SOCKET;
                        }
                    } else {
                        cur->header_size = len;
                        event_io_start(cur->remote, EVENT_IO_READ);
                        event_io_start(cur->remote, EVENT_IO_WRITE);
                    }
                    // todo: all timeout to free connection
                } else {
                    if (len == MAX_HEADER_SIZE)
                        print_log("http header too large: %d", len);
                    print_log("close client socket: %d", cur->client);
                    event_io_remove(cur->remote, EVENT_IO_READ);
                    event_io_remove(cur->remote, EVENT_IO_WRITE);
                    event_io_remove(cur->client, EVENT_IO_READ);
                    event_io_remove(cur->client, EVENT_IO_WRITE);
                    remove_proxy(cur->client, head);
                    if (clients == MAX_CLIENTS)
                        event_io_start(local_tcp, EVENT_IO_READ);
                    clients--;
                }
            }
            break;
        case EVENT_IO_WRITE:
            len = socket_send(cur->client, cur->data + cur->data_sid, cur->data_rid - cur->data_sid, 0, &retry);
            printf("%s", cur->header);
            printf("%ld %d %d\n", len, error, retry);
            if (retry == 0) {
                if (len >= 0) {
                    cur->data_sid += len;
                    if (cur->data_sid == cur->data_rid)
                        event_io_stop(cur->client, EVENT_IO_WRITE);
                } else {
                    print_log("close client socket: %d", cur->client);
                    event_io_remove(cur->remote, EVENT_IO_READ);
                    event_io_remove(cur->remote, EVENT_IO_WRITE);
                    event_io_remove(cur->client, EVENT_IO_WRITE);
                    event_io_remove(cur->client, EVENT_IO_READ);
                    remove_proxy(cur->client, head);
                    if (clients == MAX_CLIENTS)
                        event_io_start(local_tcp, EVENT_IO_READ);
                    clients--;
                }
            }
            break;
    }
    print_log("fd: %d, event: %d, callback: %s, exit", fd, event, __FUNCTION__);
}

static void local_tcp_cb(int fd, const int event, void *data) {
    print_log("fd: %d, event: %d, callback: %s, start", fd, event, __FUNCTION__);
    int sock = INVALID_SOCKET;
    char host[BUFF_SIZE] = {0};
    int error = 0, retry = 0;
    if (event == EVENT_IO_READ) {
        if (clients == MAX_CLIENTS) {
            print_log("%d clients online, stop listening for more clients", clients);
            event_io_stop(local_tcp, EVENT_IO_READ);
        } else {
            sock = socket_accept(fd, host, NULL, &retry);
            if (retry == 0) {
                if (sock != INVALID_SOCKET) {
                    clients++;
                    print_log("accept client: %s, total: %d, using socket: %d", host, clients, sock);
                    set_socket(sock);
                    PROXY *head = (PROXY *)&list;
                    PROXY *cur = add_proxy(head);
                    cur->client = sock;
                    event_io_add(cur->client, EVENT_IO_READ, head, client_tcp_cb);
                    event_io_add(cur->client, EVENT_IO_WRITE, head, client_tcp_cb);
                    event_io_start(cur->client, EVENT_IO_READ);
                } else
                    print_log("accept client error: %d", error);
            }
        }
    }
    print_log("fd: %d, event: %d, callback: %s, exit", fd, event, __FUNCTION__);
}

static void usage(const char *name) {
    printf("Usage: %s [-l http://local_server:local_port] [-p protocol://[method:password@]remote_server:remote_port] [-6] [-g] [-d] [-h]\n", name);
    printf("  -l: listen address of the local http proxy server, also support http proxy tunnel, default: \"http://localhost:7788\"\n");
    printf("  -p: remote server address as the parent proxy, now support socks5 and shadowsocks, without this option as a normal http proxy server\n");
    printf("  -6: ipv6 mode, use ipv6 socket and network address\n");
    printf("  -g: logger mode, write output to stat.log\n");
    printf("  -d: debug mode, write output to stdout\n");
    printf("  -h: show help information\n");
}

int main(int argc, char **argv) {
    char local_protocol[BUFF_SIZE] = "http";
    char local_host[BUFF_SIZE] = "127.0.0.1";
    char local_port[BUFF_SIZE] = "7788";
    char remote_protocol[BUFF_SIZE] = {0};
    char remote_host[BUFF_SIZE] = {0};
    char remote_port[BUFF_SIZE] = {0};
    char remote_method[BUFF_SIZE] = {0};
    char remote_password[BUFF_SIZE] = {0};
    int opt = 0; char result[BUFF_SIZE] = {0};
    while ((opt = getopt(argc, argv, "l:p:6gdh")) != -1) {
        switch (opt) {
            case 'l':
                if (match_regex(optarg, "(.+)://(.+):(.+)", 1, result) == 0)
                    strcpy(local_protocol, result);
                if (match_regex(optarg, "(.+)://(.+):(.+)", 2, result) == 0)
                    strcpy(local_host, result);
                if (match_regex(optarg, "(.+)://(.+):(.+)", 3, result) == 0)
                    strcpy(local_port, result);
                if (strcmp(local_protocol, "http") != 0) {
                    printf("%s is not supported local protocol, now trying http proxy\n", local_protocol);
                    sprintf(local_protocol, "http");
                }
                break;
            case 'p':
                if (match_regex(optarg, "(.+)://(.+):(.+)@(.+):(.+)", 0, NULL) == 0) {
                    relay_mode = 1;
                    if (match_regex(optarg, "(.+)://(.+):(.+)@(.+):(.+)", 1, result))
                        strcpy(remote_protocol, result);
                    if (match_regex(optarg, "(.+)://(.+):(.+)@(.+):(.+)", 2, result))
                        strcpy(remote_method, result);
                    if (match_regex(optarg, "(.+)://(.+):(.+)@(.+):(.+)", 3, result))
                        strcpy(remote_password, result);
                    if (match_regex(optarg, "(.+)://(.+):(.+)@(.+):(.+)", 4, result))
                        strcpy(remote_host, result);
                    if (match_regex(optarg, "(.+)://(.+):(.+)@(.+):(.+)", 5, result))
                        strcpy(remote_port, result);
                    if (strcmp(remote_protocol, "ss") != 0) {
                        printf("%s is not supported remote protocol, now trying shadowsocks proxy\n", remote_protocol);
                        sprintf(remote_protocol, "ss");
                    }
                } else if (match_regex(optarg, "(.+)://(.+):(.+)", 0, NULL) == 0) {
                    relay_mode = 1;
                    if (match_regex(optarg, "(.+)://(.+):(.+)", 1, result))
                        strcpy(remote_protocol, result);
                    if (match_regex(optarg, "(.+)://(.+):(.+)", 2, result))
                        strcpy(remote_host, result);
                    if (match_regex(optarg, "(.+)://(.+):(.+)", 3, result))
                        strcpy(remote_port, result);
                    if (strcmp(remote_protocol, "socks5") != 0) {
                        printf("%s is not supported remote protocol, now trying socks5 proxy\n", remote_protocol);
                        sprintf(remote_protocol, "socks5");
                    }
                } else
                    relay_mode = 0;
                break;
            case '6':
                ipv6_mode = 1;
                break;
            case 'g':
                logger_flag = 1;
                break;
            case 'd':
                debug_flag = 1;
                break;
            case 'h':
                usage(argv[0]);
            default:
                return 1;
        }
    }

    if (relay_mode) {
        if (strcmp(remote_protocol, "ss") == 0)
            printf("relay proxy mode, local_server: %s://%s:%s, remote_server: %s://%s:%s@%s:%s\n", local_protocol, local_host, local_port, remote_protocol, remote_method, remote_password, remote_host, remote_port);
        else
            printf("relay proxy mode, local_server: %s://%s:%s, remote_server: %s://%s:%s\n", local_protocol, local_host, local_port, remote_protocol, remote_host, remote_port);
    } else
        printf("normal proxy mode, local_server: %s://%s:%s\n", local_protocol, local_host, local_port);

    if (logger_flag && logger_file == NULL)
        logger_file = fopen("stat.log", "wb");

    socket_init();

    if (ipv6_mode == 0)
        local_tcp = socket_create(AF_INET, SOCK_STREAM, 0);
    else
        local_tcp = socket_create(AF_INET6, SOCK_STREAM, 0);

    set_socket(local_tcp);
    set_nodelay(local_tcp, 1);

    if (local_tcp != INVALID_SOCKET && socket_bind(local_tcp, local_host, local_port) != SOCKET_ERROR) {
        if (socket_listen(local_tcp, MAX_CLIENTS) == 0) {
            printf("listen on tcp socket success, socket: %d, host: %s, port: %s\n", local_tcp, local_host, local_port);
            event_io_add(local_tcp, EVENT_IO_READ, NULL, local_tcp_cb);
            event_io_start(local_tcp, EVENT_IO_READ);
        }
    }

    event_loop();

    if (local_tcp != INVALID_SOCKET)
        socket_close(local_tcp);

    clean_proxy(&list);
    socket_clean();

    if (logger_flag && logger_file != NULL)
        fclose(logger_file);

    return 0;
}
