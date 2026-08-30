#include "tinylsm/skiplist.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * PRNG — xorshift64, sem dependência de rand()
 * -------------------------------------------------------------------------- */

static uint64_t rng_next(uint64_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/*
 * Gera altura aleatória com P(subir nível) = 1/4.
 * Distribuição geométrica: a maioria dos nós fica no nível 1.
 */
static int random_height(uint64_t *rng) {
    int h = 1;
    while (h < SL_MAX_LEVEL && (rng_next(rng) & 3u) == 0)
        h++;
    return h;
}

/* --------------------------------------------------------------------------
 * Comparação de InternalKey: (user_key ASC, seq DESC)
 *
 * seq DESC significa: seq maior → retorna -1 → "vem antes" na lista.
 * Isso coloca a versão mais nova primeiro para a mesma user_key.
 * -------------------------------------------------------------------------- */

static int ikey_cmp(slice_t ka, uint64_t sa, slice_t kb, uint64_t sb) {
    int r = slice_cmp(ka, kb);
    if (r != 0) return r;
    if (sa > sb) return -1;  /* seq maior → mais novo → vem primeiro */
    if (sa < sb) return  1;
    return 0;
}

/* --------------------------------------------------------------------------
 * Alocação de nós na arena
 *
 * Layout: [sl_node_t header][sl_node_t *forward[height]]
 * -------------------------------------------------------------------------- */

static sl_node_t *node_alloc(arena_t *arena, int height) {
    size_t sz = sizeof(sl_node_t) + (size_t)height * sizeof(sl_node_t *);
    sl_node_t *n = arena_alloc(arena, sz, _Alignof(sl_node_t));
    if (!n) return NULL;
    memset(n, 0, sz);
    n->height = height;
    return n;
}

/* --------------------------------------------------------------------------
 * Busca interna
 *
 * Retorna o último nó cujo ikey < (key, seq).
 * Preenche prev[level] com o predecessor em cada nível (para inserção).
 * -------------------------------------------------------------------------- */

static sl_node_t *sl_find_prev(skiplist_t *sl,
                                slice_t key, uint64_t seq,
                                sl_node_t **prev)
{
    sl_node_t *cur = sl->head;
    for (int level = sl->max_height - 1; level >= 0; level--) {
        while (1) {
            sl_node_t *next = sl_node_fwd(cur)[level];
            if (!next) break;
            if (ikey_cmp(next->key, next->seq, key, seq) >= 0) break;
            cur = next;
        }
        if (prev) prev[level] = cur;
    }
    return cur;
}

/* --------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

skiplist_t *sl_new(void) {
    skiplist_t *sl = calloc(1, sizeof(*sl));
    if (!sl) return NULL;

    if (arena_init(&sl->arena) != 0) { free(sl); return NULL; }

    if (pthread_rwlock_init(&sl->rwlock, NULL) != 0) {
        arena_destroy(&sl->arena);
        free(sl);
        return NULL;
    }

    /* Sentinela head: key vazia, seq=UINT64_MAX
     * É o menor InternalKey possível — nenhum nó real fica antes dele.
     * Nunca é comparado como `next`, apenas como ponto de partida. */
    sl->head = node_alloc(&sl->arena, SL_MAX_LEVEL);
    if (!sl->head) {
        pthread_rwlock_destroy(&sl->rwlock);
        arena_destroy(&sl->arena);
        free(sl);
        return NULL;
    }
    sl->head->key = slice_empty();
    sl->head->seq = UINT64_MAX;
    sl->max_height = 1;

    /* Semente do PRNG: mistura tempo e endereço para evitar colisões entre
     * instâncias criadas no mesmo segundo. */
    sl->rng = (uint64_t)(uintptr_t)sl ^ (uint64_t)time(NULL);
    if (sl->rng == 0) sl->rng = 0xdeadbeefcafe1234ULL;

    return sl;
}

void sl_free(skiplist_t *sl) {
    if (!sl) return;
    pthread_rwlock_destroy(&sl->rwlock);
    arena_destroy(&sl->arena);
    free(sl);
}

/* --------------------------------------------------------------------------
 * Inserção (PUT e DEL compartilham o mesmo mecanismo)
 * -------------------------------------------------------------------------- */

