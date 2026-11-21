#pragma once

#include "interrupt/interrupt_controller.hpp"
#include "ppu/cgb_color.hpp"
#include "ppu/compatibility_palettes.hpp"
#include "ppu/frame_codec.hpp"
#include "ppu/oam_corruption.hpp"
#include "ppu/pixel_fifo.hpp"
#include <ravenemu/gbc/hardware_mode.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace ravenemu::cgb {

/**
 * PPU de la Game Boy et de la Game Boy Color. La classe garde l'état et les
 * accès simples ; chaque composant définit ses fonctions dans son en-tête,
 * incluse à la fin de celle-ci (voir la liste en bas de fichier).
 */
class Ppu {
public:
    static constexpr int width = 160;
    static constexpr int height = 144;
    static constexpr int frame_pixels = width * height;
    static constexpr int mode_hblank = 0;
    static constexpr int mode_vblank = 1;
    static constexpr int mode_oam = 2;
    static constexpr int mode_transfer = 3;

    using OamCorruption = cgb::OamCorruption;
    using FrameEncoding = cgb::FrameEncoding;

    Ppu(InterruptController& interrupts, gbc::HardwareMode hardware_mode)
        : interrupts_(interrupts), hardware_mode_(hardware_mode),
          opri_dmg_priority_(hardware_mode != gbc::HardwareMode::cgb_native) {
        rebuild_color_cache();
    }
    Ppu(InterruptController& interrupts, bool cgb_mode)
        : Ppu(interrupts, cgb_mode ? gbc::HardwareMode::cgb_native : gbc::HardwareMode::dmg) {}

    void set_hardware_mode(gbc::HardwareMode mode) noexcept {
        hardware_mode_ = mode;
        if (!gbc::cgb_features_enabled(mode)) {
            vram_bank_ = 0;
            speed_switch_video_access_ = speed_switch_access_inactive;
        }
    }

    [[nodiscard]] bool lcd_enabled() const noexcept { return (lcdc_ & 0x80) != 0; }
    void write_vram_bank(int value) noexcept { if (gbc::cgb_features_enabled(hardware_mode_)) vram_bank_ = value & 1; }
    [[nodiscard]] int read_vram_bank() const noexcept {
        return gbc::is_cgb_hardware(hardware_mode_) ? vram_bank_ | 0xfe : 0xff;
    }
    [[nodiscard]] int vram_bank() const noexcept { return vram_bank_; }

    void write_oam_direct(int index, int value) noexcept { oam[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value); }

    [[nodiscard]] bool oam_dma_active() const noexcept { return oam_dma_active_; }

    [[nodiscard]] int read_opri() const noexcept {
        return gbc::cgb_features_enabled(hardware_mode_) ? 0xfe | (opri_dmg_priority_ ? 1 : 0) : 0xff;
    }
    void write_opri(int value) noexcept {
        if (gbc::cgb_features_enabled(hardware_mode_)) opri_dmg_priority_ = (value & 1) != 0;
    }

    using CompatibilityPalettes = cgb::CompatibilityPalettes;
    // Palettes $00 de la boot ROM de la GBC : celles d'un jeu qu'elle ne
    // reconnaît pas, comme sur les captures de référence des suites publiques.
    static constexpr CompatibilityPalettes boot_default_palettes =
        compatibility::palettes_of(compatibility::default_entry, compatibility::default_flags);
    static_assert(boot_default_palettes.background == std::array<int, 4>{0x7fff, 0x1bef, 0x6180, 0x0000});
    // Les gris que donne B + Gauche au démarrage.
    static constexpr CompatibilityPalettes grayscale_palettes =
        compatibility::palettes_of(compatibility::manual_choices[5].entry, compatibility::manual_choices[5].flags);

