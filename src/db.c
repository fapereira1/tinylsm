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
  char *dir;
  db_opts_t opts;
  wal_t *wal;
  skiplist_t *memtable;
  char *sst_paths[MAX_SSTS];
  bloom_t blooms[MAX_SSTS];
  size_t num_ssts;
  uint64_t sst_counter; /* último número de SST gerado */
  uint64_t global_seq;
  size_t num_snapshots; /* <-- novo */

  pthread_rwlock_t rwlock;
  int compacting;
};

/* --------------------------------------------------------------------------
 * Helpers de path
 * -------------------------------------------------------------------------- */

static char *mk_path(const char *dir, const char *name) {
  size_t n = strlen(dir) + 1 + strlen(name) + 1;
  char *p = malloc(n);
  if (p)
    snprintf(p, n, "%s/%s", dir, name);
  return p;
}

static void fmt_sst(char *buf, size_t sz, uint64_t n) {
  snprintf(buf, sz, "%06llu.sst", (unsigned long long)n);
}
static void fmt_bloom(char *buf, size_t sz, uint64_t n) {
  snprintf(buf, sz, "%06llu.bloom", (unsigned long long)n);
}

/* --------------------------------------------------------------------------
 * Bloom em arquivo
 * -------------------------------------------------------------------------- */

static lsm_status_t bloom_save_file(const bloom_t *b, const char *path) {
  uint8_t *buf;
  size_t len;
  lsm_status_t s = bloom_encode(b, &buf, &len);
  if (s != LSM_OK)
    return s;
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    free(buf);
    return LSM_IO_ERROR;
  }
  int ok = (fwrite(buf, 1, len, fp) == len);
  fclose(fp);
  free(buf);
  return ok ? LSM_OK : LSM_IO_ERROR;
}

static lsm_status_t bloom_load_file(bloom_t *b, const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return LSM_IO_ERROR;
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  rewind(fp);
  if (sz <= 0) {
    fclose(fp);
    return LSM_CORRUPTION;
  }
  uint8_t *buf = malloc((size_t)sz);
  if (!buf) {
    fclose(fp);
    return LSM_OOM;
  }
  int ok = ((long)fread(buf, 1, (size_t)sz, fp) == sz);
  fclose(fp);
  if (!ok) {
    free(buf);
    return LSM_IO_ERROR;
  }
  lsm_status_t s = bloom_decode(b, buf, (size_t)sz);
  free(buf);
  return s;
}

/* --------------------------------------------------------------------------
 * Flush — chamado com db write lock adquirido
 * -------------------------------------------------------------------------- */

