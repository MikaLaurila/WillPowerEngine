#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio.h"
#include "core/alloc.h"
#include "core/log.h"
#include "platform/platform.h"
#include "scene/scene.h"

/* Scratch files live below the test's working directory. */
#define ROOT "audio_scratch"
#define RATE 48000u

static void quiet(bool on) {
    wpe_log_set_stderr_enabled(!on);
}

typedef struct Fixture {
    WPE_TestAssets assets;
    WPE_Ecs* ecs;
    WPE_Audio* audio;
    float* buffer; /* Render scratch: one second of stereo. */
} Fixture;

static Fixture make_fixture(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/sounds"));
    wpe_test_write_wav(ROOT "/sounds/beep.wav", 1, 22050, 22050 / 4,
                       440.0f); /* 0.25 s mono at another rate */
    wpe_test_write_wav(ROOT "/sounds/stereo.wav", 2, RATE, RATE / 2, 330.0f); /* 0.5 s stereo */
    wpe_test_write_wav(ROOT "/sounds/long.wav", 1, RATE, RATE * 2, 220.0f);   /* 2 s mono */
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(ROOT "/sounds/not_audio.wav", true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, "hello", 5));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));

    Fixture f = {.assets = wpe_test_assets_create(ROOT, 0)};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&f.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(f.ecs));
    WPE_TEST_ASSERT_OK(wpe_audio_create(
        &(WPE_AudioDesc){.ecs = f.ecs, .assets = f.assets.assets, .no_device = true, .sample_rate = RATE},
        &f.audio));
    f.buffer = wpe_alloc((size_t)RATE * 2 * sizeof(float));
    WPE_TEST_ASSERT(f.buffer != NULL);
    return f;
}

static void free_fixture(Fixture* f) {
    wpe_audio_destroy(f->audio);
    wpe_ecs_destroy(f->ecs);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(f->assets.assets).assets);
    wpe_test_assets_destroy(&f->assets);
    wpe_free(f->buffer);
}

static WPE_AssetHandle load_sound(Fixture* f, const char* path) {
    WPE_AssetHandle h = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(f->assets.assets, WPE_ASSET_SOUND, path, &h));
    return h;
}

/* Mixes `frames` stereo frames and returns the RMS of the left and right channels. */
static void render(Fixture* f, uint32_t frames, float* out_left, float* out_right) {
    WPE_TEST_ASSERT(frames <= RATE);
    WPE_TEST_ASSERT_OK(wpe_audio_render(f->audio, f->buffer, frames));
    double left = 0.0, right = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        left += (double)f->buffer[2 * i] * f->buffer[2 * i];
        right += (double)f->buffer[2 * i + 1] * f->buffer[2 * i + 1];
    }
    if (out_left) *out_left = (float)sqrt(left / frames);
    if (out_right) *out_right = (float)sqrt(right / frames);
}

static float render_rms(Fixture* f, uint32_t frames) {
    float left = 0.0f, right = 0.0f;
    render(f, frames, &left, &right);
    return 0.5f * (left + right);
}

/* Advances time in 10 ms blocks (render + update), like a game loop. */
static void advance(Fixture* f, float seconds) {
    const uint32_t block = RATE / 100;
    for (float t = 0.0f; t < seconds; t += 0.01f) {
        WPE_TEST_ASSERT_OK(wpe_audio_render(f->audio, f->buffer, block));
        WPE_TEST_ASSERT_OK(wpe_audio_update(f->audio));
    }
}

