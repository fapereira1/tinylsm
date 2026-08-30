#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinylsm/db.h"

#define TEST_DIR "/tmp/tinylsm_test_mvcc"

static slice_t S(const char *s) { return slice_from_str(s); }

static void cleanup(void) {
  DIR *d = opendir(TEST_DIR);
  if (!d)
    return;
  struct dirent *ent;
  char path[512];
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    snprintf(path, sizeof(path), "%s/%s", TEST_DIR, ent->d_name);
    unlink(path);
  }
  closedir(d);
  rmdir(TEST_DIR);
}

static db_opts_t default_opts(void) {
  return (db_opts_t){.mem_limit_bytes = DB_DEFAULT_MEM_LIMIT};
}

static db_opts_t tiny_opts(void) {
  return (db_opts_t){.mem_limit_bytes = 2048};
}

/* ------------------------------------------------------------------ */

static void test_snapshot_does_not_see_future_writes(void) {
  cleanup();
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  db_put(db, S("k"), S("before"));

  db_snapshot_t *snap = db_snapshot_open(db);
  assert(snap);

  db_put(db, S("k"), S("after")); /* escrita APÓS o snapshot */

  uint8_t *val;
  size_t vlen;

  /* Snapshot vê "before" */
  assert(db_snapshot_get(snap, S("k"), &val, &vlen) == LSM_OK);
  assert(vlen == 6 && memcmp(val, "before", 6) == 0);
  free(val);

  /* Leitura atual vê "after" */
  assert(db_get(db, S("k"), &val, &vlen) == LSM_OK);
  assert(vlen == 5 && memcmp(val, "after", 5) == 0);
  free(val);

  db_snapshot_release(snap);
  db_close(db);
  cleanup();
}

