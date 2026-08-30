#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinylsm/db.h"
#include "tinylsm/index.h"

#define MAIN_DIR "/tmp/tinylsm_test_main"
#define IDX_DIR "/tmp/tinylsm_test_idx"

static slice_t S(const char *s) { return slice_from_str(s); }

static void cleanup_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *ent;
  char path[512];
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    unlink(path);
  }
  closedir(d);
  rmdir(dir);
}

static void cleanup(void) {
  cleanup_dir(MAIN_DIR);
  cleanup_dir(IDX_DIR);
}

static db_opts_t opts(void) {
  return (db_opts_t){.mem_limit_bytes = DB_DEFAULT_MEM_LIMIT};
}

/* ------------------------------------------------------------------ */

static void test_basic_index(void) {
  cleanup();
  db_t *main = db_open(MAIN_DIR, opts());
  assert(main);

  /* Insere registros no main db */
  assert(db_put(main, S("u001"), S("alice:recife:alice@x.com")) == LSM_OK);
  assert(db_put(main, S("u002"), S("bob:sao_paulo:bob@x.com")) == LSM_OK);
  assert(db_put(main, S("u003"), S("carol:recife:carol@x.com")) == LSM_OK);

  /* Abre índice por email */
  sec_index_t *idx = idx_open(main, IDX_DIR);
  assert(idx);

  /* Popula o índice: email → user_id */
  assert(idx_put(idx, S("alice@x.com"), S("u001")) == LSM_OK);
  assert(idx_put(idx, S("bob@x.com"), S("u002")) == LSM_OK);
  assert(idx_put(idx, S("carol@x.com"), S("u003")) == LSM_OK);

  /* Busca pelo índice */
  uint8_t *val;
  size_t vlen;
  assert(idx_get(idx, S("alice@x.com"), &val, &vlen) == LSM_OK);
  assert(vlen == 24 && memcmp(val, "alice:recife:alice@x.com", 24) == 0);
  free(val);

  assert(idx_get(idx, S("bob@x.com"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "bob:sao_paulo:bob@x.com", 23) == 0);
  free(val);

  idx_close(idx);
  db_close(main);
  cleanup();
}

static void test_lookup_primary(void) {
  cleanup();
  db_t *main = db_open(MAIN_DIR, opts());
  assert(main);
  db_put(main, S("p001"), S("produto_a"));

  sec_index_t *idx = idx_open(main, IDX_DIR);
  assert(idx);
  idx_put(idx, S("sku-001"), S("p001"));

  /* Só resolve o prim_key — não busca no main */
  uint8_t *pkey;
  size_t plen;
  assert(idx_lookup_primary(idx, S("sku-001"), &pkey, &plen) == LSM_OK);
  assert(plen == 4 && memcmp(pkey, "p001", 4) == 0);
  free(pkey);

  idx_close(idx);
  db_close(main);
  cleanup();
}

static void test_not_found(void) {
  cleanup();
  db_t *main = db_open(MAIN_DIR, opts());
  assert(main);

  sec_index_t *idx = idx_open(main, IDX_DIR);
  assert(idx);

  uint8_t *val;
  size_t vlen;
  assert(idx_get(idx, S("naoexiste@x.com"), &val, &vlen) == LSM_NOT_FOUND);
  assert(idx_lookup_primary(idx, S("fantasma"), &val, &vlen) == LSM_NOT_FOUND);

  idx_close(idx);
  db_close(main);
  cleanup();
}

static void test_phantom_entry(void) {
  /*
   * Índice aponta para uma chave que não existe no main db.
   * Simula crash entre idx_put e db_put.
   * idx_get deve retornar LSM_NOT_FOUND.
   */
  cleanup();
  db_t *main = db_open(MAIN_DIR, opts());
  assert(main);

  sec_index_t *idx = idx_open(main, IDX_DIR);
  assert(idx);

  /* Insere no índice mas NÃO no main */
  idx_put(idx, S("orphan@x.com"), S("u999"));

  uint8_t *val;
  size_t vlen;
  assert(idx_get(idx, S("orphan@x.com"), &val, &vlen) == LSM_NOT_FOUND);

  idx_close(idx);
  db_close(main);
  cleanup();
}

static void test_delete_index_entry(void) {
  cleanup();
  db_t *main = db_open(MAIN_DIR, opts());
  assert(main);
  db_put(main, S("u001"), S("alice"));

  sec_index_t *idx = idx_open(main, IDX_DIR);
  assert(idx);
  idx_put(idx, S("alice@x.com"), S("u001"));

  /* Remove do índice */
  assert(idx_del(idx, S("alice@x.com")) == LSM_OK);

  uint8_t *val;
  size_t vlen;
  assert(idx_get(idx, S("alice@x.com"), &val, &vlen) == LSM_NOT_FOUND);

  /* Main db ainda tem a entrada */
  assert(db_get(main, S("u001"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "alice", 5) == 0);
  free(val);

  idx_close(idx);
  db_close(main);
  cleanup();
}

static void test_reopen_index(void) {
  cleanup();

  /* Sessão 1: cria e popula */
  {
    db_t *main = db_open(MAIN_DIR, opts());
    assert(main);
    db_put(main, S("u001"), S("alice"));
    db_put(main, S("u002"), S("bob"));

    sec_index_t *idx = idx_open(main, IDX_DIR);
    assert(idx);
    idx_put(idx, S("alice@x.com"), S("u001"));
    idx_put(idx, S("bob@x.com"), S("u002"));

    idx_close(idx);
    db_close(main);
  }

  /* Sessão 2: reabre e verifica persistência */
  {
    db_t *main = db_open(MAIN_DIR, opts());
    assert(main);

    sec_index_t *idx = idx_open(main, IDX_DIR);
    assert(idx);

    uint8_t *val;
    size_t vlen;
    assert(idx_get(idx, S("alice@x.com"), &val, &vlen) == LSM_OK);
    assert(memcmp(val, "alice", 5) == 0);
    free(val);

    assert(idx_get(idx, S("bob@x.com"), &val, &vlen) == LSM_OK);
    assert(memcmp(val, "bob", 3) == 0);
    free(val);

    idx_close(idx);
    db_close(main);
  }

  cleanup();
}

static void test_update_index_on_primary_update(void) {
  /*
   * Simula atualização de registro: o email muda.
   * Fluxo correto:
   *   1. idx_del(old_email)
   *   2. db_put(prim_key, new_value)
   *   3. idx_put(new_email, prim_key)
   */
  cleanup();
  db_t *main = db_open(MAIN_DIR, opts());
  assert(main);

  sec_index_t *idx = idx_open(main, IDX_DIR);
  assert(idx);

  db_put(main, S("u001"), S("alice:old@x.com"));
  idx_put(idx, S("old@x.com"), S("u001"));

  /* Atualiza */
  idx_del(idx, S("old@x.com"));
  db_put(main, S("u001"), S("alice:new@x.com"));
  idx_put(idx, S("new@x.com"), S("u001"));

  uint8_t *val;
  size_t vlen;

  /* Novo email funciona */
  assert(idx_get(idx, S("new@x.com"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "alice:new@x.com", 15) == 0);
  free(val);

  /* Email antigo não existe mais */
  assert(idx_get(idx, S("old@x.com"), &val, &vlen) == LSM_NOT_FOUND);

  idx_close(idx);
  db_close(main);
  cleanup();
}

static void test_multiple_indexes(void) {
  /*
   * Dois índices independentes sobre o mesmo main db:
   *   idx_email: email → user_id
   *   idx_city:  city  → user_id
   */
  cleanup();

  char idx2_dir[] = "/tmp/tinylsm_test_idx2";
  cleanup_dir(idx2_dir);

  db_t *main = db_open(MAIN_DIR, opts());
  assert(main);

  db_put(main, S("u001"), S("alice"));
  db_put(main, S("u002"), S("bob"));

  sec_index_t *idx_email = idx_open(main, IDX_DIR);
  sec_index_t *idx_city = idx_open(main, idx2_dir);
  assert(idx_email && idx_city);

  idx_put(idx_email, S("alice@x.com"), S("u001"));
  idx_put(idx_email, S("bob@x.com"), S("u002"));
  idx_put(idx_city, S("recife"), S("u001"));
  idx_put(idx_city, S("sao_paulo"), S("u002"));

  uint8_t *val;
  size_t vlen;

  assert(idx_get(idx_email, S("alice@x.com"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "alice", 5) == 0);
  free(val);

  assert(idx_get(idx_city, S("recife"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "alice", 5) == 0);
  free(val);

  assert(idx_get(idx_city, S("sao_paulo"), &val, &vlen) == LSM_OK);
  assert(memcmp(val, "bob", 3) == 0);
  free(val);

  idx_close(idx_email);
  idx_close(idx_city);
  db_close(main);

  cleanup();
  cleanup_dir(idx2_dir);
}

/* ---- runner ---- */

int main(void) {
  test_basic_index();
  test_lookup_primary();
  test_not_found();
  test_phantom_entry();
  test_delete_index_entry();
  test_reopen_index();
  test_update_index_on_primary_update();
  test_multiple_indexes();

  printf("PASS: index\n");
  return 0;
}