static void test_audio_sound_assets(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle beep = load_sound(&f, "sounds/beep.wav");
    double seconds = 0.0;
    uint32_t channels = 0;
    WPE_TEST_ASSERT(wpe_audio_sound_info(f.assets.assets, beep, &seconds, &channels));
    WPE_TEST_ASSERT_NEAR(0.25, seconds, 1e-3);
    WPE_TEST_ASSERT_EQ_UINT(1, channels);
    WPE_TEST_ASSERT(!wpe_audio_sound_info(f.assets.assets, WPE_ASSET_NONE, &seconds, &channels));

    WPE_AssetHandle bad = WPE_ASSET_NONE;
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_UNSUPPORTED,
                           wpe_asset_load(f.assets.assets, WPE_ASSET_SOUND, "sounds/not_audio.wav", &bad));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_asset_load(f.assets.assets, WPE_ASSET_SOUND, "sounds/missing.wav", &bad));
    quiet(false);
    const WPE_AudioStats stats = wpe_audio_stats(f.audio);
    WPE_TEST_ASSERT(!stats.device);
    WPE_TEST_ASSERT_EQ_UINT(RATE, stats.sample_rate);
    WPE_TEST_ASSERT_EQ_UINT(2, stats.channels);
    wpe_asset_release(f.assets.assets, beep);
    free_fixture(&f);
}

static void test_audio_one_shot_plays_and_finishes(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle beep = load_sound(&f, "sounds/beep.wav");
    WPE_TEST_ASSERT_NEAR(0.0, render_rms(&f, 4800), 0); /* Silence before anything plays. */

    const WPE_Voice voice = wpe_audio_play(f.audio, beep, NULL);
    WPE_TEST_ASSERT(voice != WPE_VOICE_NONE);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_asset_ref_count(f.assets.assets, beep)); /* The voice holds one. */
    float left = 0.0f, right = 0.0f;
    render(&f, 4800, &left, &right);
    /* A 0.5-amplitude sine has RMS ~0.354; a mono 2D sound plays on both channels. */
    WPE_TEST_ASSERT_MSG(left > 0.2f && right > 0.2f, "rms %f %f", (double)left, (double)right);
    WPE_TEST_ASSERT(wpe_audio_is_playing(f.audio, voice));

    advance(&f, 0.4f); /* Past its 0.25 s length (resampled from 22.05 kHz). */
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, voice));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(f.audio).voices);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_asset_ref_count(f.assets.assets, beep));
    WPE_TEST_ASSERT_NEAR(0.0, render_rms(&f, 4800), 1e-6);

    /* Looping one-shots continue until stopped; stale ids are ignored. */
    const WPE_Voice loop = wpe_audio_play(f.audio, beep, &(WPE_AudioPlayDesc){.loop = true});
    advance(&f, 0.6f);
    WPE_TEST_ASSERT(wpe_audio_is_playing(f.audio, loop));
    wpe_audio_stop(f.audio, loop);
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, loop));
    wpe_audio_stop(f.audio, loop);
    wpe_audio_stop(f.audio, voice);
    WPE_TEST_ASSERT_EQ_UINT(WPE_VOICE_NONE, wpe_audio_play(f.audio, WPE_ASSET_NONE, NULL));
    wpe_asset_release(f.assets.assets, beep);
    free_fixture(&f);
}

static void test_audio_pitch_changes_duration(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle sound = load_sound(&f, "sounds/stereo.wav"); /* 0.5 s */
    const WPE_Voice fast = wpe_audio_play(f.audio, sound, &(WPE_AudioPlayDesc){.pitch = 2.0f});
    const WPE_Voice normal = wpe_audio_play(f.audio, sound, NULL);
    advance(&f, 0.35f);
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, fast)); /* Done after ~0.25 s. */
    WPE_TEST_ASSERT(wpe_audio_is_playing(f.audio, normal));
    advance(&f, 0.3f);
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, normal));
    wpe_asset_release(f.assets.assets, sound);
    free_fixture(&f);
}

