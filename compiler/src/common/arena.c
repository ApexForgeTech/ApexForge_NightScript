#include "arena.h"

#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE (1024 * 64)

void arena_init(Arena *a) {
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t size) {
    /* align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    if (!a->head || a->head->used + size > a->head->cap) {
        size_t cap      = size > BLOCK_SIZE ? size : BLOCK_SIZE;
        ArenaBlock *blk = malloc(sizeof(ArenaBlock) + cap);
        blk->next       = a->head;
        blk->used       = 0;
        blk->cap        = cap;
        a->head         = blk;
    }

    void *ptr       = a->head->data + a->head->used;
    a->head->used  += size;
    memset(ptr, 0, size);
    return ptr;
}

char *arena_strdup(Arena *a, const char *s, int len) {
    char *p = arena_alloc(a, (size_t)len + 1);
    memcpy(p, s, (size_t)len);
    p[len] = '\0';
    return p;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}
