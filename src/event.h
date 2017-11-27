#ifndef _EVENT_H
#define _EVENT_H 1

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_IO_READ 1
#define EVENT_IO_WRITE 2
#define EVENT_IO_EXCEPT 3

typedef void (*EVENT_IO_CB)(int fd, const int event, void *data);

int event_io_add(int fd, const int event, void *data, EVENT_IO_CB cb);
int event_io_start(int fd, const int event);
int event_io_stop(int fd, const int event);
int event_io_remove(int fd, const int event);
int event_io_clean(void);

void event_loop(void);
void event_clean(void);

#ifdef __cplusplus
}
#endif

#endif
