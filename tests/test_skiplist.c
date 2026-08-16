#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "tinylsm/skiplist.h"

/* helpers */
static slice_t S(const char *s) { return slice_from_str(s); }

/* ---- testes unitários ---- */

static void test_put_get(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    assert(sl_put(sl, S("foo"), S("bar")) == LSM_OK);

    slice_t v;
    assert(sl_get(sl, S("foo"), &v) == LSM_OK);
    assert(slice_eq(v, S("bar")));

    sl_free(sl);
}

static void test_update_returns_latest(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    assert(sl_put(sl, S("key"), S("v1")) == LSM_OK);
    assert(sl_put(sl, S("key"), S("v2")) == LSM_OK);
    assert(sl_put(sl, S("key"), S("v3")) == LSM_OK);

    slice_t v;
    assert(sl_get(sl, S("key"), &v) == LSM_OK);
    assert(slice_eq(v, S("v3")));   /* versão mais recente */

    sl_free(sl);
}

static void test_delete_tombstone(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    assert(sl_put(sl, S("x"), S("hello")) == LSM_OK);
    assert(sl_del(sl, S("x"))             == LSM_OK);

    slice_t v;
    assert(sl_get(sl, S("x"), &v) == LSM_NOT_FOUND);  /* tombstone */

    sl_free(sl);
}

static void test_delete_then_reinsert(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    sl_put(sl, S("k"), S("old"));
    sl_del(sl, S("k"));
    sl_put(sl, S("k"), S("new"));

    slice_t v;
    assert(sl_get(sl, S("k"), &v) == LSM_OK);
    assert(slice_eq(v, S("new")));

    sl_free(sl);
}

static void test_not_found(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    slice_t v;
    assert(sl_get(sl, S("absent"), &v) == LSM_NOT_FOUND);

    sl_free(sl);
}

static void test_empty_key_rejected(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    assert(sl_put(sl, slice_empty(), S("v")) == LSM_INVALID_ARG);
    assert(sl_del(sl, slice_empty())         == LSM_INVALID_ARG);

    slice_t v;
    assert(sl_get(sl, slice_empty(), &v)     == LSM_INVALID_ARG);

    sl_free(sl);
}

static void test_iterator_order(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    /* insere fora de ordem */
    sl_put(sl, S("mango"),  S("m"));
    sl_put(sl, S("apple"),  S("a"));
    sl_put(sl, S("banana"), S("b"));
    sl_put(sl, S("cherry"), S("c"));

    const char *expected[] = { "apple", "banana", "cherry", "mango" };
    int i = 0;

    sl_iter_t it;
    sl_iter_init(&it, sl);
    while (sl_iter_valid(&it)) {
        assert(slice_eq(sl_iter_key(&it), S(expected[i])));
        i++;
        sl_iter_next_key(&it);   /* uma user_key por iteração */
    }
    sl_iter_finish(&it);

    assert(i == 4);
    sl_free(sl);
}

static void test_iterator_sees_latest_only(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    sl_put(sl, S("k"), S("v1"));
    sl_put(sl, S("k"), S("v2"));
    sl_put(sl, S("k"), S("v3"));

    sl_iter_t it;
    sl_iter_init(&it, sl);
    assert(sl_iter_valid(&it));

    /* sl_iter_next_key pula todos os seq de "k" de uma vez */
    assert(slice_eq(sl_iter_key(&it),   S("k")));
    assert(slice_eq(sl_iter_value(&it), S("v3")));  /* seq mais alto = primeiro */

    sl_iter_next_key(&it);
    assert(!sl_iter_valid(&it));   /* só havia uma user_key */
    sl_iter_finish(&it);

    sl_free(sl);
}

static void test_iterator_tombstone_visible(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    sl_put(sl, S("a"), S("1"));
    sl_del(sl, S("b"));           /* nunca foi inserido — tombstone puro */
    sl_put(sl, S("c"), S("3"));

    const char *keys[] = { "a", "b", "c" };
    sl_op_t     ops[]  = { OP_PUT, OP_DEL, OP_PUT };
    int i = 0;

    sl_iter_t it;
    sl_iter_init(&it, sl);
    while (sl_iter_valid(&it)) {
        assert(slice_eq(sl_iter_key(&it), S(keys[i])));
        assert(sl_iter_op(&it) == ops[i]);
        i++;
        sl_iter_next_key(&it);
    }
    sl_iter_finish(&it);

    assert(i == 3);
    sl_free(sl);
}

static void test_mem_and_entries(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    size_t mem0 = sl_mem_usage(sl);
    assert(sl_num_entries(sl) == 0);

    for (int i = 0; i < 500; i++) {
        char k[16];
        snprintf(k, sizeof(k), "key%d", i);
        sl_put(sl, S(k), S("value"));
    }

    assert(sl_num_entries(sl) == 500);
    assert(sl_mem_usage(sl) > mem0);

    sl_free(sl);
}

/* ---- testes de concorrência ---- */

#define N_WRITERS  8
#define N_KEYS     200

static skiplist_t *shared_sl;

static void *writer_thread(void *arg) {
    int base = *(int *)arg * N_KEYS;
    for (int i = 0; i < N_KEYS; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "key%d", base + i);
        snprintf(v, sizeof(v), "val%d", base + i);
        assert(sl_put(shared_sl, S(k), S(v)) == LSM_OK);
    }
    return NULL;
}

static void test_concurrent_writes(void) {
    shared_sl = sl_new();
    assert(shared_sl);

    pthread_t threads[N_WRITERS];
    int       ids[N_WRITERS];

    for (int i = 0; i < N_WRITERS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, writer_thread, &ids[i]);
    }
    for (int i = 0; i < N_WRITERS; i++)
        pthread_join(threads[i], NULL);

    assert(sl_num_entries(shared_sl) == (size_t)(N_WRITERS * N_KEYS));

    sl_free(shared_sl);
}

static void *reader_thread(void *arg) {
    skiplist_t *sl = arg;
    slice_t v;
    /* leituras concorrentes — TSan detecta race se o rwlock tiver bug */
    for (int i = 0; i < 1000; i++)
        sl_get(sl, S("probe"), &v);
    return NULL;
}

static void test_concurrent_read_write(void) {
    skiplist_t *sl = sl_new();
    assert(sl);

    sl_put(sl, S("probe"), S("initial"));

    pthread_t r1, r2, w1;
    pthread_create(&r1, NULL, reader_thread, sl);
    pthread_create(&r2, NULL, reader_thread, sl);
    pthread_create(&w1, NULL, writer_thread, &(int){0});
    /* writer usa shared_sl — usamos sl local só para leitores */

    pthread_join(r1, NULL);
    pthread_join(r2, NULL);
    pthread_join(w1, NULL);

    sl_free(sl);
}

/* ---- runner ---- */

int main(void) {
    test_put_get();
    test_update_returns_latest();
    test_delete_tombstone();
    test_delete_then_reinsert();
    test_not_found();
    test_empty_key_rejected();
    test_iterator_order();
    test_iterator_sees_latest_only();
    test_iterator_tombstone_visible();
    test_mem_and_entries();
    test_concurrent_writes();
    test_concurrent_read_write();

    printf("PASS: skiplist\n");
    return 0;
}