    void reset_for_boot_rom() noexcept {
        lcdc_ = 0; render_lcdc_ = 0;
        stat_enable_ = 0;
        scy_ = 0; scx_ = 0; ly_ = 0; lyc_ = 0;
        bgp_ = 0; obp0_ = 0; obp1_ = 0; wy_ = 0; wx_ = 0;
        mode_ = mode_hblank; reported_mode_ = mode_hblank; reported_mode_target_ = mode_hblank;
        reported_mode_delay_ = 0; lcd_startup_line_ = false; lcd_startup_lines_remaining_ = 0;
        lyc_compare_delay_ = 0; frame_mode2_delay_ = false;
        line_dot_ = 0; window_line_ = 0; transfer_x_ = 0;
        lyc_equal_ = true;
        window_started_line_ = false; window_y_condition_ = false;
        window_wx_glitch_armed_ = false; window_wx_glitch_x_ = 0;
        oam_scan_index_ = 0; line_sprite_count_ = 0;
        speed_switch_video_access_ = speed_switch_access_inactive;
        end_oam_dma();
        reset_object_pipeline();
        stat_line_ = false; entered_hblank_ = false; frame_ready_ = false;
        completed_frame.fill(0); working_frame_.fill(0);
    }

    /**
     * Un pixel sorti de la FIFO n'est coloré que trois points plus tard : les
     * palettes et les bits 0 et 1 de LCDC, lus au rendu, sont ceux de ce
     * moment-là. Mealybug Tearoom (m3_bgp_change, m3_window_timing) le mesure
     * sur DMG comme sur CGB-D, sans que la durée du mode 3 change. Le bus en
     * tient compte en validant ces écritures dès le premier point de leur
     * M-cycle (MemoryBus::write_render_register_mcycle).
     *
     * Le bit 6 (carte de la window) suit le même instant : m3_lcdc_win_map_change
     * le mesure sur DMG et CGB. Les bits 3 et 4 (carte et données du BG)
     * gardent le leur, faute d'instant qui satisfasse à la fois Mealybug
     * Tearoom et AGE (m3-bg-lcdc) ; l'allumage de l'écran aussi.
     */
    static constexpr int early_lcdc_bits = 0x43;

    void preview_lcdc_render_bits(int value) noexcept {
        render_lcdc_ = (render_lcdc_ & ~early_lcdc_bits) | (value & early_lcdc_bits);
    }

    [[nodiscard]] int lcdc() const noexcept { return lcdc_; }
    [[nodiscard]] int scy() const noexcept { return scy_; }
    [[nodiscard]] int scx() const noexcept { return scx_; }
    [[nodiscard]] int ly() const noexcept { return visible_ly(); }
    [[nodiscard]] int lyc() const noexcept { return lyc_; }
    [[nodiscard]] int bgp() const noexcept { return bgp_; }
    [[nodiscard]] int obp0() const noexcept { return obp0_; }
    [[nodiscard]] int obp1() const noexcept { return obp1_; }
    [[nodiscard]] int wy() const noexcept { return wy_; }
    [[nodiscard]] int wx() const noexcept { return wx_; }
    [[nodiscard]] int mode() const noexcept { return lcd_enabled() ? reported_mode_ : mode_hblank; }
    [[nodiscard]] int line_dot() const noexcept { return line_dot_; }
    [[nodiscard]] int transfer_x() const noexcept { return transfer_x_; }
    [[nodiscard]] int window_line() const noexcept { return window_line_; }
    [[nodiscard]] int selected_object_count() const noexcept { return line_sprite_count_; }
    void set_scy(int v) noexcept { scy_ = byte(v); } void set_scx(int v) noexcept { scx_ = byte(v); }
    void set_bgp(int v) noexcept { bgp_ = byte(v); } void set_obp0(int v) noexcept { obp0_ = byte(v); }
    void set_obp1(int v) noexcept { obp1_ = byte(v); } void set_wy(int v) noexcept { wy_ = byte(v); }
    void set_wx(int v) noexcept {
        const int next_wx = byte(v);
        if (next_wx != wx_) {
            const int next_screen_x = next_wx - 7;
            window_wx_glitch_armed_ = lcd_enabled() && mode_ == mode_transfer &&
                window_started_line_ && next_screen_x >= transfer_x_ && next_screen_x < width;
            window_wx_glitch_x_ = window_wx_glitch_armed_ ? next_screen_x : 0;
        }
        wx_ = next_wx;
    }
    [[nodiscard]] bool take_hblank_entry() noexcept { const bool value = entered_hblank_; entered_hblank_ = false; return value; }

