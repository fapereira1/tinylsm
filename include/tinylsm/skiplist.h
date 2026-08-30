#pragma once

#include "tinylsm/arena.h"
#include "tinylsm/slice.h"
#include "tinylsm/status.h"

#include <pthread.h>
#include <stdint.h>

/*
 * Skip list concorrente — backing do MemTable.
 *
 * Ordering interno: (user_key ASC, seq DESC)
 *   Para a mesma user_key, seq maior (mais novo) vem primeiro.
 *   Get faz seek(key, UINT64_MAX) e lê o primeiro resultado.
 *
 * Tombstones:
 *   Delete insere OP_DEL em vez de remover o nó.
 *   Necessário para propagar deleções ao SSTable na compaction.
 *
 * Thread safety:
 *   RWLock — múltiplos leitores ou um único escritor.
 *   O iterador mantém o rdlock durante toda a sua vida.
 */

#define SL_MAX_LEVEL 12 /* suporta ~4^12 ≈ 16 M entradas com p=1/4 */

typedef enum {
  OP_PUT = 1,
  OP_DEL = 0,
} sl_op_t;

/*
 * Nó interno. Os forward pointers ficam alocados logo após o header
 * na arena — não acesse diretamente, use sl_node_fwd().
 */
typedef struct sl_node {
  slice_t key;
  slice_t value;
  uint64_t seq;
  sl_op_t op;
  int height;
} sl_node_t;

static inline sl_node_t **sl_node_fwd(sl_node_t *n) {
  return (sl_node_t **)(n + 1);
}

typedef struct {
  sl_node_t *head;
  int max_height;
  size_t num_entries; /* total de nós incluindo tombstones */
  uint64_t seq_clock; /* incrementa a cada escrita         */
  uint64_t rng;       /* estado do PRNG xorshift64         */
  arena_t arena;
  pthread_rwlock_t rwlock;
} skiplist_t;

/* --- Ciclo de vida --- */

skiplist_t *sl_new(void);
void sl_free(skiplist_t *sl);

/* --- Escrita --- */

lsm_status_t sl_put(skiplist_t *sl, slice_t key, slice_t value);
lsm_status_t sl_del(skiplist_t *sl, slice_t key);

/* --- Leitura --- */

/*
 * Retorna o valor mais recente da chave.
 * *out aponta para memória da arena — válida até sl_free().
 */
lsm_status_t sl_get(skiplist_t *sl, slice_t key, slice_t *out);

/* --- Métricas --- */

size_t sl_mem_usage(skiplist_t *sl);
size_t sl_num_entries(skiplist_t *sl);

/* --- Iterador ---
 *
 * Mantém o rdlock durante toda a vida útil.
 * sl_iter_finish() é OBRIGATÓRIO após sl_iter_init(), mesmo em erro.
 *
 *   sl_iter_t it;
 *   sl_iter_init(&it, sl);
 *   while (sl_iter_valid(&it)) {
 *       slice_t k  = sl_iter_key(&it);
 *       sl_op_t op = sl_iter_op(&it);
 *       sl_iter_next_key(&it);   // pula para a próxima user_key
 *   }
 *   sl_iter_finish(&it);
 */
typedef struct {
  sl_node_t *cur;
  skiplist_t *sl;
} sl_iter_t;

void sl_iter_init(sl_iter_t *it, skiplist_t *sl);
int sl_iter_valid(const sl_iter_t *it);
void sl_iter_next(sl_iter_t *it);     /* avança um nó (mesma key possível) */
void sl_iter_next_key(sl_iter_t *it); /* pula todos os seq da user_key atual */
slice_t sl_iter_key(const sl_iter_t *it);
slice_t sl_iter_value(const sl_iter_t *it);
sl_op_t sl_iter_op(const sl_iter_t *it);
uint64_t sl_iter_seq(const sl_iter_t *it);
void sl_iter_finish(sl_iter_t *it);

/*
 * Como sl_get, mas também expõe o op (PUT ou DEL).
 * Retorna LSM_OK se a chave existe no MemTable (mesmo como tombstone).
 * Necessário para o DB distinguir "ausente" de "deletado".
 */
lsm_status_t sl_get_raw(skiplist_t *sl, slice_t key, slice_t *out_value,
                        sl_op_t *out_op);

/* Retorna o seq_clock atual — para captura de snapshot. */
uint64_t sl_current_seq(skiplist_t *sl);

/* Define o seq inicial (chamado após flush para manter continuidade). */
void sl_set_initial_seq(skiplist_t *sl, uint64_t seq);

/*
 * Como sl_get_raw, mas só considera entradas com seq ≤ max_seq.
 * Garante isolamento de snapshot na leitura do MemTable.
 */
lsm_status_t sl_get_at_seq(skiplist_t *sl, slice_t key, uint64_t max_seq,
                           slice_t *out_value, sl_op_t *out_op);
