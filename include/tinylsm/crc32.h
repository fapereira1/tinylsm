#pragma once

#include <stddef.h>
#include <stdint.h>

/* Polinômio Ethernet/ZIP (CRC-32). Tabela inicializada via pthread_once. */
uint32_t crc32_begin(void);
uint32_t crc32_feed(uint32_t crc, const uint8_t *buf, size_t len);
uint32_t crc32_end(uint32_t crc);
