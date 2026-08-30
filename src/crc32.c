#include "tinylsm/crc32.h"
#include <pthread.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint32_t       tab[256];

static void build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        tab[i] = c;
    }
}

uint32_t crc32_begin(void) { pthread_once(&once, build_table); return 0xFFFFFFFFu; }

uint32_t crc32_feed(uint32_t crc, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        crc = tab[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t crc32_end(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }
