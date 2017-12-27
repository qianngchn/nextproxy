#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include "actor.h"

// Simple wrapper function
#define bool_cas(ptr, old, new) __sync_bool_compare_and_swap(ptr, old, new)

#define sync_value(type, ptr, value) do {\
    type old; type new;\
    do {\
        old = (ptr);\
        new = (value);\
    } while (!bool_cas(&(ptr), old, new));\
} while (0)

#define sync_add(type, ptr, value) do {\
    type old; type new;\
    do {\
        old = (ptr);\
        new = old + (value);\
    } while (!bool_cas(&(ptr), old, new));\
} while (0)

#define sync_sub(type, ptr, value) do {\
    type old; type new;\
    do {\
        old = (ptr);\
        new = old - (value);\
    } while (!bool_cas(&(ptr), old, new));\
} while (0)

#define sync_or(type, ptr, value) do {\
    type old; type new;\
    do {\
        old = (ptr);\
        new = old | (value);\
    } while (!bool_cas(&(ptr), old, new));\
} while (0)

#define sync_xor(type, ptr, value) do {\
    type old; type new;\
    do {\
        old = (ptr);\
        new = old ^ (value);\
    } while (!bool_cas(&(ptr), old, new));\
} while (0)

static inline void *anmalloc(size_t size) {
    void *ptr = malloc(size);

    if (!ptr && size) abort();

    return ptr;
}

static inline void anfree(void *ptr) {
    free(ptr);
}

static inline int thread_alive(pthread_t thread) {
    if (thread && pthread_kill(thread, 0) == 0)
        return 1;
    else
        return 0;
}

// Hash table management
HASH_TABLE *hash_init(uint64_t size) {
    HASH_TABLE *htable = (HASH_TABLE *)anmalloc(sizeof(HASH_TABLE));

    uint32_t seed = 0x00100001;
    uint32_t i = 0, j = 0, k = 0;

    for (i = 0; i < 256; ++i) {
        for (j = i, k = 0; k < 5; j += 256, ++k) {
            htable->table[j] = 0;
            seed = (seed * 125 + 3) % 0x2AAAAB;
            htable->table[j] |= (seed & 0xFFFF) << 16;
            seed = (seed * 125 + 3) % 0x2AAAAB;
            htable->table[j] |= seed & 0xFFFF;
        }
    }

    while (size > (2 << i)) ++i; size = 2 << i;

    htable->elems = (HASH_ELEM *)anmalloc(sizeof(HASH_ELEM) * size);
    memset(htable->elems, 0, sizeof(HASH_ELEM) * size);
    for (i = 0; i < size; ++i) pthread_spin_init(&htable->elems[i].lock, PTHREAD_PROCESS_PRIVATE);
    htable->size = size;

    return htable;
}

uint32_t hash_string(const HASH_TABLE *htable, const char *str, uint32_t type) {
    uint8_t *key = (uint8_t *)str;
    uint32_t hash = 0x7FED7FED;
    uint32_t mask = 0xEEEEEEEE;

    while (*key != 0) {
        hash = htable->table[(type << 8) + (*key)] ^ (hash + mask);
        mask = (*key) + hash + mask + (mask << 5) + 3;
        ++key;
    }

    return hash;
}

void *hash_table(const HASH_TABLE *htable, const char *key, void *data, int action) {
    uint32_t hash = hash_string(htable, key, 0);
    uint32_t hash1 = hash_string(htable, key, 1);
    uint32_t hash2 = hash_string(htable, key, 2);

    uint64_t cur = hash & (htable->size - 1);
    uint64_t pos = cur, max = htable->size, exist = 0;

    while (1) {
        pthread_spin_lock(&htable->elems[pos].lock);
        if (htable->elems[pos].exist) {
            if (htable->elems[pos].hash1 == hash1 && htable->elems[pos].hash2 == hash2) {
                exist = 1; break;
            } else {
                pthread_spin_unlock(&htable->elems[pos].lock);
                pos = (pos + 1) & (htable->size - 1);
            }

            if (pos == cur) {
                pos = max; break;
            }
        } else
            break;
    }

    if (action == HTABLE_FIND) {
        if (pos < max && exist) {
            data = htable->elems[pos].data;
            pthread_spin_unlock(&htable->elems[pos].lock);

            return data;
        }
    } else if (action == HTABLE_SET) {
        if (pos < max && exist) {
            htable->elems[pos].data = data;
            pthread_spin_unlock(&htable->elems[pos].lock);

            return data;
        }
    } else if (action == HTABLE_CREATE) {
        if (pos < max && !exist) {
            htable->elems[pos].exist = 1;
            htable->elems[pos].hash1 = hash1;
            htable->elems[pos].hash2 = hash2;
            htable->elems[pos].data = data;
            pthread_spin_unlock(&htable->elems[pos].lock);

            return data;
        }
    } else if (action == HTABLE_DELETE) {
        if (pos < max && exist) {
            data = htable->elems[pos].data;
            htable->elems[pos].exist = 0;
            htable->elems[pos].hash1 = 0;
            htable->elems[pos].hash2 = 0;
            htable->elems[pos].data = NULL;
            pthread_spin_unlock(&htable->elems[pos].lock);

            return data;
        }
    }

    if (pos < max) pthread_spin_unlock(&htable->elems[pos].lock);

    return NULL;
}

