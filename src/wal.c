#include "tinylsm/wal.h"
#include "tinylsm/crc32.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put_u32le(uint8_t *b, uint32_t v) {
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8);
    b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24);
}

static uint32_t get_u32le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8)
         | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}

wal_t *wal_open(const char *path) {
    wal_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fp = fopen(path, "a+b");
    if (!w->fp) { free(w); return NULL; }
    w->fd = fileno(w->fp);
    if (pthread_mutex_init(&w->lock, NULL) != 0) {
        fclose(w->fp); free(w); return NULL;
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

lsm_status_t wal_append(wal_t *wal, sl_op_t op,
                          slice_t key, slice_t value) {
    if (slice_is_empty(key)) return LSM_INVALID_ARG;

    uint8_t hdr[9];
    hdr[0] = (uint8_t)op;
    put_u32le(hdr + 1, (uint32_t)key.len);
    put_u32le(hdr + 5, (uint32_t)value.len);

    uint32_t crc = crc32_begin();
    crc = crc32_feed(crc, hdr, sizeof(hdr));
    if (key.len   > 0) crc = crc32_feed(crc, key.data,   key.len);
    if (value.len > 0) crc = crc32_feed(crc, value.data, value.len);

    uint8_t crc_bytes[4];
    put_u32le(crc_bytes, crc32_end(crc));

    pthread_mutex_lock(&wal->lock);

    int ok = 1;
    ok = ok && fwrite(crc_bytes, 1, 4,           wal->fp) == 4;
    ok = ok && fwrite(hdr,       1, sizeof(hdr), wal->fp) == sizeof(hdr);
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

lsm_status_t wal_recover(const char *path, skiplist_t *sl) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return LSM_OK;

    lsm_status_t status = LSM_OK;

    for (;;) {
        uint8_t crc_bytes[4];
        if (fread(crc_bytes, 1, 4, fp) != 4) break;

        uint8_t hdr[9];
        if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) break;

        uint8_t  op      = hdr[0];
        uint32_t key_len = get_u32le(hdr + 1);
        uint32_t val_len = get_u32le(hdr + 5);

        if ((op != OP_PUT && op != OP_DEL) || key_len == 0) break;

        uint8_t *key_buf = malloc(key_len);
        if (!key_buf) { status = LSM_OOM; break; }
        if (fread(key_buf, 1, key_len, fp) != key_len) {
            free(key_buf); break;
        }

        uint8_t *val_buf = NULL;
        if (val_len > 0) {
            val_buf = malloc(val_len);
            if (!val_buf) { free(key_buf); status = LSM_OOM; break; }
            if (fread(val_buf, 1, val_len, fp) != val_len) {
                free(key_buf); free(val_buf); break;
            }
        }

        uint32_t crc = crc32_begin();
        crc = crc32_feed(crc, hdr, sizeof(hdr));
        if (key_len > 0) crc = crc32_feed(crc, key_buf, key_len);
        if (val_len > 0) crc = crc32_feed(crc, val_buf, val_len);

        if (crc32_end(crc) != get_u32le(crc_bytes)) {
            free(key_buf); free(val_buf); break;
        }

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
