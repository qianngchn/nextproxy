#ifndef _EVENT_H
#define _EVENT_H 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct event_loop EVENT_LOOP;

#define EVENT_CB(type) void (*cb)(struct event_loop *loop, struct type *watcher)

#define EVENT_WATCHER(type) int active; int pending; void *data; EVENT_CB(type)

typedef struct event_watcher {
    EVENT_WATCHER(event_watcher);
} EVENT_WATCHER;

#define event_watcher_init(ew, _cb) do { (ew)->active = 0; (ew)->pending = 0; (ew)->data = NULL; (ew)->cb = _cb; } while(0)

#define event_watcher_data(ew, _data) do { (ew)->data = _data; } while(0)

void event_watcher_feed(EVENT_LOOP *loop, EVENT_WATCHER *watcher);

enum {
    EVENT_IO_READ   = 0x01,
    EVENT_IO_WRITE  = 0x02,
    EVENT_IO_EXCEPT = 0x04,
};

typedef struct event_io {
    EVENT_WATCHER(event_io);
    struct event_io *next;

    int fd;
    int events;
} EVENT_IO;

#define event_io_set(ew, _fd, _events) do { (ew)->fd = _fd; (ew)->events = _events; } while(0)

#define event_io_init(ew, cb, fd, events) do { event_watcher_init((ew), (cb)); event_io_set((ew), (fd), (events)); (ew)->next = NULL; } while(0)

#define event_io_data(ew, data) do { event_watcher_data(ew, data); } while(0)

#define event_io_feed(loop, watcher) do { event_watcher_feed((loop), (EVENT_WATCHER *)(watcher)); } while(0)

void event_io_start(EVENT_LOOP *loop, EVENT_IO *watcher);

void event_io_stop(EVENT_LOOP *loop, EVENT_IO *watcher);

typedef struct event_timer {
    EVENT_WATCHER(event_timer);

    double timeout;
    double repeat;
} EVENT_TIMER;

#define event_timer_set(ew, _timeout, _repeat) do { (ew)->timeout = _timeout; (ew)->repeat = _repeat; } while(0)

#define event_timer_init(ew, cb, timeout, repeat) do { event_watcher_init((ew), (cb)); event_timer_set((ew), (timeout), (repeat)); } while(0)

#define event_timer_data(ew, data) do { event_watcher_data(ew, data); } while(0)

#define event_timer_feed(loop, watcher) do { event_watcher_feed((loop), (EVENT_WATCHER *)(watcher)); } while(0)

void event_timer_start(EVENT_LOOP *loop, EVENT_TIMER *watcher);

void event_timer_stop(EVENT_LOOP *loop, EVENT_TIMER *watcher);

enum {
    ANFD_CHANGE = 0x01,
    ANFD_FDSET  = 0x02
};

typedef struct anfd {
    EVENT_IO *head;
    int flags;
    int events;
} ANFD;

typedef struct anto {
    EVENT_TIMER *watcher;
    double at;
} ANTO;

typedef struct pending {
    EVENT_WATCHER *watcher;
} PENDING;

enum {
    EVENT_RUN_DEFAULT = 0x00,
    EVENT_RUN_ONCE    = 0x01,
    EVENT_RUN_NOWAIT  = 0x02
};

enum {
    EVENT_BREAK_NONE = 0,
    EVENT_BREAK_ONE  = 1,
    EVENT_BREAK_ALL  = 2
};

enum {
    EVENT_BACKEND_NONE   = 0x00,
    EVENT_BACKEND_SELECT = 0x01,
    EVENT_BACKEND_EPOLL  = 0x02
};

typedef struct event_loop {
    ANFD *anfds;
    int anfdmax;
    int fdvalid;

    int *fdchanges;
    int fdchangemax;
    int fdchangecnt;

    void *readfds;
    void *writefds;
    void *exceptfds;

    ANTO *antos;
    int antomax;
    int timecnt;

    double run_now;
    double hit_now;

    PENDING *pendings;
    int pendingmax;
    int pendingcnt;

    int backend;
    int (*backend_init)(EVENT_LOOP *loop);
    int (*backend_modify)(EVENT_LOOP *loop, int fd, int oevents, int nevents);
    int (*backend_poll)(EVENT_LOOP *loop, double timeout);
    void (*backend_clean)(EVENT_LOOP *loop);
;
    int activecnt;
    int breakflag;
} EVENT_LOOP;

#define event_default() event_init(EVENT_BACKEND_EPOLL | EVENT_BACKEND_SELECT | EVENT_BACKEND_NONE)

#define event_break(loop, how) do { (loop)->breakflag = how; } while(0)

EVENT_LOOP *event_init(int flags);

void event_run(EVENT_LOOP *loop, int flags);

void event_clean(EVENT_LOOP *loop);

#ifdef __cplusplus
}
#endif

#endif