void hash_clean(HASH_TABLE *htable) {
    int i = 0; for (i = 0; i < htable->size; ++i) pthread_spin_destroy(&htable->elems[i].lock);
    anfree(htable->elems);
    anfree(htable);
}

// Ring buffer function
RING_BUFFER *buffer_init(uint64_t size) {
    RING_BUFFER *buffer = (RING_BUFFER *)anmalloc(sizeof(RING_BUFFER));

    int i = 0; while (size > (2 << i)) ++i; size = 2 << i;

    buffer->size = size;
    buffer->elems = (RING_ELEM *)anmalloc(sizeof(RING_ELEM) * size);
    memset(buffer->elems, 0, sizeof(RING_ELEM) * size);
    buffer->read = 0;
    buffer->write = 0;
    buffer->valid = 0;

    return buffer;
}

int buffer_write(RING_BUFFER *buffer, void *data) {
    uint64_t old = 0, new = 0, idx = 0;

    do {
        old = buffer->write;
        new = old + 1;
    } while (!bool_cas(&buffer->write, old, new));

    idx = old & (buffer->size - 1);

    if (new < buffer->read + buffer->size) {
        buffer->elems[idx].data = data;
        sync_add(uint64_t, buffer->valid, 1);
        return 1;
    } else {
        sync_sub(uint64_t, buffer->write, 1);
        return 0;
    }
}

int buffer_read(RING_BUFFER *buffer, void **data) {
    uint64_t old = 0, new = 0, idx = 0;

    do {
        old = buffer->read;
        new = old + 1;
    } while (!bool_cas(&buffer->read, old, new));

    idx = old & (buffer->size - 1);

    if (new <= buffer->valid) {
        if (data != NULL) *data = buffer->elems[idx].data;
        return 1;
    } else {
        sync_sub(uint64_t, buffer->read, 1);
        return 0;
    }
}

void buffer_clean(RING_BUFFER *buffer) {
    anfree(buffer->elems);
    anfree(buffer);
}

// Actor model section
static inline void root_callback(ACTOR_ROOT *root, void *data) { actor_broadcast(root, data); }

static inline int send_mail(ACTOR_ROOT *root, ACTOR_NODE *node, void *data) {
    uint64_t old = node->status, new = 0;

    if (!(old & ACTOR_DEFAULT && old & ACTOR_RUNNABLE)) return 0;

    if (buffer_size(node->inbox) >= root->maxinbox - 1) return 0;

    while (!buffer_write(node->inbox, data));

    if (!(old & ACTOR_RUNTASK)) {
        new = old | ACTOR_RUNTASK;

        if (bool_cas(&node->status, old, new) && buffer_size(root->task) < root->maxnode)
            while (!buffer_write(root->task, node));
    }

    return 1;
}

