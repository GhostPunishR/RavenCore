#pragma once

#include "arm/cpu.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// Coprocesseur système de l'ARM9 (GBATEK, « ARM CP15 System Control
// Coprocessor » et « DS Memory Control - Cache and TCM ») : identifiants,
// registre de contrôle, TCM, mise en attente. Les registres de l'unité de
// protection et des caches sont gardés mais sans effet : ni protection ni
// cache ne sont encore modélisés.
namespace ravenemu::nds {

class Cp15 {
public:
    static constexpr std::size_t itcm_size = 32 * 1024;
    static constexpr std::size_t dtcm_size = 16 * 1024;

    // Bits du registre de contrôle (C1,C0,0).
    static constexpr std::uint32_t protection_unit = 1U << 0U;
    static constexpr std::uint32_t data_cache = 1U << 2U;
    static constexpr std::uint32_t instruction_cache = 1U << 12U;
    static constexpr std::uint32_t high_vectors = 1U << 13U;
    static constexpr std::uint32_t pre_armv5 = 1U << 15U;
    static constexpr std::uint32_t dtcm_enable = 1U << 16U;
    static constexpr std::uint32_t dtcm_load = 1U << 17U;
    static constexpr std::uint32_t itcm_enable = 1U << 18U;
    static constexpr std::uint32_t itcm_load = 1U << 19U;
    // Sur DS : bits 0, 2, 7 et 12-19 modifiables, 3-6 toujours levés.
    static constexpr std::uint32_t writable = 0x000f'f085U;
    static constexpr std::uint32_t fixed = 0x0000'0078U;

    // Mise sous tension : vecteurs hauts, là où se trouve le BIOS de l'ARM9.
    void reset() noexcept {
        control_ = fixed | high_vectors;
        dtcm_region_ = 0;
        itcm_region_ = 0;
        protection_.fill(0);
        regions_.fill(0);
        update();
    }

    [[nodiscard]] std::optional<std::uint32_t> read(const arm::CoprocessorAccess& access) const noexcept {
        if (access.opcode1 != 0) return 0U;  // auto-test intégré : non modélisé
        switch (access.crn) {
        case 0:
            if (access.crm != 0) return 0U;
            if (access.opcode2 == 1) return 0x0f0d'2112U;  // caches de 8 et 4 Kio
            if (access.opcode2 == 2) return 0x0014'0180U;  // ITCM 32 Kio, DTCM 16 Kio
            return 0x4105'9461U;                           // ARM946E-S, ARMv5TE
        case 1: return control_;
        case 2:
        case 3:
        case 5: return protection_[protection_index(access)];
        case 6: return regions_[access.crm & 7U];
        case 9:
            if (access.crm == 1) return access.opcode2 == 0 ? dtcm_region_ : itcm_region_;
            return 0U;
        default: return 0U;
        }
    }

    // Rend true pour Wait For Interrupt (C7,C0,4 ou C7,C8,2) : l'ARM9 s'arrête.
    bool write(const arm::CoprocessorAccess& access, std::uint32_t value) noexcept {
        if (access.opcode1 != 0) return false;
        switch (access.crn) {
        case 1:
            control_ = (value & writable) | fixed;
            update();
            break;
        case 2:
        case 3:
        case 5: protection_[protection_index(access)] = value; break;
        case 6: regions_[access.crm & 7U] = value; break;
        case 7: return (access.crm == 0 && access.opcode2 == 4) || (access.crm == 8 && access.opcode2 == 2);
        case 9:
            if (access.crm != 1) break;
            if (access.opcode2 == 0) dtcm_region_ = value & 0xffff'f03eU;
            else itcm_region_ = value & 0x0000'003eU;  // base fixe à 0 sur DS
            update();
            break;
        default: break;
        }
        return false;
    }

    [[nodiscard]] std::uint32_t control() const noexcept { return control_; }
    [[nodiscard]] std::uint32_t vector_base() const noexcept { return (control_ & high_vectors) != 0 ? 0xffff'0000U : 0U; }
    [[nodiscard]] bool load_interworking() const noexcept { return (control_ & pre_armv5) == 0; }
    [[nodiscard]] std::uint32_t dtcm_base() const noexcept { return dtcm_base_; }

    // Mémoires des TCM. `itcm_reads`, `dtcm_reads` : TCM visible en lecture
    // (activée, hors mode de chargement) ; les écritures n'exigent que
    // l'activation. L'ITCM l'emporte sur la DTCM ; la DTCM ne sert qu'aux données.
    [[nodiscard]] bool in_itcm(std::uint32_t address) const noexcept { return itcm_on_ && address < itcm_end_; }
    [[nodiscard]] bool in_dtcm(std::uint32_t address) const noexcept {
        return dtcm_on_ && address - dtcm_base_ < dtcm_window_;
    }
    [[nodiscard]] bool itcm_reads() const noexcept { return (control_ & itcm_load) == 0; }
    [[nodiscard]] bool dtcm_reads() const noexcept { return (control_ & dtcm_load) == 0; }
    std::uint8_t* itcm(std::uint32_t address) noexcept { return &itcm_[address & (itcm_size - 1)]; }
    std::uint8_t* dtcm(std::uint32_t address) noexcept { return &dtcm_[(address - dtcm_base_) & (dtcm_size - 1)]; }

private:
    // C2,C0,0..1 ; C3,C0,0 ; C5,C0,0..3.
    static std::size_t protection_index(const arm::CoprocessorAccess& access) noexcept {
        if (access.crn == 2) return access.opcode2 & 1U;
        if (access.crn == 3) return 2;
        return 3 + (access.opcode2 & 3U);
    }
    // Taille virtuelle d'une TCM : 512 << N (bits 1-5), au moins 4 Kio.
    static std::uint32_t virtual_size(std::uint32_t region) noexcept {
        const auto shift = (region >> 1U) & 31U;
        if (shift < 3) return 0x1000U;
        return shift >= 23 ? 0xffff'ffffU : 512U << shift;
    }
    void update() noexcept {
        itcm_on_ = (control_ & itcm_enable) != 0;
        dtcm_on_ = (control_ & dtcm_enable) != 0;
        itcm_end_ = virtual_size(itcm_region_);
        dtcm_window_ = virtual_size(dtcm_region_);
        dtcm_base_ = dtcm_region_ & 0xffff'f000U & ~(dtcm_window_ - 1U);
    }

    std::uint32_t control_{fixed | high_vectors};
    std::uint32_t dtcm_region_{};
    std::uint32_t itcm_region_{};
    std::array<std::uint32_t, 7> protection_{};
    std::array<std::uint32_t, 8> regions_{};
    bool itcm_on_{};
    bool dtcm_on_{};
    std::uint32_t itcm_end_{0x1000};
    std::uint32_t dtcm_window_{0x1000};
    std::uint32_t dtcm_base_{};
    std::vector<std::uint8_t> itcm_ = std::vector<std::uint8_t>(itcm_size);
    std::vector<std::uint8_t> dtcm_ = std::vector<std::uint8_t>(dtcm_size);
};

} // namespace ravenemu::nds
