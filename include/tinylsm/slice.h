#pragma once 

#include  <stddef.h>
#include <stdint.h>
#include <string.h> 

/*
 * slice_t — view não-proprietária em um buffer de bytes.
 *
 * NÃO possui a memória apontada. O caller mantém o buffer vivo.
 * Padrão do LevelDB/RocksDB: evita cópias no hot path de leitura.
 */
typedef struct {
    const uint8_t *data;
    size_t         len;
} slice_t;

static inline slice_t slice_from_str(const char *s) {
    return (slice_t){ .data = (const uint8_t *)s, .len = strlen(s) };
}

static inline slice_t slice_make(const void *data, size_t len) {
    return (slice_t){ .data = (const uint8_t *)data, .len = len };
}

static inline slice_t slice_empty(void) {
    return (slice_t){ .data = NULL, .len = 0 };
}

static inline int slice_is_empty(slice_t s) {
    return s.len == 0;
}

/*
 * Comparação lexicográfica — semântica idêntica ao memcmp.
 *
 * Trata data==NULL quando len==0 de forma segura:
 * memcmp(NULL, ptr, 0) é UB no padrão — evitamos com o guard `min > 0`.
 */
static inline int slice_cmp(slice_t a, slice_t b) {
    size_t min = a.len < b.len ? a.len : b.len;
    if (min > 0) {
        int r = memcmp(a.data, b.data, min);
        if (r != 0) return r;
    }
    if (a.len < b.len) return -1;
    if (a.len > b.len) return  1;
    return 0;
}

static inline int slice_eq(slice_t a, slice_t b) {
    return a.len == b.len &&
           (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}