ACTOR_ROOT *actor_init(const char *name, size_t maxnode, size_t maxworker, size_t maxinbox) {
    if (name == NULL || maxnode == 0 || maxworker == 0 || maxinbox == 0) return NULL;

    ACTOR_ROOT *root = (ACTOR_ROOT *)anmalloc(sizeof(ACTOR_ROOT));
    memset(root, 0 , sizeof(ACTOR_ROOT));

    root->nodestable = hash_init(maxnode + 1);
    if (hash_table(root->nodestable, name, root, HTABLE_CREATE) == NULL) {
        hash_clean(root->nodestable);
        anfree(root);
        return NULL;
    }

    root->status = ACTOR_DEFAULT | ACTOR_RUNNABLE;
    root->cb = root_callback;
    root->inbox = buffer_init(maxinbox);

    root->breakout = 0;

    root->nodes = (ACTOR_NODE *)anmalloc(sizeof(ACTOR_NODE) * maxnode);
    memset(root->nodes, 0, sizeof(ACTOR_NODE) * maxnode);

    root->maxnode = maxnode;
    root->maxworker = maxworker;
    root->maxinbox = maxinbox;
    root->task = buffer_init(maxnode + 1);

    root->master = 0;
    pthread_mutex_init(&root->masterlock, NULL);
    pthread_cond_init(&root->mastercond, NULL);

    root->workers = (pthread_t *)anmalloc(sizeof(pthread_t) * maxworker);
    memset(root->workers, 0, sizeof(pthread_t) * maxworker);
    pthread_mutex_init(&root->workerlock, NULL);
    pthread_cond_init(&root->workercond, NULL);

    return root;
}

static void *thread_worker(void *data) {
    ACTOR_ROOT *root = (ACTOR_ROOT *)data;
    ACTOR_NODE *node = NULL; uint64_t status = 0;

    while (!root->breakout) {
        if (buffer_size(root->task) == 0) {
            pthread_mutex_lock(&root->masterlock);
            pthread_cond_signal(&root->mastercond);
            pthread_mutex_unlock(&root->masterlock);

            pthread_mutex_lock(&root->workerlock);
            pthread_cond_wait(&root->workercond, &root->workerlock);
            pthread_mutex_unlock(&root->workerlock);
            continue;
        }

        if (buffer_read(root->task, &data) && data != NULL) {
            node = (ACTOR_NODE *)data; status = node->status;

            if (status & ACTOR_RUNNABLE && status & ACTOR_RUNTASK) {
                while (buffer_read(node->inbox, &data)) {
                    if (node->cb != NULL) { node->cb(root, data); loop_delay(100); }
                }
            }

            sync_xor(uint64_t, node->status, ACTOR_RUNTASK);
        }
    }

    pthread_exit(NULL);
}

static void *thread_master(void *data) {
    size_t i = 0, retry = 10; uint64_t size = 0;
    ACTOR_ROOT *root = (ACTOR_ROOT *)data;

    while (!root->breakout) {
        //loop_delay(100);
        size = buffer_size(root->task);

        if (size == 0) {
            //if (retry == 0) {
                pthread_mutex_lock(&root->masterlock);
                pthread_cond_wait(&root->mastercond, &root->masterlock);
                pthread_mutex_unlock(&root->masterlock);
            //} else
                --retry;

            continue;
        } else {
            retry = 10;

            if (size > root->maxworker)
                size = root->maxworker;
        }

        for (i = 0; i < size; ++i) {
            if (!thread_alive(root->workers[i])) {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

                if (pthread_create(root->workers + i, &attr, thread_worker, root) != 0) {
                    root->breakout = 1; break;
                }

                pthread_attr_destroy(&attr);
            } else {
                pthread_mutex_lock(&root->workerlock);
                pthread_cond_signal(&root->workercond);
                pthread_mutex_unlock(&root->workerlock);
            }
        }
    }

    pthread_mutex_lock(&root->workerlock);
    pthread_cond_broadcast(&root->workercond);
    pthread_mutex_unlock(&root->workerlock);

    for (i = 0; i < size; ++i)
        if (thread_alive(root->workers[i]))
            pthread_join(root->workers[i], NULL);

    pthread_exit(NULL);
}

void actor_run(ACTOR_ROOT *root) {
    if (root == NULL) return;

    if (thread_alive(root->master)) return;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&root->master, &attr, thread_master, root) != 0)
        root->breakout = 1;
    else
        root->breakout = 0;

    pthread_attr_destroy(&attr);
}

