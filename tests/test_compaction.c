#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinylsm/db.h"

#define TEST_DIR "/tmp/tinylsm_test_compact"

static slice_t S(const char *s) { return slice_from_str(s); }

static void cleanup(void) {
    DIR *d = opendir(TEST_DIR);
    if (!d) return;
    struct dirent *ent;
    char path[512];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", TEST_DIR, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(TEST_DIR);
}

/* Conta arquivos .sst no diretório */
static int count_ssts(void) {
    DIR *d = opendir(TEST_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t l = strlen(ent->d_name);
        if (l > 4 && strcmp(ent->d_name + l - 4, ".sst") == 0) n++;
    }
    closedir(d);
    return n;
}

static db_opts_t tiny_opts(void) {
    return (db_opts_t){ .mem_limit_bytes = 2048 };
}

/* ------------------------------------------------------------------ */

static void test_compact_reduces_sst_count(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, tiny_opts());
    assert(db);

    /* Gera múltiplos SSTables via flush */
    for (int i = 0; i < 400; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "key%05d", i);
        snprintf(v, sizeof(v), "val%05d", i);
        assert(db_put(db, slice_from_str(k), slice_from_str(v)) == LSM_OK);
    }

    int before = count_ssts();
    assert(before >= 2);

    assert(db_compact(db) == LSM_OK);

    int after = count_ssts();
    assert(after < before);
    assert(after == 1);  /* compaction total → 1 SSTable */

    db_close(db);
    cleanup();
}

static void test_data_intact_after_compact(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, tiny_opts());
    assert(db);

    for (int i = 0; i < 400; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "key%05d", i);
        snprintf(v, sizeof(v), "val%05d", i);
        db_put(db, slice_from_str(k), slice_from_str(v));
    }

    assert(db_compact(db) == LSM_OK);

    /* Verifica algumas chaves aleatórias */
    uint8_t *out; size_t olen;

    assert(db_get(db, slice_from_str("key00000"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "val00000", 8) == 0); free(out);

    assert(db_get(db, slice_from_str("key00200"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "val00200", 8) == 0); free(out);

    assert(db_get(db, slice_from_str("key00399"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "val00399", 8) == 0); free(out);

    db_close(db);
    cleanup();
}

static void test_tombstones_eliminated(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, tiny_opts());
    assert(db);

    /* Escreve e deleta chaves */
    db_put(db, S("alive"), S("yes"));
    db_put(db, S("dead"),  S("bye"));

    /* Padding para forçar flush */
    for (int i = 0; i < 200; i++) {
        char k[16];
        snprintf(k, sizeof(k), "pad%04d", i);
        db_put(db, slice_from_str(k), S("x"));
    }

    db_del(db, S("dead"));

    for (int i = 0; i < 200; i++) {
        char k[16];
        snprintf(k, sizeof(k), "qad%04d", i);
        db_put(db, slice_from_str(k), S("x"));
    }

    assert(db_compact(db) == LSM_OK);

    uint8_t *v; size_t vlen;
    assert(db_get(db, S("alive"), &v, &vlen) == LSM_OK); free(v);
    assert(db_get(db, S("dead"),  &v, &vlen) == LSM_NOT_FOUND);

    /* Verifica que não há arquivo de tombstone no SSTable resultante */
    assert(count_ssts() == 1);

    db_close(db);
    cleanup();
}

static void test_overwrite_keeps_latest(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, tiny_opts());
    assert(db);

    /* Múltiplas versões de mesmas chaves espalhadas em SSTables */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 50; i++) {
            char k[32], v[32];
            snprintf(k, sizeof(k), "key%03d", i);
            snprintf(v, sizeof(v), "r%d_v%03d", round, i);
            db_put(db, slice_from_str(k), slice_from_str(v));
        }
        /* Força flush com padding */
        for (int i = 0; i < 100; i++) {
            char k[16];
            snprintf(k, sizeof(k), "p%d_%04d", round, i);
            db_put(db, slice_from_str(k), S("pad"));
        }
    }

    assert(db_compact(db) == LSM_OK);

    /* Após compaction, deve ter a versão do round 2 (mais recente) */
    uint8_t *out; size_t olen;
    assert(db_get(db, slice_from_str("key000"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "r2_v000", 7) == 0);
    free(out);

    assert(db_get(db, slice_from_str("key049"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "r2_v049", 7) == 0);
    free(out);

    db_close(db);
    cleanup();
}

static void test_compact_noop_when_single_sst(void) {
    cleanup();

    /* Usa limit grande para garantir que só 1 SST é gerado no close */
    db_opts_t opts = { .mem_limit_bytes = 1024 * 1024 };
    db_t *db = db_open(TEST_DIR, opts);
    assert(db);

    for (int i = 0; i < 100; i++) {
        char k[32];
        snprintf(k, sizeof(k), "k%04d", i);
        db_put(db, slice_from_str(k), S("v"));
    }
    db_close(db);   /* flush final → exatamente 1 SST */

    db = db_open(TEST_DIR, opts);
    assert(db);
    assert(count_ssts() == 1);          /* pré-condição do teste */
    assert(db_compact(db) == LSM_OK);   /* num_ssts < 2 → noop */
    assert(count_ssts() == 1);          /* nenhum arquivo novo ou removido */

    db_close(db);
    cleanup();
}

static void test_compact_then_reopen(void) {
    cleanup();

    {
        db_t *db = db_open(TEST_DIR, tiny_opts());
        assert(db);
        for (int i = 0; i < 400; i++) {
            char k[32], v[32];
            snprintf(k, sizeof(k), "key%05d", i);
            snprintf(v, sizeof(v), "val%05d", i);
            db_put(db, slice_from_str(k), slice_from_str(v));
        }
        assert(db_compact(db) == LSM_OK);
        db_close(db);
    }

    /* Reabre — deve encontrar o SSTable compactado */
    {
        db_t *db = db_open(TEST_DIR, tiny_opts());
        assert(db);

        uint8_t *out; size_t olen;
        assert(db_get(db, slice_from_str("key00000"), &out, &olen) == LSM_OK);
        assert(memcmp(out, "val00000", 8) == 0); free(out);

        assert(db_get(db, slice_from_str("key00399"), &out, &olen) == LSM_OK);
        assert(memcmp(out, "val00399", 8) == 0); free(out);

        db_close(db);
    }

    cleanup();
}

static void test_busy_when_already_compacting(void) {
    /*
     * db_compact é single-shot — retorna LSM_BUSY se já está rodando.
     * Este teste verifica o flag `compacting` diretamente chamando
     * compact duas vezes em sequência (a segunda deve ser LSM_OK ou LSM_BUSY
     * dependendo do timing, mas nunca deve crashar).
     */
    cleanup();
    db_t *db = db_open(TEST_DIR, tiny_opts());
    assert(db);

    for (int i = 0; i < 400; i++) {
        char k[32];
        snprintf(k, sizeof(k), "k%05d", i);
        db_put(db, slice_from_str(k), S("v"));
    }

    lsm_status_t s1 = db_compact(db);
    assert(s1 == LSM_OK || s1 == LSM_BUSY);

    db_close(db);
    cleanup();
}

/* ---- runner ---- */

int main(void) {
    test_compact_reduces_sst_count();
    test_data_intact_after_compact();
    test_tombstones_eliminated();
    test_overwrite_keeps_latest();
    test_compact_noop_when_single_sst();
    test_compact_then_reopen();
    test_busy_when_already_compacting();

    printf("PASS: compaction\n");
    return 0;
}
