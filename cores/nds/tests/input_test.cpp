#include <ravenemu/nds/input.hpp>
#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <array>
#include <initializer_list>
#include <vector>

using namespace ravenemu::nds;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t keys = 0x04000130;
constexpr std::uint32_t control = 0x04000132;
constexpr std::uint32_t extended = 0x04000136;
constexpr std::uint32_t flags = 0x04000214;
constexpr std::uint32_t keypad_irq = 1U << 12U;
constexpr std::uint32_t lid_irq = 1U << 22U;
constexpr std::uint32_t base9 = 0x02000000;
constexpr std::uint32_t base7 = 0x03800000;

namespace {
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
std::vector<std::uint8_t> image(std::initializer_list<std::uint32_t> a9 = {0xeafffffe},
                                std::initializer_list<std::uint32_t> a7 = {0xeafffffe}, bool thumb7 = false) {
    const auto offset7 = 0x200U + a9.size() * 4U;
    std::vector<std::uint8_t> bytes(offset7 + a7.size() * 4U);
    put32(bytes,0x20,0x200); put32(bytes,0x24,base9); put32(bytes,0x28,base9);
    put32(bytes,0x2c,static_cast<std::uint32_t>(a9.size() * 4U));
    put32(bytes,0x30,static_cast<std::uint32_t>(offset7)); put32(bytes,0x34,base7 | (thumb7 ? 1U : 0U));
    put32(bytes,0x38,base7); put32(bytes,0x3c,static_cast<std::uint32_t>(a7.size() * 4U));
    std::size_t offset = 0x200;
    for (const auto op : a9) { put32(bytes,offset,op); offset += 4; }
    for (const auto op : a7) { put32(bytes,offset,op); offset += 4; }
    return bytes;
}
void acknowledge(Memory& memory) {
    memory.write32(arm9,flags,0xffffffff); memory.write32(arm7,flags,0xffffffff);
}
bool requested(const Memory& memory, Processor cpu) { return (memory.peek32(cpu,flags) & keypad_irq) != 0; }

void registers_test() {
    Memory memory;
    check(memory.read32(arm9,keys) == 0x3ff && memory.read32(arm7,keys) == 0x3ff &&
          memory.read16(arm7,extended) == 0x7f && memory.input_state() == InputState{}, "entrées initiales");
    for (unsigned i = 0; i < 12; ++i) {
        memory.set_key(static_cast<Key>(i),true);
        const auto expected = static_cast<std::uint16_t>(i < 10 ? 0x3ffU ^ (1U << i) : 0x3ffU);
        for (const auto cpu : {arm9,arm7})
            check(memory.read16(cpu,keys) == expected && memory.read8(cpu,keys) == (expected & 0xffU) &&
                  memory.read8(cpu,keys + 1) == (expected >> 8U), "bouton GBA ou voies d'octets incorrects");
        check(memory.read16(arm7,extended) == (i >= 10 ? 0x7fU ^ (1U << (i - 10U)) : 0x7fU), "bits X/Y incorrects");
        memory.set_key(static_cast<Key>(i),false);
    }
    memory.set_input({0xffff,true,true,true});
    check(memory.input_state().pressed_keys == 0xfff && memory.read16(arm9,keys) == 0 &&
          memory.read16(arm7,extended) == 0xb4 && memory.read8(arm7,extended + 1) == 0,
          "masque des douze boutons, capot, stylet et DEBUG");
    memory.write16(arm9,keys,0xffff); memory.write16(arm7,extended,0);
    check(memory.read16(arm9,keys) == 0 && memory.read16(arm7,extended) == 0xb4, "écriture sur registre d'entrée en lecture seule");
    memory.write32(arm9,keys,0xffffffff);
    check(memory.read32(arm9,keys) == 0xc3ff0000 && memory.read16(arm7,control) == 0,
          "mot KEYINPUT/KEYCNT ou contrôles indépendants");
    memory.write8(arm7,control,0x25); memory.write8(arm7,control + 1,0xff);
    check(memory.read16(arm7,control) == 0xc325 && memory.read8(arm7,control) == 0x25 &&
          memory.read8(arm7,control + 1) == 0xc3, "voies et bits réservés KEYCNT");
    const auto before = memory.input_state();
    expect_failure<std::invalid_argument>([&] { memory.set_key(static_cast<Key>(255),true); }, "bouton hors DS accepté");
    check(memory.input_state() == before, "bouton invalide a modifié les entrées");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(arm9,extended)); }, "EXTKEYIN accessible ARM9");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(memory.read16(arm7,0x04000134)); }, "RCNT implémenté par accident");
    const std::array<std::uint32_t,2> values{0,0};
    expect_failure<ravenemu::Error>([&] { memory.write_words(arm9,keys,values); }, "transfert vers trou RCNT accepté");
    check(memory.read16(arm9,control) == 0xc3ff, "écriture multiple fautive a modifié KEYCNT");
}

