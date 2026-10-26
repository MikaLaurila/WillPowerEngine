/*
 * stb_image implementation unit. Compiled without the engine's warning flags (third-party code).
 * Only JPEG and PNG are enabled to minimize the decoder attack surface; memory goes through the
 * engine's tracked allocator so leak detection covers decoded images.
 */
#include <stddef.h>

#include "core/alloc.h"

#define STBI_MALLOC(size) wpe_alloc(size)
#define STBI_REALLOC(ptr, size) wpe_realloc(ptr, size)
#define STBI_FREE(ptr) wpe_free(ptr)
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
/* Thread-local failure strings (the default) keep concurrent decoding race-free. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
