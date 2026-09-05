#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tinylsm/db.h"

#define TEST_DIR "/tmp/tinylsm_test_crash"

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

/*
 * Faz fork(), executa fn(arg) no filho, filho chama _exit() sem db_close().
 * Simula crash: WAL foi fsynced, mas cleanup não foi executado.
 */
typedef void (*crash_fn_t)(void *arg);

static void run_and_crash(crash_fn_t fn, void *arg) {
  pid_t pid = fork();
  if (pid == 0) {
    fn(arg);
    _exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
}

/* ------------------------------------------------------------------ */

static void write_basic(void *arg) {
  (void)arg;
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);
  db_put(db, S("k1"), S("v1"));
  db_put(db, S("k2"), S("v2"));
  db_put(db, S("k3"), S("v3"));
  /* _exit sem db_close — WAL fsynced, dados persistem */
}

static void test_wal_recovery_after_crash(void) {
  cleanup();
  run_and_crash(write_basic, NULL);

  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  uint8_t *val;
  size_t vlen;
  assert(db_get(db, S("k1"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v1", 2) == 0);
  free(val);
  assert(db_get(db, S("k2"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v2", 2) == 0);
  free(val);
  assert(db_get(db, S("k3"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v3", 2) == 0);
  free(val);

  db_close(db);
  cleanup();
}

static void write_with_flush(void *arg) {
  (void)arg;
  db_t *db = db_open(TEST_DIR, tiny_opts());
  assert(db);
  for (int i = 0; i < 300; i++) {
    char k[32], v[32];
    snprintf(k, sizeof(k), "key%05d", i);
    snprintf(v, sizeof(v), "val%05d", i);
    db_put(db, slice_from_str(k), slice_from_str(v));
  }
  /* Crash após múltiplos flushes */
}

static void test_recovery_after_flush(void) {
  cleanup();
  run_and_crash(write_with_flush, NULL);

  db_t *db = db_open(TEST_DIR, tiny_opts());
  assert(db);

  uint8_t *val;
  size_t vlen;
  assert(db_get(db, slice_from_str("key00000"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "val00000", 8) == 0);
  free(val);

  assert(db_get(db, slice_from_str("key00100"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "val00100", 8) == 0);
  free(val);

  db_close(db);
  cleanup();
}

static void write_del_crash(void *arg) {
  (void)arg;
  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);
  db_put(db, S("alive"), S("yes"));
  db_put(db, S("dead"), S("bye"));
  db_del(db, S("dead"));
}

static void test_delete_survives_crash(void) {
  cleanup();
  run_and_crash(write_del_crash, NULL);

  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  uint8_t *val;
  size_t vlen;
  assert(db_get(db, S("alive"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "yes", 3) == 0);
  free(val);
  assert(db_get(db, S("dead"), &val, &vlen) == LSM_NOT_FOUND);

  db_close(db);
  cleanup();
}

static void write_and_compact(void *arg) {
  (void)arg;
  db_t *db = db_open(TEST_DIR, tiny_opts());
  assert(db);
  for (int i = 0; i < 300; i++) {
    char k[32], v[32];
    snprintf(k, sizeof(k), "key%05d", i);
    snprintf(v, sizeof(v), "val%05d", i);
    db_put(db, slice_from_str(k), slice_from_str(v));
  }
  db_compact(db);
  /* Crash após compaction bem-sucedida */
}

static void test_recovery_after_compaction(void) {
  cleanup();
  run_and_crash(write_and_compact, NULL);

  db_t *db = db_open(TEST_DIR, tiny_opts());
  assert(db);

  uint8_t *val;
  size_t vlen;
  assert(db_get(db, slice_from_str("key00000"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "val00000", 8) == 0);
  free(val);
  assert(db_get(db, slice_from_str("key00299"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "val00299", 8) == 0);
  free(val);

  db_close(db);
  cleanup();
}

static void test_partial_wal_recovery(void) {
  /*
   * Simula crash no meio de uma escrita: appenda lixo no WAL.
   * Records anteriores ao lixo devem ser recuperados.
   * O WAL já faz esse check por CRC — este teste confirma no nível do DB.
   */
  cleanup();

  {
    db_t *db = db_open(TEST_DIR, default_opts());
    assert(db);
    db_put(db, S("safe1"), S("v1"));
    db_put(db, S("safe2"), S("v2"));
    db_close(db);
  }

  /* Injeta escrita parcial no final do WAL */
  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s/wal.log", TEST_DIR);
  FILE *fp = fopen(wal_path, "ab");
  if (fp) {
    fwrite("CRASH_PARTIAL_RECORD", 1, 20, fp);
    fclose(fp);
  }

  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  uint8_t *val;
  size_t vlen;
  assert(db_get(db, S("safe1"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v1", 2) == 0);
  free(val);
  assert(db_get(db, S("safe2"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v2", 2) == 0);
  free(val);

  db_close(db);
  cleanup();
}

static void test_multiple_crashes(void) {
  /* Múltiplas sessões com crash — cada uma acumula sobre a anterior. */
  cleanup();

  /* Sessão 1: escreve A=1 */
  {
    pid_t pid = fork();
    if (pid == 0) {
      db_t *db = db_open(TEST_DIR, default_opts());
      assert(db);
      db_put(db, S("A"), S("1"));
      _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
  }

  /* Sessão 2: escreve B=2 sobre estado anterior */
  {
    pid_t pid = fork();
    if (pid == 0) {
      db_t *db = db_open(TEST_DIR, default_opts());
      assert(db);
      db_put(db, S("B"), S("2"));
      _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
  }

  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  uint8_t *val;
  size_t vlen;
  assert(db_get(db, S("A"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "1", 1) == 0);
  free(val);
  assert(db_get(db, S("B"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "2", 1) == 0);
  free(val);

  db_close(db);
  cleanup();
}

static void test_overwrite_survives_crash(void) {
  /* Garante que a versão mais recente é preservada após crash. */
  cleanup();

  {
    pid_t pid = fork();
    if (pid == 0) {
      db_t *db = db_open(TEST_DIR, default_opts());
      assert(db);
      db_put(db, S("key"), S("v1"));
      db_put(db, S("key"), S("v2"));
      db_put(db, S("key"), S("v3"));
      _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
  }

  db_t *db = db_open(TEST_DIR, default_opts());
  assert(db);

  uint8_t *val;
  size_t vlen;
  assert(db_get(db, S("key"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "v3", 2) == 0);
  free(val);

  db_close(db);
  cleanup();
}

/* ---- runner ---- */

int main(void) {
  test_wal_recovery_after_crash();
  test_recovery_after_flush();
  test_delete_survives_crash();
  test_recovery_after_compaction();
  test_partial_wal_recovery();
  test_multiple_crashes();
  test_overwrite_survives_crash();

  printf("PASS: crash_recovery\n");
  return 0;
}
