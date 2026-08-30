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

#define DB_DEFAULT_MEM_LIMIT (4u * 1024u * 1024u) /* 4 MB */

typedef struct {
  size_t mem_limit_bytes; /* flush quando MemTable exceder este valor */
} db_opts_t;

#define COMPACTION_THRESHOLD 4u

typedef struct db db_t;

/* Abre ou cria um banco de dados no diretório `dir`.
 * Retorna NULL em falha. */
db_t *db_open(const char *dir, db_opts_t opts);

/* Faz flush final do MemTable e fecha todos os recursos. */
void db_close(db_t *db);

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

/*
 * Compacta todos os SSTables em um único.
 *
 * O que faz:
 *   - K-way merge de todos os SSTables (do mais antigo ao mais novo)
 *   - Mantém apenas a versão mais recente de cada chave
 *   - Elimina tombstones (chave deletada some definitivamente do disco)
 *   - Gera novo SSTable + Bloom, apaga os antigos
 *
 * Thread safety: pode ser chamado concorrentemente com Put/Get/Del.
 * Writes continuam no MemTable durante o merge. O swap final é atômico.
 *
 * Retorna LSM_BUSY se uma compaction já está em andamento.
 * Retorna LSM_OK se num_ssts < 2 (nada a fazer).
 */
lsm_status_t db_compact(db_t *db);

/*
 * db_snapshot_t — visão consistente do banco em um ponto no tempo.
 *
 * Uso:
 *   db_snapshot_t *snap = db_snapshot_open(db);
 *   db_snapshot_get(snap, key, &out, &out_len);
 *   db_snapshot_release(snap);
 *
 * Garantia: db_snapshot_get só vê escritas confirmadas ANTES de
 * db_snapshot_open retornar — independente de flushes ou compactions.
 */
typedef struct {
  db_t *db;
  uint64_t seq; /* seq capturado no momento do open */
} db_snapshot_t;

db_snapshot_t *db_snapshot_open(db_t *db);
void db_snapshot_release(db_snapshot_t *snap);

lsm_status_t db_snapshot_get(db_snapshot_t *snap, slice_t key, uint8_t **out,
                             size_t *out_len);
