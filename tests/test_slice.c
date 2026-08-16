#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tinylsm/slice.h"
#include "tinylsm/status.h"

/* ---- slice ---- */

static void test_cmp_ordering(void) {
    slice_t apple  = slice_from_str("apple");
    slice_t banana = slice_from_str("banana");
    slice_t apple2 = slice_from_str("apple");
    slice_t app    = slice_from_str("app");   /* prefixo de apple */

    assert(slice_cmp(apple, banana) < 0);    /* apple < banana */
    assert(slice_cmp(banana, apple) > 0);
    assert(slice_cmp(apple, apple2) == 0);   /* iguais */
    assert(slice_cmp(app, apple)  < 0);      /* prefixo mais curto é menor */
    assert(slice_cmp(apple, app)  > 0);
}

static void test_eq(void) {
    slice_t a = slice_from_str("hello");
    slice_t b = slice_from_str("hello");
    slice_t c = slice_from_str("world");

    assert( slice_eq(a, b));
    assert(!slice_eq(a, c));
}

static void test_empty(void) {
    slice_t e = slice_empty();
    slice_t a = slice_from_str("x");

    assert( slice_is_empty(e));
    assert(!slice_is_empty(a));

    /* slice vazio é menor que qualquer slice não-vazio */
    assert(slice_cmp(e, a) < 0);
    assert(slice_cmp(a, e) > 0);

    /* dois slices vazios são iguais — data==NULL não causa UB */
    assert(slice_cmp(e, e) == 0);
    assert(slice_eq(e, e));
}

static void test_make(void) {
    uint8_t buf[] = {0x01, 0x02, 0x03};
    slice_t s = slice_make(buf, 3);

    assert(s.len == 3);
    assert(s.data[0] == 0x01);
    assert(s.data[2] == 0x03);
}

/* ---- status ---- */

static void test_status_str(void) {
    assert(strcmp(lsm_status_str(LSM_OK),          "OK")          == 0);
    assert(strcmp(lsm_status_str(LSM_NOT_FOUND),   "NOT_FOUND")   == 0);
    assert(strcmp(lsm_status_str(LSM_OOM),         "OOM")         == 0);
    assert(strcmp(lsm_status_str(LSM_INVALID_ARG), "INVALID_ARG") == 0);
}

/* LSM_TRY — função auxiliar para testar a macro */
static lsm_status_t returns_err(void)  { return LSM_IO_ERROR; }
static lsm_status_t returns_ok(void)   { return LSM_OK; }

static lsm_status_t try_propagation(void) {
    LSM_TRY(returns_err());
    return LSM_OK;   /* nunca deve chegar aqui */
}

static lsm_status_t try_ok(void) {
    LSM_TRY(returns_ok());
    return LSM_OK;
}

static void test_try_macro(void) {
    assert(try_propagation() == LSM_IO_ERROR);
    assert(try_ok()          == LSM_OK);
}

/* ---- runner ---- */

int main(void) {
    test_cmp_ordering();
    test_eq();
    test_empty();
    test_make();
    test_status_str();
    test_try_macro();

    printf("PASS: slice + status\n");
    return 0;
}
