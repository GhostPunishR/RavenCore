#pragma once

#include "support/bits.hpp"

#include <array>

namespace ravenemu::gba {

class MemoryTiming {
public:
    MemoryTiming() noexcept { rebuild_table(); }
    [[nodiscard]] int wait_control() const noexcept { return wait_control_; }
    void set_wait_control(int value) noexcept {
        if (value == wait_control_) return;
        wait_control_ = value; rebuild_table();
    }
    [[nodiscard]] int bus_width(std::int32_t address) const noexcept {
        const auto region = (u32(address) >> 24U) & 0xffU;
        if (region == 0x02U || region == 0x05U || region == 0x06U ||
            (region >= 0x08U && region <= 0x0dU)) return 2;
        if (region == 0x0eU || region == 0x0fU) return 1;
        return 4;
    }
    [[nodiscard]] int wait_states(std::int32_t address, int width, bool sequential) const noexcept {
        const auto first = unit_wait(address, sequential);
        const auto extra = width / bus_width(address) - 1;
        return extra <= 0 ? first : first + extra * (unit_wait(address, true) + 1);
    }
    [[nodiscard]] int instruction_wait(std::int32_t address, int width, bool sequential) const noexcept {
        const auto region = (u32(address) >> 24U) & 0xffU;
        if (sequential && (wait_control_ & 0x4000) != 0 && region >= 0x08U && region <= 0x0dU) return 0;
        return wait_states(address, width, sequential);
    }
    /**
     * Même résultat que wait_states / instruction_wait, lu dans une table
     * reconstruite à chaque écriture de WAITCNT : le bus compte des attentes
     * à chaque accès, soit plusieurs fois par instruction.
     */
    [[nodiscard]] int cached_wait(std::int32_t address, int width, bool sequential, bool fetch) const noexcept {
        const auto region = u32(address) >> 24U;
        if (region > 0x0fU) return 0;
        const auto width_index = static_cast<std::uint32_t>(width >> 1);  // 1, 2, 4 -> 0, 1, 2
        return table_[((region * 3U + width_index) * 2U + (sequential ? 1U : 0U)) * 2U + (fetch ? 1U : 0U)];
    }

private:
    void rebuild_table() noexcept {
        for (std::uint32_t region = 0; region <= 0x0fU; ++region) {
            const auto address = i32(region << 24U);
            for (const auto width : {1, 2, 4}) {
                const auto width_index = width == 1 ? 0U : width == 2 ? 1U : 2U;
                for (const bool sequential : {false, true}) {
                    const auto base = ((region * 3U + width_index) * 2U + (sequential ? 1U : 0U)) * 2U;
                    table_[base] = static_cast<std::uint8_t>(wait_states(address, width, sequential));
                    table_[base + 1] = static_cast<std::uint8_t>(instruction_wait(address, width, sequential));
                }
            }
        }
    }
    std::array<std::uint8_t, 16 * 3 * 2 * 2> table_{};
    int wait_control_{};

    [[nodiscard]] int unit_wait(std::int32_t address, bool sequential) const noexcept {
        static constexpr std::array n_wait{4, 3, 2, 8};
        const auto raw = u32(address);
        const auto region = (raw >> 24U) & 0xffU;
        if (region == 0x02U) return 2;
        if (region == 0x0eU || region == 0x0fU) return n_wait[static_cast<std::size_t>(wait_control_ & 3)];
        if (region < 0x08U || region > 0x0dU) return 0;
        switch ((raw >> 25U) & 3U) {
        case 0: return sequential ? ((wait_control_ & 0x10) != 0 ? 1 : 2)
                                  : n_wait[static_cast<std::size_t>((wait_control_ >> 2) & 3)];
        case 1: return sequential ? ((wait_control_ & 0x80) != 0 ? 1 : 4)
                                  : n_wait[static_cast<std::size_t>((wait_control_ >> 5) & 3)];
        default: return sequential ? ((wait_control_ & 0x400) != 0 ? 1 : 8)
                                    : n_wait[static_cast<std::size_t>((wait_control_ >> 8) & 3)];
        }
    }
};

} // namespace ravenemu::gba
