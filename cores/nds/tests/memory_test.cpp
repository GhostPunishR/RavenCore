#include <ravenemu/nds/memory.hpp>
#include <ravenemu/core.hpp>
#include "check.hpp"

using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;

namespace {

void main_ram_test() {
    Memory ram;
    check(ram.read32(arm9, 0x02000000) == 0, "RAM initiale non nulle");
    ram.write32(arm9, 0x02000000, 0x12345678);
    check(ram.read8(arm7, 0x02000000) == 0x78, "ordre little-endian / partage ARM7");
    check(ram.read16(arm7, 0x02000002) == 0x1234, "lecture demi-mot");
    for (const auto mirror : {0x02000000U, 0x02400000U, 0x02800000U, 0x02c00000U})
        check(ram.read32(arm9, mirror) == 0x12345678, "miroir RAM principale");
    ram.write16(arm7, 0x02fffffe, 0xabcd);
    check(ram.read16(arm9, 0x023ffffe) == 0xabcd, "fin de RAM principale");
    ram.write8(arm9, 0x02000001, 0xaa);
    check(ram.read32(arm7, 0x02400000) == 0x1234aa78, "écriture octet");
}

void wram_test() {
    Memory ram;
    ram.write8(arm9, 0x03000000, 0x11);
    ram.write8(arm9, 0x03004000, 0x22);
    check(ram.read8(arm9, 0x03ff8000) == 0x11, "miroir WRAM ARM9 mode 0");
    ram.write8(arm7, 0x03800000, 0x33);
    check(ram.read8(arm7, 0x03000000) == 0x33, "WRAM privée ARM7 mode 0");
    check(ram.read8(arm7, 0x03ff0000) == 0x33, "miroir WRAM privée ARM7");
    ram.set_wram_control(1);
    check(ram.read8(arm9, 0x03000000) == 0x22, "mode 1 ARM9 moitié haute");
    check(ram.read8(arm7, 0x03000000) == 0x11, "mode 1 ARM7 moitié basse");
    check(ram.read8(arm9, 0x03004000) == 0x22, "miroir 16 Kio ARM9");
    check(ram.read8(arm7, 0x037fc000) == 0x11, "miroir 16 Kio ARM7");
    ram.write8(arm7, 0x03000001, 0x44);
    ram.set_wram_control(2);
    check(ram.read8(arm9, 0x03000000) == 0x11, "mode 2 ARM9 moitié basse");
    check(ram.read8(arm7, 0x03000000) == 0x22, "mode 2 ARM7 moitié haute");
    check(ram.read8(arm9, 0x03000001) == 0x44, "données préservées après réattribution");
    ram.set_wram_control(0xff);
    check(ram.wram_control() == 3, "masque WRAMCNT");
    check(ram.read8(arm7, 0x03000000) == 0x11, "mode 3 moitié basse");
    check(ram.read8(arm7, 0x03004000) == 0x22, "mode 3 moitié haute");
    check(ram.read8(arm7, 0x03800000) == 0x33, "WRAM privée indépendante");
    expect_failure<ravenemu::Error>([&] { ram.write8(arm9, 0x03000000, 1); }, "ARM9 mode 3");
    ram.set_wram_control(0);
    check(ram.read8(arm9, 0x03000000) == 0x11, "contenu après les quatre modes");
}

void rejected_access_test() {
    Memory ram;
    for (const auto address : {0U, 0x01ffffffU, 0x04000000U, 0x04000248U, 0xffffffffU}) {
        expect_failure<ravenemu::Error>([&] { static_cast<void>(ram.read8(arm9, address)); },
                                       "lecture hors RAM acceptée");
        expect_failure<ravenemu::Error>([&] { ram.write8(arm7, address, 1); },
                                       "écriture hors RAM acceptée");
    }
    ram.write32(arm9, 0x02000000, 0xaabbccdd);
    expect_failure<ravenemu::Error>([&] { ram.write32(arm9, 0x02000001, 0); }, "mot non aligné");
    expect_failure<ravenemu::Error>([&] { ram.write16(arm7, 0x02000001, 0); }, "demi-mot non aligné");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(ram.read32(arm9, 0x02000002)); },
                                   "lecture mot non alignée");
    expect_failure<ravenemu::Error>([&] { static_cast<void>(ram.read16(arm7, 0x02000001)); },
                                   "lecture demi-mot non alignée");
    check(ram.read32(arm7, 0x02000000) == 0xaabbccdd, "écriture rejetée a modifié la RAM");
}

} // namespace

int main() {
    main_ram_test();
    wram_test();
    rejected_access_test();
}
