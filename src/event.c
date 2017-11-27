#include <stdlib.h>
#include "event.h"

#if defined(__linux) || defined(__linux__) || defined(linux)
#include <sys/socket.h>
#else
#include <winsock2.h>
#endif

typedef struct event_io{
    int fd;
    int event;
    void *data;
    EVENT_IO_CB cb;
    int active;
    struct event_io *next;
} EVENT_IO;

static EVENT_IO *global_event_io = NULL;
static fd_set global_event_io_read_fds;
static fd_set global_event_io_write_fds;
static fd_set global_event_io_except_fds;
static int global_event_io_clean = 1;

static int global_event_clean = 1;

static void event_io_loop(void) {
    EVENT_IO *cur = global_event_io;
    if(cur == NULL) return;
    fd_set rfds = global_event_io_read_fds;
    fd_set wfds = global_event_io_write_fds;
    fd_set efds = global_event_io_except_fds;
    int maxfd = 0;
    while(cur->next != NULL) {
        if(FD_ISSET(cur->fd, &rfds) || FD_ISSET(cur->fd, &wfds) || FD_ISSET(cur->fd, &efds))
            if(cur->fd > maxfd) maxfd = cur->fd;
        cur = cur->next;
    }
    if(select(maxfd + 1, &rfds, &wfds, &efds, NULL) > 0) {
        cur = global_event_io;
        while(cur->fd <= maxfd && cur->next != NULL) {
            switch(cur->event) {
                case EVENT_IO_READ:
                    if(FD_ISSET(cur->fd, &rfds) && FD_ISSET(cur->fd, &global_event_io_read_fds) && cur->active == 1)
                        if(cur->cb != NULL) cur->cb(cur->fd, cur->event, cur->data);
                    break;
                case EVENT_IO_WRITE:
                    if(FD_ISSET(cur->fd, &wfds) && FD_ISSET(cur->fd, &global_event_io_write_fds) && cur->active == 1)
                        if(cur->cb != NULL) cur->cb(cur->fd, cur->event, cur->data);
                    break;
                case EVENT_IO_EXCEPT:
                    if(FD_ISSET(cur->fd, &efds) && FD_ISSET(cur->fd, &global_event_io_except_fds) && cur->active == 1)
                        if(cur->cb != NULL) cur->cb(cur->fd, cur->event, cur->data);
                    break;
            }
            if(global_event_clean || global_event_io_clean)
                break;
            else
                cur = cur->next;
        }
    } else
        global_event_io_clean = 1;

    if(global_event_clean || global_event_io_clean) {
        cur = global_event_io;
        while(cur != NULL) {
            EVENT_IO *temp = cur;
            cur = cur->next;
            free(temp);
            temp = NULL;
        }
    }

    cur = global_event_io;
    EVENT_IO *prev = global_event_io;
    while(cur->next != NULL) {
        if(cur->active == 0) {
            EVENT_IO *temp = cur;
            if(prev == cur) {
                prev = prev->next;
                cur = cur->next;
            } else {
                prev->next = cur->next;
                cur = cur->next;
            }
            switch(temp->event) {
                case EVENT_IO_READ:
                    FD_CLR(temp->fd, &global_event_io_read_fds);
                    break;
                case EVENT_IO_WRITE:
                    FD_CLR(temp->fd, &global_event_io_write_fds);
                    break;
                case EVENT_IO_EXCEPT:
                    FD_CLR(temp->fd, &global_event_io_except_fds);
                    break;
            }
            free(temp);
            temp = NULL;
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
}

int event_io_add(int fd, const int event, void *data, EVENT_IO_CB cb) {
    EVENT_IO *cur = global_event_io;
    if(cur == NULL) {
        global_event_clean = 0;
        global_event_io_clean = 0;
        FD_ZERO(&global_event_io_read_fds);
        FD_ZERO(&global_event_io_write_fds);
        FD_ZERO(&global_event_io_except_fds);
        cur = (EVENT_IO *)malloc(sizeof(EVENT_IO));
        if(cur == NULL) return 1;
        cur->fd = 0;
        cur->event = 0;
        cur->data = NULL;
        cur->cb = NULL;
        cur->active = 0;
        cur->next = NULL;
        global_event_io = cur;
        event_io_add(fd, event, data, cb);
    }
    while(cur->next != NULL) {
        if(cur->fd == fd && cur->event == event) return 0;
        cur = cur->next;
    }
    cur->fd = fd;
    cur->event = event;
    cur->data = data;
    cur->cb = cb;
    cur->active = 1;
    EVENT_IO *temp = (EVENT_IO *)malloc(sizeof(EVENT_IO));
    if(temp == NULL) return 1;
    temp->fd = 0;
    temp->event = 0;
    temp->data = NULL;
    temp->cb = NULL;
    temp->active = 0;
    temp->next = NULL;
    cur->next = temp;
    return 0;
}

int event_io_start(int fd, const int event) {
    EVENT_IO *cur = global_event_io;
    if(cur == NULL) return 1;
    while(cur->next != NULL) {
        if(cur->fd == fd && cur->event == event) {
            switch(cur->event) {
                case EVENT_IO_READ:
                    FD_SET(cur->fd, &global_event_io_read_fds);
                    break;
                case EVENT_IO_WRITE:
                    FD_SET(cur->fd, &global_event_io_write_fds);
                    break;
                case EVENT_IO_EXCEPT:
                    FD_SET(cur->fd, &global_event_io_except_fds);
                    break;
            }
            break;
        }
        cur = cur->next;
    }
    return 0;
}

int event_io_stop(int fd, const int event) {
    EVENT_IO *cur = global_event_io;
    if(cur == NULL) return 1;
    while(cur->next != NULL) {
        if(cur->fd == fd && cur->event == event) {
            switch(cur->event) {
                case EVENT_IO_READ:
                    FD_CLR(cur->fd, &global_event_io_read_fds);
                    break;
                case EVENT_IO_WRITE:
                    FD_CLR(cur->fd, &global_event_io_write_fds);
                    break;
                case EVENT_IO_EXCEPT:
                    FD_CLR(cur->fd, &global_event_io_except_fds);
                    break;
            }
            break;
        }
        cur = cur->next;
    }
    return 0;
}

int event_io_remove(int fd, const int event) {
    EVENT_IO *cur = global_event_io;
    if(cur == NULL) return 1;
    while(cur->next != NULL) {
        if(cur->fd == fd && cur->event == event) {
            cur->active = 0;
            break;
        }
        cur = cur->next;
    }
    return 0;
}

int event_io_clean(void) {
    global_event_io_clean = 1;
    return 0;
}

void event_loop(void) {
    while(1) {
        if(global_event_clean == 0 && global_event_io_clean == 0)
            event_io_loop();
        if(global_event_clean) break;
    }
}

void event_clean(void) {
    global_event_clean = 1;
}
