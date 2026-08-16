#pragma once

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/*
 * arena_t — alocador bump-pointer para nós do MemTable.
 *
 * Funcionamento:
 *   Mantém uma lista encadeada de blocos alocados via malloc.
 *   Dentro de cada bloco, avança um ponteiro (bump) a cada alocação.
 *   Sem free individual — arena_destroy libera tudo de uma vez.
 *
 * Thread safety:
 *   Um mutex protege o bump pointer. Contenção é baixa porque escritas
 *   no MemTable já são serializadas pelo rwlock da skip list.
 */

#define ARENA_BLOCK_SIZE 4096u

/* Bloco interno — não use diretamente. */
typedef struct arena_block {
    struct arena_block *next;
    size_t              used;
    size_t              capacity;
    uint8_t             data[];   /* alocação flexível — dados ficam aqui */
} arena_block_t;

typedef struct {
    arena_block_t  *head;       /* bloco corrente (topo da pilha)    */
    size_t          total_mem;  /* total de bytes alocados do sistema */
    pthread_mutex_t lock;
} arena_t;

/*
 * Inicializa a arena. Aloca o primeiro bloco.
 * Retorna 0 em sucesso, -1 em falha (malloc ou pthread).
 */
int arena_init(arena_t *a);

/*
 * Aloca `size` bytes alinhados a `align` (deve ser potência de 2).
 * Retorna NULL apenas em OOM — tratado como fatal pelo caller.
 */
void *arena_alloc(arena_t *a, size_t size, size_t align);

/*
 * Copia `len` bytes de `src` para dentro da arena.
 * Retorna ponteiro para a cópia, ou NULL em OOM.
 */
uint8_t *arena_dup(arena_t *a, const uint8_t *src, size_t len);

/* Bytes totais consumidos da heap do sistema (inclui overhead dos blocos). */
size_t arena_mem_usage(arena_t *a);

/*
 * Libera todos os blocos. A arena não pode ser usada após esta chamada
 * sem um novo arena_init.
 */
void arena_destroy(arena_t *a);
