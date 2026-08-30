#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinylsm/db.h"

static void die(const char *msg) {
    fprintf(stderr, "erro: %s\n", msg);
    exit(1);
}

static void usage(void) {
    fprintf(stderr,
        "uso: tinylsm_cli <dir> <comando> [args]\n"
        "\n"
        "comandos:\n"
        "  put <key> <value>   escreve key=value\n"
        "  get <key>           lê o valor de key\n"
        "  del <key>           deleta key\n"
        "\n"
        "exemplos:\n"
        "  tinylsm_cli /tmp/mydb put nome alice\n"
        "  tinylsm_cli /tmp/mydb get nome\n"
        "  tinylsm_cli /tmp/mydb del nome\n"
    );
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 3) usage();

    const char *dir = argv[1];
    const char *cmd = argv[2];

    db_opts_t opts = { .mem_limit_bytes = DB_DEFAULT_MEM_LIMIT };
    db_t *db = db_open(dir, opts);
    if (!db) die("falha ao abrir o banco");

    if (strcmp(cmd, "put") == 0) {
        if (argc != 5) usage();
        lsm_status_t s = db_put(db,
            slice_from_str(argv[3]),
            slice_from_str(argv[4]));
        if (s != LSM_OK) {
            fprintf(stderr, "put falhou: %s\n", lsm_status_str(s));
            db_close(db); return 1;
        }
        printf("OK\n");

    } else if (strcmp(cmd, "get") == 0) {
        if (argc != 4) usage();
        uint8_t *val; size_t vlen;
        lsm_status_t s = db_get(db,
            slice_from_str(argv[3]), &val, &vlen);
        if (s == LSM_NOT_FOUND) {
            printf("NOT_FOUND\n");
        } else if (s != LSM_OK) {
            fprintf(stderr, "get falhou: %s\n", lsm_status_str(s));
            db_close(db); return 1;
        } else {
            fwrite(val, 1, vlen, stdout);
            printf("\n");
            free(val);
        }

    } else if (strcmp(cmd, "del") == 0) {
        if (argc != 4) usage();
        lsm_status_t s = db_del(db, slice_from_str(argv[3]));
        if (s != LSM_OK) {
            fprintf(stderr, "del falhou: %s\n", lsm_status_str(s));
            db_close(db); return 1;
        }
        printf("OK\n");

    } else {
        db_close(db);
        usage();
    }

    db_close(db);
    return 0;
}
