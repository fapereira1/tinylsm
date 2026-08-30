#pragma once

#include "tinylsm/slice.h"
#include "tinylsm/status.h"

#include <stddef.h>

/*
 * DB — engine LSM-tree completa.
 *
 * Fluxo de escrita:
 *   Put/Del → WAL (disco) → MemTable (RAM)
 *   Quando MemTable > mem_limit → flush → SSTable + Bloom no disco
 *
 * Fluxo de leitura:
 *   Get → MemTable → SSTable L0 (newest-first, com Bloom filter)
 *
 * Recovery:
 *   db_open() varre o diretório por *.sst existentes e replaya o WAL.
 */

#define DB_DEFAULT_MEM_LIMIT (4u * 1024u * 1024u)  /* 4 MB */

typedef struct {
    size_t mem_limit_bytes; /* flush quando MemTable exceder este valor */
} db_opts_t;

typedef struct db db_t;

/* Abre ou cria um banco de dados no diretório `dir`.
 * Retorna NULL em falha. */
db_t        *db_open(const char *dir, db_opts_t opts);

/* Faz flush final do MemTable e fecha todos os recursos. */
void         db_close(db_t *db);

/* Escreve key=value. Durável após o retorno (fsync). */
lsm_status_t db_put(db_t *db, slice_t key, slice_t value);

/*
 * Lê o valor de key.
 * Em sucesso: *out é malloc'd — caller deve free().
 * Retorna LSM_NOT_FOUND se ausente ou deletado.
 */
lsm_status_t db_get(db_t *db, slice_t key, uint8_t **out, size_t *out_len);

/* Deleta key (insere tombstone). Durável após o retorno. */
lsm_status_t db_del(db_t *db, slice_t key);
