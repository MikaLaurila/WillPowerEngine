#define WPE_LOG_MODULE "audio"
#include "audio/audio.h"

#include <math.h>
#include <miniaudio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "scene/scene.h"

#define WPE__DEFAULT_SAMPLE_RATE 48000u
#define WPE__DEFAULT_CHANNELS 2u
#define WPE__MAX_SOUND_SECONDS (30u * 60u) /* Longer files should be streamed as music. */
#define WPE__VOICE_INDEX_BITS 8u
#define WPE__VOICE_INDEX_MASK ((1u << WPE__VOICE_INDEX_BITS) - 1u)
#define WPE__VOICE_MAX_GENERATION ((1u << (32u - WPE__VOICE_INDEX_BITS)) - 1u)

static_assert(WPE_AUDIO_MAX_VOICES < (1u << WPE__VOICE_INDEX_BITS), "voice index must fit its bits");

/* A decoded sound asset (WPE_ASSET_SOUND). */
typedef struct wpe__Sound {
    float* pcm; /* Interleaved 32-bit float. */
    uint64_t frames;
    uint32_t channels;
    uint32_t sample_rate;
} wpe__Sound;

typedef enum wpe__VoiceKind {
    WPE__VOICE_FREE = 0,
    WPE__VOICE_ONESHOT,
    WPE__VOICE_SOURCE,
    WPE__VOICE_MUSIC,
} wpe__VoiceKind;

typedef struct wpe__Voice {
    ma_sound sound;
    ma_audio_buffer buffer; /* Effects: reads the shared PCM of `asset`. */
    ma_decoder decoder;     /* Music: streams from the VFS. */
    WPE_AssetHandle asset;  /* Reference held while the voice exists (effects). */
    WPE_Entity entity;      /* Source voices: the owning entity. */
    uint64_t serial;        /* Start order (voice stealing takes the oldest one-shot). */
    uint32_t generation;
    wpe__VoiceKind kind;
    bool loop;
    bool stopping; /* Music fading out before it is recycled. */
} wpe__Voice;

/* miniaudio VFS over the engine VFS (miniaudio expects the callbacks first). */
typedef struct wpe__MaVfs {
    ma_vfs_callbacks callbacks;
    const WPE_Vfs* vfs;
} wpe__MaVfs;

struct WPE_Audio {
    WPE_Ecs* ecs;
    WPE_AssetManager* assets;
    ma_engine engine;
    ma_sound_group groups[WPE_AUDIO_BUS_COUNT]; /* [MASTER] unused: the engine volume is the master. */
    float volumes[WPE_AUDIO_BUS_COUNT];
    ma_allocation_callbacks allocation;
    wpe__MaVfs vfs;
    wpe__Voice voices[WPE_AUDIO_MAX_VOICES];
    WPE_Voice music; /* The current (not fading-out) music voice. */
    uint64_t serial;
    WPE_Entity listener;
    uint32_t voices_lost;
    bool device;
    bool groups_ready;
};

/* ---- Allocation ---------------------------------------------------------------------------------- */

static void* wpe__ma_malloc(size_t size, void* user_data) {
    (void)user_data;
    return wpe_alloc(size);
}

static void* wpe__ma_realloc(void* memory, size_t size, void* user_data) {
    (void)user_data;
    return wpe_realloc(memory, size);
}

static void wpe__ma_free(void* memory, void* user_data) {
    (void)user_data;
    wpe_free(memory);
}

static const ma_allocation_callbacks wpe__ma_allocation = {
    .onMalloc = wpe__ma_malloc, .onRealloc = wpe__ma_realloc, .onFree = wpe__ma_free};

/* ---- miniaudio VFS ------------------------------------------------------------------------------- */

static const WPE_Vfs* wpe__engine_vfs(ma_vfs* vfs) {
    return ((const wpe__MaVfs*)vfs)->vfs;
}

static ma_result wpe__vfs_open(ma_vfs* vfs, const char* path, ma_uint32 mode, ma_vfs_file* out_file) {
    if (mode & MA_OPEN_MODE_WRITE) return MA_ACCESS_DENIED;
    WPE_VfsFile* file = NULL;
    const WPE_Result result = wpe_vfs_open(wpe__engine_vfs(vfs), path, &file);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) return MA_DOES_NOT_EXIST;
    if (result == WPE_RESULT_ERROR_OUT_OF_MEMORY) return MA_OUT_OF_MEMORY;
    if (result != WPE_RESULT_OK) return MA_ERROR;
    *out_file = file;
    return MA_SUCCESS;
}