static lsm_status_t db_flush_locked(db_t *db) {
  if (sl_num_entries(db->memtable) == 0)
    return LSM_OK;
  if (db->num_ssts >= MAX_SSTS)
    return LSM_BUSY;

  uint64_t n = ++db->sst_counter;
  char fname[32];

  fmt_sst(fname, sizeof(fname), n);
  char *sp = mk_path(db->dir, fname);
  fmt_bloom(fname, sizeof(fname), n);
  char *bp = mk_path(db->dir, fname);
  if (!sp || !bp) {
    free(sp);
    free(bp);
    return LSM_OOM;
  }

  /* Bloom: estimativa conservadora a partir do total de entradas */
  size_t est = sl_num_entries(db->memtable);
  bloom_t bloom;
  if (bloom_init(&bloom, est > 0 ? est : 1, 0.01) != LSM_OK) {
    free(sp);
    free(bp);
    return LSM_OOM;
  }

  sst_writer_t *w = sst_writer_open(sp);
  if (!w) {
    bloom_destroy(&bloom);
    free(sp);
    free(bp);
    return LSM_IO_ERROR;
  }

  lsm_status_t s = LSM_OK;
  sl_iter_t it;
  sl_iter_init(&it, db->memtable);
  while (sl_iter_valid(&it)) {
    bloom_add(&bloom, sl_iter_key(&it));
    s = sst_writer_add(w, sl_iter_op(&it), sl_iter_seq(&it), sl_iter_key(&it),
                       sl_iter_value(&it));
    if (s != LSM_OK)
      break;
    sl_iter_next_key(&it);
  }
  sl_iter_finish(&it);

  if (s == LSM_OK)
    s = sst_writer_finish(w);
  sst_writer_free(w);

  if (s == LSM_OK)
    s = bloom_save_file(&bloom, bp);

  if (s != LSM_OK) {
    bloom_destroy(&bloom);
    unlink(sp);
    free(sp);
    free(bp);
    return s;
  }
  free(bp);

  db->sst_paths[db->num_ssts] = sp;
  db->blooms[db->num_ssts] = bloom;
  db->num_ssts++;

  /* Novo MemTable limpo */
  db->global_seq = sl_current_seq(db->memtable); /* preserva continuidade */
  sl_free(db->memtable);
  db->memtable = sl_new();
  if (!db->memtable)
    return LSM_OOM;
  sl_set_initial_seq(db->memtable,
                     db->global_seq); /* novo MemTable continua do mesmo seq */

  /* Trunca WAL — dados agora estão no SSTable */
  wal_close(db->wal);
  char *wp = mk_path(db->dir, "wal.log");
  if (!wp)
    return LSM_OOM;
  FILE *fp = fopen(wp, "wb");
  if (fp)
    fclose(fp); /* trunca */
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
  if (!d)
    return LSM_OK;

  uint64_t nums[MAX_SSTS];
  size_t count = 0;
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
    if (!sp || !bp) {
      free(sp);
      free(bp);
      return LSM_OOM;
    }

    bloom_t bloom;
    memset(&bloom, 0, sizeof(bloom)); /* fallback: may_contain = 1 sempre */
    bloom_load_file(&bloom, bp);      /* ignora erro — SST ainda é legível */
    free(bp);

    db->sst_paths[db->num_ssts] = sp;
    db->blooms[db->num_ssts] = bloom;
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
  if (!db)
    return NULL;

  db->dir = strdup(dir);
  if (!db->dir) {
    free(db);
    return NULL;
  }

  db->opts = opts;
  if (db->opts.mem_limit_bytes == 0)
    db->opts.mem_limit_bytes = DB_DEFAULT_MEM_LIMIT;

  mkdir(dir, 0755); /* cria diretório; ignora EEXIST */

  if (pthread_rwlock_init(&db->rwlock, NULL) != 0) {
    free(db->dir);
    free(db);
    return NULL;
  }

  if (db_scan_ssts(db) != LSM_OK)
    goto fail;

  db->memtable = sl_new();
  if (!db->memtable)
    goto fail;

  /* Abre WAL e replaya em memtable */
  char *wp = mk_path(db->dir, "wal.log");
  if (!wp)
    goto fail;
  wal_recover(wp, db->memtable); /* ignora erro — WAL pode não existir */
  db->wal = wal_open(wp);
  free(wp);
  if (!db->wal)
    goto fail;

  db->global_seq = sl_current_seq(db->memtable);

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
  if (!db)
    return;
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
  if (s == LSM_OK)
    s = wal_sync(db->wal);
  if (s == LSM_OK)
    s = sl_put(db->memtable, key, value);
  if (s == LSM_OK && sl_mem_usage(db->memtable) > db->opts.mem_limit_bytes)
    s = db_flush_locked(db);

  pthread_rwlock_unlock(&db->rwlock);
  return s;
}

lsm_status_t db_del(db_t *db, slice_t key) {
  pthread_rwlock_wrlock(&db->rwlock);

  lsm_status_t s = wal_append(db->wal, OP_DEL, key, slice_empty());
  if (s == LSM_OK)
    s = wal_sync(db->wal);
  if (s == LSM_OK)
    s = sl_del(db->memtable, key);
  if (s == LSM_OK && sl_mem_usage(db->memtable) > db->opts.mem_limit_bytes)
    s = db_flush_locked(db);

  pthread_rwlock_unlock(&db->rwlock);
  return s;
}

