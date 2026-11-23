#pragma once

#include "memory/memory.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

// Les neuf banques de VRAM (656 Kio) et leur projection par VRAMCNT_A à
// VRAMCNT_I (GBATEK, « DS Memory Control - VRAM »), par pages de 16 Kio.
// Deux banques projetées au même endroit : la lecture combine leurs octets
// (OU) et l'écriture va dans les deux. Les palettes étendues et les textures
// ne sont pas encore projetées : leurs banques sortent de l'espace du CPU.
namespace ravenemu::nds {

class Vram {
public:
    // Zones vues par le CPU et les moteurs 2D.
    enum Area : unsigned { lcdc, bg_a, obj_a, bg_b, obj_b, arm7, area_count };
    static constexpr std::size_t page_size = 16 * 1024;
    static constexpr std::array<unsigned, 9> bank_pages{8, 8, 8, 8, 4, 1, 1, 2, 1};       // A à I
    static constexpr std::array<unsigned, area_count> area_pages{41, 32, 16, 8, 8, 16};  // 656, 512, 256, 128, 128, 256 Kio

    Vram() {
        unsigned offset = 0;
        for (unsigned bank = 0; bank < 9; ++bank) {
            bank_offset_[bank] = offset;
            offset += bank_pages[bank];
        }
        storage_.resize(std::size_t{offset} * page_size);
    }

    [[nodiscard]] std::uint8_t control(unsigned bank) const noexcept { return control_[bank]; }
    void set_control(unsigned bank, std::uint8_t value) noexcept {
        control_[bank] = value;
        remap();
    }

    // Lecture et écriture dans une zone, à `offset` depuis son début
    // (les miroirs sont déjà repliés par l'appelant).
    [[nodiscard]] std::uint32_t read(Area area, std::uint32_t offset, unsigned width) const noexcept {
        const auto page = offset / page_size;
        if (page >= area_pages[area]) return 0;
        unsigned banks = pages_[area][page];
        std::uint32_t value = 0;
        while (banks != 0) {
            const auto bank = static_cast<unsigned>(std::countr_zero(banks));
            banks &= banks - 1U;
            value |= load(at(bank, page, offset), width);
        }
        return value;
    }
    void write(Area area, std::uint32_t offset, std::uint32_t value, unsigned width) noexcept {
        const auto page = offset / page_size;
        if (page >= area_pages[area]) return;
        unsigned banks = pages_[area][page];
        while (banks != 0) {
            const auto bank = static_cast<unsigned>(std::countr_zero(banks));
            banks &= banks - 1U;
            store(at(bank, page, offset), value, width);
        }
    }

    // Contenu brut d'une banque (affichage de la VRAM, mode 2).
    [[nodiscard]] std::uint16_t bank_halfword(unsigned bank, std::uint32_t offset) const noexcept {
        const auto size = bank_pages[bank] * page_size;
        return static_cast<std::uint16_t>(load(&storage_[bank_offset_[bank] * page_size + (offset % size)], 2));
    }
    // VRAMSTAT de l'ARM7 : banques C et D qui lui sont données.
    [[nodiscard]] std::uint8_t arm7_status() const noexcept {
        return static_cast<std::uint8_t>((area_[2] == arm7 ? 1U : 0U) | (area_[3] == arm7 ? 2U : 0U));
    }

private:
    static constexpr unsigned none = area_count;

    [[nodiscard]] std::uint8_t* at(unsigned bank, std::size_t page, std::uint32_t offset) noexcept {
        return &storage_[(bank_offset_[bank] + page - first_page_[bank]) * page_size + (offset % page_size)];
    }
    [[nodiscard]] const std::uint8_t* at(unsigned bank, std::size_t page, std::uint32_t offset) const noexcept {
        return &storage_[(bank_offset_[bank] + page - first_page_[bank]) * page_size + (offset % page_size)];
    }

    // Zone et première page de chaque banque, d'après son MST et son OFS.
    void place(unsigned bank, unsigned mst, unsigned ofs) noexcept {
        static constexpr std::array<unsigned, 9> lcdc_page{0, 8, 16, 24, 32, 36, 37, 38, 40};
        const auto small_page = (ofs & 1U) + 4U * (ofs >> 1U);  // F, G : 16 Kio × OFS.0 + 64 Kio × OFS.1
        auto set = [&](unsigned area, unsigned page) {
            area_[bank] = area;
            first_page_[bank] = page;
        };
        if (mst == 0) return set(lcdc, lcdc_page[bank]);
        switch (bank) {
        case 0:
        case 1:
            if (mst == 1) set(bg_a, ofs * 8);
            else if (mst == 2) set(obj_a, (ofs & 1U) * 8);
            break;
        case 2:
        case 3:
            if (mst == 1) set(bg_a, ofs * 8);
            else if (mst == 2) set(arm7, (ofs & 1U) * 8);
            else if (mst == 4) set(bank == 2 ? bg_b : obj_b, 0);
            break;
        case 4:
            if (mst == 1) set(bg_a, 0);
            else if (mst == 2) set(obj_a, 0);
            break;
        case 5:
        case 6:
            if (mst == 1) set(bg_a, small_page);
            else if (mst == 2) set(obj_a, small_page);
            break;
        case 7:
            if (mst == 1) set(bg_b, 0);
            break;
        default:  // I
            if (mst == 1) set(bg_b, 2);
            else if (mst == 2) set(obj_b, 0);
            break;
        }
    }

    void remap() noexcept {
        for (auto& pages : pages_) pages.fill(0);
        for (unsigned bank = 0; bank < 9; ++bank) {
            area_[bank] = none;
            const auto value = control_[bank];
            if ((value & 0x80U) == 0) continue;
            const auto mst = value & (bank <= 1 || bank >= 7 ? 3U : 7U);  // MST sur 2 bits pour A, B, H, I
            place(bank, mst, (value >> 3U) & 3U);
            if (area_[bank] == none) continue;
            for (unsigned page = 0; page < bank_pages[bank]; ++page) {
                const auto target = first_page_[bank] + page;
                if (target < area_pages[area_[bank]]) pages_[area_[bank]][target] |= static_cast<std::uint16_t>(1U << bank);
            }
        }
    }

    std::vector<std::uint8_t> storage_;
    std::array<unsigned, 9> bank_offset_{};  // en pages
    std::array<std::uint8_t, 9> control_{};
    std::array<unsigned, 9> area_{none, none, none, none, none, none, none, none, none};
    std::array<unsigned, 9> first_page_{};
    std::array<std::array<std::uint16_t, 41>, area_count> pages_{};
};

} // namespace ravenemu::nds