static ma_result wpe__vfs_open_w(ma_vfs* vfs, const wchar_t* path, ma_uint32 mode, ma_vfs_file* out_file) {
    (void)vfs;
    (void)path;
    (void)mode;
    (void)out_file;
    return MA_NOT_IMPLEMENTED; /* Asset paths are UTF-8. */
}

static ma_result wpe__vfs_close(ma_vfs* vfs, ma_vfs_file file) {
    (void)vfs;
    wpe_vfs_file_close(file);
    return MA_SUCCESS;
}

static ma_result wpe__vfs_read(ma_vfs* vfs, ma_vfs_file file, void* out, size_t size, size_t* out_read) {
    (void)vfs;
    const size_t read = wpe_vfs_file_read(file, out, size);
    if (out_read) *out_read = read;
    return read == 0 && size > 0 ? MA_AT_END : MA_SUCCESS;
}

static ma_result wpe__vfs_write(ma_vfs* vfs, ma_vfs_file file, const void* data, size_t size,
                                size_t* out_written) {
    (void)vfs;
    (void)file;
    (void)data;
    (void)size;
    if (out_written) *out_written = 0;
    return MA_ACCESS_DENIED;
}

static ma_result wpe__vfs_seek(ma_vfs* vfs, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
    (void)vfs;
    int64_t base = 0;
    if (origin == ma_seek_origin_current) base = (int64_t)wpe_vfs_file_tell(file);
    if (origin == ma_seek_origin_end) base = (int64_t)wpe_vfs_file_size(file);
    const int64_t target = base + offset;
    if (target < 0) return MA_INVALID_ARGS;
    return wpe_vfs_file_seek(file, (uint64_t)target) == WPE_RESULT_OK ? MA_SUCCESS : MA_BAD_SEEK;
}

static ma_result wpe__vfs_tell(ma_vfs* vfs, ma_vfs_file file, ma_int64* out_cursor) {
    (void)vfs;
    *out_cursor = (ma_int64)wpe_vfs_file_tell(file);
    return MA_SUCCESS;
}

static ma_result wpe__vfs_info(ma_vfs* vfs, ma_vfs_file file, ma_file_info* out_info) {
    (void)vfs;
    out_info->sizeInBytes = wpe_vfs_file_size(file);
    return MA_SUCCESS;
}

/* ---- Sound assets -------------------------------------------------------------------------------- */

static WPE_Result wpe__load_sound(WPE_AssetManager* assets, const char* path, void* user_data,
                                  void** out_asset) {
    (void)user_data;
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(wpe_asset_manager_vfs(assets), path, &data, &size);
    if (result != WPE_RESULT_OK) return result;

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0); /* Native channels and rate. */
    config.allocationCallbacks = wpe__ma_allocation;
    ma_decoder decoder;
    if (ma_decoder_init_memory(data, size, &config, &decoder) != MA_SUCCESS) {
        wpe_free(data);
        wpe_log_error("'%s' is not a WAV, FLAC or MP3 file", path);
        return WPE_RESULT_ERROR_UNSUPPORTED;
    }
    wpe__Sound* sound = wpe_alloc_zeroed(1, sizeof *sound);
    if (!sound) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (result == WPE_RESULT_OK) {
        sound->channels = decoder.outputChannels;
        sound->sample_rate = decoder.outputSampleRate;
        if (sound->channels == 0 || sound->channels > 8 || sound->sample_rate == 0) {
            wpe_log_error("'%s' has an unsupported channel layout (%u channels)", path, sound->channels);
            result = WPE_RESULT_ERROR_UNSUPPORTED;
        }
    }
    /* Decode in chunks: some formats (MP3) do not know their exact length up front. */
    const uint64_t max_frames = (uint64_t)WPE__MAX_SOUND_SECONDS * (sound ? sound->sample_rate : 0);
    uint64_t capacity = 0;
    while (result == WPE_RESULT_OK) {
        if (sound->frames == capacity) {
            capacity = capacity ? capacity * 2 : 16384;
            if (capacity > max_frames + 1) capacity = max_frames + 1;
            if (capacity <= sound->frames) {
                wpe_log_error("'%s' is longer than %u minutes; play it as music instead", path,
                              WPE__MAX_SOUND_SECONDS / 60u);
                result = WPE_RESULT_ERROR_CAPACITY;
                break;
            }
            float* pcm = wpe_realloc(sound->pcm, (size_t)(capacity * sound->channels * sizeof(float)));
            if (!pcm) {
                result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
                break;
            }
            sound->pcm = pcm;
        }
        ma_uint64 read = 0;
        const ma_result status = ma_decoder_read_pcm_frames(
            &decoder, sound->pcm + sound->frames * sound->channels, capacity - sound->frames, &read);
        sound->frames += read;
        if (status == MA_AT_END || read == 0) break;
        if (status != MA_SUCCESS) {
            wpe_log_error("'%s' failed to decode", path);
            result = WPE_RESULT_ERROR_PARSE;
        }
    }
    ma_decoder_uninit(&decoder);
    wpe_free(data);
    if (result == WPE_RESULT_OK && sound->frames == 0) {
        wpe_log_error("'%s' contains no audio", path);
        result = WPE_RESULT_ERROR_PARSE;
    }
    if (result != WPE_RESULT_OK) {
        if (sound) wpe_free(sound->pcm);
        wpe_free(sound);
        return result;
    }
    *out_asset = sound;
    return WPE_RESULT_OK;
}