static lsm_status_t sl_insert(skiplist_t *sl,
                               slice_t key, slice_t value,
                               sl_op_t op)
{
    if (slice_is_empty(key)) return LSM_INVALID_ARG;

    pthread_rwlock_wrlock(&sl->rwlock);

    uint64_t seq = ++sl->seq_clock;
    int      h   = random_height(&sl->rng);

    sl_node_t *prev[SL_MAX_LEVEL];
    sl_find_prev(sl, key, seq, prev);

    sl_node_t *n = node_alloc(&sl->arena, h);
    if (!n) { pthread_rwlock_unlock(&sl->rwlock); return LSM_OOM; }

    /* Copia key e value para a arena — o caller pode reutilizar os buffers. */
    uint8_t *kd = arena_dup(&sl->arena, key.data, key.len);
    if (!kd)   { pthread_rwlock_unlock(&sl->rwlock); return LSM_OOM; }
    n->key = slice_make(kd, key.len);

    if (op == OP_PUT && value.len > 0) {
        uint8_t *vd = arena_dup(&sl->arena, value.data, value.len);
        if (!vd) { pthread_rwlock_unlock(&sl->rwlock); return LSM_OOM; }
        n->value = slice_make(vd, value.len);
    } else {
        n->value = slice_empty();
    }

    n->seq = seq;
    n->op  = op;

    /* Expande max_height se necessário. */
    if (h > sl->max_height) {
        for (int i = sl->max_height; i < h; i++)
            prev[i] = sl->head;
        sl->max_height = h;
    }

    /* Encadeia o novo nó em cada nível. */
    sl_node_t **nf = sl_node_fwd(n);
    for (int i = 0; i < h; i++) {
        nf[i]                   = sl_node_fwd(prev[i])[i];
        sl_node_fwd(prev[i])[i] = n;
    }

    sl->num_entries++;
    pthread_rwlock_unlock(&sl->rwlock);
    return LSM_OK;
}

lsm_status_t sl_put(skiplist_t *sl, slice_t key, slice_t value) {
    return sl_insert(sl, key, value, OP_PUT);
}

lsm_status_t sl_del(skiplist_t *sl, slice_t key) {
    return sl_insert(sl, key, slice_empty(), OP_DEL);
}

/* --------------------------------------------------------------------------
 * Leitura
 *
 * Seek para (key, UINT64_MAX) — o menor InternalKey para essa user_key.
 * O primeiro nó após o seek é a versão mais recente (seq mais alto).
 * -------------------------------------------------------------------------- */

lsm_status_t sl_get(skiplist_t *sl, slice_t key, slice_t *out) {
    if (slice_is_empty(key)) return LSM_INVALID_ARG;

    pthread_rwlock_rdlock(&sl->rwlock);

    sl_node_t *prev = sl_find_prev(sl, key, UINT64_MAX, NULL);
    sl_node_t *n    = sl_node_fwd(prev)[0];

    lsm_status_t status = LSM_NOT_FOUND;
    if (n && slice_eq(n->key, key)) {
        if (n->op == OP_PUT) {
            *out   = n->value;
            status = LSM_OK;
        }
        /* OP_DEL → NOT_FOUND (tombstone) */
    }

    pthread_rwlock_unlock(&sl->rwlock);
    return status;
}

/* --------------------------------------------------------------------------
 * Métricas
 * -------------------------------------------------------------------------- */

size_t sl_mem_usage(skiplist_t *sl) {
    return arena_mem_usage(&sl->arena) + sizeof(*sl);
}

size_t sl_num_entries(skiplist_t *sl) {
    pthread_rwlock_rdlock(&sl->rwlock);
    size_t n = sl->num_entries;
    pthread_rwlock_unlock(&sl->rwlock);
    return n;
}

/* --------------------------------------------------------------------------
 * Iterador
 * -------------------------------------------------------------------------- */

void sl_iter_init(sl_iter_t *it, skiplist_t *sl) {
    it->sl  = sl;
    pthread_rwlock_rdlock(&sl->rwlock);
    it->cur = sl_node_fwd(sl->head)[0];  /* pula o sentinela */
}

int sl_iter_valid(const sl_iter_t *it) {
    return it->cur != NULL;
}

void sl_iter_next(sl_iter_t *it) {
    if (it->cur) it->cur = sl_node_fwd(it->cur)[0];
}

void sl_iter_next_key(sl_iter_t *it) {
    if (!it->cur) return;
    slice_t cur_key = it->cur->key;
    do {
        it->cur = sl_node_fwd(it->cur)[0];
    } while (it->cur && slice_eq(it->cur->key, cur_key));
}

slice_t  sl_iter_key(const sl_iter_t *it)   { return it->cur->key;   }
slice_t  sl_iter_value(const sl_iter_t *it) { return it->cur->value; }
sl_op_t  sl_iter_op(const sl_iter_t *it)    { return it->cur->op;    }
uint64_t sl_iter_seq(const sl_iter_t *it)   { return it->cur->seq;   }

void sl_iter_finish(sl_iter_t *it) {
    pthread_rwlock_unlock(&it->sl->rwlock);
    it->cur = NULL;
}

lsm_status_t sl_get_raw(skiplist_t *sl, slice_t key,
                          slice_t *out_value, sl_op_t *out_op) {
    if (slice_is_empty(key)) return LSM_INVALID_ARG;

    pthread_rwlock_rdlock(&sl->rwlock);

    sl_node_t *prev = sl_find_prev(sl, key, UINT64_MAX, NULL);
    sl_node_t *n    = sl_node_fwd(prev)[0];

    lsm_status_t s = LSM_NOT_FOUND;
    if (n && slice_eq(n->key, key)) {
        *out_value = n->value;
        *out_op    = n->op;
        s          = LSM_OK;
    }

    pthread_rwlock_unlock(&sl->rwlock);
    return s;
}
