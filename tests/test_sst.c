#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinylsm/sst.h"
#include "tinylsm/skiplist.h"

#define PATH "/tmp/tinylsm_test.sst"

static slice_t S(const char *s) { return slice_from_str(s); }
static void cleanup(void) { unlink(PATH); }

static void test_roundtrip(void) {
    cleanup();
    sst_writer_t *w = sst_writer_open(PATH);
    assert(w);
    assert(sst_writer_add(w, OP_PUT, 1, S("apple"),  S("red"))    == LSM_OK);
    assert(sst_writer_add(w, OP_PUT, 2, S("banana"), S("yellow")) == LSM_OK);
    assert(sst_writer_add(w, OP_PUT, 3, S("cherry"), S("dark"))   == LSM_OK);
    assert(sst_writer_finish(w) == LSM_OK);
    sst_writer_free(w);

    sst_reader_t *r = sst_reader_open(PATH);
    assert(r);

    sl_op_t op; uint64_t seq; uint8_t *buf; size_t len;

    assert(sst_reader_get(r, S("apple"), &op, &seq, &buf, &len) == LSM_OK);
    assert(op == OP_PUT && seq == 1);
    assert(len == 3 && memcmp(buf, "red", 3) == 0);
    free(buf);

    assert(sst_reader_get(r, S("banana"), &op, &seq, &buf, &len) == LSM_OK);
    assert(len == 6 && memcmp(buf, "yellow", 6) == 0);
    free(buf);

    assert(sst_reader_get(r, S("cherry"), &op, &seq, &buf, &len) == LSM_OK);
    assert(len == 4 && memcmp(buf, "dark", 4) == 0);
    free(buf);

    sst_reader_close(r);
    cleanup();
}

static void test_not_found(void) {
    cleanup();
    sst_writer_t *w = sst_writer_open(PATH);
    assert(w);
    sst_writer_add(w, OP_PUT, 1, S("a"), S("1"));
    sst_writer_add(w, OP_PUT, 2, S("c"), S("3"));
    sst_writer_finish(w);
    sst_writer_free(w);

    sst_reader_t *r = sst_reader_open(PATH);
    assert(r);
    sl_op_t op; uint64_t seq; uint8_t *buf; size_t len;
    assert(sst_reader_get(r, S("b"), &op, &seq, &buf, &len) == LSM_NOT_FOUND);
    assert(sst_reader_get(r, S("z"), &op, &seq, &buf, &len) == LSM_NOT_FOUND);
    sst_reader_close(r);
    cleanup();
}

static void test_tombstone(void) {
    cleanup();
    sst_writer_t *w = sst_writer_open(PATH);
    assert(w);
    sst_writer_add(w, OP_PUT, 1, S("alive"), S("yes"));
    sst_writer_add(w, OP_DEL, 2, S("dead"),  S(""));
    sst_writer_finish(w);
    sst_writer_free(w);

    sst_reader_t *r = sst_reader_open(PATH);
    assert(r);
    sl_op_t op; uint64_t seq; uint8_t *buf; size_t len;

    assert(sst_reader_get(r, S("alive"), &op, &seq, &buf, &len) == LSM_OK);
    assert(op == OP_PUT); free(buf);

    /* OP_DEL — LSM_OK mas buf=NULL */
    assert(sst_reader_get(r, S("dead"), &op, &seq, &buf, &len) == LSM_OK);
    assert(op == OP_DEL && buf == NULL && len == 0);

    sst_reader_close(r);
    cleanup();
}

static void test_iterator_order(void) {
    cleanup();
    sst_writer_t *w = sst_writer_open(PATH);
    assert(w);
    sst_writer_add(w, OP_PUT, 1, S("ant"),  S("1"));
    sst_writer_add(w, OP_PUT, 2, S("bee"),  S("2"));
    sst_writer_add(w, OP_DEL, 3, S("cat"),  S(""));
    sst_writer_add(w, OP_PUT, 4, S("duck"), S("4"));
    sst_writer_finish(w);
    sst_writer_free(w);

    sst_reader_t *r = sst_reader_open(PATH);
    assert(r);

    const char *keys[] = { "ant", "bee", "cat", "duck" };
    sl_op_t     ops[]  = { OP_PUT, OP_PUT, OP_DEL, OP_PUT };
    int i = 0;

    sst_iter_t it;
    sst_iter_init(&it, r);
    while (sst_iter_valid(&it)) {
        assert(slice_eq(sst_iter_key(&it), S(keys[i])));
        assert(sst_iter_op(&it) == ops[i]);
        i++;
        sst_iter_next(&it);
    }
    sst_iter_finish(&it);
    assert(i == 4);

    sst_reader_close(r);
    cleanup();
}

