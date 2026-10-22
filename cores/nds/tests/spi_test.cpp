#include <ravenemu/nds/machine.hpp>
#include <ravenemu/nds/spi.hpp>
#include "check.hpp"

#include <array>
#include <limits>
#include <vector>

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm7 = Processor::arm7;
constexpr auto arm9 = Processor::arm9;
constexpr std::uint32_t control = 0x040001c0, data = control + 2;
constexpr std::uint32_t flags = 0x04000214, extended = 0x04000136;
constexpr std::uint32_t spi_irq = 1U << 23U;

namespace {
InputState contact() {
    InputState input;
    input.pen_down = true;
    input.touch = {0xabc,0x321,0x456,0x789};
    return input;
}

std::uint8_t exchange(Memory& memory, std::uint8_t output, unsigned cycles = 128) {
    memory.write8(arm7,data,output);
    memory.advance_cycles(cycles);
    check(!(memory.peek16(arm7,control) & 0x80U), "transfert non terminé");
    return memory.read8(arm7,data);
}

void converter_test() {
    Touchscreen screen;
    auto input = contact();
    check(screen.pen_irq_enabled(), "PENIRQ initial désactivé");
    const std::array<std::uint8_t,4> commands{0xd0,0x90,0xb0,0xc0};
    const std::array<std::array<std::uint8_t,2>,4> replies{{{0x55,0xe0},{0x19,0x08},{0x22,0xb0},{0x3c,0x48}}};
    for (unsigned channel = 0; channel < commands.size(); ++channel) {
        screen.deselect();
        check(screen.exchange(commands[channel],input) == 0, "commande produit une réponse prématurée");
        check(screen.exchange(0,input) == replies[channel][0] && screen.exchange(0,input) == replies[channel][1] &&
              screen.exchange(0,input) == 0, "canal, bit muet, ordre MSB ou bourrage ADC 12 bits");
    }
    screen.deselect();
    static_cast<void>(screen.exchange(0xd8,input));
    check(screen.exchange(0,input) == 0x55 && screen.exchange(0,input) == 0x80, "ADC 8 bits ou bourrage incorrect");

    screen.deselect();
    static_cast<void>(screen.exchange(0xd0,input));
    check(screen.exchange(0,input) == 0x55 && screen.exchange(0x90,input) == 0xe0 &&
          screen.exchange(0,input) == 0x19 && screen.exchange(0,input) == 8,
          "nouvelle commande écrase la fin de la réponse précédente");
    screen.deselect(); static_cast<void>(screen.exchange(0xd8,input));
    check(screen.exchange(0,input) == 0x55 && screen.exchange(0x98,input) == 0x80 &&
          screen.exchange(0,input) == 0x19 && screen.exchange(0,input) == 0,
          "pipeline ADC 8 bits incorrect");
    // Start dans le dernier bit d'un octet : la commande traverse deux strobes.
    screen.deselect();
    check(screen.exchange(1,input) == 0 && screen.exchange(0xa0,input) == 0 &&
          screen.exchange(0,input) == 0xab && screen.exchange(0,input) == 0xc0,
          "commande série non alignée sur un octet");

    // Les deux modes de référence lisent les canaux tactiles.
    screen.deselect();
    static_cast<void>(screen.exchange(0xd4,input));
    check(screen.exchange(0,input) == 0x55 && screen.exchange(0,input) == 0xe0, "référence single-ended tactile");
    input.pen_down = false;
    for (const auto command : {0xd0,0x90,0xb0,0xc0,0xa4}) {
        screen.deselect();
        static_cast<void>(screen.exchange(static_cast<std::uint8_t>(command),input));
        const bool high = command == 0x90 || command == 0xc0;
        check(screen.exchange(0,input) == (high ? 0x7f : 0) && screen.exchange(0,input) == (high ? 0xf8 : 0),
              "écran relâché ou batterie à GND incorrect");
    }
    input = contact();
    input.touch = {0xffff,0x1000,0,0xfff};
    screen.deselect(); static_cast<void>(screen.exchange(0xd0,input));
    check(screen.exchange(0,input) == 0x7f && screen.exchange(0,input) == 0xf8, "ADC hors douze bits");
    screen.deselect(); static_cast<void>(screen.exchange(0x90,input));
    check(screen.exchange(0,input) == 0 && screen.exchange(0,input) == 0, "ADC nul");
}

void registers_and_timing_test() {
    Memory memory;
    check(memory.read32(arm7,control) == 0, "SPI initial non nul");
    memory.set_input(contact());
    memory.write16(arm7,data,0xffff); // bus désactivé
    memory.advance_cycles(1000);
    check(memory.read32(arm7,control) == 0 && memory.peek32(arm7,flags) == 0, "SPI désactivé transfère");
    memory.write16(arm7,control,0xffff);
    check(memory.read16(arm7,control) == 0xcf03, "masque SPICNT ou busy modifiable");
    memory.write8(arm7,control,1); memory.write8(arm7,control + 1,0x8a);
    check(memory.read16(arm7,control) == 0x8a01, "voies SPICNT");
    memory.write8(arm7,data + 1,0xff);
    check(memory.read16(arm7,data) == 0 && memory.read16(arm7,control) == 0x8a01, "octet haut SPIDATA déclenche");
    for (unsigned baud = 0; baud < 4; ++baud) {
        memory.write16(arm7,control,0);
        memory.write16(arm7,control,static_cast<std::uint16_t>(0x8a00U | baud));
        const auto duration = 64U << baud;
        const auto previous = memory.read16(arm7,data);
        memory.write16(arm7,data,0xffd0); // seul l'octet bas est transmis
        check(memory.read16(arm7,control) == (0x8a80U | baud), "busy absent au départ");
        memory.write8(arm7,data,0x90); // busy : aucun second transfert
        memory.advance_cycles(duration - 1);
        check((memory.read8(arm7,control) & 0x80U) && memory.read16(arm7,data) == previous,
              "SPI termine avant son quota ou expose la sortie");
        memory.advance_cycles(1);
        check(!(memory.read8(arm7,control) & 0x80U) && memory.read32(arm7,flags) == 0, "fin SPI sans IRQ");
        check(exchange(memory,0,duration) == 0x55 && memory.read8(arm7,data + 1) == 0 &&
              memory.read16(arm7,data) == 0x55, "baudrate, réponse ou SPIDATA haut");
        check(exchange(memory,0,duration) == 0xe0, "strobe pendant busy a altéré la commande");
    }
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read32(arm9,control)); }, "SPI ARM9 accessible");
    expect_failure<ravenemu::Error>([&] { memory.write8(arm9,data,0); }, "strobe SPI ARM9 accepté");
    expect_failure<ravenemu::Error>([&] { memory.write16(arm7,data + 1,0); }, "SPI non aligné accepté");
}

