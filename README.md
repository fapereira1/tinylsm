# tinylsm

Uma storage engine LSM-tree (Log-Structured Merge-tree) escrita do zero em C11, com indexação secundária, MVCC, crash recovery e fuzzing. Projeto de aprendizado focado em internals de sistemas de armazenamento, concorrência segura e testes rigorosos.

---

## Índice

- [O que é uma LSM-tree](#o-que-é-uma-lsm-tree)
- [Arquitetura](#arquitetura)
- [Componentes](#componentes)
- [Estrutura do projeto](#estrutura-do-projeto)
- [Build](#build)
- [Uso da API](#uso-da-api)
- [CLI interativo](#cli-interativo)
- [Testes](#testes)
- [Fuzzing](#fuzzing)
- [Decisões de design](#decisões-de-design)
- [Roadmap de aprendizado](#roadmap-de-aprendizado)

---

## O que é uma LSM-tree

Uma LSM-tree é uma estrutura de dados otimizada para **alto throughput de escrita**. Diferente de uma B-tree (que atualiza dados in-place no disco), uma LSM-tree acumula escritas em memória e as persiste em arquivos imutáveis ordenados.

```
B-tree:   write → busca posição no disco → atualiza in-place   (write amplification alta)
LSM-tree: write → RAM (MemTable) → flush sequencial para disco  (write amplification baixa)
```

Usada em produção: RocksDB, LevelDB, Cassandra, HBase, InfluxDB, TiKV.

---

## Arquitetura

```
┌──────────────────────────────────────────────────────┐
│                     Client API                       │
│             db_put / db_get / db_del                 │
└───────────────────────┬──────────────────────────────┘
                        │
             ┌──────────▼──────────┐
             │    WAL (disco)      │  ← fsync antes de confirmar ao cliente
             └──────────┬──────────┘
                        │
             ┌──────────▼──────────┐
             │   MemTable (RAM)    │  ← skip list concorrente com RWLock
             │   seq_clock: N      │  ← sequence numbers para MVCC
             └──────────┬──────────┘
                        │  flush quando mem_usage > mem_limit
             ┌──────────▼──────────┐
             │   SSTable L0        │  ← arquivo imutável + Bloom filter
             │   SSTable L1        │
             │   SSTable L2 ...    │
             └──────────┬──────────┘
                        │  db_compact()
             ┌──────────▼──────────┐
             │  SSTable compactado │  ← k-way merge, tombstones eliminados
             └─────────────────────┘
```

**Caminho de escrita:** `Put/Del → WAL (fsync) → MemTable → flush se mem_usage > limit`

**Caminho de leitura:** `MemTable → SSTable[newest] → ... → SSTable[oldest]`
O Bloom filter elimina I/Os desnecessários em cada SSTable antes de qualquer leitura de bloco.

**Índice secundário:** segundo `db_t` mapeando `sec_key → prim_key`.

---

## Componentes

### `slice_t` — view não-proprietária de bytes

View imutável em um buffer existente. Não possui a memória que aponta — zero cópias no caminho de leitura. Padrão do LevelDB/RocksDB.

```c
slice_t key = slice_from_str("hello");
slice_t val = slice_make(buf, len);
int eq      = slice_eq(a, b);
int cmp     = slice_cmp(a, b);   /* comparação lexicográfica */
```

### `arena_t` — alocador bump-pointer

Elimina `malloc` por nó no MemTable. Alocar é apenas somar um inteiro ao ponteiro corrente. Liberação é O(num_blocos), não O(num_nós). Thread-safe via mutex.

```c
arena_t a;
arena_init(&a);
void    *ptr = arena_alloc(&a, size, align);
uint8_t *dup = arena_dup(&a, data, len);
arena_destroy(&a);   /* libera todos os blocos de uma vez */
```

### `skiplist_t` — MemTable concorrente

Ordenação interna: `(user_key ASC, seq DESC)` — versão mais nova vem primeiro para a mesma chave. `Get` faz seek para `(key, UINT64_MAX)` e lê o primeiro resultado: O(log n) sem varrer versões antigas.

```c
skiplist_t *sl = sl_new();
sl_put(sl, key, value);                         /* insere/atualiza */
sl_del(sl, key);                                /* tombstone OP_DEL */
sl_get(sl, key, &out);                          /* versão mais recente */
sl_get_at_seq(sl, key, seq, &out, &op);         /* leitura filtrada por seq */

sl_iter_t it;
sl_iter_init(&it, sl);
while (sl_iter_valid(&it)) {
    slice_t k = sl_iter_key(&it);
    sl_iter_next_key(&it);                      /* próxima user_key */
}
sl_iter_finish(&it);
sl_free(sl);
```

Altura dos nós gerada por xorshift64 com `p = 1/4`. Máximo de 12 níveis (~16M entradas). RWLock POSIX para múltiplos leitores simultâneos.

### `wal_t` — Write-Ahead Log

Formato do record:
```
[crc32:4][op:1][key_len:4][val_len:4][key][value]
```

CRC-32 (polinômio Ethernet/ZIP) cobre tudo exceto o próprio CRC. Escrita parcial por crash gera CRC inválido — recovery para ali silenciosamente e aplica apenas os records válidos.

```c
wal_t *w = wal_open("data/wal.log");
wal_append(w, OP_PUT, key, value);
wal_sync(w);                          /* fflush + fsync */
wal_close(w);

wal_recover("data/wal.log", memtable);
```

### `sst_t` — SSTable (Sorted String Table)

Arquivo imutável gerado no flush do MemTable. Formato:

```
[bloco de dados 0: records + num_recs:4 + crc32:4]
[bloco de dados 1: ...]
...
[bloco de índice: num_blocks:4 + entries + crc32:4]
[footer: idx_offset:8 + idx_size:4 + n_entries:4 + magic:4]
magic = 0x4D534C54 ('TLSM' em little-endian)
```

Record dentro de um bloco: `[op:1][seq:8][key_len:4][val_len:4][key][value]`

Busca pontual usa busca binária no índice de blocos: O(log num_blocos).

### `bloom_t` — Bloom Filter

Elimina I/Os para chaves ausentes. Nunca tem falso negativo — se o filtro diz "não existe", a chave definitivamente não está no SSTable.

```c
bloom_t b;
bloom_init(&b, n_keys, 0.01);        /* 1% de falso positivo */
bloom_add(&b, key);
int maybe = bloom_may_contain(&b, key);

uint8_t *buf; size_t len;
bloom_encode(&b, &buf, &len);        /* serializa para disco */
bloom_decode(&b2, buf, len);         /* restaura do disco */
bloom_destroy(&b);
```

Fórmulas para tamanho e número de hashes ótimos:
```
m = ceil( -n × ln(p) / ln(2)² )   bits necessários
k = round( (m/n) × ln(2) )         hashes ótimos
```

Hash: double hashing com FNV-1a + MurmurHash3 finalizer (técnica de Kirsch-Mitzenmacher, 2006). Custo de duas funções de hash, qualidade de k funções independentes.

### `db_t` — API pública

```c
db_opts_t opts = { .mem_limit_bytes = 4 * 1024 * 1024 };
db_t *db = db_open("data/mydb", opts);

db_put(db, key, value);

uint8_t *out; size_t olen;
db_get(db, key, &out, &olen);   /* out é malloc'd — caller faz free() */
free(out);

db_del(db, key);
db_compact(db);
db_close(db);
```

**Flush automático:** quando `sl_mem_usage(memtable) > mem_limit_bytes`, o MemTable é flushed para um novo SSTable + Bloom filter. O WAL é truncado após o flush.

**Recovery:** `db_open` escaneia `*.sst` e `*.bloom` no diretório, carrega o índice de cada SSTable, e replaya o `wal.log` no MemTable.

### `sec_index_t` — Índice Secundário

Segundo `db_t` interno mapeando `sec_key → prim_key`.

```c
sec_index_t *idx = idx_open(main_db, "data/idx_email");
idx_put(idx, slice_from_str("alice@x.com"), slice_from_str("u001"));

uint8_t *val; size_t vlen;
idx_get(idx, slice_from_str("alice@x.com"), &val, &vlen);
/* val = conteúdo de "u001" no main_db */
free(val);

idx_close(idx);
```

Consistência: write-order (índice antes do main). Entrada fantasma (índice aponta para chave ausente no main) retorna `LSM_NOT_FOUND`.

### `db_snapshot_t` — MVCC / Snapshot Isolation

```c
db_put(db, S("k"), S("v1"));

db_snapshot_t *snap = db_snapshot_open(db);   /* captura seq corrente */

db_put(db, S("k"), S("v2"));                  /* escrita após o snapshot */

uint8_t *out; size_t olen;
db_snapshot_get(snap, S("k"), &out, &olen);   /* retorna "v1" */
free(out);

db_get(db, S("k"), &out, &olen);              /* retorna "v2" */
free(out);

db_snapshot_release(snap);
```

Cada escrita recebe um `seq` monotonicamente crescente. Um snapshot captura o `seq` corrente — leituras só consideram entradas com `seq ≤ snapshot_seq`. Compaction retorna `LSM_BUSY` enquanto há snapshots ativos (não pode eliminar versões antigas que ainda podem ser lidas).

---

## Estrutura do projeto

```
tinylsm/
├── include/
│   └── tinylsm/
│       ├── slice.h          # view não-proprietária de bytes
│       ├── status.h         # lsm_status_t + LSM_TRY macro
│       ├── arena.h          # alocador bump-pointer
│       ├── skiplist.h       # skip list concorrente (MemTable)
│       ├── wal.h            # write-ahead log
│       ├── sst.h            # SSTable reader/writer/iterator
│       ├── bloom.h          # bloom filter com serialização
│       ├── db.h             # API pública + snapshot
│       └── index.h          # índice secundário
├── src/
│   ├── arena.c
│   ├── skiplist.c
│   ├── crc32.h / crc32.c   # CRC-32 compartilhado entre WAL e SSTable
│   ├── wal.c
│   ├── sst.c
│   ├── bloom.c
│   ├── db.c
│   ├── index.c
│   ├── cli.c               # REPL interativo
│   └── version.c
├── tests/
│   ├── test_slice.c
│   ├── test_arena.c
│   ├── test_skiplist.c
│   ├── test_wal.c
│   ├── test_sst.c
│   ├── test_bloom.c
│   ├── test_db.c
│   ├── test_compaction.c
│   ├── test_index.c
│   ├── test_mvcc.c
│   └── test_crash_recovery.c
├── fuzz/
│   ├── fuzz_db.c           # libFuzzer target
│   ├── corpus/             # inputs que descobriram novos caminhos
│   └── tinylsm.dict        # dicionário de bytes relevantes
├── data/
│   └── products.csv        # dataset de exemplo para o CLI
├── scripts/
│   └── run_fuzzer.sh
├── CMakeLists.txt
└── CMakePresets.json
```

---

## Build

### Requisitos

- GCC ≥ 10 ou Clang ≥ 12
- CMake ≥ 3.20
- pthreads (POSIX)
- libm (`-lm`)
- Para fuzzing: Clang (GCC não suporta libFuzzer)

Em Fedora/RHEL:

```bash
sudo dnf install gcc cmake libasan libubsan clang
```

### Presets disponíveis

```bash
# build padrão (debug)
cmake --preset debug
cmake --build build/debug

# com AddressSanitizer + UBSan
cmake --preset asan
cmake --build build/asan

# com ThreadSanitizer
cmake --preset tsan
cmake --build build/tsan

# fuzzer (requer Clang)
cmake --preset fuzzer
cmake --build build/fuzzer
```

---

## Uso da API

```c
#include "tinylsm/db.h"
#include "tinylsm/index.h"

int main(void) {
    /* Abre o banco */
    db_opts_t opts = { .mem_limit_bytes = 4 * 1024 * 1024 };
    db_t *db = db_open("data/mydb", opts);

    /* Escrita */
    db_put(db, slice_from_str("user:1"), slice_from_str("alice"));
    db_put(db, slice_from_str("user:2"), slice_from_str("bob"));

    /* Leitura */
    uint8_t *val; size_t vlen;
    if (db_get(db, slice_from_str("user:1"), &val, &vlen) == LSM_OK) {
        printf("%.*s\n", (int)vlen, val);
        free(val);
    }

    /* Deleção */
    db_del(db, slice_from_str("user:2"));

    /* Índice secundário */
    sec_index_t *idx = idx_open(db, "data/idx_name");
    idx_put(idx, slice_from_str("alice"), slice_from_str("user:1"));

    idx_get(idx, slice_from_str("alice"), &val, &vlen);
    free(val);

    /* Snapshot isolation */
    db_snapshot_t *snap = db_snapshot_open(db);
    db_put(db, slice_from_str("user:1"), slice_from_str("alice_v2"));

    db_snapshot_get(snap, slice_from_str("user:1"), &val, &vlen);
    /* val = "alice" (versão anterior ao snapshot) */
    free(val);
    db_snapshot_release(snap);

    /* Compaction */
    db_compact(db);

    idx_close(idx);
    db_close(db);
    return 0;
}
```

---

## CLI interativo

```bash
./build/debug/tinylsm_cli
```

```
tinylsm produto CLI
Banco: data/db
Digite 'help' para ver os comandos.

tinylsm> help

Comandos disponíveis:
  load <arquivo.csv>   carrega produtos do CSV
  get <id>             busca produto por ID
  find <categoria>     lista produtos por categoria
  list                 lista todos os produtos
  del <id>             remove um produto
  compact              compacta os SSTables
  snap                 abre snapshot e consulta
  help                 mostra esta ajuda
  quit                 sai do programa

tinylsm> load data/products.csv
Carregados: 10 produto(s)

tinylsm> list
  ID        NOME                            CATEGORIA        PREÇO
  --------  ------------------------------  ---------------  --------
  P001      Teclado Mecânico                peripherals      299.90
  P002      Monitor 27pol                   monitors         1899.00
  P003      Mouse Gamer                     peripherals      189.90
  ...

tinylsm> get P003
  ID        NOME                            CATEGORIA        PREÇO
  P003      Mouse Gamer                     peripherals      189.90

tinylsm> find peripherals
  P001      Teclado Mecânico                peripherals      299.90
  P003      Mouse Gamer                     peripherals      189.90

  2 produto(s) encontrado(s).

tinylsm> del P001
Produto 'P001' removido.

tinylsm> snap
Snapshot aberto. Digite um ID para consultar (vazio para sair):
snap> P001
  P001      Teclado Mecânico                peripherals      299.90
snap>
Snapshot liberado.

tinylsm> quit
Fechando banco...
Até logo.
```

O comando `snap` demonstra o MVCC: dentro do contexto `snap>` você vê o estado no momento da abertura do snapshot — mesmo após deletes ou updates posteriores.

Estrutura de armazenamento:
```
data/
├── db/                    # main db: id → "name|category|price"
│   ├── wal.log
│   ├── 000001.sst
│   ├── 000001.bloom
│   └── ...
└── db_idx_category/       # índice secundário: category → id
    ├── wal.log
    └── ...
```

---

## Testes

```bash
# roda todos os testes
ctest --preset debug

# com AddressSanitizer (detecta leaks, use-after-free, overflow)
ctest --preset asan

# um teste específico
./build/debug/test_skiplist
./build/debug/test_mvcc
./build/debug/test_crash_recovery
```

### Cobertura de testes

| Teste | O que verifica |
|-------|---------------|
| `test_slice` | comparação, igualdade, slices vazios, macro `LSM_TRY` |
| `test_arena` | alocações pequenas/grandes, alinhamento, concorrência |
| `test_skiplist` | put/get/del, ordenação, tombstones, versões, concorrência |
| `test_wal` | append/recover, tombstones, escrita parcial, CRC corrompido |
| `test_sst` | roundtrip, not_found, tombstone, múltiplos blocos, CRC |
| `test_bloom` | zero falso negativo, taxa de falso positivo, encode/decode |
| `test_db` | put/get/del, flush, reopen via WAL e SSTable |
| `test_compaction` | redução de SSTables, tombstones eliminados, dados íntegros |
| `test_index` | busca por índice, phantom entry, reopen, múltiplos índices |
| `test_mvcc` | snapshot isolation, dois snapshots, tombstone por seq, compaction bloqueada |
| `test_crash_recovery` | recovery via WAL, via SSTable, WAL parcial, múltiplos crashes |

---

## Fuzzing

O fuzzer interpreta bytes arbitrários como sequências de operações (`PUT`, `GET`, `DEL`, `COMPACT`, `SNAPSHOT_OPEN`, `SNAPSHOT_GET`, `SNAPSHOT_RELEASE`) e executa milhares de combinações por segundo com AddressSanitizer ativo.

```bash
# build com Clang (obrigatório para libFuzzer)
cmake --preset fuzzer
cmake --build build/fuzzer

# cria o diretório de corpus e roda
mkdir -p fuzz/corpus
./build/fuzzer/fuzz_db -max_total_time=60 fuzz/corpus/

# com dicionário (mais eficiente)
./build/fuzzer/fuzz_db \
    -max_total_time=60 \
    -dict=fuzz/tinylsm.dict \
    fuzz/corpus/

# via script
./scripts/run_fuzzer.sh
```

Inputs que descobriram novos caminhos de código ficam em `fuzz/corpus/` e são reutilizados em execuções futuras (coverage-guided fuzzing).

---

## Decisões de design

### Por que skip list em vez de red-black tree?

- Iteração em ordem é apenas percorrer o nível 0: cache-friendly
- Implementação muito mais simples sem bugs sutis de rotação
- Mesma complexidade assintótica O(log n)

### Por que arena em vez de malloc por nó?

- Alocar = somar um inteiro (sem lock de heap)
- Liberar = `free` por bloco, não por nó: O(num_blocos)
- Melhor localidade de cache: nós alocados sequencialmente

### Por que tombstones em vez de deleção in-place?

Arquivos SSTable são imutáveis após criação. Uma deleção precisa "vencer" versões mais antigas da mesma chave que existem em SSTables anteriores. O tombstone (`OP_DEL`) é propagado até a compaction total, onde pode ser eliminado com segurança.

### Por que CRC-32 e não checksums mais robustos?

CRC-32 com o polinômio Ethernet/ZIP detecta todos os erros de burst de até 32 bits — suficiente para detectar truncamento e corrupção de escrita parcial. Custo computacional muito menor que SHA ou BLAKE. O mesmo padrão usado pelo ext4, ZFS e pelo protocolo Ethernet.

### Por que `seq DESC` para a mesma chave?

Com ordenação `(user_key ASC, seq DESC)`, a versão mais nova de uma chave vem imediatamente após o seek para `(key, UINT64_MAX)`. O `Get` não precisa varrer versões antigas — O(log n) puro.

### Por que compaction bloqueia com snapshots ativos?

A compaction descarta versões antigas de chaves. Se um snapshot ainda precisa ler uma versão com `seq < snapshot_seq`, e a compaction eliminar essa versão, o snapshot retornaria dados incorretos. A solução correta (usada aqui) é impedir compaction enquanto `num_snapshots > 0`.

---

## Roadmap de aprendizado

O projeto foi construído incrementalmente, um passo de cada vez, com testes passando em `debug` e `asan` antes de avançar:

| Passo | Componente | Conceitos |
|-------|-----------|-----------|
| 1 | Arena + SkipList | Alocadores, estruturas de dados ordenadas, RWLock |
| 2 | WAL | Durabilidade, CRC, serialização, crash recovery |
| 3 | SSTable | Arquivos imutáveis, índice de blocos, iteradores |
| 4 | Bloom Filter | Estruturas probabilísticas, double hashing |
| 5 | DB API | Integração de componentes, flush automático |
| 6 | Compaction | K-way merge, fases para minimizar lock contention |
| 7 | Secondary Index | Índices como primeira classe, consistência eventual |
| 8 | MVCC | Snapshot isolation, sequence numbers, guardas de compaction |
| 9 | Fuzzing + Recovery | libFuzzer, coverage-guided testing, crash simulation |

---

## Licença

MIT