static void wpe__destroy_sound(void* asset, void* user_data) {
    (void)user_data;
    wpe__Sound* sound = asset;
    wpe_free(sound->pcm);
    wpe_free(sound);
}

bool wpe_audio_sound_info(const WPE_AssetManager* assets, WPE_AssetHandle sound, double* out_seconds,
                          uint32_t* out_channels) {
    const wpe__Sound* s = wpe_asset_get(assets, sound, WPE_ASSET_SOUND);
    if (!s) return false;
    if (out_seconds) *out_seconds = (double)s->frames / (double)s->sample_rate;
    if (out_channels) *out_channels = s->channels;
    return true;
}

/* ---- Voices -------------------------------------------------------------------------------------- */

static WPE_Voice wpe__voice_id(const WPE_Audio* audio, uint32_t index) {
    return (audio->voices[index].generation << WPE__VOICE_INDEX_BITS) | (index + 1u);
}

/* Voice of a live id, or NULL. */
static wpe__Voice* wpe__voice(const WPE_Audio* audio, WPE_Voice id) {
    if (!audio || id == WPE_VOICE_NONE) return NULL;
    const uint32_t index = (id & WPE__VOICE_INDEX_MASK) - 1u;
    if (index >= WPE_AUDIO_MAX_VOICES) return NULL;
    wpe__Voice* voice = (wpe__Voice*)&audio->voices[index];
    if (voice->kind == WPE__VOICE_FREE || voice->generation != id >> WPE__VOICE_INDEX_BITS) return NULL;
    return voice;
}

static void wpe__recycle(WPE_Audio* audio, wpe__Voice* voice) {
    if (voice->kind == WPE__VOICE_FREE) return;
    ma_sound_uninit(&voice->sound); /* Detaches from the mixer before its data source goes away. */
    if (voice->kind == WPE__VOICE_MUSIC) {
        ma_decoder_uninit(&voice->decoder);
    } else {
        ma_audio_buffer_uninit(&voice->buffer);
        wpe_asset_release(audio->assets, voice->asset);
    }
    const uint32_t index = (uint32_t)(voice - audio->voices);
    if (audio->music == wpe__voice_id(audio, index)) audio->music = WPE_VOICE_NONE;
    voice->kind = WPE__VOICE_FREE;
    voice->asset = WPE_ASSET_NONE;
    voice->entity = WPE_ENTITY_NULL;
    voice->stopping = false;
    voice->generation = voice->generation >= WPE__VOICE_MAX_GENERATION ? 1u : voice->generation + 1u;
}

/* Finds a free voice, stealing the oldest non-looping one-shot if necessary. */
static wpe__Voice* wpe__acquire_voice(WPE_Audio* audio) {
    wpe__Voice* oldest = NULL;
    for (uint32_t i = 0; i < WPE_AUDIO_MAX_VOICES; ++i) {
        wpe__Voice* voice = &audio->voices[i];
        if (voice->kind == WPE__VOICE_FREE) return voice;
        if (voice->kind == WPE__VOICE_ONESHOT && !voice->loop && (!oldest || voice->serial < oldest->serial))
            oldest = voice;
    }
    ++audio->voices_lost;
    if (oldest) wpe__recycle(audio, oldest);
    return oldest;
}

static ma_sound_group* wpe__group(WPE_Audio* audio, WPE_AudioBus bus) {
    return &audio->groups[bus == WPE_AUDIO_BUS_MUSIC ? WPE_AUDIO_BUS_MUSIC : WPE_AUDIO_BUS_EFFECTS];
}

