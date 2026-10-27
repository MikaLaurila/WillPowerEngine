/**
 * @file audio.h
 * @brief Sound playback (miniaudio): 3D positioned sound sources, one-shot sounds, streamed
 *        music with crossfades, and volume buses.
 *
 * Sounds are assets (WPE_ASSET_SOUND, loaded through the asset manager from WAV, FLAC or MP3
 * files): decoded once into memory (32-bit float, native rate and channels) and shared by every
 * voice playing them; voices resample to the mixer rate as they play. Music is streamed from the VFS while it
 * plays, so long tracks cost little memory; store music uncompressed in .pak archives (wpe_pak does that
 * automatically for already-compressed MP3/FLAC files) so it streams straight from the archive.
 *
 * Component registered by wpe_audio_create():
 * - WPE_COMP_AUDIO_SOURCE (WPE_AudioSource): plays a sound at the entity's position while
 *   `playing` is true (set it to start; a non-looping source clears it when the sound ends). The
 *   component holds a reference to its sound.
 *
 * Positioning: the listener follows one entity (usually the camera; see wpe_audio_set_listener()),
 * facing its local -Z axis with +Y up. Spatial sounds are attenuated with distance (inverse
 * distance between min_distance and max_distance) and panned by direction. Doppler is off.
 *
 * Per-frame update: wpe_audio_update() after transforms are final (it reads world positions),
 * typically right before rendering.
 *
 * Without an audio device (wpe_audio_create() with no_device, or when no device can be opened) the
 * mixer runs only when wpe_audio_render() is called, which makes audio testable and recordable.
 *
 * - Dependencies: core, math, ecs, scene, assets (asset manager, VFS), miniaudio (private)
 * - Thread affinity: owning thread; mixing runs on miniaudio's device thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_AUDIO_AUDIO_H
#define WPE_AUDIO_AUDIO_H

#include <stdint.h>

#include "assets/asset_manager.h"
#include "core/result.h"
#include "ecs/ecs.h"
#include "math/wpe_math.h"

typedef enum WPE_AudioBus {
    WPE_AUDIO_BUS_MASTER = 0, /* Everything. */
    WPE_AUDIO_BUS_EFFECTS,    /* Sound sources and one-shots (default). */
    WPE_AUDIO_BUS_MUSIC,      /* Music tracks. */
} WPE_AudioBus;

/* A define rather than an enumerator: structs pairing a bus with a vector union would otherwise
 * trip clang-tidy's tagged-union heuristics. */
#define WPE_AUDIO_BUS_COUNT 3u

typedef struct WPE_Audio WPE_Audio;

/** Identifies a playing voice (one-shot); 0 is never valid. Stale ids are ignored. */
typedef uint32_t WPE_Voice;
#define WPE_VOICE_NONE ((WPE_Voice)0)

/** Maximum number of voices playing at once (sources, one-shots and music tracks together). */
#define WPE_AUDIO_MAX_VOICES 64u

typedef struct WPE_AudioSource {
    WPE_AssetHandle sound; /* WPE_ASSET_SOUND; the component holds a reference (set before adding). */
    float volume;          /* Linear gain, 1 = as recorded. */
    float pitch;           /* Playback rate, 1 = normal (> 0). */
    float min_distance;    /* Full volume up to this distance (spatial sources). */
    float max_distance;    /* No further attenuation beyond this distance. */
    WPE_AudioBus bus;      /* EFFECTS or MUSIC. */
    bool loop;
    bool spatial; /* Positioned at the entity; otherwise plays as-is (2D). */
    bool playing; /* Set to start/stop; cleared by the engine when a non-looping sound ends. */

    /* Private (managed by the audio system). */
    WPE_Voice wpe__voice;
} WPE_AudioSource;

typedef struct WPE_AudioPlayDesc {
    float volume; /* 0 = 1. */
    float pitch;  /* 0 = 1. */
    WPE_AudioBus bus;
    bool loop;
    bool spatial; /* Play at `position` (world space). */
    WPE_Vec3 position;
    float min_distance; /* 0 = 1. */
    float max_distance; /* 0 = 50. */
} WPE_AudioPlayDesc;

typedef struct WPE_AudioDesc {
    WPE_Ecs* ecs;             /* Borrowed; scene components must be registered. */
    WPE_AssetManager* assets; /* Borrowed; the sound asset type is registered on it. */
    bool no_device;           /* Never open a device; mix only in wpe_audio_render(). */
    uint32_t sample_rate;     /* Mixing rate (0 = 48000). */
    uint32_t channels;        /* Output channels (0 = 2). */
} WPE_AudioDesc;

typedef struct WPE_AudioStats {
    uint32_t voices;      /* Playing voices. */
    uint32_t voices_lost; /* Plays rejected or stolen because all voices were busy (cumulative). */
    bool device;          /* An output device is running. */
    uint32_t sample_rate;
    uint32_t channels;
} WPE_AudioStats;

/** @return A source with volume 1, pitch 1, distances 1..50, spatial, effects bus, not playing.
 * @thread_safety Any thread */
