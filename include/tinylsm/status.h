#pragma once

/*
 * lsm_status_t — código de retorno unificado para toda a engine.
 *
 * LSM_OK == 0  →  compatível com a convenção C de "0 = sucesso".
 * Toda função que pode falhar retorna este tipo — nunca errno implícito.
 */
typedef enum {
    LSM_OK          = 0,
    LSM_NOT_FOUND   = 1,
    LSM_IO_ERROR    = 2,
    LSM_CORRUPTION  = 3,
    LSM_OOM         = 4,
    LSM_BUSY        = 5,
    LSM_INVALID_ARG = 6,
} lsm_status_t;

static inline const char *lsm_status_str(lsm_status_t s) {
    switch (s) {
    case LSM_OK:          return "OK";
    case LSM_NOT_FOUND:   return "NOT_FOUND";
    case LSM_IO_ERROR:    return "IO_ERROR";
    case LSM_CORRUPTION:  return "CORRUPTION";
    case LSM_OOM:         return "OOM";
    case LSM_BUSY:        return "BUSY";
    case LSM_INVALID_ARG: return "INVALID_ARG";
    default:              return "UNKNOWN";
    }
}

/*
 * LSM_TRY — propaga erro sem boilerplate:
 *
 *   LSM_TRY(alguma_func(x));
 *   // equivale a:
 *   // lsm_status_t s = alguma_func(x);
 *   // if (s != LSM_OK) return s;
 */
#define LSM_TRY(expr)                      \
    do {                                    \
        lsm_status_t _s = (expr);          \
        if ((_s) != LSM_OK) return (_s);   \
    } while (0)
