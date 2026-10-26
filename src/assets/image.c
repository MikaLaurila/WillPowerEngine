#define WPE_LOG_MODULE "assets"
#include "assets/image.h"

#include <limits.h>
#include <stb_image.h>

#include "core/alloc.h"
#include "core/log.h"

WPE_Result wpe_image_decode(const void* data, size_t size, const char* label, WPE_Image* out_image) {
    if (!data || !out_image) return WPE_RESULT_ERROR_NULL_ARG;
    *out_image = (WPE_Image){0};
    label = label ? label : "(memory)";
    if (size == 0 || size > INT_MAX) {
        wpe_log_error("image '%s': invalid encoded size %zu", label, size);
        return WPE_RESULT_ERROR_PARSE;
    }

    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(data, (int)size, &width, &height, &channels)) {
        wpe_log_error("image '%s': unsupported or corrupt format (%s)", label, stbi_failure_reason());
        return WPE_RESULT_ERROR_UNSUPPORTED;
    }
    if (width <= 0 || height <= 0 || (uint64_t)width * (uint64_t)height > WPE_IMAGE_MAX_TEXELS) {
        wpe_log_error("image '%s': dimensions %dx%d exceed the supported maximum", label, width, height);
        return WPE_RESULT_ERROR_UNSUPPORTED;
    }

    unsigned char* rgba = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
    if (!rgba) {
        wpe_log_error("image '%s': decode failed (%s)", label, stbi_failure_reason());
        return WPE_RESULT_ERROR_PARSE;
    }

    /* Repack R,G,B,A bytes into 0xAARRGGBB words in place (same size per texel). */
    const size_t texels = (size_t)width * (size_t)height;
    uint32_t* packed = (uint32_t*)(void*)rgba;
    for (size_t i = 0; i < texels; ++i) {
        const unsigned char* p = rgba + i * 4;
        const uint32_t value = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        packed[i] = value;
    }

    *out_image = (WPE_Image){.width = (uint32_t)width, .height = (uint32_t)height, .pixels = packed};
    return WPE_RESULT_OK;
}
