#ifndef RAVENEMU_V3_BRIDGE_H
#define RAVENEMU_V3_BRIDGE_H

/* Portable C ABI for Android JNI, Swift and other frontends.
 * No C++ types or platform-specific headers cross this boundary.
 * All calls for a given handle must be serialized by the caller.
 * Copyright (c) RavenEmu contributors. SPDX-License-Identifier: AGPL-3.0-only
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ravenemu_v3_bridge ravenemu_v3_bridge;

/* Version of this C API, bumped on every breaking change (0 = pre-release).
 * Compare ravenemu_v3_api_version() with this macro before any other call:
 * a mismatch means the header and the linked library disagree. */
#define RAVENEMU_V3_API_VERSION 0
int ravenemu_v3_api_version(void);
/* Version of the RavenEmu Cores library, for example "0.1.0". */
const char* ravenemu_v3_version(void);

enum ravenemu_v3_system {
    RAVENEMU_V3_GB = 0,
    RAVENEMU_V3_GBC = 1,
    RAVENEMU_V3_GBA = 2
};

enum ravenemu_v3_button {
    RAVENEMU_V3_UP = 0,
    RAVENEMU_V3_DOWN = 1,
    RAVENEMU_V3_LEFT = 2,
    RAVENEMU_V3_RIGHT = 3,
    RAVENEMU_V3_A = 4,
    RAVENEMU_V3_B = 5,
    RAVENEMU_V3_START = 6,
    RAVENEMU_V3_SELECT = 7,
    RAVENEMU_V3_L = 8,
    RAVENEMU_V3_R = 9
};

/* Returns NULL on error. The error message is thread-local. */
ravenemu_v3_bridge* ravenemu_v3_create(int system);
void ravenemu_v3_destroy(ravenemu_v3_bridge* handle);
const char* ravenemu_v3_last_error(void);

/* ROM/battery bytes are copied by the engine. Returns 1 on success. */
int ravenemu_v3_load(ravenemu_v3_bridge* handle,
                     const uint8_t* rom, size_t rom_size,
                     const uint8_t* battery, size_t battery_size);
int ravenemu_v3_reset(ravenemu_v3_bridge* handle);
int ravenemu_v3_set_button(ravenemu_v3_bridge* handle, int button, int pressed);

/* Dimensions and audio format remain stable for the loaded system.
 * run_frame writes width*height pixels as ARGB8888 (0xAARRGGBB).
 * It is the caller's job to schedule frames at fps and play the audio. */
int ravenemu_v3_video_width(const ravenemu_v3_bridge* handle);
int ravenemu_v3_video_height(const ravenemu_v3_bridge* handle);
double ravenemu_v3_video_fps(const ravenemu_v3_bridge* handle);
int ravenemu_v3_audio_rate(const ravenemu_v3_bridge* handle);
int ravenemu_v3_run_frame(ravenemu_v3_bridge* handle,
                         uint32_t* argb, size_t pixel_capacity);
/* Returns number of interleaved int16 samples copied (not stereo frames). */
size_t ravenemu_v3_read_audio(ravenemu_v3_bridge* handle,
                             int16_t* output, size_t sample_capacity);

/* Read-only RetroAchievements address space. Returns the number of bytes
 * really available, never clocks the emulation bus or changes state.
 * Unmapped addresses return short reads; frontend/SDK must respect count.
 * Call serially on the same handle as run_frame/load_state. */
size_t ravenemu_v3_read_achievement_memory(const ravenemu_v3_bridge* handle,
                                           uint32_t address, uint8_t* output,
                                           size_t capacity);

/* Caller owns buffers. Check size, allocate and then copy.
 * Save states can change size between frames and between core versions. */
size_t ravenemu_v3_state_size(ravenemu_v3_bridge* handle);
int ravenemu_v3_save_state(ravenemu_v3_bridge* handle, uint8_t* output, size_t capacity);
int ravenemu_v3_load_state(ravenemu_v3_bridge* handle,
                          const uint8_t* state, size_t size);
size_t ravenemu_v3_battery_size(ravenemu_v3_bridge* handle);
int ravenemu_v3_copy_battery(ravenemu_v3_bridge* handle,
                            uint8_t* output, size_t capacity);

/* Battery persistence without needless writes: when ravenemu_v3_battery_dirty
 * returns 1, copy the battery with its generation, store it, then acknowledge
 * that generation. Writes made by the game after the copy keep it dirty. */
int ravenemu_v3_battery_dirty(const ravenemu_v3_bridge* handle);
int ravenemu_v3_copy_battery_generation(ravenemu_v3_bridge* handle,
                                        uint8_t* output, size_t capacity,
                                        uint64_t* generation);
int ravenemu_v3_acknowledge_battery(ravenemu_v3_bridge* handle, uint64_t generation);

#ifdef __cplusplus
}
#endif
#endif
