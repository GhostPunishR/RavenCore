#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ravenemu::nds::detail {

/** Protocole M45PE natif commun au firmware et aux sauvegardes cartouche.
 * Ne possède pas l'image : seules les sessions et l'opération interne sont
 * conservées. Chaque périphérique fournit ses octets, son ID et son /W.
 */
class SerialFlash {
public:
    struct Model {
        std::array<std::uint8_t, 3> id{};
        std::uint32_t protected_prefix{};
    };
    struct Session {
        enum class Phase { command, address, dummy, data, program, erase, id, status, ignore };
        Phase phase{Phase::command};
        std::uint32_t address{};
        unsigned address_bytes{}, id_index{};
        std::uint8_t command{};
        std::array<std::uint8_t, 256> page{};
        unsigned count{};
    };
    SerialFlash() = default;
    explicit SerialFlash(Model model) noexcept : model_(model) {}
    [[nodiscard]] bool busy() const noexcept { return operation_.remaining != 0; }
    [[nodiscard]] std::uint8_t exchange(Session& session, std::uint8_t output, std::span<const std::uint8_t> bytes) const;
    void deselect(Session& session) noexcept;
    // Renvoie true uniquement si les octets ont changé au terme de l'opération.
    [[nodiscard]] bool advance_cycles(std::uint64_t cycles, std::span<std::uint8_t> bytes) noexcept;
    void reset_serial() noexcept { asleep_ = write_enabled_ = false; }

private:
    struct Operation {
        std::uint32_t address{}, length{}, remaining{};
        std::uint8_t command{};
        std::array<std::uint8_t, 256> page{};
    };
    Model model_;
    Operation operation_;
    bool asleep_{}, write_enabled_{};
};

} // namespace ravenemu::nds::detail
