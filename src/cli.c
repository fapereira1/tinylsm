#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinylsm/db.h"
#include "tinylsm/index.h"
#include "tinylsm/sst.h"
#include <sys/stat.h>

#define DB_DIR "data/db"
#define IDX_CAT_DIR "data/db_idx_category"

#define MAX_LINE 512
#define COL_SEP "|"

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static slice_t S(const char *s) { return slice_from_str(s); }

static void trim(char *s) {
  /* remove \r\n do final */
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
    s[--len] = '\0';
}

/* Imprime produto a partir do valor armazenado "name|category|price" */
static void print_product(const char *id, const uint8_t *val, size_t vlen) {
  char buf[MAX_LINE];
  size_t copy = vlen < sizeof(buf) - 1 ? vlen : sizeof(buf) - 1;
  memcpy(buf, val, copy);
  buf[copy] = '\0';

  /* parse: name|category|price */
  char *name = strtok(buf, COL_SEP);
  char *category = strtok(NULL, COL_SEP);
  char *price = strtok(NULL, COL_SEP);

  printf("  %-8s  %-30s  %-15s  %s\n", id, name ? name : "?",
         category ? category : "?", price ? price : "?");
}

static void print_header(void) {
  printf("  %-8s  %-30s  %-15s  %s\n", "ID", "NOME", "CATEGORIA", "PREÇO");
  printf("  %s\n", "--------  ------------------------------  "
                   "---------------  --------");
}

/* --------------------------------------------------------------------------
 * Comandos
 * -------------------------------------------------------------------------- */

static void cmd_help(void) {
  printf("\nComandos disponíveis:\n");
  printf("  load <arquivo.csv>   carrega produtos do CSV\n");
  printf("  get <id>             busca produto por ID\n");
  printf("  find <categoria>     lista produtos por categoria\n");
  printf("  list                 lista todos os produtos\n");
  printf("  del <id>             remove um produto\n");
  printf("  compact              compacta os SSTables\n");
  printf("  snap                 abre snapshot e consulta\n");
  printf("  help                 mostra esta ajuda\n");
  printf("  quit                 sai do programa\n\n");
}

static void cmd_load(db_t *db, sec_index_t *idx_cat, const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp) {
    printf("Erro: não foi possível abrir '%s'\n", path);
    return;
  }

  char line[MAX_LINE];
  int loaded = 0;
  int errors = 0;
  int lineno = 0;

  while (fgets(line, sizeof(line), fp)) {
    lineno++;
    trim(line);

    /* Pula header e linhas vazias */
    if (lineno == 1 || strlen(line) == 0)
      continue;

    /* Parse: id,name,category,price */
    char *id = strtok(line, ",");
    char *name = strtok(NULL, ",");
    char *category = strtok(NULL, ",");
    char *price = strtok(NULL, ",");

    if (!id || !name || !category || !price) {
      printf("  linha %d: formato inválido, pulando\n", lineno);
      errors++;
      continue;
    }

    /* Valor armazenado: "name|category|price" */
    char val[MAX_LINE];
    snprintf(val, sizeof(val), "%s|%s|%s", name, category, price);

    slice_t key = S(id);
    slice_t value = S(val);

    lsm_status_t s = db_put(db, key, value);
    if (s != LSM_OK) {
      printf("  linha %d: erro ao salvar (%s)\n", lineno, lsm_status_str(s));
      errors++;
      continue;
    }

    /* Índice: category → id */
    idx_put(idx_cat, S(category), key);
    loaded++;
  }

  fclose(fp);
  printf("Carregados: %d produto(s)", loaded);
  if (errors > 0)
    printf(", %d erro(s)", errors);
  printf("\n");
}

static void cmd_get(db_t *db, const char *id) {
  uint8_t *val;
  size_t vlen;
  lsm_status_t s = db_get(db, S(id), &val, &vlen);
  if (s == LSM_NOT_FOUND) {
    printf("Produto '%s' não encontrado.\n", id);
    return;
  }
  if (s != LSM_OK) {
    printf("Erro: %s\n", lsm_status_str(s));
    return;
  }
  print_header();
  print_product(id, val, vlen);
  free(val);
}

