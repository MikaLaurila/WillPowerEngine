/**
 * @file crc32.h
 * @brief CRC-32 (IEEE 802.3, the zlib/PNG polynomial) for integrity checks.
 *
 * - Dependencies: none
 * - Thread affinity: any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_CRC32_H
#define WPE_CORE_CRC32_H

#include <stddef.h>
#include <stdint.h>

/**
 * Updates a running CRC-32. Start with crc = 0; feeding data in pieces gives the same result as
 * one call over the concatenation. wpe_crc32(0, "123456789", 9) == 0xCBF43926.
 *
 * @param data Bytes to add; may be NULL when size is 0.
 *
 * @thread_safety Any thread
 */
uint32_t wpe_crc32(uint32_t crc, const void* data, size_t size);

#endif /* WPE_CORE_CRC32_H */