    std::array<std::uint8_t, 0x4000> vram{};
    std::array<std::uint8_t, 0xa0> oam{};
    std::array<std::uint8_t, 64> bg_cram{};
    std::array<std::uint8_t, 64> obj_cram{};
    std::array<std::int32_t, frame_pixels> completed_frame{};

    // Lignes, modes, STAT et LYC : ppu/line_timing.hpp
    void write_lcdc(int value) noexcept;
    [[nodiscard]] int read_stat() const noexcept;
    void write_stat(int value) noexcept;
    void write_lyc(int value) noexcept;
    void tick(int cycles);

    // Bus vidéo : accès du CPU et transition de vitesse : ppu/video_bus.hpp
    [[nodiscard]] int read_vram(int address) const noexcept;
    void write_vram(int address, int value) noexcept;
    [[nodiscard]] int read_oam(int address) const noexcept;
    void write_oam(int address, int value) noexcept;
    void begin_speed_switch() noexcept;
    void end_speed_switch() noexcept;
    [[nodiscard]] bool speed_switch_active() const noexcept;
    void validate_speed_switch_state(bool controller_switching) const;

    // Port OAM : scan du mode 2, DMA OAM et corruption : ppu/oam_port.hpp
    void corrupt_oam(OamCorruption kind) noexcept;
    void begin_oam_dma() noexcept;
    void write_oam_dma_byte(int index, int value) noexcept;
    void restore_oam_dma_bus(int transferred_bytes) noexcept;
    void end_oam_dma() noexcept;

    // RAM couleur de la GBC : ppu/color_ram.hpp
    void write_bcps(int value) noexcept;
    [[nodiscard]] int read_bcps() const noexcept;
    void write_bcpd(int value) noexcept;
    [[nodiscard]] int read_bcpd() const noexcept;
    void write_ocps(int value) noexcept;
    [[nodiscard]] int read_ocps() const noexcept;
    void write_ocpd(int value) noexcept;
    [[nodiscard]] int read_ocpd() const noexcept;
    void initialize_hle_post_boot() noexcept;
    void set_compatibility_palettes(const CompatibilityPalettes& palettes) noexcept;
    void initialize_hle_compatibility_palettes() noexcept;

    // États instantanés : ppu/ppu_state.hpp
    void save(BinaryWriter& out, bool tagged_frames = true) const;
    void load(BinaryReader& in, bool tagged_frames = true);

private:
    static constexpr int speed_switch_access_inactive = -1;
    static constexpr int speed_switch_access_none = 0;
    static constexpr int speed_switch_access_background = 1;
    static constexpr int speed_switch_access_all = 2;

    struct CpuVideoBusLocks {
        bool vram_read{};
        bool vram_write{};
        bool oam_read{};
        bool oam_write{};
        bool cram{};
    };

    static constexpr int obj_fetch_idle = 0;
    static constexpr int obj_fetch_wait_bg = 1;
    static constexpr int obj_fetch_oam = 2;
    static constexpr int obj_fetch_low = 3;
    static constexpr int obj_fetch_high = 4;

    struct ObjectTiming {
        int penalty_dots{};
        int tile_key{};
        bool first_for_tile{};
    };

    // Lignes, modes, STAT et LYC : ppu/line_timing.hpp
    void advance_dot();
    void enter_vblank();
    void set_mode(int mode) noexcept;
    void check_stat() noexcept;
    [[nodiscard]] bool mode2_stat_condition() const noexcept;
    void update_lyc_compare() noexcept;
    [[nodiscard]] int visible_ly() const noexcept;

