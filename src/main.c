#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <regex.h>
#include "socket.h"
#include "event.h"

#if defined(__linux__) || defined(__unix__)
#include <netinet/tcp.h>
#else
#include <ws2tcpip.h>
#endif

#define BUFF_SIZE (1024 >> 1)
#define MAX_DATA_SIZE (1024 << 10)

#define MAX_CLIENTS FD_SETSIZE
#define PROXY_TIMEOUT 10.0

enum {
    PROXY_HAS_NONE    = 0x00,
    PROXY_HAS_CONNECT = 0x01,
    PROXY_HAS_TUNNEL  = 0x02,
    PROXY_HAS_NOTEND  = 0x04
};

typedef struct proxy {
    int client;
    EVENT_IO client_read;
    EVENT_IO client_write;

    int remote;
    EVENT_IO remote_read;
    EVENT_IO remote_write;

    EVENT_TIMER timer_clean;

    int status;

    char data[MAX_DATA_SIZE];
    ssize_t data_size;
    ssize_t data_index;
} PROXY;

static EVENT_LOOP *loop = NULL;
static EVENT_IO local_accept;

static int local = INVALID_SOCKET;
static int clients = 0;

static int ipv6_mode = 0;
static int relay_mode = 0;

static int debug_flag = 0;
static int logger_flag = 0;
static FILE *logger_file = NULL;

static void print_log(const char *format, ...) {
    if (debug_flag || logger_flag) {
        char buff[128], message[128];
        time_t now = time(NULL);
        va_list ap;

        va_start(ap, format);
        strftime(buff, sizeof(buff), "%m-%d %H:%M:%S", localtime(&now));
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

            return 1;
        }
    }

    regfree(&regex);

    return 0;
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

static inline PROXY *new_proxy(void) {
    PROXY *node = (PROXY *)malloc(sizeof(PROXY));
    if (node == NULL) return NULL;
    memset(node, 0, sizeof(PROXY));

    node->client = INVALID_SOCKET;
    node->remote = INVALID_SOCKET;
    node->status = PROXY_HAS_NONE;
    *(node->data) = 0;
    node->data_size = 0;
    node->data_index = 0;

    return node;
}

static inline void delete_proxy(PROXY *node) {
    if (node->client != INVALID_SOCKET)
        socket_close(node->client);

    if (node->remote != INVALID_SOCKET)
        socket_close(node->remote);

    free(node);
}

static int handle_header(char *buff, char *host, char *port) {
    if (strncmp(buff, "GET", 3) == 0) {
        int i = 0, j = 0, k = 0;
        while (*(buff + i) != ' ') ++i;

        j = i + 8;
        while (*(buff + j) != '/') ++j;

        for (k = i + 8; k < j; ++k)
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
        int i = 0, j = 0, k = 0;
        while (*(buff + i) != ' ') ++i;

        j = i + 1;
        while (*(buff + j) != ' ') ++j;

        for (k = i + 1; k < j; ++k)
            if (*(buff + k) == ':') break;

        if (k != j) {
            strncpy(host, buff + i + 1, k - i - 1);
            strncpy(port, buff + k + 1, j - k - 1);
        } else {
            strncpy(host, buff + i + 1, j - i - 1);
            strcpy(port, "443");
        }

        sprintf(buff, "HTTP/1.1 200 Tunnel established\n\n");
    } else
        return 0;

    // todo: relay to sock5 or shadowsocks header
    return 1;
}

static int handle_data(char * buff) {
    // todo: handle socks5 or shadowsocks data
    return 1;
}

static void timer_clean_cb(EVENT_LOOP *loop, EVENT_TIMER *watcher) {
    print_log("timeout: %lf, repeat: %lf, callback: %s, enter", watcher->timeout, watcher->repeat, __func__);

    PROXY *node = (PROXY *)(watcher->data);

    event_io_stop(loop, &node->client_read);
    event_io_stop(loop, &node->client_write);
    event_io_stop(loop, &node->remote_read);
    event_io_stop(loop, &node->remote_write);
    event_timer_stop(loop, &node->timer_clean);


    delete_proxy(node);

    if (clients == MAX_CLIENTS)
        event_io_start(loop, &local_accept);

    --clients;
}

