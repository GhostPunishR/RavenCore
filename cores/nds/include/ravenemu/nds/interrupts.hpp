#pragma once

#include <ravenemu/nds/hardware.hpp>

#include <cstdint>

namespace ravenemu::nds {

class InterruptController {
public:
    explicit InterruptController(Processor processor) noexcept;
    void reset() noexcept;
    void request(std::uint32_t sources) noexcept { flags_ |= sources & valid_mask_; }
    void acknowledge(std::uint32_t sources) noexcept { flags_ &= ~sources; }
    void set_enable(std::uint32_t value) noexcept { enable_ = value & valid_mask_; }
    void set_master_enable(bool value) noexcept { master_enable_ = value; }
    [[nodiscard]] std::uint32_t enable() const noexcept { return enable_; }
    [[nodiscard]] std::uint32_t flags() const noexcept { return flags_; }
    [[nodiscard]] bool master_enable() const noexcept { return master_enable_; }
    [[nodiscard]] bool pending() const noexcept { return master_enable_ && (enable_ & flags_) != 0; }

private:
    std::uint32_t valid_mask_;
    std::uint32_t enable_{};
    std::uint32_t flags_{};
    bool master_enable_{};
};

} // namespace ravenemu::nds