    // Bus vidéo : accès du CPU et transition de vitesse : ppu/video_bus.hpp
    [[nodiscard]] bool ppu_vram_available() const noexcept;
    [[nodiscard]] bool ppu_oam_available() const noexcept;
    [[nodiscard]] bool ppu_cram_available() const noexcept;
    [[nodiscard]] CpuVideoBusLocks cpu_video_bus_locks() const noexcept;
    [[nodiscard]] int vram_byte(int bank, int address) const noexcept;

    // Port OAM : scan du mode 2, DMA OAM et corruption : ppu/oam_port.hpp
    void advance_oam_scan() noexcept;

    // RAM couleur de la GBC : ppu/color_ram.hpp
    static void recompute_argb(const std::array<std::uint8_t, 64>& cram, std::array<std::int32_t, 32>& colors, int byte_index) noexcept;
    void rebuild_color_cache() noexcept;

    // Mode 3 : transfert des pixels : ppu/pixel_transfer.hpp
    void begin_transfer() noexcept;
    void advance_transfer();
    void advance_background_output();

    // Fetcher du fond et de la window : ppu/background_fetcher.hpp
    [[nodiscard]] bool window_enabled_for_rendering() const noexcept;
    [[nodiscard]] bool window_should_start() const noexcept;
    void start_window_fetch() noexcept;
    void reset_bg_fetcher(bool window) noexcept;
    void tick_bg_fetcher() noexcept;
    void fetch_tile_index() noexcept;
    [[nodiscard]] int fetcher_bank() const noexcept;
    void refresh_fetcher_row() noexcept;
    void fetch_tile_low() noexcept;
    void fetch_tile_high() noexcept;
    void push_fetched_tile() noexcept;
    [[nodiscard]] BgPixel pop_bg_pixel() noexcept;
    void discard_window_prefix() noexcept;

    // Fetcher des objets : ppu/object_fetcher.hpp
    void reset_object_fetch() noexcept;
    void reset_object_pipeline() noexcept;
    [[nodiscard]] bool object_fetch_may_be_canceled() const noexcept;
    [[nodiscard]] int next_object_at_current_x() const noexcept;
    void begin_object_fetch(int sprite_slot) noexcept;
    void cancel_object_fetch() noexcept;
    void advance_object_fetch() noexcept;
    void latch_object_parameters() noexcept;
    [[nodiscard]] int object_fetch_bank() const noexcept;
    void ensure_object_fifo_size(int size) noexcept;
    [[nodiscard]] bool object_pixel_has_priority(const ObjPixel& candidate, const ObjPixel& current) const noexcept;
    void merge_fetched_object() noexcept;
    [[nodiscard]] ObjPixel pop_obj_pixel() noexcept;
    [[nodiscard]] ObjectTiming object_timing(int sprite_slot, int sprite_x) const noexcept;

    // Rendu des pixels : ppu/pixel_renderer.hpp
    void render_fifo_pixel(int x, const BgPixel& pixel, const ObjPixel& object);
    void render_object_dmg(int base, int x, const ObjPixel& object) noexcept;
    void render_object_cgb(int base, int x, const ObjPixel& object) noexcept;

    // États instantanés : ppu/ppu_state.hpp
    void validate_timing_state() const;
    void validate_object_pipeline_state() const;