typedef struct wpe__VoiceParams {
    float volume, pitch, min_distance, max_distance;
    WPE_AudioBus bus;
    bool loop, spatial;
    WPE_Vec3 position;
} wpe__VoiceParams;

static void wpe__apply_spatial(wpe__Voice* voice, const wpe__VoiceParams* p) {
    ma_sound_set_attenuation_model(&voice->sound, ma_attenuation_model_inverse);
    ma_sound_set_min_distance(&voice->sound, p->min_distance);
    ma_sound_set_max_distance(&voice->sound, p->max_distance);
    ma_sound_set_doppler_factor(&voice->sound, 0.0f);
    ma_sound_set_position(&voice->sound, p->position.x, p->position.y, p->position.z);
}

/* Starts an effects voice for a sound asset; takes its own reference. */
static wpe__Voice* wpe__start_sound(WPE_Audio* audio, WPE_AssetHandle asset, wpe__VoiceKind kind,
                                    const wpe__VoiceParams* p) {
    const wpe__Sound* sound = wpe_asset_get(audio->assets, asset, WPE_ASSET_SOUND);
    if (!sound) return NULL;
    wpe__Voice* voice = wpe__acquire_voice(audio);
    if (!voice) return NULL;

    ma_audio_buffer_config config = ma_audio_buffer_config_init(ma_format_f32, sound->channels, sound->frames,
                                                                sound->pcm, &wpe__ma_allocation);
    config.sampleRate = sound->sample_rate;
    if (ma_audio_buffer_init(&config, &voice->buffer) != MA_SUCCESS) return NULL;
    const ma_uint32 flags = p->spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(&audio->engine, &voice->buffer, flags, wpe__group(audio, p->bus),
                                       &voice->sound) != MA_SUCCESS) {
        ma_audio_buffer_uninit(&voice->buffer);
        return NULL;
    }
    wpe_asset_retain(audio->assets, asset);
    voice->asset = asset;
    voice->kind = kind;
    voice->loop = p->loop;
    voice->serial = ++audio->serial;
    ma_sound_set_volume(&voice->sound, p->volume);
    ma_sound_set_pitch(&voice->sound, p->pitch);
    ma_sound_set_looping(&voice->sound, p->loop);
    if (p->spatial) wpe__apply_spatial(voice, p);
    ma_sound_start(&voice->sound);
    return voice;
}

/* ---- Audio source components --------------------------------------------------------------------- */

static void wpe__source_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Audio* audio = user_data;
    WPE_AudioSource* source = component;
    source->wpe__voice = WPE_VOICE_NONE;
    if (wpe_asset_get(audio->assets, source->sound, WPE_ASSET_SOUND)) {
        wpe_asset_retain(audio->assets, source->sound);
    } else {
        if (source->sound != WPE_ASSET_NONE)
            wpe_log_error("audio source references a handle that is not a sound");
        source->sound = WPE_ASSET_NONE;
    }
}

static void wpe__source_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Audio* audio = user_data;
    WPE_AudioSource* source = component;
    wpe__Voice* voice = wpe__voice(audio, source->wpe__voice);
    if (voice) wpe__recycle(audio, voice);
    source->wpe__voice = WPE_VOICE_NONE;
    wpe_asset_release(audio->assets, source->sound);
    source->sound = WPE_ASSET_NONE;
}

WPE_AudioSource wpe_audio_source_default(WPE_AssetHandle sound) {
    return (WPE_AudioSource){.sound = sound,
                             .volume = 1.0f,
                             .pitch = 1.0f,
                             .min_distance = 1.0f,
                             .max_distance = 50.0f,
                             .bus = WPE_AUDIO_BUS_EFFECTS,
                             .spatial = true};
}

/* ---- Lifecycle ----------------------------------------------------------------------------------- */

static WPE_Result wpe__init_engine(WPE_Audio* audio, uint32_t sample_rate, uint32_t channels, bool device) {
    ma_engine_config config = ma_engine_config_init();
    config.sampleRate = sample_rate;
    config.channels = channels;
    config.noDevice = device ? MA_FALSE : MA_TRUE;
    config.allocationCallbacks = audio->allocation;
    return ma_engine_init(&config, &audio->engine) == MA_SUCCESS ? WPE_RESULT_OK : WPE_RESULT_ERROR;
}