static void test_multiple_blocks(void) {
    cleanup();
    sst_writer_t *w = sst_writer_open(PATH);
    assert(w);

    int n = 500;
    for (int i = 0; i < n; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "key%05d", i);
        snprintf(v, sizeof(v), "val%05d", i);
        assert(sst_writer_add(w, OP_PUT, (uint64_t)(i + 1),
                               slice_from_str(k), slice_from_str(v)) == LSM_OK);
    }
    assert(sst_writer_finish(w) == LSM_OK);
    sst_writer_free(w);

    sst_reader_t *r = sst_reader_open(PATH);
    assert(r);
    assert(sst_reader_num_blocks(r) > 1);  /* deve ter gerado múltiplos blocos */

    sl_op_t op; uint64_t seq; uint8_t *buf; size_t len;

    assert(sst_reader_get(r, slice_from_str("key00000"), &op, &seq, &buf, &len) == LSM_OK);
    assert(memcmp(buf, "val00000", 8) == 0); free(buf);

    assert(sst_reader_get(r, slice_from_str("key00250"), &op, &seq, &buf, &len) == LSM_OK);
    assert(memcmp(buf, "val00250", 8) == 0); free(buf);

    assert(sst_reader_get(r, slice_from_str("key00499"), &op, &seq, &buf, &len) == LSM_OK);
    assert(memcmp(buf, "val00499", 8) == 0); free(buf);

    /* Iterador deve contar todos os records */
    int count = 0;
    sst_iter_t it;
    sst_iter_init(&it, r);
    while (sst_iter_valid(&it)) { count++; sst_iter_next(&it); }
    sst_iter_finish(&it);
    assert(count == n);

    sst_reader_close(r);
    cleanup();
}

static void test_crc_corruption(void) {
    cleanup();
    sst_writer_t *w = sst_writer_open(PATH);
    assert(w);
    sst_writer_add(w, OP_PUT, 1, S("k1"), S("v1"));
    sst_writer_add(w, OP_PUT, 2, S("k2"), S("v2"));
    sst_writer_finish(w);
    sst_writer_free(w);

    /* Inverte um byte dentro do bloco de dados */
    FILE *fp = fopen(PATH, "r+b");
    assert(fp);
    fseek(fp, 5, SEEK_SET);
    uint8_t b; fread(&b, 1, 1, fp);
    fseek(fp, 5, SEEK_SET);
    uint8_t flipped = b ^ 0xFFu;
    fwrite(&flipped, 1, 1, fp);
    fclose(fp);

    sst_reader_t *r = sst_reader_open(PATH);
    assert(r);  /* open OK — footer e índice estão íntegros */

    sl_op_t op; uint64_t seq; uint8_t *buf; size_t len;
    assert(sst_reader_get(r, S("k1"), &op, &seq, &buf, &len) == LSM_CORRUPTION);

    sst_reader_close(r);
    cleanup();
}

static void test_flush_from_skiplist(void) {
    cleanup();

    /* Constrói MemTable e faz flush para SST */
    skiplist_t *sl = sl_new();
    assert(sl);
    sl_put(sl, S("dog"),  S("woof"));
    sl_put(sl, S("cat"),  S("meow"));
    sl_put(sl, S("bird"), S("tweet"));
    sl_del(sl, S("fish"));
    sl_put(sl, S("cat"),  S("purr"));  /* update — versão mais nova */

    sst_writer_t *w = sst_writer_open(PATH);
    assert(w);

    sl_iter_t it;
    sl_iter_init(&it, sl);
    while (sl_iter_valid(&it)) {
        sst_writer_add(w, sl_iter_op(&it), sl_iter_seq(&it),
                       sl_iter_key(&it), sl_iter_value(&it));
        sl_iter_next_key(&it);  /* uma entry por user_key */
    }
    sl_iter_finish(&it);

    assert(sst_writer_finish(w) == LSM_OK);
    sst_writer_free(w);
    sl_free(sl);

    /* Lê de volta */
    sst_reader_t *r = sst_reader_open(PATH);
    assert(r);

    sl_op_t op; uint64_t seq; uint8_t *buf; size_t len;

    /* bird → PUT "tweet" */
    assert(sst_reader_get(r, S("bird"), &op, &seq, &buf, &len) == LSM_OK);
    assert(op == OP_PUT && len == 5 && memcmp(buf, "tweet", 5) == 0);
    free(buf);

    /* cat → PUT "purr" (versão mais nova) */
    assert(sst_reader_get(r, S("cat"), &op, &seq, &buf, &len) == LSM_OK);
    assert(op == OP_PUT && len == 4 && memcmp(buf, "purr", 4) == 0);
    free(buf);

    /* fish → tombstone */
    assert(sst_reader_get(r, S("fish"), &op, &seq, &buf, &len) == LSM_OK);
    assert(op == OP_DEL && buf == NULL);

    sst_reader_close(r);
    cleanup();
}

int main(void) {
    test_roundtrip();
    test_not_found();
    test_tombstone();
    test_iterator_order();
    test_multiple_blocks();
    test_crc_corruption();
    test_flush_from_skiplist();

    printf("PASS: sst\n");
    return 0;
}
