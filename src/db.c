#include "tinylsm/db.h"
#include "tinylsm/bloom.h"
#include "tinylsm/skiplist.h"
#include "tinylsm/sst.h"
#include "tinylsm/wal.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SSTS 1024u

struct db {
    char            *dir;
    db_opts_t        opts;
    wal_t           *wal;
    skiplist_t      *memtable;
    char            *sst_paths[MAX_SSTS];
    bloom_t          blooms[MAX_SSTS];
    size_t           num_ssts;
    uint64_t         sst_counter;  /* último número de SST gerado */
    pthread_rwlock_t rwlock;
};

/* --------------------------------------------------------------------------
 * Helpers de path
 * -------------------------------------------------------------------------- */

static char *mk_path(const char *dir, const char *name) {
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static void fmt_sst(char *buf, size_t sz, uint64_t n) {
    snprintf(buf, sz, "%06llu.sst",   (unsigned long long)n);
}
static void fmt_bloom(char *buf, size_t sz, uint64_t n) {
    snprintf(buf, sz, "%06llu.bloom", (unsigned long long)n);
}

/* --------------------------------------------------------------------------
 * Bloom em arquivo
 * -------------------------------------------------------------------------- */

static lsm_status_t bloom_save_file(const bloom_t *b, const char *path) {
    uint8_t *buf; size_t len;
    lsm_status_t s = bloom_encode(b, &buf, &len);
    if (s != LSM_OK) return s;
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(buf); return LSM_IO_ERROR; }
    int ok = (fwrite(buf, 1, len, fp) == len);
    fclose(fp); free(buf);
    return ok ? LSM_OK : LSM_IO_ERROR;
}

static lsm_status_t bloom_load_file(bloom_t *b, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return LSM_IO_ERROR;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz <= 0) { fclose(fp); return LSM_CORRUPTION; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return LSM_OOM; }
    int ok = ((long)fread(buf, 1, (size_t)sz, fp) == sz);
    fclose(fp);
    if (!ok) { free(buf); return LSM_IO_ERROR; }
    lsm_status_t s = bloom_decode(b, buf, (size_t)sz);
    free(buf);
    return s;
}

/* --------------------------------------------------------------------------
 * Flush — chamado com db write lock adquirido
 * -------------------------------------------------------------------------- */

static lsm_status_t db_flush_locked(db_t *db) {
    if (sl_num_entries(db->memtable) == 0) return LSM_OK;
    if (db->num_ssts >= MAX_SSTS)          return LSM_BUSY;

    uint64_t n = ++db->sst_counter;
    char fname[32];

    fmt_sst(fname, sizeof(fname), n);
    char *sp = mk_path(db->dir, fname);
    fmt_bloom(fname, sizeof(fname), n);
    char *bp = mk_path(db->dir, fname);
    if (!sp || !bp) { free(sp); free(bp); return LSM_OOM; }

    /* Bloom: estimativa conservadora a partir do total de entradas */
    size_t  est = sl_num_entries(db->memtable);
    bloom_t bloom;
    if (bloom_init(&bloom, est > 0 ? est : 1, 0.01) != LSM_OK) {
        free(sp); free(bp); return LSM_OOM;
    }

    sst_writer_t *w = sst_writer_open(sp);
    if (!w) { bloom_destroy(&bloom); free(sp); free(bp); return LSM_IO_ERROR; }

    lsm_status_t s = LSM_OK;
    sl_iter_t it;
    sl_iter_init(&it, db->memtable);
    while (sl_iter_valid(&it)) {
        bloom_add(&bloom, sl_iter_key(&it));
        s = sst_writer_add(w,
                sl_iter_op(&it), sl_iter_seq(&it),
                sl_iter_key(&it), sl_iter_value(&it));
        if (s != LSM_OK) break;
        sl_iter_next_key(&it);
    }
    sl_iter_finish(&it);

    if (s == LSM_OK) s = sst_writer_finish(w);
    sst_writer_free(w);

    if (s == LSM_OK) s = bloom_save_file(&bloom, bp);

    if (s != LSM_OK) {
        bloom_destroy(&bloom);
        unlink(sp); free(sp); free(bp);
        return s;
    }
    free(bp);

    db->sst_paths[db->num_ssts] = sp;
    db->blooms[db->num_ssts]    = bloom;
    db->num_ssts++;

    /* Novo MemTable limpo */
    sl_free(db->memtable);
    db->memtable = sl_new();
    if (!db->memtable) return LSM_OOM;

    /* Trunca WAL — dados agora estão no SSTable */
    wal_close(db->wal);
    char *wp = mk_path(db->dir, "wal.log");
    if (!wp) return LSM_OOM;
    FILE *fp = fopen(wp, "wb"); if (fp) fclose(fp);  /* trunca */
    db->wal = wal_open(wp);
    free(wp);
    return db->wal ? LSM_OK : LSM_IO_ERROR;
}

/* --------------------------------------------------------------------------
 * Recovery — varre diretório por *.sst existentes
 * -------------------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static lsm_status_t db_scan_ssts(db_t *db) {
    DIR *d = opendir(db->dir);
    if (!d) return LSM_OK;

    uint64_t nums[MAX_SSTS];
    size_t   count = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL && count < MAX_SSTS) {
        unsigned long long n;
        if (sscanf(ent->d_name, "%06llu.sst", &n) == 1)
            nums[count++] = (uint64_t)n;
    }
    closedir(d);

    qsort(nums, count, sizeof(uint64_t), cmp_u64);

    for (size_t i = 0; i < count; i++) {
        char fname[32];
        fmt_sst(fname, sizeof(fname), nums[i]);
        char *sp = mk_path(db->dir, fname);
        fmt_bloom(fname, sizeof(fname), nums[i]);
        char *bp = mk_path(db->dir, fname);
        if (!sp || !bp) { free(sp); free(bp); return LSM_OOM; }

        bloom_t bloom;
        memset(&bloom, 0, sizeof(bloom));        /* fallback: may_contain = 1 sempre */
        bloom_load_file(&bloom, bp);             /* ignora erro — SST ainda é legível */
        free(bp);

        db->sst_paths[db->num_ssts] = sp;
        db->blooms[db->num_ssts]    = bloom;
        db->num_ssts++;

        if (nums[i] > db->sst_counter)
            db->sst_counter = nums[i];
    }
    return LSM_OK;
}