void selection_and_power_test() {
    Memory memory;
    memory.set_input(contact());
    memory.write16(arm7,control,0x8201); // CS relâché à chaque octet
    check(exchange(memory,0xd0) == 0 && exchange(memory,0) == 0, "réponse survit sans maintien CS");
    memory.write16(arm7,control,0x8a01);
    static_cast<void>(exchange(memory,0xd0));
    check(exchange(memory,0) == 0x55, "CS maintenu ne conserve pas le résultat");
    memory.write16(arm7,control,0x8201); // dernier octet : doit encore rendre le bas
    check(exchange(memory,0) == 0xe0 && exchange(memory,0) == 0, "hold se termine avant le dernier octet");
    memory.write16(arm7,control,0x8a01); static_cast<void>(exchange(memory,0xd0));
    memory.write16(arm7,control,0x8901); // firmware sélectionné, sans strobe
    memory.write16(arm7,control,0x8a01);
    check(exchange(memory,0) == 0, "changement de périphérique conserve le flux tactile");
    for (unsigned mode = 0; mode < 4; ++mode) {
        memory.write16(arm7,control,0x8a01);
        memory.write8(arm7,data,static_cast<std::uint8_t>(0xd0U | mode));
        const auto before = memory.read16(arm7,extended);
        memory.advance_cycles(127);
        check(memory.read16(arm7,extended) == before, "alimentation TSC appliquée avant la fin de commande");
        memory.advance_cycles(1);
        check((memory.read16(arm7,extended) & 0x40U) == ((mode & 1U) ? 0x40U : 0U), "PENIRQ selon PD0");
        memory.write16(arm7,control,0); // CS haut ne doit pas réinitialiser PD
        check((memory.read16(arm7,extended) & 0x40U) == ((mode & 1U) ? 0x40U : 0U), "désélection efface l'alimentation");
    }
    check(memory.peek32(arm7,flags) == 0 && memory.peek32(arm9,flags) == 0, "IRQ tactile fictive");
    memory.set_input({});
    check(memory.read16(arm7,extended) & 0x40U, "contact absent mais PENIRQ actif");
    auto state = contact(); state.touch = {0xffff,0xf123,0x1000,0xffff}; memory.set_input(state);
    check(memory.input_state().touch == TouchSample{0xfff,0x123,0,0xfff}, "normalisation des ADC hôte");
    memory.set_key(Key::x,true);
    check(memory.input_state().touch == TouchSample{0xfff,0x123,0,0xfff}, "bouton efface les ADC");
    memory.clear_ram(); memory.set_input(contact());
    check(!(memory.read16(arm7,extended) & 0x40U), "reset conserve l'alimentation TSC qui masquait PENIRQ");
}