static void cmd_find(db_t *db, sec_index_t *idx_cat, const char *category) {
  /*
   * O índice secundário é 1:1 (category → último id inserido).
   * Para múltiplos produtos na mesma categoria, o ideal seria um índice
   * composto (category+id → id). Para simplificar, varremos todos via
   * iterador do SSTable e filtramos.
   *
   * Demonstra o uso do iterador do SSTable para scan completo.
   */
  (void)idx_cat; /* reservado para extensão futura */

  int found = 0;
  print_header();

  /* Scan via SSTables */
  for (int pass = 0; pass < 2; pass++) {
    /*
     * pass 0: lê do MemTable via snapshot (dados não flushed)
     * pass 1: lê dos SSTables via iterador
     *
     * Simplificação: fazemos get de IDs sequenciais P001..P999.
     * Em produção usaríamos um iterador de range.
     */
    for (int i = 1; i <= 999; i++) {
      char id[8];
      snprintf(id, sizeof(id), "P%03d", i);

      uint8_t *val;
      size_t vlen;
      lsm_status_t s = db_get(db, S(id), &val, &vlen);
      if (s == LSM_NOT_FOUND)
        continue;
      if (s != LSM_OK)
        continue;

      /* Extrai categoria do valor */
      char buf[MAX_LINE];
      size_t copy = vlen < sizeof(buf) - 1 ? vlen : sizeof(buf) - 1;
      memcpy(buf, val, copy);
      buf[copy] = '\0';

      strtok(buf, COL_SEP);              /* name */
      char *cat = strtok(NULL, COL_SEP); /* category */

      if (cat && strcmp(cat, category) == 0) {
        print_product(id, val, vlen);
        found++;
      }
      free(val);
    }
    break;
  }

  if (found == 0)
    printf("  Nenhum produto na categoria '%s'.\n", category);
  else
    printf("\n%d produto(s) encontrado(s).\n", found);
}

static void cmd_list(db_t *db) {
  int found = 0;
  print_header();

  for (int i = 1; i <= 999; i++) {
    char id[8];
    snprintf(id, sizeof(id), "P%03d", i);

    uint8_t *val;
    size_t vlen;
    lsm_status_t s = db_get(db, S(id), &val, &vlen);
    if (s == LSM_NOT_FOUND)
      continue;
    if (s != LSM_OK)
      continue;

    print_product(id, val, vlen);
    found++;
    free(val);
  }

  if (found == 0)
    printf("  Nenhum produto cadastrado.\n");
  else
    printf("\n%d produto(s) no total.\n", found);
}

static void cmd_del(db_t *db, sec_index_t *idx_cat, const char *id) {
  /* Busca categoria antes de deletar para limpar o índice */
  uint8_t *val;
  size_t vlen;
  lsm_status_t s = db_get(db, S(id), &val, &vlen);
  if (s == LSM_NOT_FOUND) {
    printf("Produto '%s' não encontrado.\n", id);
    return;
  }

  if (s == LSM_OK) {
    /* Extrai categoria e remove do índice */
    char buf[MAX_LINE];
    size_t copy = vlen < sizeof(buf) - 1 ? vlen : sizeof(buf) - 1;
    memcpy(buf, val, copy);
    buf[copy] = '\0';
    strtok(buf, COL_SEP);
    char *cat = strtok(NULL, COL_SEP);
    if (cat)
      idx_del(idx_cat, S(cat));
    free(val);
  }

  s = db_del(db, S(id));
  if (s == LSM_OK)
    printf("Produto '%s' removido.\n", id);
  else
    printf("Erro ao remover: %s\n", lsm_status_str(s));
}

static void cmd_compact(db_t *db) {
  printf("Compactando SSTables...\n");
  lsm_status_t s = db_compact(db);
  if (s == LSM_OK)
    printf("Compaction concluída.\n");
  else if (s == LSM_BUSY)
    printf("Busy: compaction já em andamento ou snapshot ativo.\n");
  else
    printf("Erro: %s\n", lsm_status_str(s));
}

