#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <vector>

namespace ravenemu::nds {

enum class BackupType { none, eeprom_512, eeprom_8k, eeprom_64k };

struct BackupSnapshot {
    std::vector<std::uint8_t> data; // image .sav brute, sans en-tête ni suffixe
    std::uint64_t generation{};
    std::uint8_t status{}; // seuls les bits non volatils ; métadonnée séparée
};

/** EEPROM série de cartouche DS ; le profil est fourni par l'hôte.
 * Aucun type n'est déduit arbitrairement de l'en-tête de la ROM.
 */
class Backup {
public:
    [[nodiscard]] BackupType type() const noexcept { return type_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool busy() const noexcept { return operation_.remaining != 0; }
    [[nodiscard]] bool dirty() const noexcept { return generation_ != saved_generation_; }
    [[nodiscard]] BackupSnapshot snapshot() const;

private:
    friend class Gamecard;
    struct Session {
        enum class Phase { command, address, read, write, status, write_status, ignore };
        Phase phase{Phase::command};
        std::uint32_t address{};
        unsigned address_bytes{};
        std::uint8_t command{}, status{};
        std::array<std::uint8_t, 128> page{};
        std::bitset<128> written;
        bool has_status{};
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
    void deselect(Session& session) noexcept;
    void advance_cycles(std::uint64_t cycles) noexcept;
    void acknowledge_saved(std::uint64_t generation) noexcept {
        if (generation == generation_) saved_generation_ = generation;
    }
    [[nodiscard]] unsigned page_size() const noexcept;
    [[nodiscard]] bool protected_address(std::uint32_t address) const noexcept;
    BackupType type_{BackupType::none};
    std::vector<std::uint8_t> bytes_;
    Operation operation_;
    std::uint64_t generation_{}, saved_generation_{};
    std::uint8_t status_{};
    bool write_enabled_{};
};

} // namespace ravenemu::nds
