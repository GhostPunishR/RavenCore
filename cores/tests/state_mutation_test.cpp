#include "check.hpp"
#include "synthetic_roms.hpp"

#include <ravenemu/core.hpp>
#include <ravenemu/gb/core.hpp>
#include <ravenemu/gbc/core.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using ravenemu::testing::check;

/**
 * Filet transversal pour les états instantanés, entrée non fiable par nature
 * (fichiers `.state` d'un frontend, ABI C, JNI). Chaque mutation remplace un
 * mot 32 bits par une valeur piège : l'état doit être refusé par
 * `SaveStateError`, ou bien accepté puis exécuté sans incident.
 *
 * Ce test ne remplace pas les tests ciblés de chaque composant : une mutation
 * aveugle atteint rarement l'état précis (mode 3, transfert RTC en cours...)
 * qui rend un champ dangereux. Il garantit en revanche qu'aucune corruption
 * simple ne plante un cœur, et la CI l'exécute sous ASan/UBSan.
 */
namespace {

constexpr std::array<std::uint32_t, 10> traps{
    0x0000'0000U, 0xffff'ffffU, 0x8000'0000U, 0x7fff'ffffU, 0x0001'0000U,
    0x0000'ffffU, 0x0000'0100U, 0x0000'0028U, 0x0000'0008U, 0x4000'0000U,
};

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
    }
}

struct Outcome {
    int rejected{};
    int accepted{};
};

void mutate_and_run(ravenemu::Core& core, const std::vector<std::uint8_t>& valid,
                    std::size_t offset, std::uint32_t trap, Outcome& outcome) {
    auto corrupt = valid;
    put_u32(corrupt, offset, trap);
    try {
        core.load_state(corrupt);
    } catch (const ravenemu::SaveStateError&) {
        ++outcome.rejected;
        return;
    }
    ++outcome.accepted;
    std::vector<std::int32_t> frame(core.video_spec().pixel_count());
    std::array<std::int16_t, 4096> pcm{};
    core.run_frame(frame, true);
    while (core.read_audio(pcm) == pcm.size()) {}
}

void exercise(const char* name, ravenemu::Core& core) {
    std::vector<std::int32_t> frame(core.video_spec().pixel_count());
    for (int i = 0; i < 3; ++i) core.run_frame(frame, true);
    const auto valid = core.save_state();
    core.load_state(valid);
    check(core.save_state() == valid, std::string{name} + " : état valide non restauré à l'identique");

    Outcome outcome;
    // Les champs de contrôle (CPU, timers, PPU, DMA...) précèdent les grandes
    // mémoires : balayage complet du premier Kio, puis tirages déterministes.
    const auto sweep_end = std::min<std::size_t>(1024, valid.size() - 4);
    for (std::size_t offset = 0; offset < sweep_end; offset += 4) {
        mutate_and_run(core, valid, offset, traps[(offset / 4) % traps.size()], outcome);
    }
    std::mt19937 random(0x5241'564eU);
    for (int i = 0; i < 200; ++i) {
        const auto offset = static_cast<std::size_t>(random() % (valid.size() - 3));
        mutate_and_run(core, valid, offset, traps[random() % traps.size()], outcome);
    }
    // Le cœur doit rester utilisable après toutes ces tentatives.
    core.load_state(valid);
    core.run_frame(frame, true);
    check(outcome.rejected > 0, std::string{name} + " : aucune corruption refusée");
}

} // namespace

int main() {
    {
        auto rom = ravenemu::testing::minimal_game_boy_rom();
        rom[0x147] = 0x1b; // MBC5 + RAM + batterie
        rom[0x149] = 0x03;
        auto core = ravenemu::gb::make_core(ravenemu::gb::HardwareModel::dmg);
        core->load_rom(rom, {});
        exercise("GB MBC5", *core);
    }
    {
        auto rom = ravenemu::testing::minimal_game_boy_rom();
        rom[0x143] = 0x80; // CGB
        rom[0x147] = 0x10; // MBC3 + RTC + RAM + batterie
        rom[0x149] = 0x03;
        auto core = ravenemu::gbc::make_core();
        core->set_clock_epoch(1'700'000'000);
        core->load_rom(rom, {});
        exercise("GBC MBC3", *core);
    }
    {
        auto rom = ravenemu::testing::minimal_gba_rom();
        constexpr char flash[] = "FLASH1M_V";
        constexpr char rtc[] = "SIIRTC_V";
        std::memcpy(rom.data() + 0x100, flash, sizeof flash - 1);
        std::memcpy(rom.data() + 0x120, rtc, sizeof rtc - 1);
        auto core = ravenemu::make_gba_core(std::nullopt);
        core->set_clock_epoch(1'700'000'000);
        core->load_rom(rom, {});
        check(core->gba_rtc_active(), "GBA : RTC de test non détecté");
        exercise("GBA Flash+RTC", *core);
    }
    return 0;
}