void interrupts_and_errors_test() {
    Memory memory;
    memory.set_input(contact());
    memory.write16(arm7,control,0xca01);
    memory.write8(arm7,data,0xd0);
    memory.advance_cycles(127);
    check(memory.peek32(arm7,flags) == 0, "IRQ SPI prématurée");
    memory.advance_cycles(1);
    check(memory.peek32(arm7,flags) == spi_irq && memory.peek32(arm9,flags) == 0 && !memory.irq_pending(arm7),
          "IRQ SPI destinataire ou indépendance IME/IE");
    memory.write32(arm7,0x04000210,spi_irq); memory.write32(arm7,0x04000208,1);
    check(memory.irq_pending(arm7), "IRQ SPI mémorisée avant IME/IE perdue");
    memory.write32(arm7,flags,spi_irq);
    memory.advance_cycles(std::numeric_limits<std::uint64_t>::max());
    check(!memory.irq_pending(arm7), "IRQ SPI répétée après acquittement ou grande avance");
    check(exchange(memory,0) == 0x55 && memory.irq_pending(arm7), "IRQ à chaque octet");
    memory.write32(arm7,flags,spi_irq);
    memory.write8(arm7,data,0);
    memory.advance_cycles(64); memory.write16(arm7,control,0); memory.advance_cycles(1000);
    check(!memory.irq_pending(arm7) && memory.read16(arm7,data) == 0x55 && memory.read16(arm7,control) == 0,
          "annulation SPI ne supprime pas l'événement différé");

    // Un accès combinant SPICNT et SPIDATA est validé avant toute mutation.
    for (const auto unsupported : {0x8100U,0x8300U,0x8400U,0x8600U}) {
        expect_failure<ravenemu::Error>([&] { memory.write32(arm7,control,0xd00000U | unsupported); }, "SPI non implémenté accepté");
        check(memory.read32(arm7,control) == 0x00550000, "écriture SPI refusée a changé un registre");
    }
    const std::array<std::uint32_t,2> words{0x00d08a01,0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm7,control,words); }, "trou après SPI accepté");
    check(memory.read32(arm7,control) == 0x00550000, "échec multiple a lancé SPI");
    memory.write32(arm7,control,0x00d0ca01);
    memory.advance_cycles(128);
    check(memory.read16(arm7,control) == 0xca01 && memory.irq_pending(arm7) && exchange(memory,0) == 0x55,
          "STR SPI combiné n'applique pas le nouveau contrôle avant le strobe");

    // L'IRQ réelle entre dans le vecteur ARM7, aucune injection externe.
    Cpu cpu{arm7}; cpu.reset(0x03800000,false); cpu.set_register(0,0x1f);
    memory.write32(arm7,0x03800000,0xe121f000); // msr cpsr_c,r0
    memory.write32(arm7,flags,spi_irq); cpu.step(memory);
    std::vector<std::uint8_t> bios(0x4000);
    const auto op = 0xe3a0402aU; // mov r4,#42
    for (unsigned i = 0; i < 4; ++i) bios[0x18 + i] = static_cast<std::uint8_t>(op >> (i * 8U));
    memory.load_bios(arm7,bios);
    memory.write8(arm7,data,0); memory.advance_cycles(128);
    cpu.step(memory);
    check(cpu.state().mode == CpuMode::irq && cpu.state().registers[15] == 0x18, "IRQ SPI ignorée par ARM7");
    cpu.step(memory);
    check(cpu.state().registers[4] == 42, "vecteur SPI non exécuté");
}

void snapshot_and_copy_test() {
    Memory memory;
    memory.set_input(contact()); memory.write16(arm7,control,0xca01);
    memory.write8(arm7,data,0xd0); memory.advance_cycles(17);
    auto copy = memory;
    memory.set_input({}); copy.set_key(Key::a,true);
    memory.advance_cycles(111);
    check((copy.read16(arm7,control) & 0x80U) && memory.peek32(arm7,flags) == spi_irq &&
          copy.peek32(arm7,flags) == 0, "copie partage le transfert en attente");
    copy.advance_cycles(111);
    check(exchange(memory,0) == 0x55 && exchange(copy,0) == 0x55, "conversion ne conserve pas l'échantillon du départ");
    memory.write16(arm7,control,0x8201); check(exchange(memory,0) == 0xe0, "copie a modifié la réponse source");
    check(exchange(copy,0x90) == 0xe0 && exchange(copy,0) == 0x19, "copie ne conserve pas le pipeline");
    memory.clear_ram();
    check(memory.read32(arm7,control) == 0 && memory.input_state() == InputState{} && memory.read16(arm7,extended) == 0x7f &&
          memory.peek32(arm7,flags) == 0, "clear_ram ne réinitialise pas SPI/TSC/entrée");
    memory.advance_cycles(1000);
    check(memory.peek32(arm7,flags) == 0, "IRQ différée survit au reset");
}