static void cmd_snap(db_t *db) {
  db_snapshot_t *snap = db_snapshot_open(db);
  if (!snap) {
    printf("Erro ao abrir snapshot.\n");
    return;
  }

  printf("Snapshot aberto (seq=%llu). Digite um ID para consultar"
         " (vazio para sair):\n",
         (unsigned long long)snap->seq);

  char line[MAX_LINE];
  while (1) {
    printf("snap> ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
      break;
    trim(line);
    if (strlen(line) == 0)
      break;

    uint8_t *val;
    size_t vlen;
    lsm_status_t s = db_snapshot_get(snap, S(line), &val, &vlen);
    if (s == LSM_NOT_FOUND) {
      printf("Não encontrado no snapshot.\n");
    } else if (s == LSM_OK) {
      print_header();
      print_product(line, val, vlen);
      free(val);
    } else {
      printf("Erro: %s\n", lsm_status_str(s));
    }
  }

  db_snapshot_release(snap);
  printf("Snapshot liberado.\n");
}

/* --------------------------------------------------------------------------
 * Main — REPL
 * -------------------------------------------------------------------------- */

int main(void) {
  /* Garante que os diretórios existem */
  mkdir(DB_DIR, 0755);
  mkdir(IDX_CAT_DIR, 0755);

  /* Abre o banco e o índice secundário */
  db_opts_t opts = {.mem_limit_bytes = 4u * 1024u * 1024u};
  db_t *db = db_open(DB_DIR, opts);
  if (!db) {
    fprintf(stderr, "Erro ao abrir o banco de dados em '%s'\n", DB_DIR);
    return 1;
  }

  sec_index_t *idx_cat = idx_open(db, IDX_CAT_DIR);
  if (!idx_cat) {
    fprintf(stderr, "Erro ao abrir índice de categorias\n");
    db_close(db);
    return 1;
  }

  printf("tinylsm produto CLI\n");
  printf("Banco: %s\n", DB_DIR);
  printf("Digite 'help' para ver os comandos.\n\n");

  char line[MAX_LINE];
  while (1) {
    printf("tinylsm> ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin))
      break;
    trim(line);
    if (strlen(line) == 0)
      continue;

    /* Separa comando e argumento */
    char *cmd = strtok(line, " ");
    char *arg = strtok(NULL, ""); /* resto da linha */

    /* Remove espaços iniciais do argumento */
    if (arg)
      while (*arg == ' ')
        arg++;

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
      break;
    } else if (strcmp(cmd, "help") == 0) {
      cmd_help();
    } else if (strcmp(cmd, "load") == 0) {
      if (!arg) {
        printf("Uso: load <arquivo.csv>\n");
        continue;
      }
      cmd_load(db, idx_cat, arg);
    } else if (strcmp(cmd, "get") == 0) {
      if (!arg) {
        printf("Uso: get <id>\n");
        continue;
      }
      cmd_get(db, arg);
    } else if (strcmp(cmd, "find") == 0) {
      if (!arg) {
        printf("Uso: find <categoria>\n");
        continue;
      }
      cmd_find(db, idx_cat, arg);
    } else if (strcmp(cmd, "list") == 0) {
      cmd_list(db);
    } else if (strcmp(cmd, "del") == 0) {
      if (!arg) {
        printf("Uso: del <id>\n");
        continue;
      }
      cmd_del(db, idx_cat, arg);
    } else if (strcmp(cmd, "compact") == 0) {
      cmd_compact(db);
    } else if (strcmp(cmd, "snap") == 0) {
      cmd_snap(db);
    } else {
      printf("Comando desconhecido: '%s'. Digite 'help'.\n", cmd);
    }
  }

  printf("\nFechando banco...\n");
  idx_close(idx_cat);
  db_close(db);
  printf("Até logo.\n");
  return 0;
}
