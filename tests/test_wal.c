#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "tinylsm/wal.h"
#include "tinylsm/skiplist.h"

#define WAL_PATH "/tmp/tinylsm_test.wal"

static slice_t S(const char *s) { return slice_from_str(s); }

static void cleanup(void) { unlink(WAL_PATH); }

/* ------------------------------------------------------------------ */

static void test_append_and_recover(void) {
    cleanup();

    wal_t *w = wal_open(WAL_PATH);
    assert(w);
    assert(wal_append(w, OP_PUT, S("name"),  S("alice")) == LSM_OK);
    assert(wal_append(w, OP_PUT, S("city"),  S("belo horizonte")) == LSM_OK);
    assert(wal_append(w, OP_PUT, S("lang"),  S("c11")) == LSM_OK);
    assert(wal_sync(w) == LSM_OK);
    wal_close(w);

    skiplist_t *sl = sl_new();
    assert(wal_recover(WAL_PATH, sl) == LSM_OK);

    slice_t v;
    assert(sl_get(sl, S("name"), &v) == LSM_OK);
    assert(slice_eq(v, S("alice")));
    assert(sl_get(sl, S("city"), &v) == LSM_OK);
    assert(slice_eq(v, S("belo horizonte")));
    assert(sl_get(sl, S("lang"), &v) == LSM_OK);
    assert(slice_eq(v, S("c11")));

    sl_free(sl);
    cleanup();
}

static void test_del_recovered_as_tombstone(void) {
    cleanup();

    wal_t *w = wal_open(WAL_PATH);
    assert(w);
    assert(wal_append(w, OP_PUT, S("k"), S("v")) == LSM_OK);
    assert(wal_append(w, OP_DEL, S("k"), S(""))  == LSM_OK);
    assert(wal_sync(w) == LSM_OK);
    wal_close(w);

    skiplist_t *sl = sl_new();
    assert(wal_recover(WAL_PATH, sl) == LSM_OK);

    slice_t v;
    /* DEL após PUT: deve resultar em NOT_FOUND */
    assert(sl_get(sl, S("k"), &v) == LSM_NOT_FOUND);

    sl_free(sl);
    cleanup();
}

static void test_empty_wal_file(void) {
    cleanup();

    /* arquivo não existe — deve retornar OK (estado vazio válido) */
    skiplist_t *sl = sl_new();
    assert(wal_recover(WAL_PATH, sl) == LSM_OK);
    assert(sl_num_entries(sl) == 0);
    sl_free(sl);
}

static void test_partial_write_at_end(void) {
    cleanup();

    /* Grava dois records completos */
    wal_t *w = wal_open(WAL_PATH);
    assert(w);
    assert(wal_append(w, OP_PUT, S("k1"), S("v1")) == LSM_OK);
    assert(wal_append(w, OP_PUT, S("k2"), S("v2")) == LSM_OK);
    assert(wal_sync(w) == LSM_OK);
    wal_close(w);

    /* Simula crash: appenda lixo no final (record incompleto) */
    FILE *fp = fopen(WAL_PATH, "ab");
    assert(fp);
    fwrite("LIXO_PARCIAL_CRASH", 1, 10, fp);
    fclose(fp);

    /* Recovery deve aplicar k1 e k2, ignorar o lixo */
    skiplist_t *sl = sl_new();
    assert(wal_recover(WAL_PATH, sl) == LSM_OK);

    slice_t v;
    assert(sl_get(sl, S("k1"), &v) == LSM_OK);
    assert(slice_eq(v, S("v1")));
    assert(sl_get(sl, S("k2"), &v) == LSM_OK);
    assert(slice_eq(v, S("v2")));

    sl_free(sl);
    cleanup();
}