WPE_Result wpe_audio_create(const WPE_AudioDesc* desc, WPE_Audio** out_audio) {
    if (!desc || !desc->ecs || !desc->assets || !out_audio) return WPE_RESULT_ERROR_NULL_ARG;
    *out_audio = NULL;
    const uint32_t sample_rate = desc->sample_rate ? desc->sample_rate : WPE__DEFAULT_SAMPLE_RATE;
    const uint32_t channels = desc->channels ? desc->channels : WPE__DEFAULT_CHANNELS;
    if (sample_rate < 8000 || sample_rate > 192000 || channels > 8) return WPE_RESULT_ERROR_INVALID_ARG;
    if (wpe_ecs_is_registered(desc->ecs, WPE_COMP_AUDIO_SOURCE)) {
        wpe_log_error("the world already has audio");
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }

    WPE_Audio* audio = wpe_alloc_zeroed(1, sizeof *audio);
    if (!audio) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    audio->ecs = desc->ecs;
    audio->assets = desc->assets;
    audio->allocation = wpe__ma_allocation;
    for (uint32_t i = 0; i < WPE_AUDIO_BUS_COUNT; ++i)
        audio->volumes[i] = 1.0f;
    for (uint32_t i = 0; i < WPE_AUDIO_MAX_VOICES; ++i)
        audio->voices[i].generation = 1;
    audio->vfs = (wpe__MaVfs){.callbacks = {.onOpen = wpe__vfs_open,
                                            .onOpenW = wpe__vfs_open_w,
                                            .onClose = wpe__vfs_close,
                                            .onRead = wpe__vfs_read,
                                            .onWrite = wpe__vfs_write,
                                            .onSeek = wpe__vfs_seek,
                                            .onTell = wpe__vfs_tell,
                                            .onInfo = wpe__vfs_info},
                              .vfs = wpe_asset_manager_vfs(desc->assets)};

    WPE_Result result = WPE_RESULT_ERROR;
    if (!desc->no_device) {
        result = wpe__init_engine(audio, sample_rate, channels, true);
        if (result == WPE_RESULT_OK) {
            audio->device = true;
        } else {
            wpe_log_warn("no audio output device could be opened; audio runs silently");
        }
    }
    if (!audio->device) result = wpe__init_engine(audio, sample_rate, channels, false);
    if (result != WPE_RESULT_OK) {
        wpe_log_error("failed to initialize the audio mixer");
        wpe_free(audio);
        return result;
    }
    audio->groups_ready =
        ma_sound_group_init(&audio->engine, 0, NULL, &audio->groups[WPE_AUDIO_BUS_EFFECTS]) == MA_SUCCESS;
    if (audio->groups_ready &&
        ma_sound_group_init(&audio->engine, 0, NULL, &audio->groups[WPE_AUDIO_BUS_MUSIC]) != MA_SUCCESS) {
        ma_sound_group_uninit(&audio->groups[WPE_AUDIO_BUS_EFFECTS]);
        audio->groups_ready = false;
    }
    if (!audio->groups_ready) {
        ma_engine_uninit(&audio->engine);
        wpe_free(audio);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    const WPE_AssetLoader loader = {.name = "sound", .load = wpe__load_sound, .destroy = wpe__destroy_sound};
    result = wpe_asset_manager_register_type(audio->assets, WPE_ASSET_SOUND, &loader);
    if (result == WPE_RESULT_ERROR_ALREADY_EXISTS) wpe_log_error("the asset manager already has audio");
    if (result == WPE_RESULT_OK) {
        result = wpe_ecs_register_component(audio->ecs, WPE_COMP_AUDIO_SOURCE,
                                            &(WPE_ComponentDesc){.name = "AudioSource",
                                                                 .size = sizeof(WPE_AudioSource),
                                                                 .alignment = alignof(WPE_AudioSource),
                                                                 .on_add = wpe__source_on_add,
                                                                 .on_remove = wpe__source_on_remove,
                                                                 .hook_user_data = audio});
        if (result != WPE_RESULT_OK) wpe_asset_manager_unregister_type(audio->assets, WPE_ASSET_SOUND);
    }
    if (result != WPE_RESULT_OK) {
        ma_sound_group_uninit(&audio->groups[WPE_AUDIO_BUS_MUSIC]);
        ma_sound_group_uninit(&audio->groups[WPE_AUDIO_BUS_EFFECTS]);
        ma_engine_uninit(&audio->engine);
        wpe_free(audio);
        return result;
    }
    wpe_log_debug("audio mixer started: %u Hz, %u channels, %s", ma_engine_get_sample_rate(&audio->engine),
                  ma_engine_get_channels(&audio->engine), audio->device ? "device" : "no device");
    *out_audio = audio;
    return WPE_RESULT_OK;
}

void wpe_audio_destroy(WPE_Audio* audio) {
    if (!audio) return;
    WPE_EcsPoolView view;
    while (wpe_ecs_pool_view(audio->ecs, WPE_COMP_AUDIO_SOURCE, &view) == WPE_RESULT_OK && view.count > 0)
        wpe_ecs_remove_component(audio->ecs, view.entities[view.count - 1], WPE_COMP_AUDIO_SOURCE);
    wpe_ecs_unregister_component(audio->ecs, WPE_COMP_AUDIO_SOURCE);
    for (uint32_t i = 0; i < WPE_AUDIO_MAX_VOICES; ++i)
        wpe__recycle(audio, &audio->voices[i]);
    if (wpe_asset_manager_unregister_type(audio->assets, WPE_ASSET_SOUND) != WPE_RESULT_OK)
        wpe_log_error(
            "sounds are still referenced at audio shutdown; release them before wpe_audio_destroy()");
    ma_sound_group_uninit(&audio->groups[WPE_AUDIO_BUS_MUSIC]);
    ma_sound_group_uninit(&audio->groups[WPE_AUDIO_BUS_EFFECTS]);
    ma_engine_uninit(&audio->engine);
    wpe_free(audio);
}

/* ---- Per-frame update ---------------------------------------------------------------------------- */

static bool wpe__entity_position(const WPE_Ecs* ecs, WPE_Entity entity, WPE_Vec3* out) {
    WPE_Mat4 world;
    if (!wpe_scene_compute_world_matrix(ecs, entity, &world)) return false;
    *out = wpe_vec3(world.m[3][0], world.m[3][1], world.m[3][2]);
    return true;
}

static wpe__VoiceParams wpe__source_params(const WPE_AudioSource* source, WPE_Vec3 position) {
    return (wpe__VoiceParams){.volume = source->volume,
                              .pitch = source->pitch > 0.0f ? source->pitch : 1.0f,
                              .min_distance = source->min_distance,
                              .max_distance = source->max_distance,
                              .bus = source->bus,
                              .loop = source->loop,
                              .spatial = source->spatial,
                              .position = position};
}

static void wpe__update_listener(WPE_Audio* audio) {
    WPE_Mat4 world = wpe_mat4_identity();
    if (audio->listener != WPE_ENTITY_NULL &&
        !wpe_scene_compute_world_matrix(audio->ecs, audio->listener, &world))
        world = wpe_mat4_identity(); /* Listener entity gone: back to the origin. */
    const WPE_Vec3 forward = wpe_vec3_normalize(wpe_mat4_transform_dir(&world, wpe_vec3(0, 0, -1)));
    const WPE_Vec3 up = wpe_vec3_normalize(wpe_mat4_transform_dir(&world, wpe_vec3(0, 1, 0)));
    ma_engine_listener_set_position(&audio->engine, 0, world.m[3][0], world.m[3][1], world.m[3][2]);
    ma_engine_listener_set_direction(&audio->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&audio->engine, 0, up.x, up.y, up.z);
}

WPE_Result wpe_audio_update(WPE_Audio* audio) {
    if (!audio) return WPE_RESULT_ERROR_NULL_ARG;
    wpe__update_listener(audio);

    /* Sources: start, follow, finish and stop according to `playing`. */
    WPE_EcsIter it = wpe_ecs_query(audio->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_AUDIO_SOURCE)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_AudioSource* source = wpe_ecs_iter_get(&it, WPE_COMP_AUDIO_SOURCE);
        wpe__Voice* voice = wpe__voice(audio, source->wpe__voice);
        if (voice && ma_sound_at_end(&voice->sound)) {
            wpe__recycle(audio, voice);
            voice = NULL;
            source->wpe__voice = WPE_VOICE_NONE;
            source->playing = false;
        }
        if (!source->playing) {
            if (voice) wpe__recycle(audio, voice);
            source->wpe__voice = WPE_VOICE_NONE;
            continue;
        }
        WPE_Vec3 position = wpe_vec3(0, 0, 0);
        wpe__entity_position(audio->ecs, it.entity, &position);
        const wpe__VoiceParams params = wpe__source_params(source, position);
        if (!voice) {
            voice = wpe__start_sound(audio, source->sound, WPE__VOICE_SOURCE, &params);
            if (!voice) {
                source->playing = false; /* No sound or no voice: report by stopping. */
                continue;
            }
            voice->entity = it.entity;
            source->wpe__voice = wpe__voice_id(audio, (uint32_t)(voice - audio->voices));
        } else {
            ma_sound_set_volume(&voice->sound, params.volume);
            ma_sound_set_pitch(&voice->sound, params.pitch);
            ma_sound_set_looping(&voice->sound, params.loop);
            if (params.spatial) wpe__apply_spatial(voice, &params);
        }
    }

    /* One-shots and music: recycle finished voices (this releases their sounds). */
    for (uint32_t i = 0; i < WPE_AUDIO_MAX_VOICES; ++i) {
        wpe__Voice* voice = &audio->voices[i];
        if (voice->kind == WPE__VOICE_ONESHOT && ma_sound_at_end(&voice->sound)) wpe__recycle(audio, voice);
        if (voice->kind == WPE__VOICE_MUSIC &&
            (ma_sound_at_end(&voice->sound) || (voice->stopping && !ma_sound_is_playing(&voice->sound))))
            wpe__recycle(audio, voice);
    }
    return WPE_RESULT_OK;
}