void irq_edges_test() {
    Memory memory;
    memory.write16(arm9,control,0x4001); memory.write16(arm7,control,0x4002);
    memory.set_key(Key::a,true);
    check(requested(memory,arm9) && !requested(memory,arm7) && !memory.irq_pending(arm9), "requêtes propres à chaque CPU, avant IME/IE");
    memory.write32(arm9,0x04000210,keypad_irq); memory.write32(arm9,0x04000208,1);
    check(memory.irq_pending(arm9), "IME/IE n'expose pas une requête clavier déjà présente");
    acknowledge(memory);
    for (unsigned i = 0; i < 3; ++i) {
        memory.set_key(Key::a,true);
        memory.advance_cycles(100);
        static_cast<void>(memory.read16(arm9,keys));
        static_cast<void>(memory.peek32(arm9,keys));
        check(!requested(memory,arm9), "maintien ou lecture répète l'IRQ clavier");
    }
    memory.set_key(Key::b,true);
    check(!requested(memory,arm9) && requested(memory,arm7), "combinaison indépendante ARM7");
    acknowledge(memory); memory.set_key(Key::a,false); memory.set_key(Key::a,true);
    check(requested(memory,arm9) && !requested(memory,arm7), "relâchement ne réarme pas le front clavier");
    acknowledge(memory);
    memory.write16(arm9,control,0x4003); // A ou B, les deux sont déjà pressés
    memory.set_key(Key::x,true);
    check(!requested(memory,arm9) && !requested(memory,arm7), "configuration d'une touche maintenue ou X génère une IRQ");
    memory.set_key(Key::a,false); memory.set_key(Key::b,false);
    memory.set_key(Key::b,true);
    check(requested(memory,arm9) && requested(memory,arm7), "OR réarmé ne déclenche pas");
    acknowledge(memory); memory.set_key(Key::a,true); memory.set_key(Key::b,false);
    check(!requested(memory,arm9), "OR doit déclencher sur la combinaison, pas chaque bouton");
    memory.write16(arm9,control,0);
    memory.set_key(Key::a,false); memory.set_key(Key::a,true);
    check(!requested(memory,arm9), "IRQ clavier désactivée active");
    memory.write16(arm9,control,0x4001);
    memory.set_key(Key::a,true);
    check(!requested(memory,arm9), "activer KEYCNT pendant l'appui génère un front fictif");
    memory.set_key(Key::a,false); memory.set_key(Key::a,true);
    check(requested(memory,arm9), "IRQ non réarmée après activation");
}

void and_and_extended_test() {
    Memory memory;
    memory.write16(arm9,control,0xc003);
    memory.set_key(Key::a,true); check(!requested(memory,arm9), "AND accepte un seul bouton");
    memory.set_key(Key::b,true); check(requested(memory,arm9), "AND n'accepte pas tous les boutons");
    acknowledge(memory); memory.set_key(Key::left,true);
    check(!requested(memory,arm9), "touche non sélectionnée réarme AND");
    memory.set_key(Key::a,false); memory.set_key(Key::a,true);
    check(requested(memory,arm9), "AND ne se réarme pas après relâchement");
    acknowledge(memory);
    for (const auto setting : {0x4000U,0xc000U}) {
        memory.write16(arm9,control,static_cast<std::uint16_t>(setting));
        memory.set_input({}); memory.set_input({0xfff,true,false,true});
        check(!requested(memory,arm9), "sélection vide produit un front clavier");
    }
    memory.write16(arm7,control,0xfc00); // seuls IRQ/AND survivent, X/Y ne sont pas sélectionnables
    memory.set_input({}); memory.set_input({0xc00,true,true,true});
    check(!requested(memory,arm9) && !requested(memory,arm7) && memory.peek32(arm7,flags) == 0,
          "X/Y, DEBUG, stylet ou fermeture déclenche une IRQ non documentée");
    memory.set_input({0xc00,true,false,true});
    check(memory.peek32(arm7,flags) == lid_irq && memory.peek32(arm9,flags) == 0, "ouverture capot ne cible pas ARM7");
    acknowledge(memory); memory.set_input(memory.input_state());
    check(memory.peek32(arm7,flags) == 0, "état ouvert répété génère une IRQ capot");
    memory.set_input({0,true,true,false}); memory.set_input({0,true,false,false});
    check(memory.peek32(arm7,flags) == lid_irq, "capot ne se réarme pas");
    memory.write32(arm7,0x04000210,lid_irq); memory.write32(arm7,0x04000208,1);
    check(memory.irq_pending(arm7), "IRQ capot présente avant IME/IE perdue");
}