static void remote_write_cb(EVENT_LOOP *loop, EVENT_IO *watcher) {
    print_log("fd: %d, events: %d, callback: %s, enter", watcher->fd, watcher->events, __func__);

    PROXY *node = (PROXY *)(watcher->data);
    event_io_stop(loop, &node->remote_write);
    event_timer_stop(loop, &node->timer_clean);

    ssize_t len = 0; int ignore = 0;
    len = socket_send(node->remote, node->data + node->data_index, node->data_size - node->data_index, 0, &ignore);
    if (len > 0) node->data_index += len;

    if (len < 0 && ignore == 0) {
        print_log("remote socket write error: %d", node->remote);

        event_timer_start(loop, &node->timer_clean);

        return;
    } else if ((len < 0 && ignore == 1) || node->data_index < node->data_size) {
        print_log("remote socket write retry: %d", node->remote);

        event_io_start(loop, &node->remote_write);
        event_timer_start(loop, &node->timer_clean);

        return;
    }

    *(node->data) = 0;
    node->data_size = 0;
    node->data_index = 0;

    if (node->status & PROXY_HAS_NOTEND) {
        event_io_start(loop, &node->client_read);
        event_timer_start(loop, &node->timer_clean);

        return;
    }

    event_io_start(loop, &node->remote_read);
    event_timer_start(loop, &node->timer_clean);
}

static void remote_read_cb(EVENT_LOOP *loop, EVENT_IO *watcher) {
    print_log("fd: %d, events: %d, callback: %s, enter", watcher->fd, watcher->events, __func__);

    PROXY *node = (PROXY *)(watcher->data);
    event_io_stop(loop, &node->remote_read);
    event_timer_stop(loop, &node->timer_clean);

    ssize_t len = 0; int ignore = 0;
    len = socket_recv(node->remote, node->data, MAX_DATA_SIZE, 0, &ignore);

    if ((len < 0 && ignore == 0) || len == 0) {
        print_log("remote socket read error: %d", node->remote);

        event_timer_start(loop, &node->timer_clean);

        return;
    } else if (len < 0 && ignore == 1) {
        print_log("remote socket read retry: %d", node->remote);

        event_io_start(loop, &node->remote_read);
        event_timer_start(loop, &node->timer_clean);

        return;
    }

    node->data_size = len;

    event_io_start(loop, &node->client_write);
    event_timer_start(loop, &node->timer_clean);
}

static void client_write_cb(EVENT_LOOP *loop, EVENT_IO *watcher) {
    print_log("fd: %d, events: %d, callback: %s, enter", watcher->fd, watcher->events, __func__);

    PROXY *node = (PROXY *)(watcher->data);
    event_io_stop(loop, &node->client_write);
    event_timer_stop(loop, &node->timer_clean);

    ssize_t len = 0; int ignore = 0;
    len = socket_send(node->client, node->data + node->data_index, node->data_size - node->data_index, 0, &ignore);
    if (len > 0) node->data_index += len;

    if (len < 0 && ignore == 0) {
        print_log("client socket write error: %d", node->client);

        event_timer_start(loop, &node->timer_clean);

        return;
    } else if ((len < 0 && ignore == 1) || node->data_index < node->data_size) {
        print_log("client socket write retry: %d", node->client);

        event_io_start(loop, &node->client_write);
        event_timer_start(loop, &node->timer_clean);

        return;
    }

    *(node->data) = 0;
    node->data_size = 0;
    node->data_index = 0;

    event_io_start(loop, &node->remote_read);
    event_timer_start(loop, &node->timer_clean);
}

static void client_read_cb(EVENT_LOOP *loop, EVENT_IO *watcher) {
    print_log("fd: %d, events: %d, callback: %s, enter", watcher->fd, watcher->events, __func__);

    PROXY *node = (PROXY *)(watcher->data);
    event_io_stop(loop, &node->client_read);
    event_timer_stop(loop, &node->timer_clean);

    ssize_t len = 0; int ignore = 0;
    len = socket_recv(node->client, node->data, MAX_DATA_SIZE, 0, &ignore);

    if ((len < 0 && ignore == 0) || len == 0) {
        print_log("client socket read error: %d", node->client);

        event_timer_start(loop, &node->timer_clean);

        return;
    } else if (len < 0 && ignore == 1) {
        print_log("client socket read retry: %d", node->client);

        event_io_start(loop, &node->client_read);
        event_timer_start(loop, &node->timer_clean);

        return;
    }

    node->data_size = len;

    if (len == MAX_DATA_SIZE)
        node->status |= PROXY_HAS_NOTEND;
    else if (node->status & PROXY_HAS_NOTEND)
        node->status ^= PROXY_HAS_NOTEND;

    if (node->status & PROXY_HAS_CONNECT) {
        event_io_start(loop, &node->remote_write);
        event_timer_start(loop, &node->timer_clean);

        return;
    }

    char host[BUFF_SIZE] = {0}, port[BUFF_SIZE] = {0};

    if (!handle_header(node->data, host, port)) {
        print_log("handle header error, not supported protocol");

        event_timer_start(loop, &node->timer_clean);

        return;
    }

    if (node->remote == INVALID_SOCKET) {
        if (ipv6_mode == 0)
            node->remote = socket_create(AF_INET, SOCK_STREAM, 0);
        else
            node->remote = socket_create(AF_INET6, SOCK_STREAM, 0);

        if (node->remote == INVALID_SOCKET) {
            print_log("remote socket create error: %d", node->remote);

            event_timer_start(loop, &node->timer_clean);

            return;
        }

        set_socket(node->remote);
        set_nodelay(node->remote, 1);

        event_io_init(&node->remote_read, remote_read_cb, node->remote, EVENT_IO_READ);
        event_io_init(&node->remote_write, remote_write_cb, node->remote, EVENT_IO_WRITE);
        event_io_data(&node->remote_read, node);
        event_io_data(&node->remote_write, node);
    }

    int ret = socket_connect(node->remote, host, port, &ignore);

    if (ret < 0 && ignore == 0) {
        print_log("connect remote socket error: %d", node->remote);

        event_timer_start(loop, &node->timer_clean);

        return;
    }

    print_log("connect to %s:%s, using socket: %d", host, port, node->remote);

    node->status |= PROXY_HAS_CONNECT;

    event_io_start(loop, &node->remote_write);
    event_timer_start(loop, &node->timer_clean);
}