static void test_crc_corruption_stops_recovery(void) {
    cleanup();

    /* Grava dois records */
    wal_t *w = wal_open(WAL_PATH);
    assert(w);
    assert(wal_append(w, OP_PUT, S("good"), S("record")) == LSM_OK);
    assert(wal_append(w, OP_PUT, S("bad"),  S("record")) == LSM_OK);
    assert(wal_sync(w) == LSM_OK);
    wal_close(w);

    /*
     * Tamanho do primeiro record:
     *   crc(4) + op(1) + key_len(4) + key(4) + val_len(4) + val(6) = 23 bytes
     * Corrompemos o primeiro byte do CRC do segundo record.
     */
    FILE *fp = fopen(WAL_PATH, "r+b");
    assert(fp);
    fseek(fp, 23, SEEK_SET);
    uint8_t b;
    fread(&b, 1, 1, fp);
    fseek(fp, 23, SEEK_SET);
    uint8_t flipped = b ^ 0xFFu;
    fwrite(&flipped, 1, 1, fp);
    fclose(fp);

    skiplist_t *sl = sl_new();
    assert(wal_recover(WAL_PATH, sl) == LSM_OK);

    slice_t v;
    assert(sl_get(sl, S("good"), &v) == LSM_OK);      /* primeiro: OK */
    assert(sl_get(sl, S("bad"),  &v) == LSM_NOT_FOUND); /* segundo: descartado */

    sl_free(sl);
    cleanup();
}

static void test_reopen_appends(void) {
    cleanup();

    /* Sessão 1 */
    wal_t *w = wal_open(WAL_PATH);
    assert(w);
    wal_append(w, OP_PUT, S("a"), S("1"));
    wal_sync(w);
    wal_close(w);

    /* Sessão 2 — reabre e continua appendando */
    w = wal_open(WAL_PATH);
    assert(w);
    wal_append(w, OP_PUT, S("b"), S("2"));
    wal_sync(w);
    wal_close(w);

    skiplist_t *sl = sl_new();
    assert(wal_recover(WAL_PATH, sl) == LSM_OK);

    slice_t v;
    assert(sl_get(sl, S("a"), &v) == LSM_OK);
    assert(sl_get(sl, S("b"), &v) == LSM_OK);

    sl_free(sl);
    cleanup();
}

static void test_empty_key_rejected(void) {
    wal_t *w = wal_open(WAL_PATH);
    assert(w);
    assert(wal_append(w, OP_PUT, slice_empty(), S("v")) == LSM_INVALID_ARG);
    wal_close(w);
    cleanup();
}

/* ---- concorrência ---- */

#define N_THREADS 8
#define N_RECORDS 200

static wal_t *shared_wal;

static void *writer_thread(void *arg) {
    int base = *(int *)arg * N_RECORDS;
    for (int i = 0; i < N_RECORDS; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "key%05d", base + i);
        snprintf(v, sizeof(v), "val%05d", base + i);
        assert(wal_append(shared_wal, OP_PUT,
                           slice_from_str(k),
                           slice_from_str(v)) == LSM_OK);
    }
    return NULL;
}

static void test_concurrent_append_and_recover(void) {
    cleanup();

    shared_wal = wal_open(WAL_PATH);
    assert(shared_wal);

    pthread_t threads[N_THREADS];
    int       ids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, writer_thread, &ids[i]);
    }
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(threads[i], NULL);

    assert(wal_sync(shared_wal) == LSM_OK);
    wal_close(shared_wal);

    /* Todos os N_THREADS * N_RECORDS records devem ser recuperáveis */
    skiplist_t *sl = sl_new();
    assert(wal_recover(WAL_PATH, sl) == LSM_OK);
    assert(sl_num_entries(sl) == N_THREADS * N_RECORDS);

    sl_free(sl);
    cleanup();
}

/* ---- runner ---- */

int main(void) {
    test_append_and_recover();
    test_del_recovered_as_tombstone();
    test_empty_wal_file();
    test_partial_write_at_end();
    test_crc_corruption_stops_recovery();
    test_reopen_appends();
    test_empty_key_rejected();
    test_concurrent_append_and_recover();

    printf("PASS: wal\n");
    return 0;
}