static void test_audio_time_scale_slows_effects(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle sound = load_sound(&f, "sounds/stereo.wav"); /* 0.5 s */
    WPE_TEST_ASSERT_NEAR(1.0, wpe_audio_time_scale(f.audio), 0);
    /* At half speed an effect played at pitch 2 runs its normal 0.5 s; music keeps its speed (0.25 s). */
    wpe_audio_set_time_scale(f.audio, 0.5f);
    const WPE_Voice effect = wpe_audio_play(f.audio, sound, &(WPE_AudioPlayDesc){.pitch = 2.0f});
    const WPE_Voice music =
        wpe_audio_play(f.audio, sound, &(WPE_AudioPlayDesc){.pitch = 2.0f, .bus = WPE_AUDIO_BUS_MUSIC});
    advance(&f, 0.35f);
    WPE_TEST_ASSERT(wpe_audio_is_playing(f.audio, effect));
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, music));
    advance(&f, 0.3f);
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, effect));

    /* At a standstill effects hold where they are, silently, and resume when time runs again. */
    wpe_audio_set_time_scale(f.audio, 1.0f);
    const WPE_Voice held = wpe_audio_play(f.audio, sound, NULL);
    advance(&f, 0.2f);
    wpe_audio_set_time_scale(f.audio, 0.0f);
    advance(&f, 1.0f);
    WPE_TEST_ASSERT(wpe_audio_is_playing(f.audio, held));
    WPE_TEST_ASSERT_NEAR(0.0, render_rms(&f, 4800), 1e-4);
    const WPE_Voice late =
        wpe_audio_play(f.audio, sound, NULL); /* Started during the standstill: held too. */
    advance(&f, 0.6f);
    WPE_TEST_ASSERT(wpe_audio_is_playing(f.audio, late));
    wpe_audio_set_time_scale(f.audio, 1.0f);
    WPE_TEST_ASSERT(render_rms(&f, 4800) > 0.1f);
    advance(&f, 0.25f); /* The rest of the held one (0.3 s, minus the render above). */
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, held));
    WPE_TEST_ASSERT(wpe_audio_is_playing(f.audio, late));
    advance(&f, 0.3f);
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, late));

    /* Nonsense is ignored. */
    wpe_audio_set_time_scale(f.audio, -1.0f);
    wpe_audio_set_time_scale(f.audio, NAN);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_audio_time_scale(f.audio), 0);
    wpe_asset_release(f.assets.assets, sound);
    free_fixture(&f);
}

static void test_audio_volume_buses(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle sound = load_sound(&f, "sounds/long.wav");
    wpe_audio_play(f.audio, sound, NULL);
    const float full = render_rms(&f, 4800);
    wpe_audio_set_volume(f.audio, WPE_AUDIO_BUS_EFFECTS, 0.5f);
    render_rms(&f, 4800); /* Let volume smoothing settle. */
    const float half = render_rms(&f, 4800);
    WPE_TEST_ASSERT_MSG(fabsf(half / full - 0.5f) < 0.05f, "ratio %f", (double)(half / full));
    WPE_TEST_ASSERT_NEAR(0.5, wpe_audio_volume(f.audio, WPE_AUDIO_BUS_EFFECTS), 0);
    wpe_audio_set_volume(f.audio, WPE_AUDIO_BUS_MUSIC, 0.0f); /* Other bus: no effect. */
    render_rms(&f, 4800);
    WPE_TEST_ASSERT_NEAR(half, render_rms(&f, 4800), half * 0.05);
    wpe_audio_set_volume(f.audio, WPE_AUDIO_BUS_MASTER, 0.0f);
    render_rms(&f, 4800);
    WPE_TEST_ASSERT_NEAR(0.0, render_rms(&f, 4800), 1e-4);
    wpe_asset_release(f.assets.assets, sound);
    free_fixture(&f);
}