void wpe_audio_set_listener(WPE_Audio* audio, WPE_Entity entity) {
    if (!wpe_check(audio != NULL)) return;
    audio->listener = entity;
}

/* ---- One-shots ----------------------------------------------------------------------------------- */

WPE_Voice wpe_audio_play(WPE_Audio* audio, WPE_AssetHandle sound, const WPE_AudioPlayDesc* desc) {
    if (!audio) return WPE_VOICE_NONE;
    const WPE_AudioPlayDesc defaults = {0};
    if (!desc) desc = &defaults;
    const wpe__VoiceParams params = {.volume = desc->volume > 0.0f ? desc->volume : 1.0f,
                                     .pitch = desc->pitch > 0.0f ? desc->pitch : 1.0f,
                                     .min_distance = desc->min_distance > 0.0f ? desc->min_distance : 1.0f,
                                     .max_distance = desc->max_distance > 0.0f ? desc->max_distance : 50.0f,
                                     .bus = desc->bus,
                                     .loop = desc->loop,
                                     .spatial = desc->spatial,
                                     .position = desc->position};
    const wpe__Voice* voice = wpe__start_sound(audio, sound, WPE__VOICE_ONESHOT, &params);
    return voice ? wpe__voice_id(audio, (uint32_t)(voice - audio->voices)) : WPE_VOICE_NONE;
}