ACTOR_NODE *actorn_manage(ACTOR_ROOT *root, ACTOR_NODE *node, ACTOR_CB cb, int action) {
    size_t i = 0; if (root == NULL) return NULL;

    if (action == ACTORN_FIND) {
        for (i = 0; i < root->maxnode; ++i)
            if (root->nodes + i == node)
                break;

        if (i == root->maxnode) return NULL;
    } else if (action == ACTORN_SET) {
        if (node == NULL || node->status & ACTOR_RUNNABLE) return NULL;

        sync_value(ACTOR_CB, node->cb, cb);
    } else if (action == ACTORN_CREATE) {
        for (i = 0; i < root->maxnode; ++i) {
            if (root->nodes + i == node) return NULL;

            if (root->nodes[i].status == 0) break;
        }

        if (i == root->maxnode) return NULL;

        node = &root->nodes[i];

        sync_value(uint64_t, node->status, ACTOR_DEFAULT);
        sync_value(ACTOR_CB, node->cb, cb);
        node->inbox = buffer_init(root->maxinbox);
    } else if (action == ACTORN_START) {
        if (node == NULL || node->status & ACTOR_RUNNABLE) return node;

        sync_or(uint64_t, node->status, ACTOR_RUNNABLE);
    } else if (action == ACTORN_STOP) {
        if (node == NULL || !(node->status & ACTOR_RUNNABLE)) return node;

        sync_xor(uint64_t, node->status, ACTOR_RUNNABLE);
    } else if (action == ACTORN_DELETE) {
        if (node == NULL || node->status != ACTOR_DEFAULT) return NULL;

        sync_value(uint64_t, node->status, 0);
        sync_value(ACTOR_CB, node->cb, NULL);
        buffer_clean(node->inbox);
    } else
        return NULL;

    return node;
}

int actorn_send(ACTOR_ROOT *root, ACTOR_NODE *node, void *data) {
    if (root == NULL || node == NULL) return 0;

    if (!send_mail(root, node, data)) return 0;

    pthread_mutex_lock(&root->masterlock);
    pthread_cond_signal(&root->mastercond);
    pthread_mutex_unlock(&root->masterlock);

    return 1;
}

int actors_manage(ACTOR_ROOT *root, const char *name, ACTOR_CB cb, int action) {
    if (root == NULL || name == NULL) return 0;

    size_t i = 0; ACTOR_NODE *node = NULL;

    if (action == ACTORS_FIND) {
        node = hash_table(root->nodestable, name, NULL, HTABLE_FIND);

        if (node == NULL) return 0;
    } else if (action == ACTORS_SET) {
        node = hash_table(root->nodestable, name, NULL, HTABLE_FIND);

        if (node == NULL) return 0;

        if (node->status & ACTOR_RUNNABLE) return 0;

        sync_value(ACTOR_CB, node->cb, cb);
    } else if (action == ACTORS_CREATE) {
        for (i = 0; i < root->maxnode; ++i)
            if (root->nodes[i].status == 0)
                break;

        if (i == root->maxnode) return 0;

        node = &root->nodes[i];

        if (hash_table(root->nodestable, name, node, HTABLE_CREATE) == NULL) return 0;

        sync_value(uint64_t, node->status, ACTOR_DEFAULT);
        sync_value(ACTOR_CB, node->cb, cb);
        node->inbox = buffer_init(root->maxinbox);
    } else if (action == ACTORS_START) {
        node = hash_table(root->nodestable, name, NULL, HTABLE_FIND);

        if (node == NULL) return 0;

        if (node->status & ACTOR_RUNNABLE) return 1;

        sync_or(uint64_t, node->status, ACTOR_RUNNABLE);
    } else if (action == ACTORS_STOP) {
        node = hash_table(root->nodestable, name, NULL, HTABLE_FIND);

        if (node == NULL) return 0;

        if (!(node->status & ACTOR_RUNNABLE)) return 1;

        sync_xor(uint64_t, node->status, ACTOR_RUNNABLE);
    } else if (action == ACTORS_DELETE) {
        node = hash_table(root->nodestable, name, NULL, HTABLE_DELETE);

        if (node == NULL) return 0;

        if (node->status != ACTOR_DEFAULT) return 0;

        sync_value(uint64_t, node->status, 0);
        sync_value(ACTOR_CB, node->cb, NULL);
        buffer_clean(node->inbox);
    } else
        return 0;

    return 1;
}

int actors_send(ACTOR_ROOT *root, const char *name, void *data) {
    if (root == NULL || name == NULL) return 0;

    ACTOR_NODE *node = hash_table(root->nodestable, name, NULL, HTABLE_FIND);

    if (node == NULL) return 0;

    if (!send_mail(root, node, data)) return 0;

    pthread_mutex_lock(&root->masterlock);
    pthread_cond_signal(&root->mastercond);
    pthread_mutex_unlock(&root->masterlock);

    return 1;
}

int actor_broadcast(ACTOR_ROOT *root, void *data) {
    uint64_t i = 0; if (root == NULL) return 0;

    for (i = 0; i < root->maxnode; ++i)
        if (!send_mail(root, &root->nodes[i], data)) return 0;

    pthread_mutex_lock(&root->masterlock);
    pthread_cond_signal(&root->mastercond);
    pthread_mutex_unlock(&root->masterlock);

    return 1;
}

