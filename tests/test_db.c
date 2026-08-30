#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinylsm/db.h"

#define TEST_DIR "/tmp/tinylsm_test_db"

static slice_t S(const char *s) { return slice_from_str(s); }

/* Remove todos os arquivos do diretório e o diretório em si */
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

static db_opts_t default_opts(void) {
    return (db_opts_t){ .mem_limit_bytes = DB_DEFAULT_MEM_LIMIT };
}

/* opts com limite pequeno para forçar flush rápido */
static db_opts_t tiny_opts(void) {
    return (db_opts_t){ .mem_limit_bytes = 4096 };
}

/* --------------------------------------------------------------------------
 * Testes
 * -------------------------------------------------------------------------- */

static void test_basic_put_get(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, default_opts());
    assert(db);

    assert(db_put(db, S("name"),  S("alice")) == LSM_OK);
    assert(db_put(db, S("city"),  S("recife")) == LSM_OK);
    assert(db_put(db, S("lang"),  S("c11"))   == LSM_OK);

    uint8_t *v; size_t vlen;
    assert(db_get(db, S("name"), &v, &vlen) == LSM_OK);
    assert(vlen == 5 && memcmp(v, "alice", 5) == 0); free(v);

    assert(db_get(db, S("city"), &v, &vlen) == LSM_OK);
    assert(memcmp(v, "recife", 6) == 0); free(v);

    assert(db_get(db, S("absent"), &v, &vlen) == LSM_NOT_FOUND);

    db_close(db);
    cleanup();
}

static void test_delete(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, default_opts());
    assert(db);

    db_put(db, S("k"), S("v"));
    assert(db_del(db, S("k")) == LSM_OK);

    uint8_t *v; size_t vlen;
    assert(db_get(db, S("k"), &v, &vlen) == LSM_NOT_FOUND);

    db_close(db);
    cleanup();
}

static void test_overwrite(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, default_opts());
    assert(db);

    db_put(db, S("k"), S("v1"));
    db_put(db, S("k"), S("v2"));
    db_put(db, S("k"), S("v3"));

    uint8_t *v; size_t vlen;
    assert(db_get(db, S("k"), &v, &vlen) == LSM_OK);
    assert(vlen == 2 && memcmp(v, "v3", 2) == 0); free(v);

    db_close(db);
    cleanup();
}

static void test_flush_and_get(void) {
    /*
     * mem_limit pequeno → cada put pode disparar um flush.
     * Verifica que dados no SSTable são legíveis.
     */
    cleanup();
    db_t *db = db_open(TEST_DIR, tiny_opts());
    assert(db);

    for (int i = 0; i < 200; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "key%05d", i);
        snprintf(v, sizeof(v), "val%05d", i);
        assert(db_put(db, slice_from_str(k), slice_from_str(v)) == LSM_OK);
    }

    uint8_t *out; size_t olen;
    assert(db_get(db, slice_from_str("key00000"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "val00000", 8) == 0); free(out);

    assert(db_get(db, slice_from_str("key00100"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "val00100", 8) == 0); free(out);

    assert(db_get(db, slice_from_str("key00199"), &out, &olen) == LSM_OK);
    assert(memcmp(out, "val00199", 8) == 0); free(out);

    db_close(db);
    cleanup();
}

static void test_tombstone_survives_flush(void) {
    cleanup();
    db_t *db = db_open(TEST_DIR, tiny_opts());
    assert(db);

    /* Escreve e deleta dentro de um flush */
    db_put(db, S("gone"), S("here"));
    db_del(db, S("gone"));

    /* Escreve mais dados para forçar flush */
    for (int i = 0; i < 200; i++) {
        char k[32];
        snprintf(k, sizeof(k), "pad%05d", i);
        db_put(db, slice_from_str(k), S("padding"));
    }

    uint8_t *v; size_t vlen;
    assert(db_get(db, S("gone"), &v, &vlen) == LSM_NOT_FOUND);

    db_close(db);
    cleanup();
}

static void test_reopen_persists(void) {
    cleanup();

    /* Sessão 1 — escreve e fecha */
    {
        db_t *db = db_open(TEST_DIR, default_opts());
        assert(db);
        db_put(db, S("persistent"), S("yes"));
        db_put(db, S("another"),    S("value"));
        db_close(db);
    }

    /* Sessão 2 — reabre e verifica */
    {
        db_t *db = db_open(TEST_DIR, default_opts());
        assert(db);

        uint8_t *v; size_t vlen;
        assert(db_get(db, S("persistent"), &v, &vlen) == LSM_OK);
        assert(vlen == 3 && memcmp(v, "yes", 3) == 0); free(v);

        assert(db_get(db, S("another"), &v, &vlen) == LSM_OK);
        assert(memcmp(v, "value", 5) == 0); free(v);

        db_close(db);
    }

    cleanup();
}

static void test_reopen_after_flush(void) {
    cleanup();

    /* Sessão 1 — muitos writes para gerar SSTables */
    {
        db_t *db = db_open(TEST_DIR, tiny_opts());
        assert(db);
        for (int i = 0; i < 300; i++) {
            char k[32], v[32];
            snprintf(k, sizeof(k), "key%05d", i);
            snprintf(v, sizeof(v), "val%05d", i);
            db_put(db, slice_from_str(k), slice_from_str(v));
        }
        db_close(db);
    }

    /* Sessão 2 — verifica persistência via SSTables */
    {
        db_t *db = db_open(TEST_DIR, tiny_opts());
        assert(db);

        uint8_t *out; size_t olen;
        assert(db_get(db, slice_from_str("key00000"), &out, &olen) == LSM_OK);
        assert(memcmp(out, "val00000", 8) == 0); free(out);

        assert(db_get(db, slice_from_str("key00299"), &out, &olen) == LSM_OK);
        assert(memcmp(out, "val00299", 8) == 0); free(out);

        db_close(db);
    }

    cleanup();
}

static void test_delete_then_reopen(void) {
    cleanup();

    {
        db_t *db = db_open(TEST_DIR, tiny_opts());
        assert(db);
        db_put(db, S("x"), S("1"));
        /* Força flush com dados extras */
        for (int i = 0; i < 200; i++) {
            char k[16];
            snprintf(k, sizeof(k), "p%05d", i);
            db_put(db, slice_from_str(k), S("v"));
        }
        db_del(db, S("x"));
        db_close(db);
    }

    {
        db_t *db = db_open(TEST_DIR, tiny_opts());
        assert(db);
        uint8_t *v; size_t vlen;
        /* x foi deletado — mesmo após reopen deve ser NOT_FOUND */
        assert(db_get(db, S("x"), &v, &vlen) == LSM_NOT_FOUND);
        db_close(db);
    }

    cleanup();
}

/* --------------------------------------------------------------------------
 * Runner
 * -------------------------------------------------------------------------- */

int main(void) {
    test_basic_put_get();
    test_delete();
    test_overwrite();
    test_flush_and_get();
    test_tombstone_survives_flush();
    test_reopen_persists();
    test_reopen_after_flush();
    test_delete_then_reopen();

    printf("PASS: db\n");
    return 0;
}