void irq_execution_test() {
    for (const auto cpu_id : {arm9,arm7}) {
        Memory memory;
        Cpu cpu{cpu_id}; cpu.reset(base9,false); cpu.set_register(0,0x1f);
        memory.write32(cpu_id,base9,0xe121f000); memory.write32(cpu_id,base9 + 4,0xeafffffe);
        cpu.step(memory);
        const auto status = cpu.state().cpsr();
        std::vector<std::uint8_t> bios(cpu_id == arm9 ? 0x1000 : 0x4000);
        put32(bios,0x18,0xe3a0202a); put32(bios,0x1c,0xe25ef004);
        memory.load_bios(cpu_id,bios);
        memory.write16(cpu_id,control,0x4001);
        memory.write32(cpu_id,0x04000210,keypad_irq); memory.write32(cpu_id,0x04000208,1);
        memory.set_key(Key::a,true); cpu.step(memory);
        check(cpu.state().mode == CpuMode::irq && cpu.state().registers[15] == (cpu_id == arm9 ? 0xffff0018U : 0x18U) &&
              cpu.state().registers[14] == base9 + 8 && cpu.state().spsr() == status && cpu.state().instructions == 1,
              "entrée IRQ clavier / vecteur / SPSR");
        memory.write32(cpu_id,flags,keypad_irq); cpu.step(memory); cpu.step(memory);
        check(cpu.state().mode == CpuMode::system && cpu.state().registers[15] == base9 + 4 &&
              cpu.state().registers[2] == 42 && !requested(memory,cpu_id), "retour IRQ clavier avec touche maintenue");
    }
    Machine machine;
    machine.load_cartridge(Cartridge{image({0xee070f90,0xe3a0002a,0xeafffffe})});
    machine.memory().write16(arm9,control,0x4001);
    machine.memory().write32(arm9,0x04000210,keypad_irq); machine.memory().write32(arm9,0x04000208,1);
    machine.run_cycles(1);
    check(machine.cpu_state(arm9).waiting_for_interrupt, "programme n'attend pas WFI");
    machine.set_key(Key::a,true); machine.run_cycles(1);
    check(!machine.cpu_state(arm9).waiting_for_interrupt && machine.cpu_state(arm9).registers[0] == 42 &&
          machine.cpu_state(arm9).instructions == 2 && machine.cpu_state(arm7).instructions == 2,
          "bouton ne réveille pas WFI dans l'ordonnanceur");
}

void tcm_and_mpu_test() {
    Memory memory;
    memory.cp15().write(9,1,1,0x2e); memory.cp15().write(1,0,0,0x42078); // ITCM sur 4 Gio
    memory.write32(arm9,keys,0x40010055); // TCM seulement, pas KEYCNT
    memory.set_key(Key::a,true);
    check(memory.read32(arm9,keys) == 0x40010055 && memory.read16(arm7,keys) == 0x3fe &&
          memory.read16(arm7,control) == 0 && !requested(memory,arm9), "TCM et périphérique d'entrée confondus");
    memory.cp15().write(1,0,0,0x2078);
    check(memory.read16(arm9,control) == 0, "écriture masquée par TCM a modifié KEYCNT");
    memory.write32(arm9,base9,0xe1d010b0);
    memory.cp15().write(6,0,0,base9 | 0x17U); memory.cp15().write(5,0,3,3);
    memory.cp15().write(1,0,0,0x2079); // code exécutable, données interdites
    Cpu cpu{arm9}; cpu.reset(base9,false); cpu.set_register(0,keys); cpu.step(memory);
    check(cpu.state().mode == CpuMode::abort && cpu.state().registers[15] == 0xffff0010 &&
          memory.read16(arm9,keys) == 0x3fe, "lecture clavier contourne MPU, ou inspection hôte bloquée");
}

