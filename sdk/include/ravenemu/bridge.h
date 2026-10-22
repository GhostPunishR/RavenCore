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
/* Version of the RavenCore library, for example "0.4.1". */
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

/* Sensors of the loaded game's cartridge, as bit flags, so that a frontend
 * can feed them from the device's own sensors or from its controls. */
enum ravenemu_sensor {
    RAVENEMU_SENSOR_SOLAR = 1,    /* Boktai 1 to 3 */
    RAVENEMU_SENSOR_ROTATION = 2, /* WarioWare: Twisted! */
    RAVENEMU_SENSOR_TILT = 4      /* Yoshi Topsy-Turvy, Koro Koro Puzzle, GB MBC7 */
};
int ravenemu_sensors(const ravenemu_bridge* handle);

/* Sensor readings. They are kept across ravenemu_load, ravenemu_reset and
 * state loads, and ignored when the game has no such sensor; out-of-range
 * values are clamped. Each returns 1, or 0 for a null handle. */
/* Light on the solar sensor: 0 (darkness) to 255 (full sun). */
int ravenemu_set_solar_light(ravenemu_bridge* handle, int level);
/* Rotation around the screen's axis: -32768..32767, positive clockwise as
 * the player sees the screen. */
int ravenemu_set_rotation(ravenemu_bridge* handle, int speed);
/* Tilt: -32768..32767 per axis for -1 g..1 g. x > 0 when the right side of
 * the console goes down, y > 0 when its top edge moves away from the player. */
int ravenemu_set_tilt(ravenemu_bridge* handle, int x, int y);

/* Link cable between handles of this process, for example two games side by
 * side on one device. Link 2 GB/GBC handles (a GB and a GBC can be mixed) or
 * 2 to 4 GBA handles; the first GBA is the parent of multiplayer games. Each
 * handle keeps its ROM, saves, states and audio: load them as usual.
 * While linked, advance the handles with ravenemu_link_run_frame only:
 * ravenemu_run_frame fails on them. Destroying one of the handles unplugs
 * the cable for all of them; ravenemu_link_destroy must still be called.
 * Returns NULL on error (see ravenemu_last_error). */
typedef struct ravenemu_link ravenemu_link;
ravenemu_link* ravenemu_link_create(ravenemu_bridge* const* handles, size_t count);
void ravenemu_link_destroy(ravenemu_link* link);
/* One frame of every linked handle, run together. argb[i] receives the frame
 * of handles[i], in the format of ravenemu_run_frame; each buffer holds at
 * least pixel_capacity pixels. Returns 1 on success. */
int ravenemu_link_run_frame(ravenemu_link* link, uint32_t* const* argb, size_t pixel_capacity);

/* Link cable to a console on another device: another phone, a PC...
 * The host carries the messages between the two RavenCore instances, in
 * order and without loss (a TCP connection, for example): `send` receives
 * each message for the other side, and each message from it goes to
 * ravenemu_remote_link_receive. Then the host plugs the cable in.
 * GB/GBC: either console may clock a transfer. GBA: two players; `player` 0
 * is the parent of multiplayer games, the other device takes 1.
 * The console that clocks a transfer waits for the other's reply in real
 * time, inside ravenemu_run_frame: emulated time does not move, so the game
 * sees no delay. The other answers from its own ravenemu_run_frame, or
 * between frames from ravenemu_remote_link_service.
 * Threads: `send` is called from the thread running the handle and from the
 * one plugging the cable in; it must be thread-safe and must not block for
 * long. ravenemu_remote_link_receive and ravenemu_remote_link_set_connected
 * may be called from any thread; the other calls follow the handle's rule.
 * A handle joined by ravenemu_link_create takes no remote cable, and the
 * reverse. Returns NULL on error (see ravenemu_last_error). */
typedef struct ravenemu_remote_link ravenemu_remote_link;
typedef void (*ravenemu_remote_send)(void* user, const uint8_t* message, size_t size);
ravenemu_remote_link* ravenemu_remote_link_create(ravenemu_bridge* handle, int player,
                                                  ravenemu_remote_send send, void* user);
/* Unplugs and frees the cable: the handle plays alone again. Not while the
 * handle runs a frame: unplug first, a waiting handle then returns at once. */
void ravenemu_remote_link_destroy(ravenemu_remote_link* link);
/* A message from the other device. Returns 0 when it is malformed: it is
 * then ignored. */
int ravenemu_remote_link_receive(ravenemu_remote_link* link, const uint8_t* message, size_t size);
/* Plugs the cable in (1) or pulls it out (0). Plugged in, the console
 * introduces itself to the other one; the cable carries transfers once the
 * other has done the same. Pulled out, a transfer in progress ends as
 * without a cable. */
void ravenemu_remote_link_set_connected(ravenemu_remote_link* link, int connected);
enum ravenemu_remote_state {
    RAVENEMU_REMOTE_UNPLUGGED = 0,
    RAVENEMU_REMOTE_WAITING = 1,      /* plugged in, the other has not introduced itself yet */
    RAVENEMU_REMOTE_PLUGGED = 2,
    RAVENEMU_REMOTE_INCOMPATIBLE = 3  /* another protocol version or console family */
};
int ravenemu_remote_link_state(const ravenemu_remote_link* link);
/* Between two frames: answers the transfers the other console clocked in
 * the meantime, without waiting for the next frame. Returns 1 on success. */
int ravenemu_remote_link_service(ravenemu_remote_link* link);
/* 1 while the game uses its link port: a transfer asked for or awaited in
 * the last three seconds, or a GBA in multiplayer mode. A partner is welcome. */
int ravenemu_link_wanted(const ravenemu_bridge* handle);

#ifdef __cplusplus
}
#endif
#endif