lsm_status_t db_get(db_t *db, slice_t key, uint8_t **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0;

  pthread_rwlock_rdlock(&db->rwlock);

  /* 1. MemTable — mais recente */
  slice_t v;
  sl_op_t op;
  lsm_status_t s = sl_get_raw(db->memtable, key, &v, &op);
  if (s == LSM_OK) {
    if (op == OP_PUT) {
      if (v.len > 0) {
        *out = malloc(v.len);
        if (!*out) {
          pthread_rwlock_unlock(&db->rwlock);
          return LSM_OOM;
        }
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
    if (!bloom_may_contain(&db->blooms[i], key))
      continue;

    sst_reader_t *r = sst_reader_open(db->sst_paths[i]);
    if (!r)
      continue;

    sl_op_t sst_op;
    uint64_t seq;
    s = sst_reader_get(r, key, &sst_op, &seq, out, out_len);
    sst_reader_close(r);

    if (s == LSM_OK) {
      if (sst_op == OP_DEL) {
        free(*out);
        *out = NULL;
        *out_len = 0;
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

/* --------------------------------------------------------------------------
 * Compaction — k-way merge de todos os SSTables
 * -------------------------------------------------------------------------- */

typedef struct {
  sst_reader_t *reader;
  sst_iter_t iter;
  int valid;
} merge_src_t;

static void merge_srcs_close(merge_src_t *srcs, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!srcs[i].reader)
      continue;
    sst_iter_finish(&srcs[i].iter);
    sst_reader_close(srcs[i].reader);
    srcs[i].reader = NULL;
  }
  free(srcs);
}

static int any_valid(merge_src_t *srcs, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (srcs[i].valid)
      return 1;
  return 0;
}

/* Retorna a menor chave corrente entre todos os iteradores válidos. */
static slice_t find_min_key(merge_src_t *srcs, size_t n) {
  slice_t min = slice_empty();
  int found = 0;
  for (size_t i = 0; i < n; i++) {
    if (!srcs[i].valid)
      continue;
    slice_t k = sst_iter_key(&srcs[i].iter);
    if (!found || slice_cmp(k, min) < 0) {
      min = k;
      found = 1;
    }
  }
  return min;
}

/* Deriva o path do .bloom a partir do path do .sst */
static char *sst_to_bloom_path(const char *sst_path) {
  size_t len = strlen(sst_path);
  char *bp = malloc(len + 3); /* .sst → .bloom: 3 bytes extras */
  if (!bp)
    return NULL;
  memcpy(bp, sst_path, len + 1);
  char *ext = strrchr(bp, '.');
  if (ext)
    memcpy(ext, ".bloom", 7);
  return bp;
}

lsm_status_t db_compact(db_t *db) {
  /* ----------------------------------------------------------------
   * Fase 1: snapshot do estado atual (write lock brevemente)
   * ---------------------------------------------------------------- */
  pthread_rwlock_wrlock(&db->rwlock);

  if (db->compacting) {
    pthread_rwlock_unlock(&db->rwlock);
    return LSM_BUSY;
  }
  if (db->num_snapshots > 0) { /* <-- ANTES do num_ssts check */
    pthread_rwlock_unlock(&db->rwlock);
    return LSM_BUSY;
  }

  if (db->num_ssts < 2) {
    pthread_rwlock_unlock(&db->rwlock);
    return LSM_OK;
  }

  size_t n = db->num_ssts;
  uint64_t new_n = ++db->sst_counter;
  db->compacting = 1;

  /* Copia os paths dos SSTables a compactar */
  char **paths_snap = malloc(n * sizeof(char *));
  if (!paths_snap) {
    db->compacting = 0;
    pthread_rwlock_unlock(&db->rwlock);
    return LSM_OOM;
  }
  memcpy(paths_snap, db->sst_paths, n * sizeof(char *));

  pthread_rwlock_unlock(&db->rwlock);
  /* Lock liberado — reads e writes continuam normalmente no MemTable */

  /* ----------------------------------------------------------------
   * Fase 2: abre iteradores e faz o merge (sem lock)
   * ---------------------------------------------------------------- */
  merge_src_t *srcs = calloc(n, sizeof(*srcs));
  if (!srcs) {
    free(paths_snap);
    goto fail_early;
  }

  lsm_status_t s = LSM_OK;
  for (size_t i = 0; i < n; i++) {
    srcs[i].reader = sst_reader_open(paths_snap[i]);
    if (!srcs[i].reader) {
      s = LSM_IO_ERROR;
      goto fail_merge;
    }
    sst_iter_init(&srcs[i].iter, srcs[i].reader);
    srcs[i].valid = sst_iter_valid(&srcs[i].iter);
  }

  /* Prepara SSTable e Bloom de saída */
  char fname[32];
  fmt_sst(fname, sizeof(fname), new_n);
  char *new_sst_path = mk_path(db->dir, fname);
  fmt_bloom(fname, sizeof(fname), new_n);
  char *new_bloom_path = mk_path(db->dir, fname);
  if (!new_sst_path || !new_bloom_path) {
    s = LSM_OOM;
    goto fail_paths;
  }

  bloom_t new_bloom;
  if (bloom_init(&new_bloom, n * 1000, 0.01) != LSM_OK) {
    s = LSM_OOM;
    goto fail_paths;
  }

  sst_writer_t *w = sst_writer_open(new_sst_path);
  if (!w) {
    bloom_destroy(&new_bloom);
    s = LSM_IO_ERROR;
    goto fail_paths;
  }

  /* K-way merge */
  while (s == LSM_OK && any_valid(srcs, n)) {
    slice_t min_key = find_min_key(srcs, n);

    /*
     * Copia min_key para buffer local ANTES de avançar qualquer iterador.
     * Os dados dos iteradores vivem em blk_data que é liberado ao avançar
     * para um novo bloco.
     */
    uint8_t *mk_buf = malloc(min_key.len > 0 ? min_key.len : 1);
    if (!mk_buf) {
      s = LSM_OOM;
      break;
    }
    memcpy(mk_buf, min_key.data, min_key.len);
    slice_t mk = slice_make(mk_buf, min_key.len);

    /*
     * Vencedor: o SSTable de índice mais alto (mais novo) que contém mk.
     * Iteramos de trás para frente — o primeiro match é o vencedor.
     */
    int winner = -1;
    for (int i = (int)n - 1; i >= 0; i--) {
      if (srcs[i].valid && slice_eq(sst_iter_key(&srcs[i].iter), mk)) {
        winner = i;
        break;
      }
    }

    /*
     * Grava vencedor no SSTable de saída ANTES de avançar qualquer
     * iterador — os dados ainda são válidos neste momento.
     * Tombstones (OP_DEL) são descartados: compaction total não precisa
     * preservá-los (não há SSTables mais antigos após o merge).
     */
    if (winner >= 0 && sst_iter_op(&srcs[winner].iter) == OP_PUT) {
      s = sst_writer_add(w, OP_PUT, sst_iter_seq(&srcs[winner].iter),
                         sst_iter_key(&srcs[winner].iter),
                         sst_iter_value(&srcs[winner].iter));
      if (s == LSM_OK)
        bloom_add(&new_bloom, sst_iter_key(&srcs[winner].iter));
    }

    /* Avança TODOS os iteradores que apontam para mk */
    for (size_t i = 0; i < n; i++) {
      if (!srcs[i].valid)
        continue;
      if (slice_eq(sst_iter_key(&srcs[i].iter), mk)) {
        sst_iter_next(&srcs[i].iter);
        srcs[i].valid = sst_iter_valid(&srcs[i].iter);
      }
    }

    free(mk_buf);
  }

  merge_srcs_close(srcs, n);
  srcs = NULL;

  if (s == LSM_OK)
    s = sst_writer_finish(w);
  sst_writer_free(w);

  if (s == LSM_OK)
    s = bloom_save_file(&new_bloom, new_bloom_path);
  free(new_bloom_path);

  if (s != LSM_OK) {
    bloom_destroy(&new_bloom);
    unlink(new_sst_path);
    free(new_sst_path);
    free(paths_snap);
    pthread_rwlock_wrlock(&db->rwlock);
    db->compacting = 0;
    pthread_rwlock_unlock(&db->rwlock);
    return s;
  }

  /* ----------------------------------------------------------------
   * Fase 3: swap atômico da lista de SSTables (write lock brevemente)
   * ---------------------------------------------------------------- */
  pthread_rwlock_wrlock(&db->rwlock);

  /*
   * SSTables [0..n-1]: os que compactamos (a substituir)
   * SSTables [n..num_ssts-1]: adicionados por flushes durante compaction
   *
   * Resultado: [compacted] + [flushes durante compaction]
   */
  size_t remaining = db->num_ssts - n;

  /* Salva blooms antigos para destruição (após liberar o lock) */
  bloom_t *old_blooms = malloc(n * sizeof(bloom_t));
  if (old_blooms)
    memcpy(old_blooms, db->blooms, n * sizeof(bloom_t));

  if (remaining > 0) {
    memmove(&db->sst_paths[1], &db->sst_paths[n], remaining * sizeof(char *));
    memmove(&db->blooms[1], &db->blooms[n], remaining * sizeof(bloom_t));
  }

  db->sst_paths[0] = new_sst_path;
  db->blooms[0] = new_bloom;
  db->num_ssts = 1 + remaining;
  db->compacting = 0;

  pthread_rwlock_unlock(&db->rwlock);

  /* Fase 4: limpeza dos arquivos antigos (fora do lock) */
  for (size_t i = 0; i < n; i++) {
    if (old_blooms)
      bloom_destroy(&old_blooms[i]);
    char *bp = sst_to_bloom_path(paths_snap[i]);
    if (bp) {
      unlink(bp);
      free(bp);
    }
    unlink(paths_snap[i]);
    free(paths_snap[i]); /* <-- estava faltando */
  }
  free(old_blooms);
  free(paths_snap);
  return LSM_OK;

fail_paths:
  free(new_sst_path);
  free(new_bloom_path);
fail_merge:
  if (srcs)
    merge_srcs_close(srcs, n);
fail_early:
  free(paths_snap);
  pthread_rwlock_wrlock(&db->rwlock);
  db->compacting = 0;
  pthread_rwlock_unlock(&db->rwlock);
  return s == LSM_OK ? LSM_OOM : s;
}

/* --------------------------------------------------------------------------
 * Snapshot API
 * -------------------------------------------------------------------------- */

db_snapshot_t *db_snapshot_open(db_t *db) {
  db_snapshot_t *snap = malloc(sizeof(*snap));
  if (!snap)
    return NULL;

  /* wrlock — modifica num_snapshots e lê seq atomicamente */
  pthread_rwlock_wrlock(&db->rwlock);
  snap->db = db;
  snap->seq = sl_current_seq(db->memtable);
  db->num_snapshots++; /* <-- estava faltando */
  pthread_rwlock_unlock(&db->rwlock);

  return snap;
}

void db_snapshot_release(db_snapshot_t *snap) {
  pthread_rwlock_wrlock(&snap->db->rwlock);
  snap->db->num_snapshots--; /* <-- estava faltando */
  pthread_rwlock_unlock(&snap->db->rwlock);
  free(snap);
}

lsm_status_t db_snapshot_get(db_snapshot_t *snap, slice_t key, uint8_t **out,
                             size_t *out_len) {
  *out = NULL;
  *out_len = 0;

  pthread_rwlock_rdlock(&snap->db->rwlock);

  /* 1. MemTable — só entradas com seq ≤ snapshot_seq */
  slice_t v;
  sl_op_t op;
  lsm_status_t s = sl_get_at_seq(snap->db->memtable, key, snap->seq, &v, &op);
  if (s == LSM_OK) {
    if (op == OP_PUT) {
      if (v.len > 0) {
        *out = malloc(v.len);
        if (!*out) {
          pthread_rwlock_unlock(&snap->db->rwlock);
          return LSM_OOM;
        }
        memcpy(*out, v.data, v.len);
      }
      *out_len = v.len;
      pthread_rwlock_unlock(&snap->db->rwlock);
      return LSM_OK;
    }
    /* Tombstone visível no snapshot — chave foi deletada antes do snap */
    pthread_rwlock_unlock(&snap->db->rwlock);
    return LSM_NOT_FOUND;
  }

  /* 2. SSTables do mais novo para o mais antigo, filtrando por seq */
  for (int i = (int)snap->db->num_ssts - 1; i >= 0; i--) {
    if (!bloom_may_contain(&snap->db->blooms[i], key))
      continue;

    sst_reader_t *r = sst_reader_open(snap->db->sst_paths[i]);
    if (!r)
      continue;

    sl_op_t sst_op;
    uint64_t seq;
    s = sst_reader_get(r, key, &sst_op, &seq, out, out_len);
    sst_reader_close(r);

    if (s == LSM_OK) {
      if (seq > snap->seq) {
        /*
         * Esta versão foi escrita APÓS o snapshot.
         * Descarta e tenta o SSTable mais antigo — pode ter
         * uma versão anterior da mesma chave.
         */
        free(*out);
        *out = NULL;
        *out_len = 0;
        continue;
      }
      if (sst_op == OP_DEL) {
        *out_len = 0;
        s = LSM_NOT_FOUND;
      }
      pthread_rwlock_unlock(&snap->db->rwlock);
      return s;
    }
    if (s != LSM_NOT_FOUND) {
      pthread_rwlock_unlock(&snap->db->rwlock);
      return s;
    }
  }

  pthread_rwlock_unlock(&snap->db->rwlock);
  return LSM_NOT_FOUND;
}