void put32(std::vector<std::uint8_t>& bytes, unsigned offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

std::vector<std::uint8_t> program() {
    std::vector<std::uint32_t> code;
    struct Literal { unsigned instruction, reg; std::uint32_t value; };
    std::vector<Literal> literals;
    const auto load = [&](unsigned reg, std::uint32_t value) {
        literals.push_back({static_cast<unsigned>(code.size()),reg,value}); code.push_back(0);
    };
    const auto poll = [&] { code.insert(code.end(),{0xe1d010b0,0xe3110080,0x1afffffc}); };
    load(0,control); load(1,0xca01); code.push_back(0xe1c010b0); // strh contrôle
    code.insert(code.end(),{0xe3a010d0,0xe5c01002}); poll(); // commande X
    code.insert(code.end(),{0xe3a01000,0xe5c01002}); poll();
    code.push_back(0xe5d02002); // octet haut dans r2
    load(3,0xc201); code.push_back(0xe1c030b0); // libérer CS après le prochain transfert
    code.insert(code.end(),{0xe3a01000,0xe5c01002}); poll();
    code.insert(code.end(),{0xe5d01002,0xe1a02402,0xe1822001,0xe1a021a2});
    load(3,0x02001000); code.insert(code.end(),{0xe5832000,0xeafffffe});
    for (const auto& literal : literals) {
        const auto displacement = static_cast<unsigned>(code.size()) * 4U - literal.instruction * 4U - 8U;
        code[literal.instruction] = 0xe59f0000U | (literal.reg << 12U) | displacement;
        code.push_back(literal.value);
    }
    std::vector<std::uint8_t> bytes(0x204 + code.size() * 4);
    put32(bytes,0x20,0x200); put32(bytes,0x24,0x02000000); put32(bytes,0x28,0x02000000); put32(bytes,0x2c,4);
    put32(bytes,0x30,0x204); put32(bytes,0x34,0x03800000); put32(bytes,0x38,0x03800000);
    put32(bytes,0x3c,static_cast<std::uint32_t>(code.size() * 4));
    put32(bytes,0x200,0xeafffffe);
    for (unsigned i = 0; i < code.size(); ++i) put32(bytes,0x204 + i * 4,code[i]);
    return bytes;
}

void executable_and_reload_test() {
    const auto rom = program();
    Machine machine; machine.load_cartridge(Cartridge{rom}); machine.set_input(contact());
    machine.run_cycles(1000);
    check(machine.memory().read32(arm9,0x02001000) == 0xabc && machine.memory().peek32(arm7,flags) == spi_irq &&
          machine.memory().peek32(arm9,flags) == 0 && machine.scheduler_state().cycles == 1000,
          "programme ARM7 ne publie pas l'ADC via le bus SPI et l'ordonnanceur");
    machine.memory().write16(arm7,control,0xca01); machine.memory().write8(arm7,data,0xd3);
    machine.advance_cycles(29);
    auto copy = machine;
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&] { copy.load_cartridge(Cartridge{invalid}); }, "destination invalide acceptée");
    check(copy.input_state() == machine.input_state() && copy.scheduler_state() == machine.scheduler_state() &&
          (copy.memory().read16(arm7,control) & 0x80U), "chargement refusé perd le transfert ou l'entrée");
    copy.advance_cycles(99);
    check(copy.memory().read16(arm7,extended) & 0x40U, "copie machine ne termine pas la commande d'alimentation");
    check(!(machine.memory().read16(arm7,extended) & 0x40U), "copie machine partage TSC");
    machine.load_cartridge(Cartridge{rom});
    check(machine.memory().read32(arm7,control) == 0 && machine.memory().peek32(arm7,flags) == 0 &&
          machine.input_state() == InputState{} && machine.scheduler_state().cycles == 0,
          "rechargement conserve SPI, ADC, IRQ ou ordonnanceur");
    machine.run_cycles(1000);
    check(machine.memory().read32(arm9,0x02001000) == 0, "programme après reset ne lit pas l'écran relâché");
}
} // namespace

int main() {
    converter_test();
    registers_and_timing_test();
    selection_and_power_test();
    interrupts_and_errors_test();
    snapshot_and_copy_test();
    executable_and_reload_test();
}
