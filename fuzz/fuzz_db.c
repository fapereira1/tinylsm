#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinylsm/db.h"

/*
 * fuzz_db.c — libFuzzer target para a DB API do tinylsm.
 *
 * Formato do input (sequência de operações):
 *   [op:1][key_len:1][key:key_len][val_len:1][val:val_len]
 *
 *   op % 7:
 *     0 = PUT
 *     1 = GET
 *     2 = DEL
 *     3 = COMPACT
 *     4 = SNAPSHOT_OPEN   (substitui snap corrente se houver)
 *     5 = SNAPSHOT_GET
 *     6 = SNAPSHOT_RELEASE
 *
 * O fuzzer exercita todos os caminhos da engine com inputs arbitrários.
 * Qualquer crash, assert, ou memory error é reportado como bug.
 */

#define FUZZ_DIR "/tmp/tinylsm_fuzz_run"

static void cleanup_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *ent;
  char path[512];
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    unlink(path);
  }
  closedir(d);
  rmdir(dir);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  cleanup_dir(FUZZ_DIR);

  db_opts_t opts = {.mem_limit_bytes = 4096};
  db_t *db = db_open(FUZZ_DIR, opts);
  if (!db)
    return 0;

  db_snapshot_t *snap = NULL;
  size_t pos = 0;

  while (pos + 2 <= size) {
    uint8_t op = data[pos++];
    uint8_t key_len = data[pos++];

    if (pos + key_len > size)
      break;
    const uint8_t *key_data = data + pos;
    pos += key_len;

    uint8_t val_len = 0;
    if (pos < size)
      val_len = data[pos++];
    if (pos + val_len > size)
      val_len = (uint8_t)(size - pos);
    const uint8_t *val_data = data + pos;
    pos += val_len;

    if (key_len == 0)
      continue;

    slice_t key = slice_make(key_data, key_len);
    slice_t val = slice_make(val_data, val_len);

    switch (op % 7) {
    case 0:
      db_put(db, key, val);
      break;
    case 1: {
      uint8_t *out;
      size_t olen;
      lsm_status_t s = db_get(db, key, &out, &olen);
      if (s == LSM_OK)
        free(out);
      break;
    }
    case 2:
      db_del(db, key);
      break;
    case 3:
      db_compact(db);
      break;
    case 4:
      if (snap) {
        db_snapshot_release(snap);
        snap = NULL;
      }
      snap = db_snapshot_open(db);
      break;
    case 5: {
      if (!snap)
        break;
      uint8_t *out;
      size_t olen;
      lsm_status_t s = db_snapshot_get(snap, key, &out, &olen);
      if (s == LSM_OK)
        free(out);
      break;
    }
    case 6:
      if (snap) {
        db_snapshot_release(snap);
        snap = NULL;
      }
      break;
    }
  }

  if (snap)
    db_snapshot_release(snap);
  db_close(db);
  cleanup_dir(FUZZ_DIR);
  return 0;
}
