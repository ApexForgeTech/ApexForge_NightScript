#ifndef NIGHT_ARENA_H
#define NIGHT_ARENA_H

#include <stddef.h>

typedef struct ArenaBlock ArenaBlock;

struct ArenaBlock {
    ArenaBlock *next;
    size_t      used;
    size_t      cap;
    char        data[];
};

typedef struct {
    ArenaBlock *head;
} Arena;

void  arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t size);
char *arena_strdup(Arena *a, const char *s, int len);
void  arena_free(Arena *a);

#endif /* NIGHT_ARENA_H */
