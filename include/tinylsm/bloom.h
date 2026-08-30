#pragma once

#include "tinylsm/slice.h"
#include "tinylsm/status.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Bloom filter — estrutura probabilística para eliminar I/Os desnecessários.
 *
 * Propriedades:
 *   - Falso negativo: IMPOSSÍVEL. "não existe" é garantia absoluta.
 *   - Falso positivo: possível com probabilidade `p` configurável.
 *   - Tamanho: proporcional a n * ln(p), independente do tamanho das chaves.
 *
 * Uso típico (por SSTable):
 *   1. Durante o flush: bloom_add() para cada chave
 *   2. Serializa com bloom_encode() e grava antes do footer
 *   3. No Get: bloom_may_contain() antes de qualquer I/O de bloco
 */
typedef struct {
    uint8_t *bits;       /* array de bits (heap-allocated)   */
    size_t   num_bits;   /* tamanho total em bits            */
    uint32_t num_hashes; /* k — número de funções de hash    */
} bloom_t;

/*
 * Inicializa o bloom filter para `n` chaves esperadas com taxa de falso
 * positivo `fpr` (ex.: 0.01 = 1%).
 * Retorna LSM_OK ou LSM_OOM.
 */
lsm_status_t bloom_init(bloom_t *b, size_t n, double fpr);

/* Libera a memória interna. */
void bloom_destroy(bloom_t *b);

/* Adiciona uma chave ao filtro. */
void bloom_add(bloom_t *b, slice_t key);

/*
 * Retorna 1 se a chave PODE existir (positivo — pode ser falso).
 * Retorna 0 se a chave DEFINITIVAMENTE não existe.
 */
int bloom_may_contain(const bloom_t *b, slice_t key);

/*
 * Serialização — para gravar no SSTable.
 *
 * Formato: [num_bits:4][num_hashes:4][bits: ceil(num_bits/8) bytes]
 *
 * bloom_encode: aloca *out (caller faz free), preenche *out_len.
 * bloom_decode: inicializa *b a partir do buffer serializado.
 */
lsm_status_t bloom_encode(const bloom_t *b, uint8_t **out, size_t *out_len);
lsm_status_t bloom_decode(bloom_t *b, const uint8_t *buf, size_t buf_len);
