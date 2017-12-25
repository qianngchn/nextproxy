#ifndef _ACTOR_H
#define _ACTOR_H 1

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define loop_delay(cnt) do { size_t count = (cnt); while (count) --count; } while (0)

typedef struct hash_elem {
    uint32_t volatile exist;
    uint32_t volatile hash1;
    uint32_t volatile hash2;
    void * volatile data;
    pthread_spinlock_t lock;
    uint64_t p1, p2, p3, p4;
} __attribute__ ((aligned(8))) HASH_ELEM;

typedef struct hash_table {
    uint32_t table[1280];
    HASH_ELEM *elems;
    size_t size;
} __attribute__ ((aligned(8))) HASH_TABLE;

enum {
    HTABLE_FIND,
    HTABLE_SET,
    HTABLE_CREATE,
    HTABLE_DELETE
};

HASH_TABLE *hash_init(size_t size);

uint32_t hash_string(const HASH_TABLE *htable, const char *str, uint32_t type);

void *hash_table(const HASH_TABLE *htable, const char *key, void *data, int action);

#define hash_find(htable, key) hash_table(htable, key, NULL, HTABLE_FIND)
#define hash_set(htable, key, data) hash_table(htable, key, data, HTABLE_SET)
#define hash_create(htable, key, data) hash_table(htable, key, data, HTABLE_CREATE)
#define hash_delete(htable, key) hash_table(htable, key, NULL, HTABLE_DELETE)

void hash_clean(HASH_TABLE *htable);

typedef struct ring_elem {
    void * volatile data;
    uint64_t p1, p2, p3, p4, p5, p6, p7;
} __attribute__ ((aligned(8))) RING_ELEM;

typedef struct ring_buffer {
    uint64_t size;
    RING_ELEM *elems;
    uint64_t volatile read;
    uint64_t volatile write;
    uint64_t volatile valid;
    uint64_t p1, p2, p3;
} __attribute__ ((aligned(8))) RING_BUFFER;

RING_BUFFER *buffer_init(uint64_t size);

int buffer_write(RING_BUFFER *buffer, void *data);

int buffer_read(RING_BUFFER *buffer, void **data);

static inline uint64_t buffer_size(RING_BUFFER *buffer) {
    return buffer->valid - buffer->read;
}

void buffer_clean(RING_BUFFER *buffer);

typedef struct actor_root ACTOR_ROOT;

typedef void (*ACTOR_CB)(struct actor_root *root, void *data);

typedef struct actor_node {
    int volatile status;
    ACTOR_CB volatile cb;
    RING_BUFFER *inbox;
    uint64_t p1, p2, p3, p4, p5;
} __attribute__ ((aligned(8))) ACTOR_NODE;

typedef struct actor_root {
    int volatile status;
    ACTOR_CB volatile cb;
    RING_BUFFER *inbox;
    uint64_t p1, p2, p3, p4, p5;

    int volatile breakout;
    size_t volatile nodecnt;

    HASH_TABLE *nodestable;
    ACTOR_NODE *nodes;

    size_t maxnode;
    size_t maxworker;
    size_t maxinbox;
    RING_BUFFER *task;

    pthread_t master;
    pthread_mutex_t masterlock;
    pthread_cond_t mastercond;

    pthread_t *workers;
    pthread_mutex_t workerlock;
    pthread_cond_t workercond;
} __attribute__ ((aligned(8))) ACTOR_ROOT;

enum {
    ACTOR_DEFAULT  = 0x01,
    ACTOR_RUNNABLE = 0x02,
    ACTOR_RUNTASK  = 0x04
};

enum {
    ACTORN_FIND,
    ACTORN_SET,
    ACTORN_CREATE,
    ACTORN_START,
    ACTORN_STOP,
    ACTORN_DELETE
};

enum {
    ACTORS_FIND,
    ACTORS_SET,
    ACTORS_CREATE,
    ACTORS_START,
    ACTORS_STOP,
    ACTORS_DELETE
};

#define ACTOR_ROOTNAME "root"
#define ACTOR_MAXNODE 1024
#define ACTOR_MAXWORKER 4
#define ACTOR_MAXINBOX 1024

ACTOR_ROOT *actor_init(const char *name, size_t maxnode, size_t maxworker, size_t maxinbox);

#define actor_default() actor_init(ACTOR_ROOTNAME, ACTOR_MAXNODE, ACTOR_MAXWORKER, ACTOR_MAXINBOX)

void actor_run(ACTOR_ROOT *root);

ACTOR_NODE *actorn_manage(ACTOR_ROOT *root, ACTOR_NODE *node, ACTOR_CB cb, int action);

#define actorn_find(root, node) actorn_manage(root, node, NULL, ACTORN_FIND)
#define actorn_set(root, node, cb) actorn_manage(root, node, cb, ACTORN_SET)
#define actorn_create(root, cb) actorn_manage(root, NULL, cb, ACTORN_CREATE)
#define actorn_start(root, node) actorn_manage(root, node, NULL, ACTORN_START)
#define actorn_stop(root, node) actorn_manage(root, node, NULL, ACTORN_STOP)
#define actorn_delete(root, node) actorn_manage(root, node, NULL, ACTORN_DELETE)

int actorn_send(ACTOR_ROOT *root, ACTOR_NODE *node, void *data);

int actors_manage(ACTOR_ROOT *root, const char *name, ACTOR_CB cb, int action);

#define actors_find(root, name) actors_manage(root, name, NULL, ACTORS_FIND)
#define actors_set(root, name, cb) actors_manage(root, name, cb, ACTORS_SET)
#define actors_create(root, name, cb) actors_manage(root, name, cb, ACTORS_CREATE)
#define actors_start(root, name) actors_manage(root, name, NULL, ACTORS_START)
#define actors_stop(root, name) actors_manage(root, name, NULL, ACTORS_STOP)
#define actors_delete(root, name) actors_manage(root, name, NULL, ACTORS_DELETE)

int actors_send(ACTOR_ROOT *root, const char *name, void *data);

int actor_broadcast(ACTOR_ROOT *root, void *data);

void actor_wait(ACTOR_ROOT *root);

void actor_break(ACTOR_ROOT *root);

void actor_clean(ACTOR_ROOT *root);

#ifdef __cplusplus
}
#endif

#endif