void actor_wait(ACTOR_ROOT *root) {
    if (root == NULL) return;

    if (thread_alive(root->master))
        pthread_join(root->master, NULL);
}

void actor_break(ACTOR_ROOT *root) {
    if (root == NULL) return;

    if (thread_alive(root->master)) {
        root->breakout = 1;

        pthread_mutex_lock(&root->masterlock);
        pthread_cond_signal(&root->mastercond);
        pthread_mutex_unlock(&root->masterlock);
    }
}

void actor_clean(ACTOR_ROOT *root) {
    int i = 0; if (root == NULL) return;

    buffer_clean(root->inbox);
    hash_clean(root->nodestable);

    for (i = 0; i < root->maxnode; ++i)
        if (root->nodes[i].status != 0)
            buffer_clean(root->nodes[i].inbox);

    buffer_clean(root->task);

    pthread_mutex_destroy(&root->masterlock);
    pthread_cond_destroy(&root->mastercond);
    pthread_mutex_destroy(&root->workerlock);
    pthread_cond_destroy(&root->workercond);

    anfree(root->workers);
    anfree(root->nodes);
    anfree(root);
}






#include <stdio.h>
#include <unistd.h>

void count_cb(ACTOR_ROOT *root, void *data) {
    int *count = (int *)data;

    ++(*count);
}

void producer_cb(ACTOR_ROOT *root, void *data) {
    int i = 0;

    for (i = 0; i < 10000; ++i)
        while (!actors_send(root, "count", data));
}

void producer2_cb(ACTOR_ROOT *root, void *data) {
    int i = 0;

    for (i = 0; i < 10000; ++i)
        while (!actors_send(root, "count", data));
}

void consumer_cb(ACTOR_ROOT *root, void *data) {
    int i = 0;

    for (i = 0; i < 20000; ++i)
        while (!actors_send(root, "count", data));
}

void consumer2_cb(ACTOR_ROOT *root, void *data) {
    int i = 0;

    for (i = 0; i < 20000; ++i)
        while (!actors_send(root, "count", data));
}

void ping_cb(ACTOR_ROOT *root, void *data) {
    int *count = (int *)data;

    ++(*count);
    ++(*count);

    printf("count: %d\n", *count);
    while (!actors_send(root, "pong", data));
}

void pong_cb(ACTOR_ROOT *root, void *data) {
    int *count = (int *)data;

    --(*count);

    printf("count: %d\n", *count);
    if (*count != 10000)
        while (!actors_send(root, "ping", data));
    else
        actor_break(root);
}

int main(int argc, char **argv) {
    ACTOR_ROOT *root = actor_init("root", 1024, 4, 60000);

    actors_create(root, "count", count_cb);
    actors_create(root, "producer", producer_cb);
    actors_create(root, "producer2", producer_cb);
    actors_create(root, "consumer", consumer_cb);
    actors_create(root, "consumer2", consumer_cb);
    actors_create(root, "ping", ping_cb);
    actors_create(root, "pong", pong_cb);

    actor_run(root);

    actors_start(root, "count");
    actors_start(root, "producer");
    actors_start(root, "producer2");
    actors_start(root, "consumer");
    actors_start(root, "consumer2");
    actors_start(root, "ping");
    actors_start(root, "pong");

    int data = 0, count = 0;
    actors_send(root, "producer", &data);
    actors_send(root, "producer2", &data);
    actors_send(root, "consumer", &data);
    actors_send(root, "consumer2", &data);
    actors_send(root, "ping", &count);

    sleep(1);
    printf("data: %p, %d, breakout: %d\n", &data, data, root->breakout);
    printf("node[0] size: %lu\n", buffer_size(root->nodes[0].inbox));
    printf("node[1] size: %lu\n", buffer_size(root->nodes[1].inbox));
    printf("node[2] size: %lu\n", buffer_size(root->nodes[2].inbox));
    printf("node[3] size: %lu\n", buffer_size(root->nodes[3].inbox));
    printf("node[4] size: %lu\n", buffer_size(root->nodes[4].inbox));
    printf("node[5] size: %lu\n", buffer_size(root->nodes[5].inbox));
    printf("node[6] size: %lu\n", buffer_size(root->nodes[6].inbox));
    printf("task size: %lu\n", buffer_size(root->task));

    actor_wait(root);

    actor_clean(root);

    return 0;
}
