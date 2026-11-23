#pragma once

#include "interrupts/interrupt_controller.hpp"
#include "machine/scheduler.hpp"

#include <array>
#include <cstdint>

// Balayage de l'écran : 263 lignes de 355 points de 6 cycles, dont 192
// visibles (GBATEK, « DS Display Dimensions / Timings »). Chaque CPU a son
// DISPSTAT : ses IRQ et sa ligne de comparaison. Le drapeau H-Blank se lève
// 1 606 cycles après le début de la ligne sur l'ARM9, 1 613 sur l'ARM7.
namespace ravenemu::nds {

class LcdTiming {
public:
    static constexpr Cycles line_cycles = 355 * 6;
    static constexpr unsigned lines = 263;
    static constexpr unsigned visible_lines = 192;
    static constexpr Cycles frame_cycles = line_cycles * lines;
    static constexpr std::array<Cycles, 2> hblank_start{1606, 1613};  // ARM9, ARM7

    LcdTiming(InterruptController& arm9, InterruptController& arm7) noexcept : irq_{&arm9, &arm7} {}

    // Premier événement : début de la ligne 0 à `now`.
    void start(Scheduler& scheduler, Cycles now) noexcept {
        line_ = lines - 1;
        line_start_ = now;
        phase_ = Phase::line_start;
        scheduler.schedule(Event::lcd, now);
    }

    // Traite l'événement échu et programme le suivant.
    void on_event(Scheduler& scheduler, Cycles now) noexcept {
        switch (phase_) {
        case Phase::line_start:
            begin_line(now);
            phase_ = Phase::hblank9;
            scheduler.schedule(Event::lcd, line_start_ + hblank_start[0]);
            break;
        case Phase::hblank9:
            enter_hblank(0);
            phase_ = Phase::hblank7;
            scheduler.schedule(Event::lcd, line_start_ + hblank_start[1]);
            break;
        case Phase::hblank7:
            enter_hblank(1);
            phase_ = Phase::line_start;
            scheduler.schedule(Event::lcd, line_start_ + line_cycles);
            break;
        }
    }

    [[nodiscard]] unsigned line() const noexcept { return line_; }
    // Nombre de V-Blank commencés : une image affichée chacun.
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

    // Mot 0x04000004 : DISPSTAT (bits 0-15) et VCOUNT (bits 16-24).
    [[nodiscard]] std::uint32_t read(unsigned cpu) const noexcept {
        const auto& own = cpus_[cpu];
        std::uint32_t status = own.control;
        if (line_ >= visible_lines && line_ != lines - 1) status |= 1U;  // pas sur la dernière ligne
        if (own.hblank) status |= 2U;
        if (line_ == own.compare_line) status |= 4U;
        status |= (own.compare_line & 0xffU) << 8U;
        status |= ((own.compare_line >> 8U) & 1U) << 7U;
        return status | (std::uint32_t{line_} << 16U);
    }
    void write(unsigned cpu, std::uint32_t value, std::uint32_t mask) noexcept {
        auto& own = cpus_[cpu];
        if ((mask & 0x38U) != 0) own.control = (own.control & ~(mask & 0x38U)) | (value & mask & 0x38U);
        if ((mask & 0x80U) != 0) own.compare_line = (own.compare_line & 0xffU) | (((value >> 7U) & 1U) << 8U);
        if ((mask & 0xff00U) != 0) own.compare_line = (own.compare_line & 0x100U) | ((value >> 8U) & 0xffU);
        // VCOUNT s'écrit pour synchroniser deux consoles reliées : non modélisé.
    }

private:
    enum class Phase { line_start, hblank9, hblank7 };
    struct Cpu {
        std::uint32_t control{};       // bits 3-5 : IRQ V-Blank, H-Blank, V-Count
        std::uint32_t compare_line{};  // LYC sur 9 bits
        bool hblank{};
    };

    void begin_line(Cycles now) noexcept {
        line_start_ = now;
        line_ = (line_ + 1) % lines;
        if (line_ == visible_lines) ++frames_;
        for (unsigned cpu = 0; cpu < 2; ++cpu) {
            auto& own = cpus_[cpu];
            own.hblank = false;
            if (line_ == visible_lines && (own.control & 0x08U) != 0) irq_[cpu]->request(irq::vblank);
            if (line_ == own.compare_line && (own.control & 0x20U) != 0) irq_[cpu]->request(irq::vcount);
        }
    }
    void enter_hblank(unsigned cpu) noexcept {
        cpus_[cpu].hblank = true;
        if ((cpus_[cpu].control & 0x10U) != 0) irq_[cpu]->request(irq::hblank);
    }

    std::array<InterruptController*, 2> irq_;
    std::array<Cpu, 2> cpus_{};
    unsigned line_{lines - 1};
    Cycles line_start_{};
    Phase phase_{Phase::line_start};
    std::uint64_t frames_{};
};

} // namespace ravenemu::nds