static void test_audio_spatial_panning_and_attenuation(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle sound = load_sound(&f, "sounds/long.wav");
    /* Listener at the origin facing -Z (no listener entity): +X is to the right. */
    WPE_TEST_ASSERT_OK(wpe_audio_update(f.audio));
    const WPE_Voice right = wpe_audio_play(
        f.audio, sound, &(WPE_AudioPlayDesc){.spatial = true, .position = wpe_vec3(4.0f, 0.0f, 0.0f)});
    render(&f, 4800, NULL, NULL); /* Spatial gains are smoothed in. */
    float l = 0.0f, r = 0.0f;
    render(&f, 4800, &l, &r);
    WPE_TEST_ASSERT_MSG(r > l * 2.0f, "right %f left %f", (double)r, (double)l);
    const float near_level = 0.5f * (l + r);

    /* Ten times farther is much quieter (inverse distance from min_distance 1). */
    wpe_audio_set_voice_position(f.audio, right, wpe_vec3(40.0f, 0.0f, 0.0f));
    render(&f, 4800, NULL, NULL);
    render(&f, 4800, &l, &r);
    WPE_TEST_ASSERT_MSG(0.5f * (l + r) < near_level * 0.2f, "far %f near %f", (double)(0.5f * (l + r)),
                        (double)near_level);

    /* Turning the listener around swaps the sides. */
    const WPE_Entity listener = wpe_ecs_spawn(f.ecs);
    WPE_Transform t = wpe_transform_identity();
    t.rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, listener, WPE_COMP_TRANSFORM, &t, NULL));
    wpe_audio_set_listener(f.audio, listener);
    wpe_audio_set_voice_position(f.audio, right, wpe_vec3(4.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_audio_update(f.audio));
    render(&f, 4800, NULL, NULL);
    render(&f, 4800, &l, &r);
    WPE_TEST_ASSERT_MSG(l > r * 2.0f, "right %f left %f", (double)r, (double)l);
    wpe_asset_release(f.assets.assets, sound);
    free_fixture(&f);
}

static void test_audio_source_components(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle beep = load_sound(&f, "sounds/beep.wav");
    const WPE_Entity e = wpe_ecs_spawn(f.ecs);
    const WPE_Transform t = wpe_transform_identity();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_AudioSource source = wpe_audio_source_default(beep);
    source.spatial = false;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, e, WPE_COMP_AUDIO_SOURCE, &source, NULL));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_asset_ref_count(f.assets.assets, beep)); /* The component holds one. */
    wpe_asset_release(f.assets.assets, beep);                               /* ...and keeps it alive. */

    WPE_TEST_ASSERT_OK(wpe_audio_update(f.audio));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(f.audio).voices); /* Not playing yet. */
    ((WPE_AudioSource*)wpe_ecs_get_component(f.ecs, e, WPE_COMP_AUDIO_SOURCE))->playing = true;
    WPE_TEST_ASSERT_OK(wpe_audio_update(f.audio));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_audio_stats(f.audio).voices);
    WPE_TEST_ASSERT(render_rms(&f, 2400) > 0.2f);

    /* A non-looping source stops itself and clears `playing`. */
    advance(&f, 0.4f);
    const WPE_AudioSource* s = wpe_ecs_get_component(f.ecs, e, WPE_COMP_AUDIO_SOURCE);
    WPE_TEST_ASSERT(!s->playing);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(f.audio).voices);

    /* Looping keeps going; clearing `playing` stops it; despawning releases everything. */
    WPE_AudioSource* live = wpe_ecs_get_component(f.ecs, e, WPE_COMP_AUDIO_SOURCE);
    live->loop = true;
    live->playing = true;
    advance(&f, 0.6f);
    WPE_TEST_ASSERT(((WPE_AudioSource*)wpe_ecs_get_component(f.ecs, e, WPE_COMP_AUDIO_SOURCE))->playing);
    ((WPE_AudioSource*)wpe_ecs_get_component(f.ecs, e, WPE_COMP_AUDIO_SOURCE))->playing = false;
    WPE_TEST_ASSERT_OK(wpe_audio_update(f.audio));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(f.audio).voices);
    ((WPE_AudioSource*)wpe_ecs_get_component(f.ecs, e, WPE_COMP_AUDIO_SOURCE))->playing = true;
    WPE_TEST_ASSERT_OK(wpe_audio_update(f.audio));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(f.ecs, e));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(f.audio).voices);
    WPE_TEST_ASSERT(!wpe_asset_is_valid(f.assets.assets, beep)); /* Last reference gone. */

    /* A source with an invalid handle never plays. */
    const WPE_Entity broken = wpe_ecs_spawn(f.ecs);
    WPE_AudioSource none = wpe_audio_source_default(0x1234u);
    none.playing = true;
    quiet(true);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, broken, WPE_COMP_AUDIO_SOURCE, &none, NULL));
    quiet(false);
    WPE_TEST_ASSERT_OK(wpe_audio_update(f.audio));
    WPE_TEST_ASSERT(
        !((WPE_AudioSource*)wpe_ecs_get_component(f.ecs, broken, WPE_COMP_AUDIO_SOURCE))->playing);
    free_fixture(&f);
}

