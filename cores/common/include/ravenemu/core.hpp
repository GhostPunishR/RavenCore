#pragma once

#include <ravenemu/cheats.hpp>
#include <ravenemu/link_endpoint.hpp>
#include <ravenemu/infrared_endpoint.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ravenemu {

enum class Console : std::uint8_t {
    game_boy = 0,
    game_boy_advance = 2,
};

enum class Button : std::uint8_t {
    up,
    down,
    left,
    right,
    a,
    b,
    start,
    select,
    l,
    r,
};

enum class FramebufferFormat : std::uint8_t {
    argb_8888,
    indexed_4,
};

enum class GbaSaveType : std::uint8_t {
    none,
    sram,
    flash_64k,
    flash_128k,
    eeprom_512,
    eeprom_8k,
};

/** Capteurs portés par la cartouche chargée. */
struct CartridgeSensors {
    bool solar{};     ///< capteur solaire : Boktai 1 à 3
    bool rotation{};  ///< gyroscope : WarioWare: Twisted!
    bool tilt{};      ///< inclinaison : Yoshi Topsy-Turvy, Koro Koro Puzzle, cartouches GB MBC7
};

struct VideoSpec {
    int width{};
    int height{};
    double refresh_rate_hz{};

    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

struct AudioSpec {
    int sample_rate_hz{};
    int channel_count{};
};

struct BatterySnapshot {
    std::vector<std::uint8_t> data;
    std::uint64_t generation{};
};

enum class DiagnosticEvent : std::uint8_t {
    unsupported_swi,
    undefined_instruction,
    unsupported_access,
    missing_interrupt,
    decompression_error,
};

struct DiagnosticMessage {
    DiagnosticEvent event{};
    std::string detail;
};

/** POD mirror of the Kotlin GbaDebugSnapshot contract. */
struct GbaDebugSnapshot {
    std::int32_t instructions_per_frame{};
    std::int32_t program_counter{};
    bool thumb{};
    bool halted{};
    std::int32_t last_swi{-1};
    std::int32_t last_interrupt_mask{};
    std::int32_t vcount{};
    std::int32_t last_dma_channel{-1};
    bool dma_active{};
    std::int32_t fifo_a_size{};
    std::int32_t fifo_b_size{};
    std::int32_t fifo_a_empty_reads{};
    std::int32_t fifo_b_empty_reads{};
    std::int32_t audio_underruns{};
    std::int32_t unsupported_swi_count{};
    std::int32_t undefined_instruction_count{};
    std::int32_t unsupported_access_count{};
    std::int32_t missing_interrupt_count{};
    std::int32_t decompression_error_count{};
    std::int32_t first_unsupported_address{};
    std::int32_t dispcnt{};
    std::int32_t bg0_control{};
    std::int32_t bg1_control{};
    std::int32_t bg2_control{};
    std::int32_t bg3_control{};
    std::int32_t blend_control{};
    std::int32_t blend_alpha{};
    std::int32_t blend_brightness{};
    std::int32_t window_inside{};
    std::int32_t window_outside{};
    std::int32_t luma_min{};
    std::int32_t luma_max{};
    std::int32_t luma_mean{};
    std::vector<std::int32_t> layer_pixels;
    std::int32_t bg2_reference_x{};
    std::int32_t bg2_reference_y{};
    std::int32_t bg2_scale_x{};
    std::int32_t bg2_scale_y{};
    std::int32_t bg2_matrix_writes{};
    std::int32_t bg2_reference_writes{};
    std::vector<std::int32_t> swi_counts;
    double ppu_millis{};
    double dma_millis{};
    double apu_millis{};
};

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RomLoadError final : public Error {
public:
    using Error::Error;
};

class SaveStateError final : public Error {
public:
    using Error::Error;
};

/** Code de triche illisible ou non pris en charge ; le message dit pourquoi, en français. */
class CheatError final : public Error {
public:
    using Error::Error;
};

/**
 * Décode une entrée de triche pour `console` : une ou plusieurs lignes,
 * séparées par des espaces, des retours à la ligne ou des « + ».
 * - Game Boy et Game Boy Color : Game Genie (`ABC-DEF-GHI` ou `ABC-DEF`) et
 *   GameShark (8 chiffres, comme `010238CD`), mélangeables.
 * - GBA : Codebreaker non chiffré (`XAAAAAAA YYYY`), GameShark v1/v2 et Pro
 *   Action Replay v3 chiffrés tels que l'appareil les affiche ; ces deux-là
 *   se reconnaissent à leur déchiffrement.
 * Lève CheatError pour un code illisible ou une fonction absente (patch de
 * ROM GBA, changement de chiffrement DEADFACE, Codebreaker chiffré).
 */
[[nodiscard]] Cheat parse_cheat(Console console, std::string_view code);

class Core;
class RemoteLink;

/**
 * Câble link entre cœurs d'un même processus, par exemple deux jeux sur un
 * même appareil. Une fois reliés, les cœurs avancent ensemble par le
 * run_frame du câble, et non plus chacun par le sien. Ils doivent survivre
 * au câble ; le détruire les rend indépendants.
 */
class LinkCable {
public:
    virtual ~LinkCable() = default;
    [[nodiscard]] virtual std::size_t players() const noexcept = 0;
    /** Une image de chaque cœur : `framebuffers[i]` reçoit celle du joueur i. */
    virtual void run_frame(std::span<const std::span<std::int32_t>> framebuffers, bool render_video) = 0;
};

class Core {
public:
    virtual ~Core() = default;

