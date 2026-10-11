#include "cpu/cpu.hpp"
#include "check.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using ravenemu::testing::check;

namespace ravenemu::cgb::testing {

struct TraceEvent {
    enum class Kind { tick, halted_tick, read, write } kind{};
    int mcycle{};
    int address{-1};
    int value{-1};
};

class TraceBus final : public Bus {
public:
    [[nodiscard]] int read(int address) override {
        const int normalized = word(address);
        int value = memory[static_cast<std::size_t>(normalized)];
        if (mapped_interrupts != nullptr && normalized == 0xff0f) value = mapped_interrupts->read_flags();
        if (mapped_interrupts != nullptr && normalized == 0xffff) value = mapped_interrupts->read_enable();
        events.push_back({TraceEvent::Kind::read, mcycle, normalized, value});
        return value;
    }

    void write(int address, int value) override {
        const int normalized = word(address);
        memory[static_cast<std::size_t>(normalized)] = static_cast<std::uint8_t>(byte(value));
        if (mapped_interrupts != nullptr && normalized == 0xff0f) mapped_interrupts->write_flags(value);
        if (mapped_interrupts != nullptr && normalized == 0xffff) mapped_interrupts->write_enable(value);
        events.push_back({TraceEvent::Kind::write, mcycle, normalized, byte(value)});
    }

    void tick_mcycle() override {
        ++mcycle;
        events.push_back({TraceEvent::Kind::tick, mcycle});
        if (interrupt_on_mcycle == mcycle && mapped_interrupts != nullptr) {
            mapped_interrupts->flags |= interrupt_to_raise;
        }
    }

    void tick_halted_mcycle() override {
        ++mcycle;
        events.push_back({TraceEvent::Kind::halted_tick, mcycle});
    }

    bool on_stop() override { return speed_switch_on_stop; }
    bool stop_wake_requested() override {
        const bool result = stop_wake;
        stop_wake = false;
        return result;
    }

    void program(std::initializer_list<int> bytes, int address = 0x0100) {
        for (const int value : bytes) memory[static_cast<std::size_t>(address++)] = static_cast<std::uint8_t>(value);
    }

    [[nodiscard]] const TraceEvent& find(TraceEvent::Kind kind, int address) const {
        for (const auto& event : events) {
            if (event.kind == kind && event.address == address) return event;
        }
        throw std::runtime_error("événement de bus attendu absent");
    }