WPE_AudioSource wpe_audio_source_default(WPE_AssetHandle sound);

/**
 * Starts the mixer (with an output device unless desc->no_device; if no device can be opened the
 * mixer falls back to device-less mode with a warning). Registers WPE_COMP_AUDIO_SOURCE in the
 * world and the WPE_ASSET_SOUND loader in the asset manager. Destroy it before the world and the
 * asset manager.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (channels > 8 or
 *         sample rate out of 8000..192000); WPE_RESULT_ERROR_ALREADY_EXISTS (the world or asset
 *         manager already has audio); WPE_RESULT_ERROR_OUT_OF_MEMORY; WPE_RESULT_ERROR (mixer
 *         initialization failed; logged).
 *
 * @thread_safety Owning thread
 */
[[nodiscard]] WPE_Result wpe_audio_create(const WPE_AudioDesc* desc, WPE_Audio** out_audio);

/**
 * Stops everything, removes all audio source components, unregisters the sound asset type and
 * shuts the mixer down (ownership consumed); NULL is a no-op. Release every other sound handle
 * first: while sounds are still loaded the type cannot be unregistered (an error is logged and it
 * stays registered, which keeps the remaining sounds valid).
 *
 * @thread_safety Owning thread
 */
void wpe_audio_destroy(WPE_Audio* audio);

/**
 * Per-frame update: moves the listener and spatial voices to their entities, starts and stops
 * sources according to `playing`, and recycles finished voices (releasing their sounds).
 *
 * @return WPE_RESULT_OK or WPE_RESULT_ERROR_NULL_ARG.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_audio_update(WPE_Audio* audio);

/** Makes `entity` the listener (WPE_ENTITY_NULL: listener at the origin facing -Z). @thread_safety Owning
 * thread */
void wpe_audio_set_listener(WPE_Audio* audio, WPE_Entity entity);

/**
 * Plays a sound once (or looping, until stopped). The voice holds a reference to the sound.
 *
 * @param desc Optional (NULL = defaults: volume 1, pitch 1, effects bus, 2D).
 * @return The voice, or WPE_VOICE_NONE if the handle is not a sound or no voice was free (a
 *         playing non-looping one-shot is stolen first; counted in voices_lost).
 *
 * @thread_safety Owning thread
 */
WPE_Voice wpe_audio_play(WPE_Audio* audio, WPE_AssetHandle sound, const WPE_AudioPlayDesc* desc);

/** Stops a voice started by wpe_audio_play() (no-op for stale ids). @thread_safety Owning thread */
void wpe_audio_stop(WPE_Audio* audio, WPE_Voice voice);

/** @return Whether a voice started by wpe_audio_play() is still playing. @thread_safety Owning thread */
bool wpe_audio_is_playing(const WPE_Audio* audio, WPE_Voice voice);

/** Moves a spatial voice started by wpe_audio_play(). @thread_safety Owning thread */
void wpe_audio_set_voice_position(WPE_Audio* audio, WPE_Voice voice, WPE_Vec3 position);

/**
 * Streams a music track (WAV, FLAC or MP3 asset path) on the music bus, crossfading from the
 * current track.
 *
 * @param fade_seconds Fade-in of the new track and fade-out of the previous one (0 = cut).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (bad path or fade);
 *         WPE_RESULT_ERROR_NOT_FOUND; WPE_RESULT_ERROR_UNSUPPORTED (not a decodable audio file; logged);
 *         WPE_RESULT_ERROR_CAPACITY (no free voice); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_audio_play_music(WPE_Audio* audio, const char* path, float fade_seconds, bool loop);

/** Fades out and stops the current music track. @thread_safety Owning thread */
void wpe_audio_stop_music(WPE_Audio* audio, float fade_seconds);

/** @return Whether a music track is playing (excluding one that is fading out). @thread_safety Owning thread
 */
bool wpe_audio_music_playing(const WPE_Audio* audio);

/** Sets a bus volume (linear gain >= 0). @thread_safety Owning thread */
void wpe_audio_set_volume(WPE_Audio* audio, WPE_AudioBus bus, float volume);

/** @return A bus volume. @thread_safety Owning thread */
float wpe_audio_volume(const WPE_Audio* audio, WPE_AudioBus bus);

/**
 * Mixes the next `frames` frames into `out` (interleaved float, wpe_audio_stats().channels per
 * frame). Only valid without a device.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_INVALID_STATE (a device is
 *         running).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_audio_render(WPE_Audio* audio, float* out, uint32_t frames);

/** @return Current statistics. @thread_safety Owning thread */
WPE_AudioStats wpe_audio_stats(const WPE_Audio* audio);

/**
 * Sound asset information.
 *
 * @return false if the handle is not a loaded sound.
 *
 * @thread_safety Owning thread
 */
bool wpe_audio_sound_info(const WPE_AssetManager* assets, WPE_AssetHandle sound, double* out_seconds,
                          uint32_t* out_channels);

#endif /* WPE_AUDIO_AUDIO_H */
