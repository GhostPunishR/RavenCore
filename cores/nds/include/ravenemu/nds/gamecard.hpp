#pragma once

#include <ravenemu/nds/cartridge.hpp>
#include <ravenemu/nds/backup.hpp>

#include <array>
#include <optional>

namespace ravenemu::nds {

/** Port cartouche : protocole RAW ou lecture de données après démarrage direct.
 * Le modèle travaille sur les commandes/réponses déchiffrées ; pas de boot KEY1.
 * Une seule donnée 32 bits peut être prête à la fois, sans référence au bus.
 */
class Gamecard {
public:
    enum class Mode { raw, main_data };
    struct Events { bool ready{}, completion_irq{}; };
    struct Read { std::uint32_t value{}; Events events; };
    void load(const Cartridge& cartridge, Mode mode = Mode::main_data);
    // À configurer après le média ; remplacer une cartouche retire sa sauvegarde.
    void load_backup(BackupType type, std::span<const std::uint8_t> bytes = {}, std::uint8_t status = 0);
    [[nodiscard]] const Backup& backup() const noexcept { return backup_; }
    void acknowledge_backup_saved(std::uint64_t generation) noexcept { backup_.acknowledge_saved(generation); }
    void reset() noexcept; // conserve média et mode de démarrage
    [[nodiscard]] bool busy() const noexcept { return busy_ || spi_wait_; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] std::uint8_t read_byte(unsigned offset) const noexcept;
    void validate_write(unsigned offset, std::uint32_t value, unsigned width) const;
    void write(unsigned offset, std::uint32_t value, unsigned width);
    [[nodiscard]] Events advance_cycles(std::uint64_t cycles) noexcept;
    [[nodiscard]] std::uint32_t peek_data() const noexcept { return data_; }
    [[nodiscard]] Read read_data() noexcept;

private:
    [[nodiscard]] std::uint32_t merge(unsigned offset, std::uint32_t value, unsigned width,
                                      unsigned first, unsigned count, std::uint32_t previous) const noexcept;
    [[nodiscard]] std::uint32_t reply_word() const noexcept;
    void start(std::uint32_t control) noexcept;
    [[nodiscard]] std::uint16_t proposed_auxiliary(unsigned offset, std::uint32_t value, unsigned width) const noexcept;
    void advance_spi(std::uint64_t cycles) noexcept;
    std::optional<Cartridge> cartridge_;
    Backup backup_;
    Backup::Session spi_session_, pending_spi_session_;
    Mode mode_{Mode::raw};
    std::array<std::uint8_t, 8> command_{};
    std::uint32_t capacity_{0x800000}, chip_id_{0xffffffff}, control_{}, data_{};
    std::uint32_t remaining_{}, transferred_{}, wait_{}, base_{}, offset_{};
    std::uint16_t auxiliary_{};
    std::uint32_t spi_wait_{};
    std::uint8_t spi_data_{}, spi_received_{};
    std::uint8_t active_command_{};
    bool busy_{}, ready_{};
};

} // namespace ravenemu::nds