static void test_audio_voice_stealing(void) {
    Fixture f = make_fixture();
    const WPE_AssetHandle sound = load_sound(&f, "sounds/long.wav");
    WPE_Voice first = WPE_VOICE_NONE;
    for (uint32_t i = 0; i < WPE_AUDIO_MAX_VOICES; ++i) {
        const WPE_Voice v = wpe_audio_play(f.audio, sound, &(WPE_AudioPlayDesc){.volume = 0.01f});
        WPE_TEST_ASSERT(v != WPE_VOICE_NONE);
        if (i == 0) first = v;
    }
    WPE_TEST_ASSERT_EQ_UINT(WPE_AUDIO_MAX_VOICES, wpe_audio_stats(f.audio).voices);
    const WPE_Voice extra = wpe_audio_play(f.audio, sound, NULL);
    WPE_TEST_ASSERT(extra != WPE_VOICE_NONE);
    WPE_TEST_ASSERT(!wpe_audio_is_playing(f.audio, first)); /* The oldest was stolen. */
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_audio_stats(f.audio).voices_lost);
    WPE_TEST_ASSERT_EQ_UINT(WPE_AUDIO_MAX_VOICES + 1, wpe_asset_ref_count(f.assets.assets, sound));
    wpe_asset_release(f.assets.assets, sound);
    free_fixture(&f); /* Destroying audio stops and releases every voice. */
}