    // Registres LCD
    InterruptController& interrupts_;
    gbc::HardwareMode hardware_mode_{gbc::HardwareMode::dmg};
    int lcdc_{0x91};
    // Bits 0, 1 et 6 de LCDC tels que le rendu et le fetcher les voient ;
    // égal à lcdc_ entre deux instructions (voir preview_lcdc_render_bits).
    int render_lcdc_{0x91};
    int stat_enable_{};
    int scy_{};
    int scx_{};
    int ly_{};
    int lyc_{};
    int bgp_{0xfc};
    int obp0_{};
    int obp1_{};
    int wy_{};
    int wx_{};
    int vram_bank_{};
    // RAM couleur : index et cache ARGB
    int bcps_index_{};
    bool bcps_auto_{};
    int ocps_index_{};
    bool ocps_auto_{};
    std::array<std::int32_t, 32> bg_argb_{};
    std::array<std::int32_t, 32> obj_argb_{};
    // Séquencement des lignes et des modes
    int mode_{mode_oam};
    int reported_mode_{mode_oam};
    int reported_mode_target_{mode_oam};
    int reported_mode_delay_{};
    bool lcd_startup_line_{};
    int lcd_startup_lines_remaining_{};
    int lyc_compare_delay_{};
    bool frame_mode2_delay_{};
    int speed_switch_video_access_{speed_switch_access_inactive};
    int line_dot_{};
    int window_line_{};
    bool lyc_equal_{true};
    // Mode 3 et window
    int transfer_x_{};
    bool window_started_line_{};
    // The HLE/post-boot state starts at LY=0 with WY=0 and the LCD already on.
    // Consequently the WY condition has already been observed for the first
    // scanline.  LCD enable/disable and frame transitions maintain it from
    // this point onward.
    bool window_y_condition_{true};
    bool window_wx_glitch_armed_{};
    int window_wx_glitch_x_{};
    bool opri_dmg_priority_{};
    int scx_latched_{};
    int startup_dots_remaining_{};
    int discard_pixels_remaining_{};
    int window_restart_dots_remaining_{};
    int window_initial_skip_{};
    // FIFO et fetcher du fond
    std::array<BgPixel, 16> bg_fifo_{};
    int bg_fifo_head_{};
    int bg_fifo_size_{};
    bool fetcher_window_{};
    int fetcher_tile_x_{};
    int fetcher_phase_{};
    int fetcher_phase_dots_{2};
    int fetcher_tile_{};
    int fetcher_attributes_{};
    int fetcher_row_{};
    int fetcher_tile_data_address_{};
    int fetcher_data_low_{};
    int fetcher_data_high_{};
    // FIFO et fetcher des objets
    std::array<ObjPixel, 16> obj_fifo_{};
    int obj_fifo_head_{};
    int obj_fifo_size_{};
    int obj_fetch_phase_{obj_fetch_idle};
    int obj_fetch_phase_dots_remaining_{};
    int obj_fetch_slot_{-1};
    int obj_fetch_sprite_{-1};
    int obj_fetch_screen_x_{};
    int obj_fetch_raw_x_{};
    int obj_fetch_tile_{};
    int obj_fetch_attributes_{};
    int obj_fetch_row_{};
    int obj_fetch_address_{};
    int obj_fetch_data_low_{};
    int obj_fetch_data_high_{};
    int obj_fetch_output_overlap_remaining_{};
    // Port OAM : DMA et objets retenus par le scan
    bool oam_dma_active_{};
    int oam_dma_bus_low_{0xff};
    int oam_dma_bus_high_{0xff};
    int line_sprite_count_{};
    int oam_scan_index_{};
    std::array<bool, 10> sprite_fetch_consumed_{};
    std::array<int, 10> considered_sprite_tiles_{};
    int considered_sprite_tile_count_{};
    // Interruption STAT et image en cours
    bool stat_line_{};
    bool entered_hblank_{};
    bool frame_ready_{};
    std::array<std::int32_t, frame_pixels> working_frame_{};
    std::array<int, width> bg_color_line_{};
    std::array<bool, width> bg_priority_line_{};
    std::array<int, 10> sprite_indices_{};
    std::array<int, 10> sprite_y_{};
    std::array<int, 10> sprite_x_{};
};

} // namespace ravenemu::cgb

// Définitions des fonctions membres, une en-tête par composant.
#include "ppu/line_timing.hpp"
#include "ppu/video_bus.hpp"
#include "ppu/oam_port.hpp"
#include "ppu/color_ram.hpp"
#include "ppu/pixel_transfer.hpp"
#include "ppu/background_fetcher.hpp"
#include "ppu/object_fetcher.hpp"
#include "ppu/pixel_renderer.hpp"
#include "ppu/ppu_state.hpp"
