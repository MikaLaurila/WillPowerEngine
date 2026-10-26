/**
 * @file image.h
 * @brief Decoding of compressed images (JPEG, PNG) from memory.
 *
 * - Dependencies: core, platform (WPE_Image), stb_image (private)
 * - Thread affinity: any thread; decoding different images concurrently is safe
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ASSETS_IMAGE_H
#define WPE_ASSETS_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"
#include "platform/platform.h"

/** Images with more texels than this are rejected before decoding (protects against bombs). */
#define WPE_IMAGE_MAX_TEXELS (UINT64_C(16384) * UINT64_C(16384))

/**
 * Decodes a JPEG or PNG image to 0xAARRGGBB texels (opaque formats get alpha 255).
 *
 * @param data Encoded bytes; must not be NULL. Borrowed for the duration of the call.
 * @param size Byte count.
 * @param out_image Receives the image; release with wpe_image_free().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_UNSUPPORTED (unknown format
 *         or too large), WPE_RESULT_ERROR_PARSE (corrupt data) or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *         Failures are logged with `label` (may be NULL).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_image_decode(const void* data, size_t size, const char* label, WPE_Image* out_image);

#endif /* WPE_ASSETS_IMAGE_H */
