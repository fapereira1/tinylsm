#include "tinylsm/arena.h"

#include <stdlib.h>
#include <string.h>

static inline size_t align_up(size_t n, size_t align) {
    return (n + align - 1u) & ~(align - 1u);
}

static arena_block_t *block_new(size_t min_capacity) {
    size_t cap = min_capacity > ARENA_BLOCK_SIZE
                 ? min_capacity
                 : ARENA_BLOCK_SIZE;
    arena_block_t *b = malloc(sizeof(arena_block_t) + cap);
    if (!b) return NULL;
    b->next     = NULL;
    b->used     = 0;
    b->capacity = cap;
    return b;
}

int arena_init(arena_t *a) {
    a->head = block_new(ARENA_BLOCK_SIZE);
    if (!a->head) return -1;
    a->total_mem = sizeof(arena_block_t) + ARENA_BLOCK_SIZE;
    if (pthread_mutex_init(&a->lock, NULL) != 0) {
        free(a->head);
        a->head = NULL;
        return -1;
    }
    return 0;
}

void *arena_alloc(arena_t *a, size_t size, size_t align) {
    if (size == 0) return NULL;

    pthread_mutex_lock(&a->lock);

    /*
     * O alinhamento tem que ser calculado a partir do endereço REAL de
     * data[], não do offset `used`.
     *
     * Por quê? malloc garante alinhamento a 16 bytes, mas data[] começa
     * em sizeof(arena_block_t) = 24 bytes depois — logo data[0] é apenas
     * 8-byte aligned. Para align=16/32/64, precisamos compensar esse offset.
     *
     *   base  = endereço real de data[0]
     *   pos   = (próximo endereço alinhado a partir de base+used) - base
     */
    uintptr_t base = (uintptr_t)a->head->data;
    size_t pos = (size_t)(align_up(base + a->head->used, align) - base);

    if (pos + size > a->head->capacity) {
        size_t need = size + (align - 1u);
        arena_block_t *b = block_new(need);
        if (!b) {
            pthread_mutex_unlock(&a->lock);
            return NULL;
        }
        a->total_mem += sizeof(arena_block_t) + b->capacity;
        b->next = a->head;
        a->head = b;

        /* Recalcula pos para o novo bloco. */
        base = (uintptr_t)a->head->data;
        pos  = (size_t)(align_up(base, align) - base);
    }

    void *ptr     = a->head->data + pos;
    a->head->used = pos + size;

    pthread_mutex_unlock(&a->lock);
    return ptr;
}

uint8_t *arena_dup(arena_t *a, const uint8_t *src, size_t len) {
    if (len == 0) return NULL;
    uint8_t *dst = arena_alloc(a, len, 1u);
    if (dst) memcpy(dst, src, len);
    return dst;
}

size_t arena_mem_usage(arena_t *a) {
    pthread_mutex_lock(&a->lock);
    size_t mem = a->total_mem;
    pthread_mutex_unlock(&a->lock);
    return mem;
}

void arena_destroy(arena_t *a) {
    pthread_mutex_lock(&a->lock);
    arena_block_t *b = a->head;
    while (b) {
        arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    a->head      = NULL;
    a->total_mem = 0;
    pthread_mutex_unlock(&a->lock);
    pthread_mutex_destroy(&a->lock);
}
