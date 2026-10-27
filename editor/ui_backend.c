#define WPE_LOG_MODULE "editor"
#include "editor/ui_backend.h"

#include <SDL3/SDL.h>
#include <cimgui.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

/* Engine-side copy of an ImGui vertex: screen position, texture coordinates, color (0..1). */
typedef struct wpe__UiVertex {
    float x, y;
    float u, v;
    float color[4];
} wpe__UiVertex;

typedef struct wpe__UiUniforms {
    const WPE_Texture* texture;
    float inv_width, inv_height;
} wpe__UiUniforms;

/* A texture slot: ImTextureID n refers to textures[n - 1]. */
typedef struct wpe__UiTexture {
    const WPE_Texture* texture;
    WPE_Texture* owned; /* Created for an ImGui texture (NULL for registered engine textures). */
} wpe__UiTexture;

struct WPE_UiBackend {
    ImGuiContext* context;
    wpe__UiVertex* vertices;
    uint32_t vertex_capacity;
    uint32_t* indices;
    uint32_t index_capacity;
    wpe__UiTexture* textures;
    uint32_t texture_count, texture_capacity;
    uint32_t* staging; /* Texel conversion scratch. */
    size_t staging_capacity;
};

static bool wpe__ui_exists = false;

static const WPE_Sampler wpe__ui_sampler = {.mag_filter = WPE_FILTER_NEAREST,
                                            .min_filter = WPE_FILTER_NEAREST,
                                            .wrap_u = WPE_WRAP_CLAMP,
                                            .wrap_v = WPE_WRAP_CLAMP};

static void wpe__ui_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    const wpe__UiUniforms* u = uniforms;
    const wpe__UiVertex* v = vertex;
    *out_position =
        wpe_vec4(v->x * u->inv_width * 2.0f - 1.0f, 1.0f - v->y * u->inv_height * 2.0f, 0.0f, 1.0f);
    out[0] = v->u;
    out[1] = v->v;
    for (int i = 0; i < 4; ++i)
        out[2 + i] = v->color[i];
}

static bool wpe__ui_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const wpe__UiUniforms* u = uniforms;
    const float* v = in->varyings;
    WPE_Vec4 color = wpe_vec4(v[2], v[3], v[4], v[5]);
    if (u->texture) {
        const WPE_Vec4 t =
            wpe_texture_sample_lod(u->texture, &wpe__ui_sampler, (WPE_Vec2){.x = v[0], .y = v[1]}, 0.0f);
        color = wpe_vec4(color.x * t.x, color.y * t.y, color.z * t.z, color.w * t.w);
    }
    if (!(color.w > 0.0f)) return false;
    *out_color = color;
    return true;
}

static const WPE_ShaderProgram wpe__ui_program = {
    .vertex = wpe__ui_vs, .fragment = wpe__ui_fs, .varying_count = 6};

/* ImGui allocates through the engine so its memory is tracked like everything else. */
static void* wpe__ui_alloc(size_t size, void* user_data) {
    (void)user_data;
    return wpe_alloc(size ? size : 1);
}

static void wpe__ui_free(void* pointer, void* user_data) {
    (void)user_data;
    wpe_free(pointer);
}