void wpe_audio_stop(WPE_Audio* audio, WPE_Voice id) {
    wpe__Voice* voice = wpe__voice(audio, id);
    if (voice && voice->kind == WPE__VOICE_ONESHOT) wpe__recycle(audio, voice);
}

bool wpe_audio_is_playing(const WPE_Audio* audio, WPE_Voice id) {
    const wpe__Voice* voice = wpe__voice(audio, id);
    return voice && voice->kind == WPE__VOICE_ONESHOT && !ma_sound_at_end(&voice->sound);
}

void wpe_audio_set_voice_position(WPE_Audio* audio, WPE_Voice id, WPE_Vec3 position) {
    wpe__Voice* voice = wpe__voice(audio, id);
    if (voice && voice->kind == WPE__VOICE_ONESHOT)
        ma_sound_set_position(&voice->sound, position.x, position.y, position.z);
}

/* ---- Music --------------------------------------------------------------------------------------- */

static uint64_t wpe__fade_ms(float seconds) {
    return seconds > 0.0f ? (uint64_t)(seconds * 1000.0f + 0.5f) : 0;
}

WPE_Result wpe_audio_play_music(WPE_Audio* audio, const char* path, float fade_seconds, bool loop) {
    if (!audio || !path) return WPE_RESULT_ERROR_NULL_ARG;
    if (!(fade_seconds >= 0.0f) || !isfinite(fade_seconds)) return WPE_RESULT_ERROR_INVALID_ARG;
    char normalized[WPE_VFS_PATH_MAX];
    if (!wpe_vfs_path_normalize(path, normalized, sizeof normalized)) {
        wpe_log_error("'%s' is not a valid asset path", path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (!wpe_vfs_exists(audio->vfs.vfs, normalized)) {
        wpe_log_error("music '%s' not found", normalized);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    wpe__Voice* voice = wpe__acquire_voice(audio);
    if (!voice) {
        wpe_log_error("no free voice for music '%s'", normalized);
        return WPE_RESULT_ERROR_CAPACITY;
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.allocationCallbacks = wpe__ma_allocation;
    if (ma_decoder_init_vfs(&audio->vfs, normalized, &config, &voice->decoder) != MA_SUCCESS) {
        wpe_log_error("'%s' is not a WAV, FLAC or MP3 file", normalized);
        return WPE_RESULT_ERROR_UNSUPPORTED;
    }
    if (ma_sound_init_from_data_source(&audio->engine, &voice->decoder, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       &audio->groups[WPE_AUDIO_BUS_MUSIC], &voice->sound) != MA_SUCCESS) {
        ma_decoder_uninit(&voice->decoder);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    voice->kind = WPE__VOICE_MUSIC;
    voice->loop = loop;
    voice->serial = ++audio->serial;

    wpe_audio_stop_music(audio, fade_seconds); /* The previous track fades out. */
    ma_sound_set_looping(&voice->sound, loop);
    if (fade_seconds > 0.0f)
        ma_sound_set_fade_in_milliseconds(&voice->sound, 0.0f, 1.0f, wpe__fade_ms(fade_seconds));
    ma_sound_start(&voice->sound);
    audio->music = wpe__voice_id(audio, (uint32_t)(voice - audio->voices));
    return WPE_RESULT_OK;
}

void wpe_audio_stop_music(WPE_Audio* audio, float fade_seconds) {
    if (!audio) return;
    wpe__Voice* voice = wpe__voice(audio, audio->music);
    audio->music = WPE_VOICE_NONE;
    if (!voice) return;
    if (fade_seconds > 0.0f && isfinite(fade_seconds)) {
        ma_sound_stop_with_fade_in_milliseconds(&voice->sound, wpe__fade_ms(fade_seconds));
        voice->stopping = true; /* Recycled by wpe_audio_update() once silent. */
    } else {
        wpe__recycle(audio, voice);
    }
}

bool wpe_audio_music_playing(const WPE_Audio* audio) {
    const wpe__Voice* voice = wpe__voice(audio, audio ? audio->music : WPE_VOICE_NONE);
    return voice && !ma_sound_at_end(&voice->sound);
}

/* ---- Mixing -------------------------------------------------------------------------------------- */

void wpe_audio_set_volume(WPE_Audio* audio, WPE_AudioBus bus, float volume) {
    if (!audio || bus >= WPE_AUDIO_BUS_COUNT || !(volume >= 0.0f) || !isfinite(volume)) {
        wpe_log_error("invalid volume %g for bus %d", (double)volume, (int)bus);
        return;
    }
    audio->volumes[bus] = volume;
    if (bus == WPE_AUDIO_BUS_MASTER) {
        ma_engine_set_volume(&audio->engine, volume);
    } else {
        ma_sound_group_set_volume(&audio->groups[bus], volume);
    }
}

float wpe_audio_volume(const WPE_Audio* audio, WPE_AudioBus bus) {
    return audio && bus < WPE_AUDIO_BUS_COUNT ? audio->volumes[bus] : 0.0f;
}

WPE_Result wpe_audio_render(WPE_Audio* audio, float* out, uint32_t frames) {
    if (!audio || !out) return WPE_RESULT_ERROR_NULL_ARG;
    if (audio->device) return WPE_RESULT_ERROR_INVALID_STATE;
    ma_uint64 read = 0;
    if (ma_engine_read_pcm_frames(&audio->engine, out, frames, &read) != MA_SUCCESS) read = 0;
    /* The mixer always produces full blocks; keep the tail silent if it did not. */
    const uint32_t channels = ma_engine_get_channels(&audio->engine);
    if (read < frames) memset(out + read * channels, 0, (size_t)(frames - read) * channels * sizeof *out);
    return WPE_RESULT_OK;
}

WPE_AudioStats wpe_audio_stats(const WPE_Audio* audio) {
    WPE_AudioStats stats = {0};
    if (!audio) return stats;
    for (uint32_t i = 0; i < WPE_AUDIO_MAX_VOICES; ++i)
        stats.voices += audio->voices[i].kind != WPE__VOICE_FREE;
    stats.voices_lost = audio->voices_lost;
    stats.device = audio->device;
    stats.sample_rate = ma_engine_get_sample_rate(&audio->engine);
    stats.channels = ma_engine_get_channels(&audio->engine);
    return stats;
}
