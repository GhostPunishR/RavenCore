#pragma once

#include <ravenemu/core.hpp>
#include <ravenemu/nds/hardware.hpp>

#include <array>
#include <optional>
#include <string>

namespace ravenemu::nds {

// Quatre canaux par CPU ; registres programmés et compteurs internes séparés.
class DmaController {
public:
    struct Transfer {
        unsigned channel{};
        std::uint32_t source{}, destination{}, remaining{}, control{};
    };

    explicit DmaController(Processor processor) noexcept : processor_(processor) {}
    void reset() noexcept;
    // Offset relatif à DMA0SAD (0x040000b0), plage 0..0x2f.
    // Hors plage : lecture zéro, écriture ignorée.
    [[nodiscard]] std::uint8_t read_byte(unsigned offset) const noexcept;
    void write_byte(unsigned offset, std::uint8_t value) noexcept;
    [[nodiscard]] std::optional<Transfer> next() const noexcept;
    // Après une unité réussie : avance les pointeurs et retourne le masque IF de fin.
    [[nodiscard]] std::uint32_t complete_unit() noexcept;

private:
    struct Channel {
        std::array<std::uint32_t, 3> registers{};
        Transfer transfer;
        bool pending{};
    };
    Processor processor_;
    std::array<Channel, 4> channels_{};
    [[nodiscard]] std::uint32_t address_mask(unsigned channel, bool source) const noexcept;
    [[nodiscard]] std::uint32_t count_mask(unsigned channel) const noexcept;
};

class DmaTransferError final : public Error {
public:
    DmaTransferError(Processor processor, DmaController::Transfer transfer, const std::string& reason);
    [[nodiscard]] Processor processor() const noexcept { return processor_; }
    [[nodiscard]] const DmaController::Transfer& transfer() const noexcept { return transfer_; }

private:
    Processor processor_;
    DmaController::Transfer transfer_;
};

} // namespace ravenemu::nds
