#include "tinylsm/wal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * CRC-32 — polinômio Ethernet/ZIP (0xEDB88320), inicializado uma vez.
 * -------------------------------------------------------------------------- */

static pthread_once_t crc_once  = PTHREAD_ONCE_INIT;
static uint32_t       crc_tab[256];

static void crc_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
}

static uint32_t crc_begin(void) {
    pthread_once(&crc_once, crc_build_table);
    return 0xFFFFFFFFu;
}

static uint32_t crc_feed(uint32_t crc, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        crc = crc_tab[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static uint32_t crc_end(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

/* --------------------------------------------------------------------------
 * Serialização little-endian explícita.
 * Evita dependência de endianness da plataforma.
 * -------------------------------------------------------------------------- */

static void put_u32le(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >>  8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32le(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] <<  8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

/* --------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

wal_t *wal_open(const char *path) {
    wal_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    /* "a+b": append, cria se não existir, leitura permitida */
    w->fp = fopen(path, "a+b");
    if (!w->fp) { free(w); return NULL; }

    w->fd = fileno(w->fp);

    if (pthread_mutex_init(&w->lock, NULL) != 0) {
        fclose(w->fp);
        free(w);
        return NULL;
    }
    return w;
}

void wal_close(wal_t *wal) {
    if (!wal) return;
    wal_sync(wal);
    fclose(wal->fp);
    pthread_mutex_destroy(&wal->lock);
    free(wal);
}

/* --------------------------------------------------------------------------
 * Escrita
 *
 * Monta o header [op:1][key_len:4][val_len:4], computa CRC de forma
 * incremental (sem buffer extra) e escreve [crc:4][header][key][val].
 * -------------------------------------------------------------------------- */

lsm_status_t wal_append(wal_t *wal, sl_op_t op,
                          slice_t key, slice_t value) {
    if (slice_is_empty(key)) return LSM_INVALID_ARG;

    uint8_t hdr[9];
    hdr[0] = (uint8_t)op;
    put_u32le(hdr + 1, (uint32_t)key.len);
    put_u32le(hdr + 5, (uint32_t)value.len);

    uint32_t crc = crc_begin();
    crc = crc_feed(crc, hdr, sizeof(hdr));
    if (key.len   > 0) crc = crc_feed(crc, key.data,   key.len);
    if (value.len > 0) crc = crc_feed(crc, value.data, value.len);

    uint8_t crc_bytes[4];
    put_u32le(crc_bytes, crc_end(crc));

    pthread_mutex_lock(&wal->lock);

    int ok = 1;
    ok = ok && fwrite(crc_bytes,  1, 4,            wal->fp) == 4;
    ok = ok && fwrite(hdr,        1, sizeof(hdr),  wal->fp) == sizeof(hdr);
    if (key.len   > 0)
        ok = ok && fwrite(key.data,   1, key.len,   wal->fp) == key.len;
    if (value.len > 0)
        ok = ok && fwrite(value.data, 1, value.len, wal->fp) == value.len;

    pthread_mutex_unlock(&wal->lock);

    return ok ? LSM_OK : LSM_IO_ERROR;
}

lsm_status_t wal_sync(wal_t *wal) {
    if (fflush(wal->fp) != 0) return LSM_IO_ERROR;
    if (fsync(wal->fd)  != 0) return LSM_IO_ERROR;
    return LSM_OK;
}

/* --------------------------------------------------------------------------
 * Recovery
 *
 * Lê records sequencialmente. Critérios de parada:
 *   - EOF antes do fim do record  (escrita parcial)
 *   - CRC incorreto               (escrita parcial ou corrupção)
 *   - op inválido                 (dado corrompido)
 * Em qualquer desses casos, para silenciosamente: o log é válido até ali.
 * -------------------------------------------------------------------------- */

lsm_status_t wal_recover(const char *path, skiplist_t *sl) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return LSM_OK;   /* sem WAL = estado vazio = OK */

    lsm_status_t status = LSM_OK;

    for (;;) {
        /* --- lê CRC --- */
        uint8_t crc_bytes[4];
        if (fread(crc_bytes, 1, 4, fp) != 4) break;

        /* --- lê header --- */
        uint8_t hdr[9];
        if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) break;

        uint8_t  op      = hdr[0];
        uint32_t key_len = get_u32le(hdr + 1);
        uint32_t val_len = get_u32le(hdr + 5);

        if ((op != OP_PUT && op != OP_DEL) || key_len == 0) break;

        /* --- lê key --- */
        uint8_t *key_buf = malloc(key_len);
        if (!key_buf) { status = LSM_OOM; break; }
        if (fread(key_buf, 1, key_len, fp) != key_len) {
            free(key_buf); break;
        }

        /* --- lê value --- */
        uint8_t *val_buf = NULL;
        if (val_len > 0) {
            val_buf = malloc(val_len);
            if (!val_buf) { free(key_buf); status = LSM_OOM; break; }
            if (fread(val_buf, 1, val_len, fp) != val_len) {
                free(key_buf); free(val_buf); break;
            }
        }

        /* --- verifica CRC --- */
        uint32_t crc = crc_begin();
        crc = crc_feed(crc, hdr, sizeof(hdr));
        if (key_len > 0) crc = crc_feed(crc, key_buf, key_len);
        if (val_len > 0) crc = crc_feed(crc, val_buf, val_len);

        if (crc_end(crc) != get_u32le(crc_bytes)) {
            /* escrita parcial — fim do log válido */
            free(key_buf); free(val_buf); break;
        }

        /* --- aplica ao MemTable ---
         * sl_put/del copia key+value para a arena interna.
         * Podemos liberar os buffers logo em seguida. */
        slice_t k = slice_make(key_buf, key_len);
        slice_t v = slice_make(val_buf, val_len);

        status = (op == OP_PUT) ? sl_put(sl, k, v) : sl_del(sl, k);

        free(key_buf);
        free(val_buf);

        if (status != LSM_OK) break;
    }

    fclose(fp);
    return status;
}