static void test_snapshot_sees_prior_writes(void) {
  cleanup();
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  db_put(db, S("a"), S("1"));
  db_put(db, S("b"), S("2"));
  db_put(db, S("c"), S("3"));

  db_snapshot_t *snap = db_snapshot_open(db);
  assert(snap);

  uint8_t *val;
  size_t vlen;
  assert(db_snapshot_get(snap, S("a"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "1", 1) == 0);
  free(val);
  assert(db_snapshot_get(snap, S("b"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "2", 1) == 0);
  free(val);
  assert(db_snapshot_get(snap, S("c"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "3", 1) == 0);
  free(val);

  db_snapshot_release(snap);
  db_close(db);
  cleanup();
}

static void test_two_snapshots_see_different_data(void) {
  cleanup();
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  db_put(db, S("k"), S("v1"));
  db_snapshot_t *snap1 = db_snapshot_open(db);

  db_put(db, S("k"), S("v2"));
  db_snapshot_t *snap2 = db_snapshot_open(db);

  db_put(db, S("k"), S("v3"));

  uint8_t *val;
  size_t vlen;

  assert(db_snapshot_get(snap1, S("k"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v1", 2) == 0);
  free(val);

  assert(db_snapshot_get(snap2, S("k"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v2", 2) == 0);
  free(val);

  assert(db_get(db, S("k"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v3", 2) == 0);
  free(val);

  db_snapshot_release(snap1);
  db_snapshot_release(snap2);
  db_close(db);
  cleanup();
}

static void test_snapshot_not_found_before_write(void) {
  cleanup();
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  db_snapshot_t *snap = db_snapshot_open(db);

  db_put(db, S("new_key"), S("value"));

  uint8_t *val;
  size_t vlen;
  /* Snapshot foi tirado antes da escrita — NOT_FOUND */
  assert(db_snapshot_get(snap, S("new_key"), &val, &vlen) == LSM_NOT_FOUND);

  /* Leitura atual encontra */
  assert(db_get(db, S("new_key"), &val, &vlen) == LSM_OK);
  free(val);

  db_snapshot_release(snap);
  db_close(db);
  cleanup();
}

static void test_snapshot_sees_tombstone(void) {
  cleanup();
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  db_put(db, S("x"), S("exists"));
  db_snapshot_t *snap_before_del = db_snapshot_open(db);

  db_del(db, S("x"));
  db_snapshot_t *snap_after_del = db_snapshot_open(db);

  uint8_t *val;
  size_t vlen;

  /* Snapshot antes do delete vê a chave */
  assert(db_snapshot_get(snap_before_del, S("x"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "exists", 6) == 0);
  free(val);

  /* Snapshot após o delete NÃO vê a chave */
  assert(db_snapshot_get(snap_after_del, S("x"), &val, &vlen) == LSM_NOT_FOUND);

  db_snapshot_release(snap_before_del);
  db_snapshot_release(snap_after_del);
  db_close(db);
  cleanup();
}

static void test_snapshot_across_flush(void) {
  /*
   * Verifica que seq é contínuo entre flushes de MemTable.
   * O snapshot tirado antes do flush deve ver os dados corretos
   * mesmo depois que o MemTable é descartado e os dados vão para SSTable.
   */
  cleanup();
  db_t *db = db_open(TEST_DIR, tiny_opts());
  assert(db);

  db_put(db, S("stable"), S("value"));

  db_snapshot_t *snap = db_snapshot_open(db);
  assert(snap);

  /* Força múltiplos flushes com escritas após o snapshot */
  for (int i = 0; i < 300; i++) {
    char k[32], v[32];
    snprintf(k, sizeof(k), "new%05d", i);
    snprintf(v, sizeof(v), "val%05d", i);
    db_put(db, slice_from_str(k), slice_from_str(v));
  }

  uint8_t *val;
  size_t vlen;

  /* "stable" foi escrito antes do snapshot — deve ser visível */
  assert(db_snapshot_get(snap, S("stable"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "value", 5) == 0);
  free(val);

  /* "new*" foram escritas após o snapshot — não devem ser visíveis */
  assert(db_snapshot_get(snap, slice_from_str("new00000"), &val, &vlen) ==
         LSM_NOT_FOUND);

  db_snapshot_release(snap);
  db_close(db);
  cleanup();
}

static void test_snapshot_across_compaction(void) {
  /*
   * Compaction descarta versões antigas — não pode rodar com snapshots
   * ativos. O comportamento correto é retornar LSM_BUSY.
   * Após liberar o snapshot, a compaction deve funcionar normalmente.
   */
  cleanup();
  db_t *db = db_open(TEST_DIR, tiny_opts());
  assert(db);

  db_put(db, S("k"), S("v1"));
  db_snapshot_t *snap = db_snapshot_open(db);

  for (int i = 0; i < 300; i++) {
    char k[32];
    snprintf(k, sizeof(k), "fill%05d", i);
    db_put(db, slice_from_str(k), S("x"));
  }
  db_put(db, S("k"), S("v2"));

  /* Compaction bloqueada enquanto snapshot está aberto */
  assert(db_compact(db) == LSM_BUSY);

  /* Snapshot ainda vê v1 */
  uint8_t *val;
  size_t vlen;
  assert(db_snapshot_get(snap, S("k"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v1", 2) == 0);
  free(val);

  /* Libera snapshot — agora compaction pode rodar */
  db_snapshot_release(snap);
  assert(db_compact(db) == LSM_OK);

  /* Leitura atual vê v2 */
  assert(db_get(db, S("k"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v2", 2) == 0);
  free(val);

  db_close(db);
  cleanup();
}

/* ---- concorrência ---- */

typedef struct {
  db_t *db;
  db_snapshot_t *snap;
  int writes_done;
} thread_ctx_t;

static void *write_thread(void *arg) {
  thread_ctx_t *ctx = arg;
  for (int i = 0; i < 500; i++) {
    char k[32], v[32];
    snprintf(k, sizeof(k), "w%05d", i);
    snprintf(v, sizeof(v), "v%05d", i);
    db_put(ctx->db, slice_from_str(k), slice_from_str(v));
  }
  ctx->writes_done = 1;
  return NULL;
}

static void test_concurrent_write_does_not_affect_snapshot(void) {
  cleanup();
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  db_put(db, S("anchor"), S("stable"));

  thread_ctx_t ctx = {.db = db, .snap = NULL, .writes_done = 0};
  ctx.snap = db_snapshot_open(db);
  assert(ctx.snap);

  pthread_t t;
  pthread_create(&t, NULL, write_thread, &ctx);

  /* Lê do snapshot enquanto writes acontecem */
  uint8_t *val;
  size_t vlen;
  for (int i = 0; i < 100; i++) {
    assert(db_snapshot_get(ctx.snap, S("anchor"), &val, &vlen) == LSM_OK);
    assert(memcmp(val, "stable", 6) == 0);
    free(val);
  }

  pthread_join(t, NULL);

  /* Snapshot não vê nenhuma das chaves escritas pela thread */
  assert(db_snapshot_get(ctx.snap, slice_from_str("w00000"), &val, &vlen) ==
         LSM_NOT_FOUND);

  db_snapshot_release(ctx.snap);
  db_close(db);
  cleanup();
}

/* ---- runner ---- */

int main(void) {
  test_snapshot_does_not_see_future_writes();
  test_snapshot_sees_prior_writes();
  test_two_snapshots_see_different_data();
  test_snapshot_not_found_before_write();
  test_snapshot_sees_tombstone();
  test_snapshot_across_flush();
  test_snapshot_across_compaction();
  test_concurrent_write_does_not_affect_snapshot();

  printf("PASS: mvcc\n");
  return 0;
}
