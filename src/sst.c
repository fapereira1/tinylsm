#include "tinylsm/sst.h"
#include "tinylsm/crc32.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Serialização little-endian explícita
 * -------------------------------------------------------------------------- */

static void put_u32le(uint8_t *b, uint32_t v) {
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8);
    b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24);
}
static uint32_t get_u32le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8)
         | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static void put_u64le(uint8_t *b, uint64_t v) {
    for (int i = 0; i < 8; i++) { b[i] = (uint8_t)v; v >>= 8; }
}
static uint64_t get_u64le(const uint8_t *b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

/* --------------------------------------------------------------------------
 * Codificação de record: [op:1][seq:8][key_len:4][val_len:4][key][value]
 * -------------------------------------------------------------------------- */

#define REC_HDR_SIZE 17u

static void rec_encode(uint8_t *dst, sl_op_t op, uint64_t seq,
                       slice_t key, slice_t value) {
    dst[0] = (uint8_t)op;
    put_u64le(dst + 1,  seq);
    put_u32le(dst + 9,  (uint32_t)key.len);
    put_u32le(dst + 13, (uint32_t)value.len);
    if (key.len   > 0) memcpy(dst + 17,           key.data,   key.len);
    if (value.len > 0) memcpy(dst + 17 + key.len, value.data, value.len);
}

static void rec_decode(const uint8_t *src, sl_op_t *op, uint64_t *seq,
                       slice_t *key, slice_t *value) {
    *op  = (sl_op_t)src[0];
    *seq = get_u64le(src + 1);
    uint32_t kl = get_u32le(src + 9);
    uint32_t vl = get_u32le(src + 13);
    *key   = slice_make(src + 17,    kl);
    *value = slice_make(src + 17 + kl, vl);
}

/* --------------------------------------------------------------------------
 * Structs internas (opacas no header)
 * -------------------------------------------------------------------------- */

struct sst_writer {
    FILE    *fp;
    uint64_t file_off;

    uint8_t  blk[SST_BLOCK_SIZE]; /* buffer do bloco corrente */
    size_t   blk_sz;              /* bytes de record data no buffer */
    uint32_t blk_num_recs;
    uint8_t *blk_last_key;        /* malloc'd — última key do bloco */
    size_t   blk_last_key_len;

    sst_idx_entry_t *idx;
    size_t           idx_len;
    size_t           idx_cap;

    size_t total_entries;
};

struct sst_reader {
    FILE            *fp;
    sst_idx_entry_t *idx;
    size_t           num_blocks;
    size_t           num_entries;
};

/* --------------------------------------------------------------------------
 * I/O de blocos
 * -------------------------------------------------------------------------- */

/* Escreve o bloco corrente em disco e registra no índice. */
static lsm_status_t block_flush(sst_writer_t *w) {
    if (w->blk_sz == 0) return LSM_OK;

    uint8_t trailer[8];
    put_u32le(trailer, w->blk_num_recs);

    uint32_t crc = crc32_begin();
    crc = crc32_feed(crc, w->blk,  w->blk_sz);
    crc = crc32_feed(crc, trailer, 4);   /* CRC inclui num_recs */
    put_u32le(trailer + 4, crc32_end(crc));

    if (fwrite(w->blk,   1, w->blk_sz, w->fp) != w->blk_sz) return LSM_IO_ERROR;
    if (fwrite(trailer,  1, 8,         w->fp) != 8)          return LSM_IO_ERROR;

    /* Cresce o array de índice se necessário */
    if (w->idx_len >= w->idx_cap) {
        size_t nc = w->idx_cap * 2;
        sst_idx_entry_t *ni = realloc(w->idx, nc * sizeof(*ni));
        if (!ni) return LSM_OOM;
        w->idx     = ni;
        w->idx_cap = nc;
    }

    sst_idx_entry_t *e = &w->idx[w->idx_len++];
    e->last_key     = w->blk_last_key;   /* transfere ownership */
    e->last_key_len = w->blk_last_key_len;
    e->offset       = w->file_off;
    e->size         = (uint32_t)(w->blk_sz + 8);

    w->file_off        += w->blk_sz + 8;
    w->blk_sz           = 0;
    w->blk_num_recs     = 0;
    w->blk_last_key     = NULL;
    w->blk_last_key_len = 0;

    return LSM_OK;
}

/*
 * Lê e verifica um bloco do disco.
 * *out_data é malloc'd — caller é responsável pelo free.
 */
static lsm_status_t block_load(FILE *fp, const sst_idx_entry_t *e,
                                uint8_t **out_data, size_t *out_sz,
                                uint32_t *out_num_recs) {
    if (e->size < 8) return LSM_CORRUPTION;

    uint8_t *buf = malloc(e->size);
    if (!buf) return LSM_OOM;

    if (fseek(fp, (long)e->offset, SEEK_SET) != 0 ||
        fread(buf, 1, e->size, fp) != e->size) {
        free(buf); return LSM_IO_ERROR;
    }

    size_t raw_sz = e->size - 8;

    uint32_t crc = crc32_begin();
    crc = crc32_feed(crc, buf,          raw_sz);
    crc = crc32_feed(crc, buf + raw_sz, 4);
    if (crc32_end(crc) != get_u32le(buf + raw_sz + 4)) {
        free(buf); return LSM_CORRUPTION;
    }

    *out_num_recs = get_u32le(buf + raw_sz);
    *out_data     = buf;
    *out_sz       = raw_sz;
    return LSM_OK;
}

/* --------------------------------------------------------------------------
 * Writer
 * -------------------------------------------------------------------------- */

sst_writer_t *sst_writer_open(const char *path) {
    sst_writer_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->fp = fopen(path, "wb");
    if (!w->fp) { free(w); return NULL; }

    w->idx_cap = 16;
    w->idx     = malloc(w->idx_cap * sizeof(*w->idx));
    if (!w->idx) { fclose(w->fp); free(w); return NULL; }

    return w;
}

lsm_status_t sst_writer_add(sst_writer_t *w, sl_op_t op, uint64_t seq,
                              slice_t key, slice_t value) {
    if (slice_is_empty(key)) return LSM_INVALID_ARG;

    size_t rec_sz = REC_HDR_SIZE + key.len + value.len;
    if (rec_sz > SST_BLOCK_SIZE - 8) return LSM_INVALID_ARG;  /* record enorme */

    /* Não cabe no bloco corrente — flush e começa um novo */
    if (w->blk_sz + rec_sz > SST_BLOCK_SIZE - 8) {
        lsm_status_t s = block_flush(w);
        if (s != LSM_OK) return s;
    }

    rec_encode(w->blk + w->blk_sz, op, seq, key, value);
    w->blk_sz      += rec_sz;
    w->blk_num_recs++;

    free(w->blk_last_key);
    w->blk_last_key = malloc(key.len);
    if (!w->blk_last_key) return LSM_OOM;
    memcpy(w->blk_last_key, key.data, key.len);
    w->blk_last_key_len = key.len;

    w->total_entries++;
    return LSM_OK;
}

lsm_status_t sst_writer_finish(sst_writer_t *w) {
    lsm_status_t s = block_flush(w);
    if (s != LSM_OK) return s;

    /* ---- Bloco de índice ---- */
    uint64_t idx_offset = w->file_off;
    uint32_t crc        = crc32_begin();
    uint8_t  tmp[8];
    uint32_t idx_sz = 0;

    put_u32le(tmp, (uint32_t)w->idx_len);
    crc = crc32_feed(crc, tmp, 4);
    if (fwrite(tmp, 1, 4, w->fp) != 4) return LSM_IO_ERROR;
    idx_sz += 4;

    for (size_t i = 0; i < w->idx_len; i++) {
        sst_idx_entry_t *e = &w->idx[i];

        put_u32le(tmp, (uint32_t)e->last_key_len);
        crc = crc32_feed(crc, tmp, 4);
        if (fwrite(tmp, 1, 4, w->fp) != 4) return LSM_IO_ERROR;
        idx_sz += 4;

        if (e->last_key_len > 0) {
            crc = crc32_feed(crc, e->last_key, e->last_key_len);
            if (fwrite(e->last_key, 1, e->last_key_len, w->fp) != e->last_key_len)
                return LSM_IO_ERROR;
            idx_sz += (uint32_t)e->last_key_len;
        }

        put_u64le(tmp, e->offset);
        crc = crc32_feed(crc, tmp, 8);
        if (fwrite(tmp, 1, 8, w->fp) != 8) return LSM_IO_ERROR;
        idx_sz += 8;

        put_u32le(tmp, e->size);
        crc = crc32_feed(crc, tmp, 4);
        if (fwrite(tmp, 1, 4, w->fp) != 4) return LSM_IO_ERROR;
        idx_sz += 4;
    }

    put_u32le(tmp, crc32_end(crc));
    if (fwrite(tmp, 1, 4, w->fp) != 4) return LSM_IO_ERROR;
    idx_sz += 4;

    /* ---- Footer ---- */
    uint8_t footer[SST_FOOTER_SIZE];
    put_u64le(footer + 0,  idx_offset);
    put_u32le(footer + 8,  idx_sz);
    put_u32le(footer + 12, (uint32_t)w->total_entries);
    put_u32le(footer + 16, SST_MAGIC);

    if (fwrite(footer, 1, SST_FOOTER_SIZE, w->fp) != SST_FOOTER_SIZE)
        return LSM_IO_ERROR;

    fclose(w->fp);
    w->fp = NULL;
    return LSM_OK;
}

void sst_writer_free(sst_writer_t *w) {
    if (!w) return;
    if (w->fp) fclose(w->fp);
    free(w->blk_last_key);
    for (size_t i = 0; i < w->idx_len; i++)
        free(w->idx[i].last_key);
    free(w->idx);
    free(w);
}

/* --------------------------------------------------------------------------
 * Reader
 * -------------------------------------------------------------------------- */

sst_reader_t *sst_reader_open(const char *path) {
    sst_reader_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->fp = fopen(path, "rb");
    if (!r->fp) { free(r); return NULL; }

    /* Footer */
    uint8_t footer[SST_FOOTER_SIZE];
    if (fseek(r->fp, -(long)SST_FOOTER_SIZE, SEEK_END) != 0 ||
        fread(footer, 1, SST_FOOTER_SIZE, r->fp) != SST_FOOTER_SIZE ||
        get_u32le(footer + 16) != SST_MAGIC) {
        fclose(r->fp); free(r); return NULL;
    }

    uint64_t idx_offset = get_u64le(footer + 0);
    uint32_t idx_sz     = get_u32le(footer + 8);
    r->num_entries      = get_u32le(footer + 12);

    /* Lê bloco de índice */
    uint8_t *ibuf = malloc(idx_sz);
    if (!ibuf) { fclose(r->fp); free(r); return NULL; }

    if (fseek(r->fp, (long)idx_offset, SEEK_SET) != 0 ||
        fread(ibuf, 1, idx_sz, r->fp) != idx_sz) {
        free(ibuf); fclose(r->fp); free(r); return NULL;
    }

    /* Verifica CRC do índice */
    {
        uint32_t crc = crc32_begin();
        crc = crc32_feed(crc, ibuf, idx_sz - 4);
        if (crc32_end(crc) != get_u32le(ibuf + idx_sz - 4)) {
            free(ibuf); fclose(r->fp); free(r); return NULL;
        }
    }

    /* Parse das entradas */
    uint32_t num_blocks = get_u32le(ibuf);
    r->num_blocks = num_blocks;

    if (num_blocks == 0) { free(ibuf); return r; }

    r->idx = calloc(num_blocks, sizeof(*r->idx));  /* calloc: last_key = NULL */
    if (!r->idx) { free(ibuf); fclose(r->fp); free(r); return NULL; }

    size_t pos = 4;
    for (uint32_t i = 0; i < num_blocks; i++) {
        uint32_t kl = get_u32le(ibuf + pos); pos += 4;

        r->idx[i].last_key = malloc(kl > 0 ? kl : 1);
        if (!r->idx[i].last_key) {
            for (uint32_t j = 0; j < i; j++) free(r->idx[j].last_key);
            free(r->idx); free(ibuf); fclose(r->fp); free(r); return NULL;
        }
        if (kl > 0) memcpy(r->idx[i].last_key, ibuf + pos, kl);
        r->idx[i].last_key_len = kl;
        pos += kl;

        r->idx[i].offset = get_u64le(ibuf + pos); pos += 8;
        r->idx[i].size   = get_u32le(ibuf + pos); pos += 4;
    }

    free(ibuf);
    return r;
}

lsm_status_t sst_reader_get(sst_reader_t *r, slice_t key,
                              sl_op_t *out_op, uint64_t *out_seq,
                              uint8_t **out_buf, size_t *out_len) {
    if (slice_is_empty(key)) return LSM_INVALID_ARG;

    /* Busca binária: primeiro bloco onde last_key >= key */
    size_t lo = 0, hi = r->num_blocks;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        slice_t mk = slice_make(r->idx[mid].last_key, r->idx[mid].last_key_len);
        if (slice_cmp(mk, key) < 0) lo = mid + 1;
        else                        hi = mid;
    }
    if (lo >= r->num_blocks) return LSM_NOT_FOUND;

    uint8_t  *data; size_t data_sz; uint32_t num_recs;
    lsm_status_t s = block_load(r->fp, &r->idx[lo], &data, &data_sz, &num_recs);
    if (s != LSM_OK) return s;

    size_t pos = 0;
    for (uint32_t i = 0; i < num_recs && pos + REC_HDR_SIZE <= data_sz; i++) {
        sl_op_t op; uint64_t seq; slice_t k, v;
        rec_decode(data + pos, &op, &seq, &k, &v);

        int cmp = slice_cmp(k, key);
        if (cmp == 0) {
            *out_op  = op;
            *out_seq = seq;
            if (op == OP_PUT && v.len > 0) {
                *out_buf = malloc(v.len);
                if (!*out_buf) { free(data); return LSM_OOM; }
                memcpy(*out_buf, v.data, v.len);
                *out_len = v.len;
            } else {
                *out_buf = NULL;
                *out_len = 0;
            }
            free(data);
            return LSM_OK;
        }
        if (cmp > 0) break;

        pos += REC_HDR_SIZE + k.len + v.len;
    }

    free(data);
    return LSM_NOT_FOUND;
}