WPE_Result wpe_ui_create(WPE_UiBackend** out_ui) {
    if (!out_ui) return WPE_RESULT_ERROR_NULL_ARG;
    *out_ui = NULL;
    if (wpe__ui_exists) {
        wpe_log_error("only one UI backend can exist at a time");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    WPE_UiBackend* ui = wpe_alloc_zeroed(1, sizeof *ui);
    if (!ui) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    igSetAllocatorFunctions(wpe__ui_alloc, wpe__ui_free, NULL);
    ui->context = igCreateContext(NULL);
    if (!ui->context) {
        wpe_free(ui);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    ImGuiIO* io = igGetIO_Nil();
    io->IniFilename = NULL; /* Layouts are built in code; nothing is written next to the editor. */
    io->ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io->BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io->BackendRendererName = "wpe_software";
    io->BackendPlatformName = "wpe_sdl3";
    igStyleColorsDark(NULL);
    wpe__ui_exists = true;
    *out_ui = ui;
    return WPE_RESULT_OK;
}

void wpe_ui_destroy(WPE_UiBackend* ui) {
    if (!ui) return;
    /* ImGui marks its textures for destruction when the context goes; the slots own them either way. */
    igDestroyContext(ui->context);
    for (uint32_t i = 0; i < ui->texture_count; ++i)
        wpe_texture_destroy(ui->textures[i].owned);
    wpe_free(ui->textures);
    wpe_free(ui->vertices);
    wpe_free(ui->indices);
    wpe_free(ui->staging);
    wpe_free(ui);
    wpe__ui_exists = false;
}

/* ---- Textures ------------------------------------------------------------------------------------- */

static uint32_t wpe__ui_slot(WPE_UiBackend* ui) {
    for (uint32_t i = 0; ui->textures && i < ui->texture_count; ++i) {
        if (!ui->textures[i].texture) return i;
    }
    if (!ui->textures || ui->texture_count == ui->texture_capacity) {
        const uint32_t capacity = ui->texture_capacity ? ui->texture_capacity * 2 : 8;
        wpe__UiTexture* textures = wpe_realloc(ui->textures, (size_t)capacity * sizeof *textures);
        if (!textures) return UINT32_MAX;
        ui->textures = textures;
        ui->texture_capacity = capacity;
    }
    ui->textures[ui->texture_count] = (wpe__UiTexture){0};
    return ui->texture_count++;
}

uint64_t wpe_ui_texture_id(WPE_UiBackend* ui, const WPE_Texture* texture) {
    if (!ui || !texture) return 0;
    for (uint32_t i = 0; i < ui->texture_count; ++i) {
        if (ui->textures[i].texture == texture) return (uint64_t)i + 1;
    }
    const uint32_t slot = wpe__ui_slot(ui);
    if (slot == UINT32_MAX) return 0;
    ui->textures[slot] = (wpe__UiTexture){.texture = texture};
    return (uint64_t)slot + 1;
}

/* Converts ImGui's RGBA32 (bytes R, G, B, A) or Alpha8 texels to the engine's 0xAARRGGBB. */
static const uint32_t* wpe__ui_convert(WPE_UiBackend* ui, const ImTextureData* t) {
    const size_t count = (size_t)t->Width * (size_t)t->Height;
    if (count > ui->staging_capacity) {
        uint32_t* staging = wpe_realloc(ui->staging, count * sizeof *staging);
        if (!staging) return NULL;
        ui->staging = staging;
        ui->staging_capacity = count;
    }
    const unsigned char* p = t->Pixels;
    for (size_t i = 0; i < count; ++i) {
        if (t->Format == ImTextureFormat_Alpha8) {
            ui->staging[i] = ((uint32_t)p[i] << 24) | 0x00FFFFFFu;
        } else {
            const unsigned char* px = p + i * 4;
            ui->staging[i] =
                ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        }
    }
    return ui->staging;
}

/* Carries out ImGui's texture requests (create, update, destroy). */
static WPE_Result wpe__ui_update_textures(WPE_UiBackend* ui, ImDrawData* data) {
    if (!data->Textures) return WPE_RESULT_OK;
    for (int i = 0; i < data->Textures->Size; ++i) {
        ImTextureData* t = data->Textures->Data[i];
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            const uint32_t* texels = wpe__ui_convert(ui, t);
            if (!texels) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
            const uint64_t id = t->TexID;
            wpe__UiTexture* slot = id >= 1 && id <= ui->texture_count ? &ui->textures[id - 1] : NULL;
            if (slot && slot->owned && slot->owned->levels[0].width == (uint32_t)t->Width &&
                slot->owned->levels[0].height == (uint32_t)t->Height) {
                const WPE_Result result = wpe_texture_update(slot->owned, texels);
                if (result != WPE_RESULT_OK) return result;
            } else {
                /* New, or resized (the atlas grew): build a fresh texture in the same or a new slot. */
                WPE_Texture* texture = NULL;
                const WPE_Result result = wpe_texture_create(&(WPE_TextureDesc){.width = (uint32_t)t->Width,
                                                                                .height = (uint32_t)t->Height,
                                                                                .texels = texels},
                                                             &texture);
                if (result != WPE_RESULT_OK) return result;
                if (!slot) {
                    const uint32_t index = wpe__ui_slot(ui);
                    if (index == UINT32_MAX) {
                        wpe_texture_destroy(texture);
                        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
                    }
                    slot = &ui->textures[index];
                    ImTextureData_SetTexID(t, (ImTextureID)index + 1);
                }
                wpe_texture_destroy(slot->owned);
                *slot = (wpe__UiTexture){.texture = texture, .owned = texture};
            }
            ImTextureData_SetStatus(t, ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
            const uint64_t id = t->TexID;
            if (id >= 1 && id <= ui->texture_count) {
                wpe_texture_destroy(ui->textures[id - 1].owned);
                ui->textures[id - 1] = (wpe__UiTexture){0};
            }
            ImTextureData_SetTexID(t, ImTextureID_Invalid);
            ImTextureData_SetStatus(t, ImTextureStatus_Destroyed);
        }
    }
    return WPE_RESULT_OK;
}

/* ---- Frames --------------------------------------------------------------------------------------- */

void wpe_ui_new_frame(WPE_UiBackend* ui, uint32_t width, uint32_t height, float dt) {
    if (!wpe_check(ui != NULL)) return;
    ImGuiIO* io = igGetIO_Nil();
    io->DisplaySize = (ImVec2_c){(float)width, (float)height};
    io->DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
    igNewFrame();
}

static bool wpe__ui_reserve(WPE_UiBackend* ui, uint32_t vertices, uint32_t indices) {
    if (vertices > ui->vertex_capacity) {
        wpe__UiVertex* v = wpe_realloc(ui->vertices, (size_t)vertices * sizeof *v);
        if (!v) return false;
        ui->vertices = v;
        ui->vertex_capacity = vertices;
    }
    if (indices > ui->index_capacity) {
        uint32_t* i = wpe_realloc(ui->indices, (size_t)indices * sizeof *i);
        if (!i) return false;
        ui->indices = i;
        ui->index_capacity = indices;
    }
    return true;
}

static uint32_t wpe__ui_clip(float value, uint32_t limit) {
    if (!(value > 0.0f)) return 0;
    return value >= (float)limit ? limit : (uint32_t)value;
}

WPE_Result wpe_ui_render(WPE_UiBackend* ui, WPE_Renderer* renderer) {
    if (!ui || !renderer) return WPE_RESULT_ERROR_NULL_ARG;
    igRender();
    ImDrawData* data = igGetDrawData();
    WPE_Result result = wpe__ui_update_textures(ui, data);
    if (result != WPE_RESULT_OK || data->TotalVtxCount == 0) return result;
    if (!wpe__ui_reserve(ui, (uint32_t)data->TotalVtxCount, (uint32_t)data->TotalIdxCount))
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;

    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    const uint32_t width = fb ? fb->width : 1, height = fb ? fb->height : 1;
    const wpe__UiUniforms base = {.inv_width = 1.0f / (float)width, .inv_height = 1.0f / (float)height};
    WPE_RenderState state = wpe_render_state_default();
    state.cull = WPE_CULL_NONE;
    state.depth_func = WPE_DEPTH_ALWAYS;
    state.depth_write = false;
    state.blend = WPE_BLEND_ALPHA;

    /* One vertex and index array for the whole frame; indices are made absolute. */
    uint32_t vertex_base = 0, index_base = 0;
    uint64_t order = 0;
    for (int l = 0; l < data->CmdLists.Size && result == WPE_RESULT_OK; ++l) {
        const ImDrawList* list = data->CmdLists.Data[l];
        for (int v = 0; v < list->VtxBuffer.Size; ++v) {
            const ImDrawVert* src = &list->VtxBuffer.Data[v];
            wpe__UiVertex* dst = &ui->vertices[vertex_base + (uint32_t)v];
            dst->x = src->pos.x - data->DisplayPos.x;
            dst->y = src->pos.y - data->DisplayPos.y;
            dst->u = src->uv.x;
            dst->v = src->uv.y;
            for (int c = 0; c < 4; ++c)
                dst->color[c] = (float)((src->col >> (8 * c)) & 0xFFu) / 255.0f; /* ImU32 is ABGR. */
        }
        for (int c = 0; c < list->CmdBuffer.Size && result == WPE_RESULT_OK; ++c) {
            ImDrawCmd* cmd = &list->CmdBuffer.Data[c];
            if (cmd->UserCallback || cmd->ElemCount == 0) continue; /* The editor registers no callbacks. */
            const uint32_t first = index_base + cmd->IdxOffset;
            for (uint32_t i = 0; i < cmd->ElemCount; ++i)
                ui->indices[first + i] =
                    vertex_base + cmd->VtxOffset + list->IdxBuffer.Data[cmd->IdxOffset + i];
            const uint64_t id = ImDrawCmd_GetTexID(cmd);
            wpe__UiUniforms uniforms = base;
            uniforms.texture = id >= 1 && id <= ui->texture_count ? ui->textures[id - 1].texture : NULL;
            const uint32_t x0 = wpe__ui_clip(cmd->ClipRect.x - data->DisplayPos.x, width);
            const uint32_t y0 = wpe__ui_clip(cmd->ClipRect.y - data->DisplayPos.y, height);
            const uint32_t x1 = wpe__ui_clip(cmd->ClipRect.z - data->DisplayPos.x + 0.999f, width);
            const uint32_t y1 = wpe__ui_clip(cmd->ClipRect.w - data->DisplayPos.y + 0.999f, height);
            if (x1 <= x0 || y1 <= y0) continue;
            result =
                wpe_renderer_submit(renderer, &(WPE_DrawDesc){.program = &wpe__ui_program,
                                                              .uniforms = &uniforms,
                                                              .uniforms_size = sizeof uniforms,
                                                              .vertices = ui->vertices,
                                                              .vertex_stride = sizeof(wpe__UiVertex),
                                                              .vertex_count = (uint32_t)data->TotalVtxCount,
                                                              .indices = ui->indices + first,
                                                              .index_count = cmd->ElemCount,
                                                              .state = state,
                                                              .sort_key = order++,
                                                              .scissor = true,
                                                              .scissor_x = x0,
                                                              .scissor_y = y0,
                                                              .scissor_width = x1 - x0,
                                                              .scissor_height = y1 - y0});
        }
        vertex_base += (uint32_t)list->VtxBuffer.Size;
        index_base += (uint32_t)list->IdxBuffer.Size;
    }
    return result;
}

/* ---- Input ---------------------------------------------------------------------------------------- */

static ImGuiKey wpe__ui_key(SDL_Scancode scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return (ImGuiKey)(ImGuiKey_A + (scancode - SDL_SCANCODE_A));
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
        return (ImGuiKey)(ImGuiKey_1 + (scancode - SDL_SCANCODE_1));
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
        return (ImGuiKey)(ImGuiKey_F1 + (scancode - SDL_SCANCODE_F1));
    static const struct {
        SDL_Scancode scancode;
        ImGuiKey key;
    } map[] = {
        {SDL_SCANCODE_0, ImGuiKey_0},
        {SDL_SCANCODE_RETURN, ImGuiKey_Enter},
        {SDL_SCANCODE_ESCAPE, ImGuiKey_Escape},
        {SDL_SCANCODE_BACKSPACE, ImGuiKey_Backspace},
        {SDL_SCANCODE_TAB, ImGuiKey_Tab},
        {SDL_SCANCODE_SPACE, ImGuiKey_Space},
        {SDL_SCANCODE_MINUS, ImGuiKey_Minus},
        {SDL_SCANCODE_EQUALS, ImGuiKey_Equal},
        {SDL_SCANCODE_LEFTBRACKET, ImGuiKey_LeftBracket},
        {SDL_SCANCODE_RIGHTBRACKET, ImGuiKey_RightBracket},
        {SDL_SCANCODE_BACKSLASH, ImGuiKey_Backslash},
        {SDL_SCANCODE_SEMICOLON, ImGuiKey_Semicolon},
        {SDL_SCANCODE_APOSTROPHE, ImGuiKey_Apostrophe},
        {SDL_SCANCODE_GRAVE, ImGuiKey_GraveAccent},
        {SDL_SCANCODE_COMMA, ImGuiKey_Comma},
        {SDL_SCANCODE_PERIOD, ImGuiKey_Period},
        {SDL_SCANCODE_SLASH, ImGuiKey_Slash},
        {SDL_SCANCODE_INSERT, ImGuiKey_Insert},
        {SDL_SCANCODE_HOME, ImGuiKey_Home},
        {SDL_SCANCODE_PAGEUP, ImGuiKey_PageUp},
        {SDL_SCANCODE_DELETE, ImGuiKey_Delete},
        {SDL_SCANCODE_END, ImGuiKey_End},
        {SDL_SCANCODE_PAGEDOWN, ImGuiKey_PageDown},
        {SDL_SCANCODE_RIGHT, ImGuiKey_RightArrow},
        {SDL_SCANCODE_LEFT, ImGuiKey_LeftArrow},
        {SDL_SCANCODE_DOWN, ImGuiKey_DownArrow},
        {SDL_SCANCODE_UP, ImGuiKey_UpArrow},
        {SDL_SCANCODE_KP_ENTER, ImGuiKey_KeypadEnter},
        {SDL_SCANCODE_LCTRL, ImGuiKey_LeftCtrl},
        {SDL_SCANCODE_LSHIFT, ImGuiKey_LeftShift},
        {SDL_SCANCODE_LALT, ImGuiKey_LeftAlt},
        {SDL_SCANCODE_LGUI, ImGuiKey_LeftSuper},
        {SDL_SCANCODE_RCTRL, ImGuiKey_RightCtrl},
        {SDL_SCANCODE_RSHIFT, ImGuiKey_RightShift},
        {SDL_SCANCODE_RALT, ImGuiKey_RightAlt},
        {SDL_SCANCODE_RGUI, ImGuiKey_RightSuper},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) {
        if (map[i].scancode == scancode) return map[i].key;
    }
    return ImGuiKey_None;
}

void wpe_ui_handle_event(WPE_UiBackend* ui, const WPE_PlatformEvent* platform_event) {
    if (!wpe_check(ui != NULL && platform_event != NULL)) return;
    const SDL_Event* e = (const SDL_Event*)platform_event;
    ImGuiIO* io = igGetIO_Nil();
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        ImGuiIO_AddMousePosEvent(io, e->motion.x, e->motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        /* SDL: 1 left, 2 middle, 3 right; ImGui: 0 left, 1 right, 2 middle. */
        static const int buttons[] = {-1, 0, 2, 1, 3, 4};
        if (e->button.button < sizeof buttons / sizeof buttons[0] && buttons[e->button.button] >= 0) {
            ImGuiIO_AddMousePosEvent(io, e->button.x, e->button.y);
            ImGuiIO_AddMouseButtonEvent(io, buttons[e->button.button], e->button.down);
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        ImGuiIO_AddMouseWheelEvent(io, -e->wheel.x, e->wheel.y);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const SDL_Keymod mod = e->key.mod;
        ImGuiIO_AddKeyEvent(io, ImGuiMod_Ctrl, (mod & SDL_KMOD_CTRL) != 0);
        ImGuiIO_AddKeyEvent(io, ImGuiMod_Shift, (mod & SDL_KMOD_SHIFT) != 0);
        ImGuiIO_AddKeyEvent(io, ImGuiMod_Alt, (mod & SDL_KMOD_ALT) != 0);
        ImGuiIO_AddKeyEvent(io, ImGuiMod_Super, (mod & SDL_KMOD_GUI) != 0);
        const ImGuiKey key = wpe__ui_key(e->key.scancode);
        if (key != ImGuiKey_None) ImGuiIO_AddKeyEvent(io, key, e->key.down);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        ImGuiIO_AddInputCharactersUTF8(io, e->text.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ImGuiIO_AddFocusEvent(io, e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        ImGuiIO_AddMousePosEvent(io, -3.402823466e38f, -3.402823466e38f); /* -FLT_MAX: no mouse. */
        break;
    default:
        break;
    }
}

bool wpe_ui_wants_mouse(const WPE_UiBackend* ui) {
    return ui && igGetIO_Nil()->WantCaptureMouse;
}

bool wpe_ui_wants_keyboard(const WPE_UiBackend* ui) {
    return ui && igGetIO_Nil()->WantCaptureKeyboard;
}