    [[nodiscard]] virtual Console console() const noexcept = 0;
    [[nodiscard]] virtual VideoSpec video_spec() const noexcept = 0;
    [[nodiscard]] virtual AudioSpec audio_spec() const noexcept = 0;
    [[nodiscard]] virtual FramebufferFormat framebuffer_format() const noexcept = 0;
    [[nodiscard]] virtual bool supports_video_frame_skipping() const noexcept { return false; }

    virtual void load_rom(
        std::span<const std::uint8_t> rom,
        std::span<const std::uint8_t> battery_ram
    ) = 0;
    virtual void reset() = 0;
    virtual void run_frame(std::span<std::int32_t> framebuffer, bool render_video) = 0;
    virtual void set_button(Button button, bool pressed) = 0;
    // RetroAchievements address space; observational reads, never clock the bus.
    virtual std::size_t read_achievement_memory(std::uint32_t, std::span<std::uint8_t>) const { return 0; }
    virtual std::size_t read_audio(std::span<std::int16_t> destination) = 0;
    [[nodiscard]] virtual bool connect_link_endpoint(LinkEndpoint*) noexcept { return false; }
    /** Câble de `players`, ce cœur en tête : voir connect_link_cable(). */
    [[nodiscard]] virtual std::unique_ptr<LinkCable> create_link_cable(std::span<Core* const> players);
    /**
     * Câble vers une console d'un autre appareil (voir RemoteLink) ; nullptr
     * le retire. GBA : `player` 0 est le parent des jeux multijoueurs, l'autre
     * appareil prend 1. Faux pour une console qui ne se relie pas ainsi.
     * Le câble doit survivre au cœur ou lui être retiré avant.
     */
    [[nodiscard]] virtual bool connect_remote_link(RemoteLink*, int /*player*/) { return false; }
    /** Répond aux transferts du partenaire distant entre deux images, sans attendre la suivante. */
    virtual void service_remote_link() {}
    /** Le jeu s'est servi de son port link ces dernières secondes : un partenaire est bienvenu. */
    [[nodiscard]] virtual bool link_wanted() const noexcept { return false; }
    [[nodiscard]] virtual bool connect_infrared_endpoint(InfraredEndpoint*) noexcept { return false; }
    [[nodiscard]] virtual bool rumble_active() const noexcept { return false; }
    /** Inclinaison cartouche en unités brutes autour du repos (0, 0). */
    virtual void set_game_boy_acceleration(int, int) noexcept {}
    [[nodiscard]] virtual CartridgeSensors cartridge_sensors() const noexcept { return {}; }
    /** Lumière reçue par le capteur solaire : 0 (obscurité) à 255 (plein soleil). */
    virtual void set_solar_light(int) noexcept {}
    /** Rotation autour de l'axe de l'écran : -32768..32767, positive dans le sens horaire. */
    virtual void set_rotation_speed(int) noexcept {}
    /**
     * Inclinaison du capteur GBA : -32768..32767 par axe pour -1 g..1 g ;
     * x positif quand le côté droit de la console descend, y positif quand
     * son bord supérieur s'éloigne du joueur.
     */
    virtual void set_gba_tilt(int, int) noexcept {}
    /**
     * Codes de triche actifs à partir de l'image suivante, à la place des
     * précédents ; une liste vide les retire. Ils survivent aux changements
     * de ROM, aux réinitialisations et aux états, sans y être enregistrés.
     * Lève std::invalid_argument pour un code décodé pour une autre console.
     */
    virtual void set_cheats(std::span<const Cheat> cheats) {
        if (!cheats.empty()) throw std::invalid_argument("Cette console ne prend pas de codes de triche");
    }

    [[nodiscard]] virtual bool has_battery_ram() const noexcept = 0;
    [[nodiscard]] virtual bool battery_ram_dirty() const noexcept = 0;
    [[nodiscard]] virtual std::optional<BatterySnapshot> snapshot_battery_ram() = 0;
    virtual void acknowledge_battery_ram_saved(std::uint64_t generation) noexcept = 0;

    [[nodiscard]] virtual std::vector<std::uint8_t> save_state() const = 0;
    virtual void load_state(std::span<const std::uint8_t> state) = 0;

    virtual void set_clock_epoch(std::optional<std::int64_t>) noexcept {}
    virtual void set_measuring_time(bool) noexcept {}
    [[nodiscard]] virtual bool measuring_time() const noexcept { return false; }
    [[nodiscard]] virtual std::optional<GbaDebugSnapshot> debug_snapshot() const { return std::nullopt; }
    [[nodiscard]] virtual std::vector<DiagnosticMessage> drain_diagnostics() { return {}; }
    [[nodiscard]] virtual GbaSaveType gba_save_type() const noexcept { return GbaSaveType::none; }
    virtual void set_gba_forced_save_type(std::optional<GbaSaveType>) noexcept {}
    /** Horloge de cartouche réellement présente sur le jeu chargé. */
    [[nodiscard]] virtual bool gba_rtc_active() const noexcept { return false; }
    /**
     * Impose la présence ou l'absence de l'horloge de cartouche, ou rend la
     * main à la détection avec `std::nullopt`. Pris en compte au prochain
     * chargement de ROM ou à la prochaine réinitialisation.
     */
    virtual void set_gba_forced_rtc(std::optional<bool>) noexcept {}
};

/**
 * Relie deux cœurs Game Boy ou Game Boy Color (mélangés si besoin), ou deux à
 * quatre cœurs GBA dont le premier est le parent. Lève std::invalid_argument
 * pour une combinaison impossible.
 */
[[nodiscard]] std::unique_ptr<LinkCable> connect_link_cable(std::span<Core* const> players);

[[nodiscard]] std::unique_ptr<Core> make_game_boy_core();
[[nodiscard]] std::unique_ptr<Core> make_gba_core(std::optional<GbaSaveType> forced_save_type);

} // namespace ravenemu