void interactive_program_test() {
    // ARM9 lit A et colore le backdrop ; ARM7 Thumb publie KEYINPUT en RAM.
    const auto rom = image({0xe59f0018,0xe59f1018,0xe1d020b0,0xe3120001,0x03a0301f,
        0x13a03c7c,0xe1c130b0,0xeafffff9,keys,0x05000000},
        {0x49034802,0x800a8802,0x46c0e7fc,keys,0x02001000},true);
    Machine machine; machine.load_cartridge(Cartridge{rom});
    machine.memory().write16(arm9,0x04000304,0x8003);
    machine.memory().write32(arm9,0x04000000,0x10000);
    machine.run_cycles(32);
    check(machine.render_frame().upper[0] == 0x7c00 && machine.memory().read16(arm9,0x02001000) == 0x3ff,
          "programme interactif : état relâché");
    machine.set_key(Key::a,true); machine.run_cycles(16);
    check(machine.render_frame().upper[0] == 0x1f && machine.memory().read16(arm9,0x02001000) == 0x3fe,
          "appui A ne change pas la couleur via le CPU ou ARM7 ne lit pas l'appui");
    machine.set_key(Key::a,false); machine.set_key(Key::x,true); machine.run_cycles(16);
    check(machine.render_frame().upper[0] == 0x7c00 && machine.memory().read16(arm9,0x02001000) == 0x3ff &&
          machine.memory().read16(arm7,extended) == 0x7e, "relâchement ou séparation X/KEYINPUT");
}

void copy_and_reset_test() {
    const auto rom = image();
    Machine machine; machine.load_cartridge(Cartridge{rom});
    machine.memory().write16(arm9,control,0xc003);
    machine.set_input({1,true,true,true}); machine.run_cycles(3);
    auto copy = machine;
    copy.set_key(Key::b,true);
    auto opened = copy.input_state(); opened.lid_closed = false; copy.set_input(opened);
    check(!requested(machine.memory(),arm9) && requested(copy.memory(),arm9) && machine.input_state().pressed_keys == 1 &&
          copy.input_state().pressed_keys == 3 && machine.input_state().lid_closed && !copy.input_state().lid_closed &&
          machine.memory().peek32(arm7,flags) == 0 && copy.memory().peek32(arm7,flags) == lid_irq,
          "copie partage entrées, historique de combinaison ou IRQ");
    auto invalid = rom; put32(invalid,0x28,0x04000000);
    const auto state = copy.input_state(); const auto clock = copy.scheduler_state();
    expect_failure<ravenemu::RomLoadError>([&] { copy.load_cartridge(Cartridge{invalid}); }, "ROM invalide acceptée");
    check(copy.input_state() == state && copy.scheduler_state() == clock && copy.memory().read16(arm9,control) == 0xc003 &&
          requested(copy.memory(),arm9), "échec chargement efface entrées ou IRQ");
    copy.load_cartridge(Cartridge{rom});
    check(copy.input_state() == InputState{} && copy.memory().read16(arm9,keys) == 0x3ff &&
          copy.memory().read16(arm7,extended) == 0x7f && copy.memory().read16(arm9,control) == 0 &&
          copy.memory().peek32(arm9,flags) == 0 && copy.memory().peek32(arm7,flags) == 0,
          "rechargement ne remet pas les entrées à zéro ou produit une fausse ouverture");
    machine.memory().clear_ram();
    check(machine.input_state() == InputState{} && machine.memory().read16(arm9,control) == 0 &&
          machine.memory().peek32(arm7,flags) == 0, "clear_ram conserve état/IRQ clavier");
}
} // namespace

int main() {
    registers_test();
    irq_edges_test();
    and_and_extended_test();
    irq_execution_test();
    tcm_and_mpu_test();
    interactive_program_test();
    copy_and_reset_test();
}
