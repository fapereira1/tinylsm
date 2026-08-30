#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinylsm/bloom.h"

static slice_t S(const char *s) { return slice_from_str(s); }

static void test_no_false_negatives(void) {
    bloom_t b;
    assert(bloom_init(&b, 1000, 0.01) == LSM_OK);

    char key[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "key%d", i);
        bloom_add(&b, slice_from_str(key));
    }

    /* Tudo que foi inserido DEVE ser encontrado — zero falso negativo */
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "key%d", i);
        assert(bloom_may_contain(&b, slice_from_str(key)) == 1);
    }

    bloom_destroy(&b);
}

static void test_false_positive_rate(void) {
    /*
     * Insere 10.000 chaves com fpr=1% e mede a taxa real em 100.000
     * chaves que NÃO foram inseridas. Aceita até 3x o fpr teórico —
     * margem para variação de hash e arredondamentos.
     */
    int n = 10000;
    double fpr = 0.01;

    bloom_t b;
    assert(bloom_init(&b, (size_t)n, fpr) == LSM_OK);

    char key[32];
    for (int i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "ins%d", i);
        bloom_add(&b, slice_from_str(key));
    }

    int probes = 100000;
    int fp     = 0;
    for (int i = 0; i < probes; i++) {
        snprintf(key, sizeof(key), "probe%d", i);  /* nunca inserido */
        if (bloom_may_contain(&b, slice_from_str(key))) fp++;
    }

    double measured = (double)fp / (double)probes;
    /* taxa medida deve ser < 3 * fpr teórico */
    assert(measured < 3.0 * fpr);

    bloom_destroy(&b);
}

static void test_empty_filter(void) {
    bloom_t b;
    assert(bloom_init(&b, 100, 0.01) == LSM_OK);

    /* Filtro vazio: nada deve ser encontrado */
    assert(bloom_may_contain(&b, S("anything")) == 0);
    assert(bloom_may_contain(&b, S("foo"))      == 0);

    bloom_destroy(&b);
}

static void test_single_key(void) {
    bloom_t b;
    assert(bloom_init(&b, 1, 0.01) == LSM_OK);

    bloom_add(&b, S("only"));
    assert(bloom_may_contain(&b, S("only")) == 1);
    /* Alta probabilidade de não estar — não é garantia, mas vale testar */

    bloom_destroy(&b);
}

static void test_encode_decode_roundtrip(void) {
    bloom_t b;
    assert(bloom_init(&b, 500, 0.01) == LSM_OK);

    bloom_add(&b, S("alpha"));
    bloom_add(&b, S("beta"));
    bloom_add(&b, S("gamma"));

    uint8_t *buf; size_t len;
    assert(bloom_encode(&b, &buf, &len) == LSM_OK);

    bloom_t b2;
    assert(bloom_decode(&b2, buf, len) == LSM_OK);
    free(buf);

    /* Após decode, as mesmas chaves devem ser encontradas */
    assert(bloom_may_contain(&b2, S("alpha")) == 1);
    assert(bloom_may_contain(&b2, S("beta"))  == 1);
    assert(bloom_may_contain(&b2, S("gamma")) == 1);

    /* num_bits e num_hashes devem ser idênticos */
    assert(b.num_bits   == b2.num_bits);
    assert(b.num_hashes == b2.num_hashes);

    bloom_destroy(&b);
    bloom_destroy(&b2);
}

static void test_decode_corruption(void) {
    /* Buffer muito curto */
    uint8_t short_buf[4] = {0};
    bloom_t b;
    assert(bloom_decode(&b, short_buf, sizeof(short_buf)) == LSM_CORRUPTION);

    /* num_bits não múltiplo de 8 */
    uint8_t bad[9] = {0};
    bad[0] = 7;   /* num_bits = 7 — não múltiplo de 8 */
    bad[4] = 1;   /* num_hashes = 1 */
    assert(bloom_decode(&b, bad, sizeof(bad)) == LSM_CORRUPTION);
}

static void test_large_filter(void) {
    /* 1 milhão de chaves — verifica que a memória e o cálculo escalam */
    bloom_t b;
    assert(bloom_init(&b, 1000000, 0.001) == LSM_OK);

    /* Tamanho esperado: ~1.8 MB para fpr=0.1% e n=1M */
    assert(b.num_bits > 1000000u);
    assert(b.num_hashes >= 7u && b.num_hashes <= 15u);

    bloom_add(&b, S("sentinel"));
    assert(bloom_may_contain(&b, S("sentinel")) == 1);

    bloom_destroy(&b);
}

/* CMakeLists.txt precisa de -lm para log/ceil/round */
int main(void) {
    test_no_false_negatives();
    test_false_positive_rate();
    test_empty_filter();
    test_single_key();
    test_encode_decode_roundtrip();
    test_decode_corruption();
    test_large_filter();

    printf("PASS: bloom\n");
    return 0;
}
