#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "event.h"

static inline double now_time(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define MALLOC_PAGE 4096

#define array_alloc(type, base, cur, cnt) do { if ((cnt) > (cur)) { int old = cur; (base) = (type *)array_realloc(sizeof(type), (base), &(cur), (cnt)); memset((base) + (old), 0, sizeof(type) * ((cur) - (old))); } } while(0)

#define array_free(base, max, cnt) do { free(base); (max) = 0; (cnt) = 0; (base) = NULL; } while(0)

static inline void *array_realloc(size_t size, void *base, int *cur, int cnt) {
    int nextcur = *cur + 1;

    do nextcur <<= 1; while (cnt > nextcur);

    if (size * nextcur > MALLOC_PAGE - sizeof(void *) * 4) {
        nextcur *= size;
        nextcur = (nextcur + size + sizeof(void *) * 4 + (MALLOC_PAGE - 1)) & ~(MALLOC_PAGE - 1);
        nextcur -= sizeof(void *) * 4;
        nextcur /= size;
    }

    *cur = nextcur;

    base = realloc(base, size * nextcur);

    if (!base && size) abort();

    return base;
}

#define LIST_TYPE EVENT_IO *

static inline void list_add(LIST_TYPE *head, LIST_TYPE node) {
    node->next = *head;
    *head = node;
}

static inline void list_remove(LIST_TYPE *head, LIST_TYPE node) {
    while (*head) {
        if (*head == node) {
            *head = node->next;
            break;
        }

        head = &(*head)->next;
    }

    node->next = NULL;
}

#define HEAP_TYPE ANTO
#define HEAP_ROOT 1
#define HEAP_PARENT(k) (((k) - 1 + HEAP_ROOT) >> 1)
#define HEAP_CHILD(k) ((((k) << 1) + 1) - HEAP_ROOT)

static inline void heap_up(HEAP_TYPE *heap, int k) {
    ANTO cur = heap[k];

    while (k >= HEAP_ROOT) {
        int p = HEAP_PARENT(k);

        if (p < HEAP_ROOT || heap[p].at <= cur.at) break;

        heap[k] = heap[p];
        heap[k].watcher->active = k;
        k = p;
    }

    heap[k] = cur;
    heap[k].watcher->active = k;
}

static inline void heap_down(HEAP_TYPE *heap, int n, int k) {
    ANTO cur = heap[k];

    while (k >= HEAP_ROOT) {
        int c = HEAP_CHILD(k);

        if (c >= n + HEAP_ROOT) break;

        c += c + 1 < n + HEAP_ROOT && heap[c].at > heap[c + 1].at ? 1 : 0;

        if (cur.at <= heap[c].at) break;

        heap[k] = heap[c];
        heap[k].watcher->active = k;
        k = c;
    }

    heap[k] = cur;
    heap[k].watcher->active = k;
}

static inline void heap_adjust(HEAP_TYPE *heap, int n, int k) {
    if (k > HEAP_ROOT && heap[k].at <= heap[HEAP_PARENT(k)].at)
        heap_up(heap, k);
    else
        heap_down(heap, n, k);
}

#define TIME_JUMP 1.0

static inline void timer_adjust(EVENT_LOOP *loop, double adjust) {
    int i = 0;

    for (i = 0; i < loop->timecnt; ++i)
        loop->antos[i].at += adjust;
}

static inline void timer_update(EVENT_LOOP *loop, double maxtime) {
    loop->run_now = now_time();

    if (loop->hit_now > loop->run_now || loop->run_now > loop->hit_now + maxtime + TIME_JUMP)
        timer_adjust(loop, loop->run_now - loop->hit_now);

    loop->hit_now = loop->run_now;
}

static inline void pending_add(EVENT_LOOP *loop, int fd, int events) {
    ANFD *anfd = loop->anfds + fd;

    if (!(anfd->flags & ANFD_CHANGE)) {
        EVENT_IO *watcher = NULL;

        for (watcher = anfd->head; watcher; watcher = watcher->next) {
            printf("add: %d\n", watcher->fd);
            if (watcher->events & events)
                event_watcher_feed(loop, (EVENT_WATCHER *)watcher);
        }
    }
}

static inline void pending_remove(EVENT_LOOP *loop, int pending) {
    if (pending) {
        loop->pendings[pending - 1] = loop->pendings[loop->pendingcnt - 1];
        --(loop->pendingcnt);
    }
}

static inline void pending_invoke(EVENT_LOOP *loop) {
    int i = 0;

    for (i = 0; i < loop->pendingcnt; ++i) {
        if (loop->pendings[i].watcher->pending) {
            loop->pendings[i].watcher->pending = 0;

            printf("invoke: %d\n", loop->pendingcnt);
            if (loop->pendings[i].watcher->cb != NULL)
                loop->pendings[i].watcher->cb(loop, loop->pendings[i].watcher);
        }
    }

    loop->pendingcnt = 0;
}

static void timer_reify(EVENT_LOOP *loop) {
    if (loop->timecnt && loop->antos[HEAP_ROOT].at < now_time()) {
        do {
            ANTO *anto = loop->antos + HEAP_ROOT;
            printf("p: %d %d\n", loop->antos[1].watcher->active, loop->antos[2].watcher->active);

            if (anto->watcher->repeat) {
                anto->at += anto->watcher->repeat;

                if (anto->at < now_time()) anto->at = now_time();

                heap_down(loop->antos, loop->timecnt, HEAP_ROOT);
            } else
                event_timer_stop(loop, anto->watcher);

            event_watcher_feed(loop, (EVENT_WATCHER *)anto->watcher);
        } while (loop->timecnt && loop->antos[HEAP_ROOT].at < now_time());
    }
}

static void fd_reify(EVENT_LOOP *loop) {
    int i = 0;

    for (i = 0; i < loop->fdchangecnt; ++i) {
        int fd = loop->fdchanges[i];
        ANFD *anfd = loop->anfds + fd;
        EVENT_IO *watcher = NULL;
        int events = 0;

        if (anfd->head == NULL) {
            if (loop->backend_modify(loop, fd, anfd->events, events)) {
                if (anfd->flags & ANFD_FDSET)
                    --(loop->fdvalid);

                anfd->flags ^= ANFD_FDSET;
                anfd->events = events;
            }
        } else {
            for (watcher = anfd->head; watcher; watcher = watcher->next)
                events |= watcher->events;

            if (loop->backend_modify(loop, fd, anfd->events, events)) {
                if (!(anfd->flags & ANFD_FDSET))
                    ++(loop->fdvalid);

                anfd->flags |= ANFD_FDSET;
                anfd->events = events;
            }
        }

        anfd->flags ^= ANFD_CHANGE;
    }

    loop->fdchangecnt = 0;
}

static int select_modify(EVENT_LOOP *loop, int fd, int oevents, int nevents) {
#if defined(__linux__) || defined(__unix__)
    if (fd >= FD_SETSIZE) return 0;
#else
    if (loop->fdvalid >= FD_SETSIZE) return 0;
#endif

    int events = oevents ^ nevents;

    if (events) {
        if (events & EVENT_IO_READ) {
            if (nevents & EVENT_IO_READ)
                FD_SET(fd, (fd_set *)(loop->readfds));
            else
                FD_CLR(fd, (fd_set *)(loop->readfds));
        }

        if (events & EVENT_IO_WRITE) {
            if (nevents & EVENT_IO_WRITE)
                FD_SET(fd, (fd_set *)(loop->writefds));
            else
                FD_CLR(fd, (fd_set *)(loop->writefds));
        }

        if (events & EVENT_IO_EXCEPT) {
            if (nevents & EVENT_IO_EXCEPT)
                FD_SET(fd, (fd_set *)(loop->exceptfds));
            else
                FD_CLR(fd, (fd_set *)(loop->exceptfds));
        }
    }

    return 1;
}

static int select_poll(EVENT_LOOP *loop, double timeout) {
    struct timeval tv = {(long)timeout, (long)((timeout - (long)timeout) * 1e6)};
    int fd = 0, nfds = loop->anfdmax < FD_SETSIZE ? loop->anfdmax : FD_SETSIZE;

    fd_set rfds = *(fd_set *)(loop->readfds);
    fd_set wfds = *(fd_set *)(loop->writefds);
    fd_set efds = *(fd_set *)(loop->exceptfds);

    if (select(nfds, &rfds, &wfds, &efds, &tv) >= 0) {
        for (fd = 0; fd < loop->anfdmax; ++fd) {
            if (loop->anfds[fd].events) {
                int events = 0;

                if (FD_ISSET(fd, &rfds)) events |= EVENT_IO_READ;
                if (FD_ISSET(fd, &wfds)) events |= EVENT_IO_WRITE;
                if (FD_ISSET(fd, &efds)) events |= EVENT_IO_EXCEPT;

                printf("poll: %d %d\n", fd, events);
                if (events) pending_add(loop, fd, events);
            }
        }

        return 1;
    } else {
        for (fd = 0; fd < loop->anfdmax; ++fd) {
            if (loop->anfds[fd].events) {
                EVENT_IO *watcher = NULL;

                for (watcher = loop->anfds[fd].head; watcher; watcher = watcher->next)
                    event_io_stop(loop, watcher);

                break;
            }
        }

        return 0;
    }
}

static inline int select_init(EVENT_LOOP *loop) {
    loop->readfds = malloc(sizeof(fd_set));
    loop->writefds = malloc(sizeof(fd_set));
    loop->exceptfds = malloc(sizeof(fd_set));

    if (loop->readfds == NULL || loop->writefds == NULL || loop->exceptfds == NULL)
        return 0;

    FD_ZERO((fd_set *)(loop->readfds));
    FD_ZERO((fd_set *)(loop->writefds));
    FD_ZERO((fd_set *)(loop->exceptfds));

    return 1;
}

static inline void select_clean(EVENT_LOOP *loop) {
    free(loop->readfds);
    free(loop->writefds);
    free(loop->exceptfds);

    loop->readfds = NULL;
    loop->writefds = NULL;
    loop->exceptfds = NULL;
}

static int epoll_modify(EVENT_LOOP *loop, int fd, int oevents, int nevents) {
    return 1;
}

static int epoll_poll(EVENT_LOOP *loop, double timeout) {
    return 1;
}

static inline int epoll_init(EVENT_LOOP *loop) {
    return 1;
}

static inline void epoll_clean(EVENT_LOOP *loop) {
}

EVENT_LOOP *event_init(int flags) {
    if (flags & ~(EVENT_BACKEND_EPOLL | EVENT_BACKEND_SELECT | EVENT_BACKEND_NONE))
        flags = EVENT_BACKEND_NONE;

    EVENT_LOOP *loop = (EVENT_LOOP *)malloc(sizeof(EVENT_LOOP));
    if (loop == NULL) return NULL;
    memset(loop, 0, sizeof(EVENT_LOOP));

    if (flags & EVENT_BACKEND_EPOLL) {
        loop->backend = EVENT_BACKEND_EPOLL;
        loop->backend_init = epoll_init;
        loop->backend_modify = epoll_modify;
        loop->backend_poll = epoll_poll;
        loop->backend_clean = epoll_clean;
    } else if (flags & EVENT_BACKEND_SELECT) {
        loop->backend = EVENT_BACKEND_SELECT;
        loop->backend_init = select_init;
        loop->backend_modify = select_modify;
        loop->backend_poll = select_poll;
        loop->backend_clean = select_clean;
    } else {
        loop->backend = EVENT_BACKEND_NONE;
        loop->backend_init = NULL;
        loop->backend_modify = NULL;
        loop->backend_poll = NULL;
        loop->backend_clean = NULL;
    }

    loop->anfds = NULL;
    loop->anfdmax = 0;
    loop->fdvalid = 0;
    loop->fdchanges = NULL;
    loop->fdchangemax = 0;
    loop->fdchangecnt = 0;
    loop->antos = NULL;
    loop->antomax = 0;
    loop->timecnt = 0;
    loop->run_now = now_time();
    loop->hit_now = loop->run_now;
    loop->pendings = NULL;
    loop->pendingmax = 0;
    loop->pendingcnt = 0;

    if (loop->backend != EVENT_BACKEND_NONE && !loop->backend_init(loop))
        return NULL;

    loop->activecnt = 0;
    loop->breakflag = EVENT_BREAK_NONE;

    return loop;
}

#define MAX_TIME 1.0e10
#define MIN_TIME 1.0e-3
#define BLOCK_TIME 60.0

void event_run(EVENT_LOOP *loop, int flags) {
    if (loop == NULL) return;
    if (flags & ~(EVENT_RUN_ONCE | EVENT_RUN_NOWAIT | EVENT_RUN_DEFAULT))
        flags = EVENT_RUN_DEFAULT;

    loop->breakflag = EVENT_BREAK_NONE;
    pending_invoke(loop);

    do {
        fd_reify(loop);

        double waittime = 0.0;

        if (!(flags & EVENT_RUN_NOWAIT || !loop->activecnt)) {
            waittime = BLOCK_TIME;

            if (loop->timecnt) {
                double lefttime = loop->antos[HEAP_ROOT].at - now_time();
                if (waittime > lefttime) waittime = lefttime;
            }

            if (waittime < MIN_TIME) waittime = MIN_TIME;
        }

        timer_update(loop, MAX_TIME);

        if (!loop->backend_poll(loop, waittime))
            sleep(waittime);

        timer_update(loop, waittime);

        timer_reify(loop);

        pending_invoke(loop);

        if (loop->breakflag & ~(EVENT_BREAK_ONE | EVENT_BREAK_ALL))
            loop->breakflag = EVENT_BREAK_NONE;
    } while (loop->activecnt && !loop->breakflag && !(flags & (EVENT_RUN_ONCE | EVENT_RUN_NOWAIT)));

    if (loop->breakflag == EVENT_BREAK_ONE)
        loop->breakflag = EVENT_BREAK_NONE;
}

void event_clean(EVENT_LOOP *loop) {
    if (loop == NULL) return;

    if (loop->backend != EVENT_BACKEND_NONE)
        loop->backend_clean(loop);

    array_free(loop->anfds, loop->anfdmax, loop->anfdmax);
    array_free(loop->fdchanges, loop->fdchangemax, loop->fdchangecnt);
    array_free(loop->antos, loop->antomax, loop->timecnt);
    array_free(loop->pendings, loop->pendingmax, loop->pendingcnt);

    free(loop);
}

void event_watcher_feed(EVENT_LOOP *loop, EVENT_WATCHER *watcher) {
    if (loop == NULL || watcher == NULL) return;

    printf("feed: %d %d\n", watcher->pending, loop->pendingcnt);
    if (!watcher->pending) {
        watcher->pending = ++(loop->pendingcnt);
    printf("feed: %d %d\n", watcher->pending, loop->pendingcnt);
        array_alloc(PENDING, loop->pendings, loop->pendingmax, loop->pendingcnt);
        loop->pendings[loop->pendingcnt - 1].watcher = watcher;
    }
}

void event_io_start(EVENT_LOOP *loop, EVENT_IO *watcher) {
    if (loop == NULL || watcher == NULL) return;
    if (watcher->active) return;
    if (watcher->fd < 0) return;
    if (watcher->events <= 0 || watcher->events & ~(EVENT_IO_READ | EVENT_IO_WRITE | EVENT_IO_EXCEPT)) return;

    watcher->active = 1;

    array_alloc(ANFD, loop->anfds, loop->anfdmax, watcher->fd + 1);
    ANFD *anfd = loop->anfds + watcher->fd;
    list_add(&anfd->head, watcher);

    if (!(anfd->flags & ANFD_CHANGE)) {
        ++(loop->fdchangecnt);
        array_alloc(int, loop->fdchanges, loop->fdchangemax, loop->fdchangecnt);
        loop->fdchanges[loop->fdchangecnt - 1] = watcher->fd;

        anfd->flags |= ANFD_CHANGE;
    }

    ++(loop->activecnt);
}

void event_io_stop(EVENT_LOOP *loop, EVENT_IO *watcher) {
    if (loop == NULL || watcher == NULL) return;
    pending_remove(loop, watcher->pending);
    watcher->pending = 0;
    if (!watcher->active) return;
    if (watcher->fd < 0) return;
    if (watcher->events <= 0 || watcher->events & ~(EVENT_IO_READ | EVENT_IO_WRITE | EVENT_IO_EXCEPT)) return;

    watcher->active = 0;
    --(loop->activecnt);

    ANFD *anfd = loop->anfds + watcher->fd;
    list_remove(&anfd->head, watcher);

    if (!(anfd->flags & ANFD_CHANGE)) {
        ++(loop->fdchangecnt);
        array_alloc(int, loop->fdchanges, loop->fdchangemax, loop->fdchangecnt);
        loop->fdchanges[loop->fdchangecnt - 1] = watcher->fd;

        anfd->flags |= ANFD_CHANGE;
    }
}

void event_timer_start(EVENT_LOOP *loop, EVENT_TIMER *watcher) {
    if (loop == NULL || watcher == NULL) return;
    if (watcher->active > 0) return;
    if (watcher->timeout < 0 || watcher->repeat < 0 ) return;

    watcher->active = ++(loop->timecnt);

    array_alloc(ANTO, loop->antos, loop->antomax, watcher->active + 1);
    loop->antos[watcher->active].watcher = watcher;
    loop->antos[watcher->active].at = now_time() + watcher->timeout;
    heap_up(loop->antos, watcher->active);

    ++(loop->activecnt);
}

void event_timer_stop(EVENT_LOOP *loop, EVENT_TIMER *watcher) {
    if (loop == NULL || watcher == NULL) return;
    printf("stop: %d %d\n", watcher->pending, loop->pendingcnt);
    pending_remove(loop, watcher->pending);
    watcher->pending = 0;
    printf("stop: %d %d\n", watcher->pending, loop->pendingcnt);
    if (watcher->active <= 0) return;
    if (watcher->timeout < 0 || watcher->repeat < 0 ) return;

    --(loop->timecnt);
    if (watcher->active < loop->timecnt + HEAP_ROOT) {
        loop->antos[watcher->active] = loop->antos[loop->timecnt + HEAP_ROOT];
        loop->antos[watcher->active].watcher->active = watcher->active;
        //printf("timecnt + root: %d %d\n", loop->antos[loop->timecnt + HEAP_ROOT].watcher->active, loop->antos[watcher->active].watcher->active);
        heap_adjust(loop->antos, loop->timecnt, watcher->active);
    }
    loop->antos[watcher->active].at = 0;

    printf("stop: %d %d\n", watcher->pending, loop->pendingcnt);
    watcher->active = 0;
    --(loop->activecnt);
}
