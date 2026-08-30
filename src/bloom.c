#include "tinylsm/bloom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Hash — double hashing com MurmurHash3 finalizer.
 *
 * Gera k hashes independentes a partir de dois valores base (h1, h2)
 * usando a técnica de Kirsch-Mitzenmacher:
 *
 *   g_i(x) = h1(x) + i * h2(x)   mod m
 *
 * Custo: duas chamadas de hash, não k. Qualidade equivalente a k hashes
 * independentes para Bloom filters (provado por Kirsch & Mitzenmacher, 2006).
 * -------------------------------------------------------------------------- */

static uint32_t murmur_mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static void bloom_hashes(slice_t key, uint32_t *h1, uint32_t *h2) {
    /* FNV-1a como base — simples, boa distribuição, sem dependência */
    uint32_t a = 0x811c9dc5u;
    uint32_t b = 0x811c9dc5u ^ 0x9e3779b9u;

    for (size_t i = 0; i < key.len; i++) {
        a ^= key.data[i];
        a *= 0x01000193u;
        b ^= key.data[i];
        b *= 0x01000193u;
        b  = (b << 5) | (b >> 27);   /* rotate para diferenciar h2 de h1 */
    }
    *h1 = murmur_mix(a);
    *h2 = murmur_mix(b);
}

/* --------------------------------------------------------------------------
 * Manipulação de bits
 * -------------------------------------------------------------------------- */

static void bit_set(uint8_t *bits, size_t pos) {
    bits[pos >> 3] |= (uint8_t)(1u << (pos & 7u));
}

static int bit_get(const uint8_t *bits, size_t pos) {
    return (bits[pos >> 3] >> (pos & 7u)) & 1u;
}

/* --------------------------------------------------------------------------
 * API pública
 * -------------------------------------------------------------------------- */

lsm_status_t bloom_init(bloom_t *b, size_t n, double fpr) {
    if (n == 0) n = 1;

    /*
     * m = ceil( -n * ln(fpr) / ln(2)^2 )
     * k = round( (m/n) * ln(2) )
     *
     * Mínimo de 8 bits e 1 hash para casos degenerados.
     */
    double ln2  = 0.6931471805599453;
    double m_f  = -(double)n * log(fpr) / (ln2 * ln2);
    size_t m    = (size_t)ceil(m_f);
    if (m < 8) m = 8;

    double k_f  = ((double)m / (double)n) * ln2;
    uint32_t k  = (uint32_t)round(k_f);
    if (k < 1) k = 1;
    if (k > 30) k = 30;  /* limite prático — mais hashes não ajudam */

    /* Arredonda m para múltiplo de 8 (alinha bytes) */
    m = (m + 7u) & ~7u;

    b->bits = calloc(m / 8, 1);
    if (!b->bits) return LSM_OOM;

    b->num_bits   = m;
    b->num_hashes = k;
    return LSM_OK;
}

void bloom_destroy(bloom_t *b) {
    free(b->bits);
    b->bits       = NULL;
    b->num_bits   = 0;
    b->num_hashes = 0;
}

void bloom_add(bloom_t *b, slice_t key) {
    if (!b->bits || b->num_bits == 0) return;
    uint32_t h1, h2;
    bloom_hashes(key, &h1, &h2);
    for (uint32_t i = 0; i < b->num_hashes; i++) {
        size_t pos = (h1 + i * h2) % b->num_bits;
        bit_set(b->bits, pos);
    }
}

int bloom_may_contain(const bloom_t *b, slice_t key) {
    if (!b->bits || b->num_bits == 0) return 1;
    uint32_t h1, h2;
    bloom_hashes(key, &h1, &h2);
    for (uint32_t i = 0; i < b->num_hashes; i++) {
        size_t pos = (h1 + i * h2) % b->num_bits;
        if (!bit_get(b->bits, pos)) return 0;
    }
    return 1;
}

lsm_status_t bloom_encode(const bloom_t *b, uint8_t **out, size_t *out_len) {
    size_t byte_sz = b->num_bits / 8;
    size_t total   = 4 + 4 + byte_sz;   /* num_bits + num_hashes + bits */

    uint8_t *buf = malloc(total);
    if (!buf) return LSM_OOM;

    /* little-endian */
    buf[0] = (uint8_t)(b->num_bits);
    buf[1] = (uint8_t)(b->num_bits >>  8);
    buf[2] = (uint8_t)(b->num_bits >> 16);
    buf[3] = (uint8_t)(b->num_bits >> 24);

    buf[4] = (uint8_t)(b->num_hashes);
    buf[5] = (uint8_t)(b->num_hashes >>  8);
    buf[6] = (uint8_t)(b->num_hashes >> 16);
    buf[7] = (uint8_t)(b->num_hashes >> 24);

    memcpy(buf + 8, b->bits, byte_sz);

    *out     = buf;
    *out_len = total;
    return LSM_OK;
}

lsm_status_t bloom_decode(bloom_t *b, const uint8_t *buf, size_t buf_len) {
    if (buf_len < 8) return LSM_CORRUPTION;

    uint32_t num_bits   = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                        | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint32_t num_hashes = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8)
                        | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);

    if (num_bits == 0 || num_bits % 8 != 0) return LSM_CORRUPTION;

    size_t byte_sz = num_bits / 8;
    if (buf_len < 8 + byte_sz) return LSM_CORRUPTION;

    b->bits = malloc(byte_sz);
    if (!b->bits) return LSM_OOM;

    memcpy(b->bits, buf + 8, byte_sz);
    b->num_bits   = num_bits;
    b->num_hashes = num_hashes;
    return LSM_OK;
}