static void local_accept_cb(EVENT_LOOP *loop, EVENT_IO *watcher) {
    print_log("fd: %d, events: %d, callback: %s, enter", watcher->fd, watcher->events, __func__);

    if (clients == MAX_CLIENTS) {
        print_log("%d client(s) online, stop listening for more clients", clients);

        event_io_stop(loop, &local_accept);
    } else {
        char host[BUFF_SIZE] = {0}, port[BUFF_SIZE] = {0};

        int client = socket_accept(local, host, port, NULL);

        if (client == INVALID_SOCKET) return;

        set_socket(client);
        set_nodelay(client, 1);

        PROXY *node = new_proxy();

        if (node == NULL) { socket_close(client); return; }

        ++clients;

        print_log("accept client: %s/%s, total: %d, using socket: %d", host, port, clients, client);

        node->client = client;

        event_io_init(&node->client_read, client_read_cb, client, EVENT_IO_READ);
        event_io_init(&node->client_write, client_write_cb, client, EVENT_IO_WRITE);
        event_timer_init(&node->timer_clean, timer_clean_cb, PROXY_TIMEOUT, 0);

        event_io_data(&node->client_read, node);
        event_io_data(&node->client_write, node);
        event_timer_data(&node->timer_clean, node);

        event_io_start(loop, &node->client_read);
        event_timer_start(loop, &node->timer_clean);
    }
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
                if (match_regex(optarg, "(.+)://(.+):(.+)", 1, result))
                    strcpy(local_protocol, result);
                if (match_regex(optarg, "(.+)://(.+):(.+)", 2, result))
                    strcpy(local_host, result);
                if (match_regex(optarg, "(.+)://(.+):(.+)", 3, result))
                    strcpy(local_port, result);

                if (strcmp(local_protocol, "http") != 0) {
                    printf("%s is not supported local protocol, now trying http proxy\n", local_protocol);
                    sprintf(local_protocol, "http");
                }
                break;
            case 'p':
                if (match_regex(optarg, "(.+)://(.+):(.+)@(.+):(.+)", 0, NULL)) {
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
                } else if (match_regex(optarg, "(.+)://(.+):(.+)", 0, NULL)) {
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

    if (socket_init() == SOCKET_ERROR) return 1;

    if (ipv6_mode == 0)
        local = socket_create(AF_INET, SOCK_STREAM, 0);
    else
        local = socket_create(AF_INET6, SOCK_STREAM, 0);

    if (logger_flag && logger_file == NULL)
        logger_file = fopen("stat.log", "wb");

    loop = event_init(EVENT_BACKEND_SELECT);

    if (local != INVALID_SOCKET && loop != NULL) {
        set_socket(local);
        set_nodelay(local, 1);

        if (socket_bind(local, local_host, local_port) != SOCKET_ERROR) {
            if (socket_listen(local, MAX_CLIENTS) != SOCKET_ERROR) {
                printf("listen on tcp socket success, socket: %d, host: %s, port: %s\n", local, local_host, local_port);

                event_io_init(&local_accept, local_accept_cb, local, EVENT_IO_READ);
                event_io_start(loop, &local_accept);
            }
        }
    }

    event_run(loop, EVENT_RUN_DEFAULT);

    event_clean(loop);

    if (logger_flag && logger_file != NULL)
        fclose(logger_file);

    if (local != INVALID_SOCKET)
        socket_close(local);

    socket_clean();

    return 0;
}
