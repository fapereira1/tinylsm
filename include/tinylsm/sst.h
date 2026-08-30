#pragma once

#include "tinylsm/skiplist.h"
#include "tinylsm/slice.h"
#include "tinylsm/status.h"

#include <stdint.h>
#include <stdio.h>

#define SST_BLOCK_SIZE  4096u
#define SST_FOOTER_SIZE 20u
#define SST_MAGIC       0x4D534C54u   /* bytes LE: 'T','L','S','M' */

/*
 * Entrada do índice — uma por bloco de dados.
 * Em memória após sst_reader_open().
 */
typedef struct {
    uint8_t *last_key;       /* malloc'd — última user_key do bloco */
    size_t   last_key_len;
    uint64_t offset;         /* offset do bloco no arquivo          */
    uint32_t size;           /* bytes totais do bloco (dados + 8)   */
} sst_idx_entry_t;

/* ---- Writer (opaco) ---- */
typedef struct sst_writer sst_writer_t;

sst_writer_t *sst_writer_open(const char *path);

/*
 * Adiciona um record. Records DEVEM ser inseridos em ordem crescente de
 * key (o iterador do MemTable garante isso via sl_iter_next_key).
 */
lsm_status_t  sst_writer_add(sst_writer_t *w, sl_op_t op, uint64_t seq,
                              slice_t key, slice_t value);

lsm_status_t  sst_writer_finish(sst_writer_t *w); /* flush + fecha fp */
void          sst_writer_free(sst_writer_t *w);    /* sempre chamar após finish */

/* ---- Reader (opaco) ---- */
typedef struct sst_reader sst_reader_t;

sst_reader_t *sst_reader_open(const char *path);

/*
 * Busca pontual. Retorna:
 *   LSM_OK         chave encontrada — *out_op preenchido.
 *                  Se OP_PUT: *out_buf malloc'd, caller deve free().
 *                  Se OP_DEL: *out_buf = NULL (tombstone).
 *   LSM_NOT_FOUND  chave ausente neste SSTable.
 *   LSM_CORRUPTION CRC incorreto.
 */
lsm_status_t  sst_reader_get(sst_reader_t *r, slice_t key,
                              sl_op_t *out_op, uint64_t *out_seq,
                              uint8_t **out_buf, size_t *out_len);

void          sst_reader_close(sst_reader_t *r);

/* ---- Iterador ----
 *
 * cur_key / cur_value apontam para blk_data (válido até sst_iter_next
 * ou sst_iter_finish). Sempre chame sst_iter_finish para liberar memória.
 */
typedef struct {
    sst_reader_t *reader;
    size_t        blk_idx;
    uint32_t      rec_idx;
    uint32_t      blk_num_recs;
    uint8_t      *blk_data;      /* record bytes do bloco corrente, malloc'd */
    size_t        blk_data_sz;
    size_t        data_pos;

    sl_op_t   cur_op;
    uint64_t  cur_seq;
    slice_t   cur_key;
    slice_t   cur_value;
    int       valid;
} sst_iter_t;

void     sst_iter_init(sst_iter_t *it, sst_reader_t *r);
int      sst_iter_valid(const sst_iter_t *it);
void     sst_iter_next(sst_iter_t *it);
slice_t  sst_iter_key(const sst_iter_t *it);
slice_t  sst_iter_value(const sst_iter_t *it);
sl_op_t  sst_iter_op(const sst_iter_t *it);
uint64_t sst_iter_seq(const sst_iter_t *it);
void     sst_iter_finish(sst_iter_t *it);
size_t sst_reader_num_blocks(const sst_reader_t *r);
