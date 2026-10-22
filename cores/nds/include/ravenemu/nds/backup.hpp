#pragma once

#include <ravenemu/nds/detail/serial_flash.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <vector>

namespace ravenemu::nds {

enum class BackupType { none, eeprom_512, eeprom_8k, eeprom_64k, flash_256k, flash_512k, flash_1m, fram_8k, fram_32k };

struct BackupSnapshot {
    std::vector<std::uint8_t> data; // image .sav brute, sans en-tête ni suffixe
    std::uint64_t generation{};
    std::uint8_t status{}; // seuls les bits non volatils ; métadonnée séparée
};

/** EEPROM, flash M45PE ou FRAM série de cartouche DS ; profil fourni par l'hôte.
 * Aucun type n'est déduit arbitrairement de l'en-tête de la ROM.
 */
class Backup {
public:
    [[nodiscard]] BackupType type() const noexcept { return type_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool busy() const noexcept { return operation_.remaining != 0 || flash_.busy(); }
    [[nodiscard]] bool dirty() const noexcept { return generation_ != saved_generation_; }
    [[nodiscard]] BackupSnapshot snapshot() const;

private:
    friend class Gamecard;
    struct Session {
        enum class Phase { command, address, read, write, status, write_status, ignore };
        enum class Commit { none, data, status };
        Phase phase{Phase::command};
        std::uint32_t address{};
        unsigned address_bytes{};
        std::uint8_t command{}, status{};
        std::array<std::uint8_t, 128> page{};
        std::bitset<128> written;
        bool has_status{};
        // FRAM : seul l'octet série terminé peut modifier l'image, avant CS haut.
        Commit commit{Commit::none};
        std::uint32_t write_address{};
        std::uint8_t write_value{};
        bool completed_write{};
        detail::SerialFlash::Session flash;
    };
    struct Operation {
        std::uint32_t address{}, remaining{};
        std::array<std::uint8_t, 128> page{};
        std::bitset<128> written;
        std::uint8_t status{};
        bool write_status{};
    };
    void load(BackupType type, std::span<const std::uint8_t> bytes, std::uint8_t status);
    [[nodiscard]] std::uint8_t exchange(Session& session, std::uint8_t output) const;
    void complete_byte(Session& session) noexcept;
    void abort_byte(Session& session) noexcept {
        // CS termine aussi les écritures FRAM antérieures à l'octet tronqué.
        if (is_fram() && (session.completed_write || session.has_status)) write_enabled_ = false;
        session = {};
    }
    void deselect(Session& session) noexcept;
    void advance_cycles(std::uint64_t cycles) noexcept;
    void reset_serial() noexcept { write_enabled_ = false; flash_.reset_serial(); }
    [[nodiscard]] bool session_idle(const Session& session) const noexcept {
        return is_flash() ? session.flash.phase == detail::SerialFlash::Session::Phase::command :
                            session.phase == Session::Phase::command;
    }
    void acknowledge_saved(std::uint64_t generation) noexcept {
        if (generation == generation_) saved_generation_ = generation;
    }
    [[nodiscard]] unsigned page_size() const noexcept;
    [[nodiscard]] bool is_fram() const noexcept { return type_ == BackupType::fram_8k || type_ == BackupType::fram_32k; }
    [[nodiscard]] bool is_flash() const noexcept {
        return type_ == BackupType::flash_256k || type_ == BackupType::flash_512k || type_ == BackupType::flash_1m;
    }
    [[nodiscard]] bool protected_address(std::uint32_t address) const noexcept;
    BackupType type_{BackupType::none};
    std::vector<std::uint8_t> bytes_;
    Operation operation_;
    detail::SerialFlash flash_;
    std::uint64_t generation_{}, saved_generation_{};
    std::uint8_t status_{};
    bool write_enabled_{};
};

} // namespace ravenemu::nds
