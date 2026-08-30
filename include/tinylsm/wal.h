#pragma once

#include "tinylsm/skiplist.h"
#include "tinylsm/slice.h"
#include "tinylsm/status.h"

#include <pthread.h>
#include <stdio.h>

/*
 * WAL — log append-only para crash recovery.
 *
 * Contrato de uso:
 *   1. wal_append() grava o record em disco (buffered)
 *   2. wal_sync()   faz fflush + fsync — chame antes de confirmar ao cliente
 *   3. Em restart, wal_recover() reconstrói o MemTable a partir do log
 *
 * Thread safety: mutex interno serializa os appends.
 */
typedef struct {
    FILE           *fp;
    int             fd;   /* para fsync() */
    pthread_mutex_t lock;
} wal_t;

/* Abre (ou cria) o WAL em `path`. Retorna NULL em erro. */
wal_t *wal_open(const char *path);

/* Fecha o WAL fazendo fsync antes. */
void wal_close(wal_t *wal);

/* Grava um record. Thread-safe. */
lsm_status_t wal_append(wal_t *wal, sl_op_t op, slice_t key, slice_t value);

/* fflush + fsync — durabilidade garantida após este retorno. */
lsm_status_t wal_sync(wal_t *wal);

/*
 * Lê `path` e aplica todos os records válidos em `sl`.
 * Para no primeiro record incompleto ou com CRC errado (escrita parcial).
 * Retorna LSM_OK se o arquivo não existir (WAL vazio é estado válido).
 */
lsm_status_t wal_recover(const char *path, skiplist_t *sl);
