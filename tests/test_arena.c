#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "tinylsm/arena.h"

/* ---- testes básicos ---- */

static void test_init_destroy(void) {
    arena_t a;
    assert(arena_init(&a) == 0);
    assert(arena_mem_usage(&a) > 0);
    arena_destroy(&a);
}

static void test_alloc_small(void) {
    arena_t a;
    assert(arena_init(&a) == 0);

    /* múltiplas alocações pequenas dentro do mesmo bloco */
    for (int i = 0; i < 100; i++) {
        void *p = arena_alloc(&a, 16, 8);
        assert(p != NULL);
    }

    arena_destroy(&a);
}

static void test_alloc_large(void) {
    arena_t a;
    assert(arena_init(&a) == 0);

    /* alocação maior que ARENA_BLOCK_SIZE — deve abrir novo bloco */
    size_t big = ARENA_BLOCK_SIZE * 2;
    void *p = arena_alloc(&a, big, 1);
    assert(p != NULL);
    assert(arena_mem_usage(&a) >= big);

    arena_destroy(&a);
}

static void test_dup(void) {
    arena_t a;
    assert(arena_init(&a) == 0);

    const char *src = "tinylsm";
    uint8_t *copy = arena_dup(&a, (const uint8_t *)src, strlen(src));

    assert(copy != NULL);
    assert(memcmp(copy, src, strlen(src)) == 0);
    /* garantia de que é uma cópia real, não o mesmo ponteiro */
    assert((void *)copy != (void *)src);

    arena_destroy(&a);
}

static void test_alignment(void) {
    arena_t a;
    assert(arena_init(&a) == 0);

    /* qualquer alinhamento de 1 a 64 deve ser satisfeito */
    for (size_t align = 1; align <= 64; align *= 2) {
        void *p = arena_alloc(&a, align, align);
        assert(p != NULL);
        assert(((uintptr_t)p % align) == 0);
    }

    arena_destroy(&a);
}

static void test_alloc_zero(void) {
    arena_t a;
    assert(arena_init(&a) == 0);

    /* size==0 deve retornar NULL sem crashar */
    void *p = arena_alloc(&a, 0, 1);
    assert(p == NULL);

    arena_destroy(&a);
}

static void test_mem_grows(void) {
    arena_t a;
    assert(arena_init(&a) == 0);

    size_t before = arena_mem_usage(&a);

    /* força abertura de um segundo bloco */
    arena_alloc(&a, ARENA_BLOCK_SIZE, 1);
    arena_alloc(&a, ARENA_BLOCK_SIZE, 1);

    size_t after = arena_mem_usage(&a);
    assert(after > before);

    arena_destroy(&a);
}

/* ---- teste de concorrência ---- */

#define N_THREADS  8
#define N_ALLOCS   1000
#define ALLOC_SIZE 64

static arena_t shared_arena;

static void *thread_alloc(void *arg) {
    (void)arg;
    for (int i = 0; i < N_ALLOCS; i++) {
        void *p = arena_alloc(&shared_arena, ALLOC_SIZE, 8);
        assert(p != NULL);
        /* escreve no bloco para TSan detectar data races */
        memset(p, 0xAB, ALLOC_SIZE);
    }
    return NULL;
}

static void test_concurrent_alloc(void) {
    assert(arena_init(&shared_arena) == 0);

    pthread_t threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        pthread_create(&threads[i], NULL, thread_alloc, NULL);
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(threads[i], NULL);

    /* total esperado: N_THREADS * N_ALLOCS * ALLOC_SIZE bytes de dados */
    size_t expected_data = N_THREADS * N_ALLOCS * ALLOC_SIZE;
    assert(arena_mem_usage(&shared_arena) >= expected_data);

    arena_destroy(&shared_arena);
}

/* ---- runner ---- */

int main(void) {
    test_init_destroy();
    test_alloc_small();
    test_alloc_large();
    test_dup();
    test_alignment();
    test_alloc_zero();
    test_mem_grows();
    test_concurrent_alloc();

    printf("PASS: arena\n");
    return 0;
}