/* --------------------------------------------------------------------------
 * API pública
 * -------------------------------------------------------------------------- */

db_t *db_open(const char *dir, db_opts_t opts) {
    db_t *db = calloc(1, sizeof(*db));
    if (!db) return NULL;

    db->dir = strdup(dir);
    if (!db->dir) { free(db); return NULL; }

    db->opts = opts;
    if (db->opts.mem_limit_bytes == 0)
        db->opts.mem_limit_bytes = DB_DEFAULT_MEM_LIMIT;

    mkdir(dir, 0755);  /* cria diretório; ignora EEXIST */

    if (pthread_rwlock_init(&db->rwlock, NULL) != 0) {
        free(db->dir); free(db); return NULL;
    }

    if (db_scan_ssts(db) != LSM_OK) goto fail;

    db->memtable = sl_new();
    if (!db->memtable) goto fail;

    /* Abre WAL e replaya em memtable */
    char *wp = mk_path(db->dir, "wal.log");
    if (!wp) goto fail;
    wal_recover(wp, db->memtable);   /* ignora erro — WAL pode não existir */
    db->wal = wal_open(wp);
    free(wp);
    if (!db->wal) goto fail;

    return db;

fail:
    sl_free(db->memtable);
    for (size_t i = 0; i < db->num_ssts; i++) {
        bloom_destroy(&db->blooms[i]);
        free(db->sst_paths[i]);
    }
    pthread_rwlock_destroy(&db->rwlock);
    free(db->dir);
    free(db);
    return NULL;
}

void db_close(db_t *db) {
    if (!db) return;
    pthread_rwlock_wrlock(&db->rwlock);
    db_flush_locked(db);
    pthread_rwlock_unlock(&db->rwlock);

    wal_close(db->wal);
    sl_free(db->memtable);
    for (size_t i = 0; i < db->num_ssts; i++) {
        bloom_destroy(&db->blooms[i]);
        free(db->sst_paths[i]);
    }
    pthread_rwlock_destroy(&db->rwlock);
    free(db->dir);
    free(db);
}

lsm_status_t db_put(db_t *db, slice_t key, slice_t value) {
    pthread_rwlock_wrlock(&db->rwlock);

    lsm_status_t s = wal_append(db->wal, OP_PUT, key, value);
    if (s == LSM_OK) s = wal_sync(db->wal);
    if (s == LSM_OK) s = sl_put(db->memtable, key, value);
    if (s == LSM_OK &&
        sl_mem_usage(db->memtable) > db->opts.mem_limit_bytes)
        s = db_flush_locked(db);

    pthread_rwlock_unlock(&db->rwlock);
    return s;
}

lsm_status_t db_del(db_t *db, slice_t key) {
    pthread_rwlock_wrlock(&db->rwlock);

    lsm_status_t s = wal_append(db->wal, OP_DEL, key, slice_empty());
    if (s == LSM_OK) s = wal_sync(db->wal);
    if (s == LSM_OK) s = sl_del(db->memtable, key);
    if (s == LSM_OK &&
        sl_mem_usage(db->memtable) > db->opts.mem_limit_bytes)
        s = db_flush_locked(db);

    pthread_rwlock_unlock(&db->rwlock);
    return s;
}

lsm_status_t db_get(db_t *db, slice_t key, uint8_t **out, size_t *out_len) {
    *out     = NULL;
    *out_len = 0;

    pthread_rwlock_rdlock(&db->rwlock);

    /* 1. MemTable — mais recente */
    slice_t v; sl_op_t op;
    lsm_status_t s = sl_get_raw(db->memtable, key, &v, &op);
    if (s == LSM_OK) {
        if (op == OP_PUT) {
            if (v.len > 0) {
                *out = malloc(v.len);
                if (!*out) { pthread_rwlock_unlock(&db->rwlock); return LSM_OOM; }
                memcpy(*out, v.data, v.len);
            }
            *out_len = v.len;
            pthread_rwlock_unlock(&db->rwlock);
            return LSM_OK;
        }
        /* Tombstone no MemTable — chave deletada, não busca SSTables */
        pthread_rwlock_unlock(&db->rwlock);
        return LSM_NOT_FOUND;
    }

    /* 2. SSTables do mais novo para o mais antigo */
    for (int i = (int)db->num_ssts - 1; i >= 0; i--) {
        if (!bloom_may_contain(&db->blooms[i], key)) continue;

        sst_reader_t *r = sst_reader_open(db->sst_paths[i]);
        if (!r) continue;

        sl_op_t sst_op; uint64_t seq;
        s = sst_reader_get(r, key, &sst_op, &seq, out, out_len);
        sst_reader_close(r);

        if (s == LSM_OK) {
            if (sst_op == OP_DEL) {
                free(*out); *out = NULL; *out_len = 0;
                s = LSM_NOT_FOUND;
            }
            pthread_rwlock_unlock(&db->rwlock);
            return s;
        }
        if (s != LSM_NOT_FOUND) {
            pthread_rwlock_unlock(&db->rwlock);
            return s;
        }
    }

    pthread_rwlock_unlock(&db->rwlock);
    return LSM_NOT_FOUND;
}
