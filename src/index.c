#include "tinylsm/index.h"

#include <stdlib.h>
#include <string.h>

sec_index_t *idx_open(db_t *main, const char *idx_dir) {
  sec_index_t *idx = calloc(1, sizeof(*idx));
  if (!idx)
    return NULL;

  db_opts_t opts = {.mem_limit_bytes = DB_DEFAULT_MEM_LIMIT};
  idx->idx_db = db_open(idx_dir, opts);
  if (!idx->idx_db) {
    free(idx);
    return NULL;
  }

  idx->main_db = main;
  return idx;
}

void idx_close(sec_index_t *idx) {
  if (!idx)
    return;
  db_close(idx->idx_db);
  free(idx);
}

lsm_status_t idx_put(sec_index_t *idx, slice_t sec_key, slice_t prim_key) {
  return db_put(idx->idx_db, sec_key, prim_key);
}

lsm_status_t idx_del(sec_index_t *idx, slice_t sec_key) {
  return db_del(idx->idx_db, sec_key);
}

lsm_status_t idx_lookup_primary(sec_index_t *idx, slice_t sec_key,
                                uint8_t **out, size_t *out_len) {
  return db_get(idx->idx_db, sec_key, out, out_len);
}

lsm_status_t idx_get(sec_index_t *idx, slice_t sec_key, uint8_t **out,
                     size_t *out_len) {
  /* 1. Resolve sec_key → prim_key */
  uint8_t *pkey;
  size_t plen;
  lsm_status_t s = db_get(idx->idx_db, sec_key, &pkey, &plen);
  if (s != LSM_OK)
    return s;

  /* 2. Busca prim_key no main db */
  slice_t pk = slice_make(pkey, plen);
  s = db_get(idx->main_db, pk, out, out_len);
  free(pkey);

  /*
   * LSM_NOT_FOUND aqui significa entrada fantasma no índice
   * (main db não tem a chave). Tratamos como NOT_FOUND normal.
   */
  return s;
}