void sst_reader_close(sst_reader_t *r) {
    if (!r) return;
    if (r->fp) fclose(r->fp);
    for (size_t i = 0; i < r->num_blocks; i++)
        free(r->idx[i].last_key);
    free(r->idx);
    free(r);
}

/* --------------------------------------------------------------------------
 * Iterador
 * -------------------------------------------------------------------------- */

static void iter_load_block(sst_iter_t *it) {
    free(it->blk_data);
    it->blk_data = NULL;

    if (it->blk_idx >= it->reader->num_blocks) { it->valid = 0; return; }

    uint32_t num_recs; size_t data_sz;
    lsm_status_t s = block_load(it->reader->fp,
                                 &it->reader->idx[it->blk_idx],
                                 &it->blk_data, &data_sz, &num_recs);
    if (s != LSM_OK || num_recs == 0) { it->valid = 0; return; }

    it->blk_data_sz  = data_sz;
    it->blk_num_recs = num_recs;
    it->rec_idx      = 0;
    it->data_pos     = 0;

    rec_decode(it->blk_data,
               &it->cur_op, &it->cur_seq, &it->cur_key, &it->cur_value);
    it->valid = 1;
}

void sst_iter_init(sst_iter_t *it, sst_reader_t *r) {
    memset(it, 0, sizeof(*it));
    it->reader  = r;
    it->blk_idx = 0;
    iter_load_block(it);
}

int sst_iter_valid(const sst_iter_t *it) { return it->valid; }

void sst_iter_next(sst_iter_t *it) {
    if (!it->valid) return;

    it->data_pos += REC_HDR_SIZE + it->cur_key.len + it->cur_value.len;
    it->rec_idx++;

    if (it->rec_idx >= it->blk_num_recs) {
        it->blk_idx++;
        iter_load_block(it);
    } else {
        rec_decode(it->blk_data + it->data_pos,
                   &it->cur_op, &it->cur_seq, &it->cur_key, &it->cur_value);
    }
}

slice_t  sst_iter_key(const sst_iter_t *it)   { return it->cur_key;   }
slice_t  sst_iter_value(const sst_iter_t *it) { return it->cur_value; }
sl_op_t  sst_iter_op(const sst_iter_t *it)    { return it->cur_op;    }
uint64_t sst_iter_seq(const sst_iter_t *it)   { return it->cur_seq;   }

void sst_iter_finish(sst_iter_t *it) {
    free(it->blk_data);
    it->blk_data = NULL;
    it->valid    = 0;
}

size_t sst_reader_num_blocks(const sst_reader_t *r) {
    return r->num_blocks;
}