static void test_audio_music_streams_and_crossfades(void) {
    Fixture f = make_fixture();
    /* Music streamed from an archive (stored uncompressed: incompressible enough here is not
     * guaranteed, so force it). */
    wpe_test_write_wav(ROOT "/track_a.wav", 2, RATE, RATE * 3, 262.0f);
    wpe_test_write_wav(ROOT "/track_b.wav", 2, RATE, RATE * 3, 392.0f);
    WPE_PakWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_pak_writer_create(ROOT "/music.pak", 0, &writer));
    const char* names[2] = {"track_a.wav", "track_b.wav"};
    for (int i = 0; i < 2; ++i) {
        char os_path[64];
        snprintf(os_path, sizeof os_path, ROOT "/%s", names[i]);
        void* data = NULL;
        size_t size = 0;
        WPE_TEST_ASSERT_OK(wpe_file_read_all(os_path, &data, &size));
        char pak_path[64];
        snprintf(pak_path, sizeof pak_path, "music/%s", names[i]);
        WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, pak_path, data, size,
                                              i == 0 ? WPE_PAK_COMPRESS_NONE : WPE_PAK_COMPRESS_ALWAYS));
        wpe_free(data);
    }
    WPE_TEST_ASSERT_OK(wpe_pak_writer_finish(writer));
    wpe_pak_writer_destroy(writer);
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(f.assets.vfs, ROOT "/music.pak"));

    WPE_TEST_ASSERT(!wpe_audio_music_playing(f.audio));
    WPE_TEST_ASSERT_OK(wpe_audio_play_music(f.audio, "music/track_a.wav", 0.0f, true));
    WPE_TEST_ASSERT(wpe_audio_music_playing(f.audio));
    WPE_TEST_ASSERT(render_rms(&f, 4800) > 0.2f);

    /* Crossfade: both tracks play during the fade, then only the new one remains. */
    WPE_TEST_ASSERT_OK(wpe_audio_play_music(f.audio, "music/track_b.wav", 0.5f, false));
    advance(&f, 0.1f);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_audio_stats(f.audio).voices);
    advance(&f, 0.6f);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_audio_stats(f.audio).voices);
    WPE_TEST_ASSERT(render_rms(&f, 4800) > 0.2f);

    /* Music has its own bus. */
    wpe_audio_set_volume(f.audio, WPE_AUDIO_BUS_EFFECTS, 0.0f);
    render_rms(&f, 4800);
    WPE_TEST_ASSERT(render_rms(&f, 4800) > 0.2f);

    /* Non-looping music ends by itself (3 s track); stopping with a fade is gradual. */
    WPE_TEST_ASSERT_OK(wpe_audio_play_music(f.audio, "music/track_a.wav", 0.0f, false));
    wpe_audio_stop_music(f.audio, 0.3f);
    WPE_TEST_ASSERT(!wpe_audio_music_playing(f.audio));
    WPE_TEST_ASSERT(render_rms(&f, 2400) > 0.05f); /* Still fading out. */
    advance(&f, 0.5f);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(f.audio).voices);
    WPE_TEST_ASSERT_NEAR(0.0, render_rms(&f, 4800), 1e-6);

    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_audio_play_music(f.audio, "music/none.wav", 0.0f, true));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_audio_play_music(f.audio, "../x.wav", 0.0f, true));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_UNSUPPORTED,
                           wpe_audio_play_music(f.audio, "sounds/not_audio.wav", 0.0f, true));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(f.audio).voices);
    free_fixture(&f);
}

static void test_audio_create_validation(void) {
    WPE_TestAssets assets = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_Audio* audio = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_audio_create(&(WPE_AudioDesc){.ecs = ecs}, &audio));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_audio_create(
            &(WPE_AudioDesc){.ecs = ecs, .assets = assets.assets, .no_device = true, .channels = 9}, &audio));
    WPE_TEST_ASSERT_OK(
        wpe_audio_create(&(WPE_AudioDesc){.ecs = ecs, .assets = assets.assets, .no_device = true}, &audio));
    WPE_Audio* second = NULL;
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_ALREADY_EXISTS,
        wpe_audio_create(&(WPE_AudioDesc){.ecs = ecs, .assets = assets.assets, .no_device = true}, &second));
    quiet(false);
    float out[16];
    WPE_TEST_ASSERT_OK(wpe_audio_render(audio, out, 8));
    wpe_audio_destroy(audio);
    /* Recreating works once the previous instance is gone. */
    WPE_TEST_ASSERT_OK(
        wpe_audio_create(&(WPE_AudioDesc){.ecs = ecs, .assets = assets.assets, .no_device = true}, &audio));
    wpe_audio_destroy(audio);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&assets);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_audio_sound_assets), WPE_TEST_CASE(test_audio_one_shot_plays_and_finishes),
              WPE_TEST_CASE(test_audio_pitch_changes_duration),
              WPE_TEST_CASE(test_audio_time_scale_slows_effects), WPE_TEST_CASE(test_audio_volume_buses),
              WPE_TEST_CASE(test_audio_spatial_panning_and_attenuation),
              WPE_TEST_CASE(test_audio_source_components), WPE_TEST_CASE(test_audio_voice_stealing),
              WPE_TEST_CASE(test_audio_music_streams_and_crossfades),
              WPE_TEST_CASE(test_audio_create_validation))