    std::array<std::uint8_t, 0x10000> memory{};
    std::vector<TraceEvent> events;
    int mcycle{};
    bool speed_switch_on_stop{};
    bool stop_wake{};
    InterruptController* mapped_interrupts{};
    int interrupt_on_mcycle{-1};
    int interrupt_to_raise{};
};

void memory_access_schedule_test() {
    TraceBus bus;
    InterruptController interrupts;
    Cpu cpu(bus, interrupts);
    cpu.sp = 0x1234;
    bus.program({0x08, 0x00, 0xc0}); // LD [C000],SP

    check(cpu.step() == 20, "LD [imm16],SP n'a pas duré cinq M-cycles");
    check(bus.find(TraceEvent::Kind::read, 0x0100).mcycle == 1, "opcode lu hors du premier M-cycle");
    check(bus.find(TraceEvent::Kind::read, 0x0101).mcycle == 2, "octet immédiat bas lu au mauvais M-cycle");
    check(bus.find(TraceEvent::Kind::read, 0x0102).mcycle == 3, "octet immédiat haut lu au mauvais M-cycle");
    check(bus.find(TraceEvent::Kind::write, 0xc000).mcycle == 4, "SP bas écrit au mauvais M-cycle");
    check(bus.find(TraceEvent::Kind::write, 0xc001).mcycle == 5, "SP haut écrit au mauvais M-cycle");
    check(bus.memory[0xc000] == 0x34 && bus.memory[0xc001] == 0x12, "ordre little-endian de LD [imm16],SP incorrect");
}

void call_and_interrupt_schedule_test() {
    {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        cpu.sp = 0xfffe;
        bus.program({0xcd, 0x34, 0x12}); // CALL 1234

        check(cpu.step() == 24, "CALL n'a pas duré six M-cycles");
        check(bus.find(TraceEvent::Kind::write, 0xfffd).mcycle == 5, "CALL a empilé PC haut avant le cinquième M-cycle");
        check(bus.find(TraceEvent::Kind::write, 0xfffc).mcycle == 6, "CALL a empilé PC bas avant le sixième M-cycle");
        check(cpu.pc == 0x1234, "CALL n'a pas chargé sa cible");
    }

    {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        cpu.pc = 0x4567;
        cpu.sp = 0xfffe;
        cpu.ime = true;
        interrupts.enable = interrupt_mask(Interrupt::timer);
        interrupts.flags = interrupt_mask(Interrupt::timer);

        check(cpu.step() == 20, "service d'interruption différent de cinq M-cycles");
        check(bus.find(TraceEvent::Kind::write, 0xfffd).mcycle == 3, "interruption sans ses deux M-cycles d'attente");
        check(bus.find(TraceEvent::Kind::write, 0xfffc).mcycle == 4, "PC bas d'interruption écrit au mauvais M-cycle");
        check(cpu.pc == 0x50, "priorité/vecteur de l'interruption timer incorrect");
        check(!cpu.ime && (interrupts.flags & interrupt_mask(Interrupt::timer)) == 0,
              "IME ou IF non acquitté pendant le service d'interruption");
    }
}

void interrupt_dispatch_sampling_test() {
    {
        TraceBus bus;
        InterruptController interrupts;
        bus.mapped_interrupts = &interrupts;
        Cpu cpu(bus, interrupts);
        cpu.pc = 0x0200;
        cpu.sp = 0x0000;
        cpu.ime = true;
        interrupts.enable = interrupt_mask(Interrupt::timer);
        interrupts.flags = interrupt_mask(Interrupt::timer);

        check(cpu.step() == 20 && cpu.pc == 0x0000,
              "écriture haute de PC dans IE n'a pas annulé le dispatch IRQ");
        check((interrupts.flags & interrupt_mask(Interrupt::timer)) != 0 && !cpu.ime,
              "annulation du dispatch a acquitté IF ou conservé IME");
    }

    {
        TraceBus bus;
        InterruptController interrupts;
        bus.mapped_interrupts = &interrupts;
        Cpu cpu(bus, interrupts);
        cpu.pc = 0x0200; // octet haut $02 : IE ne garde que STAT
        cpu.sp = 0x0000;
        cpu.ime = true;
        interrupts.enable = interrupt_mask(Interrupt::vblank) | interrupt_mask(Interrupt::stat);
        interrupts.flags = interrupts.enable;

        check(cpu.step() == 20 && cpu.pc == 0x0048,
              "écriture haute de PC dans IE n'a pas repriorisé vers STAT");
        check((interrupts.flags & 0x03) == interrupt_mask(Interrupt::vblank),
              "repriorisation IRQ n'a pas acquitté uniquement STAT");
    }

    {
        TraceBus bus;
        InterruptController interrupts;
        bus.mapped_interrupts = &interrupts;
        Cpu cpu(bus, interrupts);
        cpu.pc = 0x1235; // octet bas $35 écrit dans IE au second push
        cpu.sp = 0x0001;
        cpu.ime = true;
        interrupts.enable = interrupt_mask(Interrupt::serial);
        interrupts.flags = interrupt_mask(Interrupt::serial);

        check(cpu.step() == 20 && cpu.pc == 0x0058,
              "écriture basse de PC dans IE a annulé trop tard le dispatch série");
        check((interrupts.flags & interrupt_mask(Interrupt::serial)) == 0,
              "IRQ série sélectionnée avant le push bas n'a pas été acquittée");
    }
}

void interrupt_raised_during_fetch_aborts_opcode_before_full_dispatch_test() {
    TraceBus bus;
    InterruptController interrupts;
    bus.mapped_interrupts = &interrupts;
    bus.interrupt_on_mcycle = 1;
    bus.interrupt_to_raise = interrupt_mask(Interrupt::timer);
    Cpu cpu(bus, interrupts);
    cpu.b = 0;
    cpu.ime = true;
    interrupts.enable = interrupt_mask(Interrupt::timer);
    interrupts.flags = 0;
    bus.program({0x04}); // INC B est annulé par l'IRQ levée pendant son fetch.

    check(cpu.step() == 24 && bus.mcycle == 6,
          "IRQ levée pendant le fetch a réutilisé ce cycle dans le dispatch");
    check(cpu.b == 0 && cpu.pc == 0x0050,
          "opcode exécuté malgré une IRQ échantillonnée pendant son fetch");
    check(bus.memory[0xfffd] == 0x01 && bus.memory[0xfffc] == 0x00,
          "IRQ au fetch n'a pas empilé l'adresse de l'opcode annulé ($0100)");
    check(bus.find(TraceEvent::Kind::write, 0xfffd).mcycle == 4 &&
          bus.find(TraceEvent::Kind::write, 0xfffc).mcycle == 5,
          "séquence de push IRQ décalée après la frontière d'instruction");
}

void di_wins_over_interrupt_raised_during_fetch_test() {
    TraceBus bus;
    InterruptController interrupts;
    bus.mapped_interrupts = &interrupts;
    bus.interrupt_on_mcycle = 1;
    bus.interrupt_to_raise = interrupt_mask(Interrupt::vblank);
    Cpu cpu(bus, interrupts);
    cpu.ime = true;
    interrupts.enable = interrupt_mask(Interrupt::vblank);
    interrupts.flags = 0;
    bus.program({0xf3});

    check(cpu.step() == 4 && cpu.pc == 0x0101 && !cpu.ime,
          "DI n'a pas gagné contre une IRQ levée pendant son fetch");
    check((interrupts.flags & interrupt_mask(Interrupt::vblank)) != 0,
          "DI a acquitté la requête qu'il devait seulement masquer");
}

void cgb_interrupt_wins_over_di_fetch_test() {
    TraceBus bus;
    InterruptController interrupts;
    bus.mapped_interrupts = &interrupts;
    bus.interrupt_on_mcycle = 1;
    bus.interrupt_to_raise = interrupt_mask(Interrupt::vblank);
    Cpu cpu(bus, interrupts, true);
    cpu.ime = true;
    interrupts.enable = interrupt_mask(Interrupt::vblank);
    interrupts.flags = 0;
    bus.program({0xf3});

    check(cpu.step() == 24 && cpu.pc == 0x0040 && !cpu.ime,
          "CGB a laissé DI annuler une IRQ déjà échantillonnée pendant son fetch");
    check((interrupts.flags & interrupt_mask(Interrupt::vblank)) == 0 &&
          bus.memory[0xfffc] == 0x00,
          "course DI/IRQ CGB non acquittée ou mauvaise adresse de retour");
}

void ei_di_delay_test() {
    {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        bus.program({0xfb, 0xf3, 0x00}); // EI ; DI ; NOP
        interrupts.enable = interrupt_mask(Interrupt::vblank);
        interrupts.flags = interrupt_mask(Interrupt::vblank);

        check(cpu.step() == 4 && !cpu.ime, "EI a activé IME sans délai");
        check(cpu.step() == 4 && !cpu.ime, "DI n'a pas annulé le EI différé");
        check(cpu.step() == 4 && cpu.pc == 0x0103, "interruption servie malgré EI immédiatement suivi de DI");
    }

    {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        bus.program({0xfb, 0x00}); // EI ; NOP
        interrupts.enable = interrupt_mask(Interrupt::vblank);
        interrupts.flags = interrupt_mask(Interrupt::vblank);

        static_cast<void>(cpu.step());
        check(cpu.step() == 4 && cpu.ime, "IME n'a pas été activé après l'instruction suivant EI");
        check(cpu.step() == 20 && cpu.pc == 0x40, "interruption non servie après le délai exact de EI");
    }
}

void halt_bug_and_halt_clock_test() {
    {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        bus.program({0xfb, 0x76, 0x00}); // EI ; HALT ; NOP
        bus.memory[0x0040] = 0xd9;       // RETI
        interrupts.enable = interrupt_mask(Interrupt::vblank);
        interrupts.flags = interrupt_mask(Interrupt::vblank);

        check(cpu.step() == 4 && !cpu.ime, "EI a activé IME avant HALT");
        check(cpu.step() == 4 && cpu.ime && cpu.halt_bug,
              "EI ; HALT n'a pas exposé le HALT bug avant le service IRQ");
        check(cpu.step() == 20 && cpu.pc == 0x0040,
              "EI ; HALT n'a pas servi l'interruption au cycle suivant");
        check(!cpu.halt_bug && bus.memory[0xfffc] == 0x01 && bus.memory[0xfffd] == 0x01,
              "EI ; HALT n'a pas empilé l'adresse du HALT ($0101)");
        check(cpu.step() == 16 && cpu.pc == 0x0101,
              "RETI après EI ; HALT n'est pas revenu sur le HALT");
        check(cpu.step() == 4 && cpu.halted,
              "HALT réexécuté après RETI n'a pas attendu la prochaine interruption");
    }

    {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        cpu.b = 0;
        bus.program({0x76, 0x04}); // HALT ; INC B
        interrupts.enable = interrupt_mask(Interrupt::vblank);
        interrupts.flags = interrupt_mask(Interrupt::vblank);

        static_cast<void>(cpu.step());
        check(cpu.halt_bug && !cpu.halted, "HALT bug non armé avec IME=0 et IRQ en attente");
        static_cast<void>(cpu.step());
        check(cpu.b == 1 && cpu.pc == 0x0101, "HALT bug n'a pas supprimé l'incrément de PC");
        static_cast<void>(cpu.step());
        check(cpu.b == 2 && cpu.pc == 0x0102, "opcode suivant le HALT bug non relu normalement");
    }

    {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        bus.program({0x76});
        static_cast<void>(cpu.step());
        check(cpu.halted, "HALT normal non conservé");
        check(cpu.step() == 4, "HALT n'a pas consommé un M-cycle d'horloge");
        check(!bus.events.empty() && bus.events.back().kind == TraceEvent::Kind::halted_tick,
              "HALT n'a pas utilisé le domaine d'horloge suspendant le HDMA");
    }
}

void stop_padding_timing_test() {
    TraceBus bus;
    InterruptController interrupts;
    Cpu cpu(bus, interrupts);
    bus.program({0x10, 0x00});

    check(cpu.step() == 4, "STOP avec octet de remplissage ne doit durer qu'un M-cycle");
    check(cpu.pc == 0x0102 && cpu.stopped, "STOP n'a pas consommé son second octet ou ne s'est pas arrêté");
    check(bus.mcycle == 1, "STOP a ajouté un M-cycle pour son octet de remplissage");
}

int expected_base_mcycles(int opcode, bool condition_taken) {
    const int x = opcode >> 6;
    const int y = (opcode >> 3) & 7;
    const int z = opcode & 7;
    const int q = y & 1;

    if (x == 0) {
        switch (z) {
        case 0:
            if (y == 0 || y == 2) return 1;
            if (y == 1) return 5;
            if (y == 3) return 3;
            return condition_taken ? 3 : 2;
        case 1: return q == 0 ? 3 : 2;
        case 2: return 2;
        case 3: return 2;
        case 4: case 5: return y == 6 ? 3 : 1;
        case 6: return y == 6 ? 3 : 2;
        default: return 1;
        }
    }
    if (x == 1) {
        if (opcode == 0x76) return 1;
        return y == 6 || z == 6 ? 2 : 1;
    }
    if (x == 2) return z == 6 ? 2 : 1;

    switch (z) {
    case 0:
        if (y < 4) return condition_taken ? 5 : 2;
        return std::array{3, 4, 3, 3}[static_cast<std::size_t>(y - 4)];
    case 1:
        if (q == 0) return 3;
        return std::array{4, 4, 1, 2}[static_cast<std::size_t>(y >> 1)];
    case 2:
        if (y < 4) return condition_taken ? 4 : 3;
        return std::array{2, 4, 2, 4}[static_cast<std::size_t>(y - 4)];
    case 3:
        if (y == 0) return 4;
        if (y == 1) return 2; // CB suivi ici de RLC B.
        return 1;
    case 4: return y < 4 ? (condition_taken ? 6 : 3) : 1;
    case 5:
        if (q == 0) return 4;
        return y == 1 ? 6 : 1;
    case 6: return 2;
    default: return 4;
    }
}

void set_branch_condition(Cpu& cpu, int opcode, bool taken) {
    int condition = -1;
    if ((opcode & 0xe7) == 0x20) condition = (opcode >> 3) & 3; // JR cc
    if ((opcode & 0xe7) == 0xc0 || (opcode & 0xe7) == 0xc2 ||
        (opcode & 0xe7) == 0xc4) {
        condition = (opcode >> 3) & 3; // RET/JP/CALL cc
    }
    if (condition < 0) return;
    const bool tests_set_flag = condition == 1 || condition == 3;
    const bool flag_set = taken == tests_set_flag;
    cpu.f = flag_set ? (condition >= 2 ? 0x10 : 0x80) : 0;
}

[[noreturn]] void timing_failure(std::string_view family, int opcode,
                                 int expected, int actual) {
    std::ostringstream message;
    message << "timing " << family << " incorrect pour opcode $"
            << std::hex << std::setw(2) << std::setfill('0') << opcode
            << " : attendu " << std::dec << expected << " M-cycles, obtenu " << actual;
    throw std::runtime_error(message.str());
}

void exhaustive_official_timing_test() {
    for (const bool taken : {false, true}) {
        for (int opcode = 0; opcode <= 0xff; ++opcode) {
            TraceBus bus;
            InterruptController interrupts;
            Cpu cpu(bus, interrupts);
            cpu.sp = 0x9000;
            cpu.set_hl(0x8000);
            set_branch_condition(cpu, opcode, taken);
            bus.memory[0x0100] = static_cast<std::uint8_t>(opcode);
            bus.memory[0x0101] = 0; // immédiat et RLC B pour le préfixe CB
            const int actual = cpu.step() / 4;
            const int expected = expected_base_mcycles(opcode, taken);
            if (actual != expected || bus.mcycle != expected) {
                timing_failure("base", opcode, expected, actual);
            }
        }
    }

    for (int opcode = 0; opcode <= 0xff; ++opcode) {
        TraceBus bus;
        InterruptController interrupts;
        Cpu cpu(bus, interrupts);
        cpu.set_hl(0x8000);
        bus.memory[0x0100] = 0xcb;
        bus.memory[0x0101] = static_cast<std::uint8_t>(opcode);
        const bool memory_operand = (opcode & 7) == 6;
        const bool bit_test = (opcode >> 6) == 1;
        const int expected = memory_operand ? (bit_test ? 3 : 4) : 2;
        const int actual = cpu.step() / 4;
        if (actual != expected || bus.mcycle != expected) {
            timing_failure("CB", opcode, expected, actual);
        }
    }
}

} // namespace ravenemu::cgb::testing

int main() {
    using namespace ravenemu::cgb::testing;
    memory_access_schedule_test();
    call_and_interrupt_schedule_test();
    interrupt_dispatch_sampling_test();
    interrupt_raised_during_fetch_aborts_opcode_before_full_dispatch_test();
    di_wins_over_interrupt_raised_during_fetch_test();
    cgb_interrupt_wins_over_di_fetch_test();
    ei_di_delay_test();
    halt_bug_and_halt_clock_test();
    stop_padding_timing_test();
    exhaustive_official_timing_test();
    return 0;
}
