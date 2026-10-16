#pragma once

#include "support/bits.hpp"

namespace ravenemu::gba {

constexpr std::size_t save_size(GbaSaveType type) noexcept {
    switch (type) {
    case GbaSaveType::sram: return 32U * 1024U;
    case GbaSaveType::flash_64k: return 64U * 1024U;
    case GbaSaveType::flash_128k: return 128U * 1024U;
    case GbaSaveType::eeprom_512: return 512U;
    case GbaSaveType::eeprom_8k: return 8U * 1024U;
    case GbaSaveType::none: return 0;
    }
    return 0;
}

class SaveMemory {
public:
    // Une mémoire vierge ou effacée se lit 0xFF, quel que soit le composant.
    explicit SaveMemory(GbaSaveType type) : type_(type), data_(save_size(type), 0xffU) {}
    virtual ~SaveMemory() = default;

    [[nodiscard]] GbaSaveType type() const noexcept { return type_; }
    [[nodiscard]] bool dirty() const noexcept { return generation_ != saved_generation_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return data_; }

    void import(std::span<const std::uint8_t> saved) {
        const auto count = std::min(saved.size(), data_.size());
        std::copy_n(saved.begin(), static_cast<std::ptrdiff_t>(count), data_.begin());
        std::fill(data_.begin() + static_cast<std::ptrdiff_t>(count), data_.end(), 0);
        saved_generation_ = generation_;
    }
    // Un réglage enregistré avec la mémoire a changé (horloge de la cartouche).
    void mark_modified() noexcept { written(); }
    void acknowledge(std::uint64_t generation) noexcept {
        if (generation == generation_) saved_generation_ = generation;
    }
    void mark_restored_after(std::uint64_t previous_generation) noexcept {
        generation_ = previous_generation + 1;
        saved_generation_ = previous_generation;
    }
    // Reprend le compteur de la mémoire qu'elle remplace au reset : une
    // génération déjà rendue ne resservira jamais, et une écriture pas encore
    // enregistrée reste à enregistrer.
    void continue_after(std::uint64_t previous_generation, bool previous_dirty) noexcept {
        generation_ = previous_generation + (previous_dirty ? 1U : 0U);
        saved_generation_ = previous_generation;
    }
    virtual void save_state(BinaryWriter& out) const { out.raw(data_); }
    virtual void load_state(BinaryReader& in) { in.raw(data_); }
    virtual int read(int address) = 0;
    virtual void write(int address, int value) = 0;
    virtual void hint_transfer_length(int) noexcept {}

protected:
    void written() noexcept { ++generation_; }

private:
    GbaSaveType type_;
    std::vector<std::uint8_t> data_;
    std::uint64_t generation_{};
    std::uint64_t saved_generation_{};
};

} // namespace ravenemu::gba
