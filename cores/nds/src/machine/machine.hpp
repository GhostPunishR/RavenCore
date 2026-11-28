#pragma once

#include "arm/cpu.hpp"
#include "bios/bios.hpp"
#include "cartridge/header.hpp"
#include "machine/hardware.hpp"
#include "memory/bus7.hpp"
#include "memory/bus9.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Le DS : deux processeurs et leur matériel. Le temps avance par tranches,
// jusqu'au prochain événement et d'au plus `max_slice` cycles système :
// l'ARM9 parcourt la tranche, puis l'ARM7, puis les événements échus
// s'exécutent. Une écriture de l'un (IPC, WRAM, IF de l'autre) peut donc
// être vue par l'autre avec au plus une tranche d'écart ; la longueur des
// tranches se règlera sur mesure.
namespace ravenemu::nds {

using Arm9 = arm::Cpu<arm::Architecture::armv5te, Bus9>;
using Arm7 = arm::Cpu<arm::Architecture::armv4t, Bus7>;

class Machine {
public:
    static constexpr Cycles max_slice = 64;

    // Démarrage direct de la ROM, sans BIOS ni firmware de Nintendo : l'état
    // que ceux-ci laissent est reconstitué (direct_boot.cpp).
    explicit Machine(std::vector<std::uint8_t> rom);

    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    void run_for(Cycles cycles) { run_until(hw.now + cycles); }
    void run_until(Cycles target);
    // Jusqu'au début du V-Blank suivant.
    void run_frame();
    // Écran tactile : point touché (pixels de l'écran du bas), ou relâché ;
    // EXTKEYIN bit 6 suit le crayon.
    void set_touch(bool pressed, unsigned x = 0, unsigned y = 0) noexcept;
    // Son produit depuis le dernier retrait, en stéréo (gauche, droite) à
    // 32 728,5 Hz (1024 périodes système) : l'hôte le lit puis le vide.
    [[nodiscard]] std::vector<std::int16_t>& audio() noexcept { return hw.sound.output(); }
    // Date et heure de l'horloge temps réel (2000 à 2099), qui avancent
    // ensuite avec le temps émulé.
    void set_date_time(const DateTime& date_time) noexcept { hw.rtc.set_date_time(date_time, hw.now); }
    // Les deux écrans de la dernière image, en ARGB8888 : 256 × 192 en haut,
    // puis 256 × 192 en bas.
    [[nodiscard]] const std::vector<std::uint32_t>& frame() const noexcept { return hw.frame; }

    Hardware hw;
    Bus9 bus9{hw};
    Bus7 bus7{hw};
    Arm9 arm9{bus9};
    Arm7 arm7{bus7};
    Bios<Arm9, Bus9, true> bios9{hw, arm9, bus9};
    Bios<Arm7, Bus7, false> bios7{hw, arm7, bus7};
    CartridgeHeader header;

private:
    void direct_boot();
    void boot_memory();
    void boot_processors();
    void run_arm9(Cycles end);
    void run_arm7(Cycles end);
    void settle(Cycles now);
    void render_line(unsigned line);
    void arm_main_memory_display() noexcept;
    void feed_main_memory_display();
    void on_lcd_event(LcdTiming::Phase phase);
    void on_card_event();
    void sync_cp15() noexcept;
};

} // namespace ravenemu::nds
