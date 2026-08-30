#pragma once

#include "tinylsm/db.h"
#include "tinylsm/slice.h"
#include "tinylsm/status.h"

#include <stddef.h>
#include <stdint.h>

/*
 * sec_index_t — índice secundário sobre um db_t principal.
 *
 * Internamente é um segundo db_t cujas entradas são:
 *   key:   secondary_key
 *   value: primary_key
 *
 * Consistência:
 *   As escritas no índice e no db principal NÃO são atômicas —
 *   usamos write-order: grava no índice ANTES do principal.
 *   Em crash entre as duas escritas, o índice pode ter uma entrada
 *   "fantasma" que aponta para uma chave inexistente no principal.
 *   idx_get() trata esse caso retornando LSM_NOT_FOUND.
 *
 * Uso típico:
 *   sec_index_t *idx = idx_open(main_db, "/tmp/mydb/idx_email");
 *   idx_put(idx, slice_from_str("alice@x.com"), slice_from_str("u001"));
 *   // ... main db já tem u001 → {name:alice, ...}
 *
 *   uint8_t *pkey; size_t plen;
 *   idx_lookup_primary(idx, slice_from_str("alice@x.com"), &pkey, &plen);
 *   // pkey = "u001"
 *
 *   uint8_t *val; size_t vlen;
 *   idx_get(idx, slice_from_str("alice@x.com"), &val, &vlen);
 *   // val = conteúdo completo de u001 no main db
 */
typedef struct {
  db_t *idx_db;  /* db interno do índice: sec_key → prim_key */
  db_t *main_db; /* db principal — usado em idx_get()        */
} sec_index_t;

/*
 * Abre (ou cria) um índice secundário.
 *
 * `idx_dir`: diretório exclusivo para os arquivos do índice.
 * `main`:    db principal — não é fechado por idx_close().
 */
sec_index_t *idx_open(db_t *main, const char *idx_dir);

/* Fecha o índice (não fecha o main db). */
void idx_close(sec_index_t *idx);

/*
 * Associa sec_key → prim_key no índice.
 * Chame APÓS garantir que prim_key existe no main db.
 */
lsm_status_t idx_put(sec_index_t *idx, slice_t sec_key, slice_t prim_key);

/*
 * Remove a associação de sec_key.
 * Não remove a entrada correspondente no main db.
 */
lsm_status_t idx_del(sec_index_t *idx, slice_t sec_key);

/*
 * Retorna a primary_key associada a sec_key.
 * *out é malloc'd — caller faz free().
 */
lsm_status_t idx_lookup_primary(sec_index_t *idx, slice_t sec_key,
                                uint8_t **out, size_t *out_len);

/*
 * Atalho: sec_key → prim_key → value no main db.
 * *out é malloc'd — caller faz free().
 * Retorna LSM_NOT_FOUND se sec_key não existe OU se prim_key
 * não existe no main (entrada fantasma).
 */
lsm_status_t idx_get(sec_index_t *idx, slice_t sec_key, uint8_t **out,
                     size_t *out_len);
