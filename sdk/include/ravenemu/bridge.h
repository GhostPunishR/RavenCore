#ifndef RAVENEMU_BRIDGE_H
#define RAVENEMU_BRIDGE_H

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

typedef struct ravenemu_bridge ravenemu_bridge;

/* Version of this C API, bumped on every breaking change (0 = pre-release).
 * Compare ravenemu_api_version() with this macro before any other call:
 * a mismatch means the header and the linked library disagree. */
#define RAVENEMU_API_VERSION 0
int ravenemu_api_version(void);
/* Version of the RavenCore library, for example "0.1.2". */
const char* ravenemu_version(void);

enum ravenemu_system {
    RAVENEMU_GB = 0,
    RAVENEMU_GBC = 1,
    RAVENEMU_GBA = 2
};

enum ravenemu_button {
    RAVENEMU_UP = 0,
    RAVENEMU_DOWN = 1,
    RAVENEMU_LEFT = 2,
    RAVENEMU_RIGHT = 3,
    RAVENEMU_A = 4,
    RAVENEMU_B = 5,
    RAVENEMU_START = 6,
    RAVENEMU_SELECT = 7,
    RAVENEMU_L = 8,
    RAVENEMU_R = 9
};

/* Returns NULL on error. The error message is thread-local. */
ravenemu_bridge* ravenemu_create(int system);
void ravenemu_destroy(ravenemu_bridge* handle);
const char* ravenemu_last_error(void);

/* ROM/battery bytes are copied by the engine. Returns 1 on success. */
int ravenemu_load(ravenemu_bridge* handle,
                     const uint8_t* rom, size_t rom_size,
                     const uint8_t* battery, size_t battery_size);
int ravenemu_reset(ravenemu_bridge* handle);
int ravenemu_set_button(ravenemu_bridge* handle, int button, int pressed);

/* Dimensions and audio format remain stable for the loaded system.
 * run_frame writes width*height pixels as ARGB8888 (0xAARRGGBB).
 * It is the caller's job to schedule frames at fps and play the audio. */
int ravenemu_video_width(const ravenemu_bridge* handle);
int ravenemu_video_height(const ravenemu_bridge* handle);
double ravenemu_video_fps(const ravenemu_bridge* handle);
int ravenemu_audio_rate(const ravenemu_bridge* handle);
int ravenemu_run_frame(ravenemu_bridge* handle,
                         uint32_t* argb, size_t pixel_capacity);
/* Returns number of interleaved int16 samples copied (not stereo frames). */
size_t ravenemu_read_audio(ravenemu_bridge* handle,
                             int16_t* output, size_t sample_capacity);

/* Read-only RetroAchievements address space. Returns the number of bytes
 * really available, never clocks the emulation bus or changes state.
 * Unmapped addresses return short reads; frontend/SDK must respect count.
 * Call serially on the same handle as run_frame/load_state. */
size_t ravenemu_read_achievement_memory(const ravenemu_bridge* handle,
                                           uint32_t address, uint8_t* output,
                                           size_t capacity);

/* Caller owns buffers. Check size, allocate and then copy.
 * Save states can change size between frames and between core versions. */
size_t ravenemu_state_size(ravenemu_bridge* handle);
int ravenemu_save_state(ravenemu_bridge* handle, uint8_t* output, size_t capacity);
int ravenemu_load_state(ravenemu_bridge* handle,
                          const uint8_t* state, size_t size);
size_t ravenemu_battery_size(ravenemu_bridge* handle);
int ravenemu_copy_battery(ravenemu_bridge* handle,
                            uint8_t* output, size_t capacity);

/* Battery persistence without needless writes: when ravenemu_battery_dirty
 * returns 1, copy the battery with its generation, store it, then acknowledge
 * that generation. Writes made by the game after the copy keep it dirty. */
int ravenemu_battery_dirty(const ravenemu_bridge* handle);
int ravenemu_copy_battery_generation(ravenemu_bridge* handle,
                                        uint8_t* output, size_t capacity,
                                        uint64_t* generation);
int ravenemu_acknowledge_battery(ravenemu_bridge* handle, uint64_t generation);

/* GBA cartridge hardware, for games whose detection fails. Each setting takes
 * effect at the next ravenemu_load or ravenemu_reset. Returns 1 on
 * success, 0 for a GB/GBC handle or an unknown value. */
enum ravenemu_gba_save_type {
    RAVENEMU_GBA_SAVE_AUTO = 0,
    RAVENEMU_GBA_SAVE_NONE = 1,
    RAVENEMU_GBA_SAVE_SRAM = 2,
    RAVENEMU_GBA_SAVE_FLASH_64K = 3,
    RAVENEMU_GBA_SAVE_FLASH_128K = 4,
    RAVENEMU_GBA_SAVE_EEPROM_512 = 5,
    RAVENEMU_GBA_SAVE_EEPROM_8K = 6
};
int ravenemu_set_gba_save_type(ravenemu_bridge* handle, int save_type);

enum ravenemu_gba_rtc {
    RAVENEMU_GBA_RTC_AUTO = 0,
    RAVENEMU_GBA_RTC_OFF = 1,
    RAVENEMU_GBA_RTC_ON = 2
};
int ravenemu_set_gba_rtc(ravenemu_bridge* handle, int rtc);

/* 1 while the cartridge's rumble motor runs (GB MBC5 Rumble, GBA Drill Dozer
 * and WarioWare: Twisted!). Sample it after each ravenemu_run_frame. */
int ravenemu_rumble_active(const ravenemu_bridge* handle);

#ifdef __cplusplus
}
#endif
#endif
