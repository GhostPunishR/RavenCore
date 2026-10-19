#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <algorithm>
#include <array>
#include <vector>

using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::nds::VideoFrame;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9;
constexpr auto arm7 = Processor::arm7;
constexpr std::uint32_t power = 0x04000304;
constexpr std::uint16_t red = 0x001f, green = 0x03e0, blue = 0x7c00, yellow = 0x03ff, backdrop = 0x4210;
constexpr std::array<std::uint32_t,9> controls{0x04000240,0x04000241,0x04000242,0x04000243,0x04000244,
    0x04000245,0x04000246,0x04000248,0x04000249};
constexpr std::array<std::uint32_t,9> starts{0,0x20000,0x40000,0x60000,0x80000,0x90000,0x94000,0x98000,0xa0000};

namespace {
std::uint32_t port(unsigned e) { return 0x04000000 + e * 0x1000; }
std::uint32_t bg(unsigned e) { return 0x06000000 + e * 0x200000; }
std::uint32_t obj(unsigned e) { return 0x06400000 + e * 0x200000; }
std::uint32_t pal(unsigned e) { return 0x05000000 + e * 0x400; }
std::uint32_t oam(unsigned e) { return 0x07000000 + e * 0x400; }
const auto& screen(const VideoFrame& frame, unsigned e) { return e == 0 ? frame.upper : frame.lower; }
std::uint16_t pixel(Memory& m, unsigned e) { return screen(m.render_frame(),e)[0]; }
void tile(Memory& m, std::uint32_t address, std::uint16_t pattern) {
    for(unsigned i=0;i<64;i+=2) m.write16(arm9,address+i,pattern);
}
void setup(Memory& m) {
    m.write16(arm9,power,0x8203); m.write32(arm9,controls[0],0x84848281);
    for(unsigned e=0;e<2;++e) {
        m.write16(arm9,pal(e),backdrop); m.write16(arm9,pal(e)+2,red); m.write16(arm9,pal(e)+34,green);
        m.write16(arm9,pal(e)+0x202,red); m.write16(arm9,pal(e)+0x222,green);
        for(unsigned n=0;n<128;++n) m.write16(arm9,oam(e)+n*8,0x200);
        tile(m,bg(e),0x0101); tile(m,obj(e),0x0101);
        for(unsigned layer=0;layer<4;++layer) m.write16(arm9,port(e)+8+layer*2,0x1080);
    }
}
void upload(Memory& m, unsigned bank, unsigned offset, std::uint16_t value) {
    const auto previous=m.read8(arm9,controls[bank]);
    m.write8(arm9,controls[bank],0x80); m.write16(arm9,0x06800000+starts[bank]+offset,value);
    m.write8(arm9,controls[bank],previous);
}
void identity(Memory& m, unsigned e) {
    m.write32(arm9,port(e)+0x20,0x100); m.write32(arm9,port(e)+0x24,0x01000000);
}

void bg_slots_test(unsigned e) {
    Memory m; setup(m);
    const unsigned bank=e?7:4;
    const std::array<std::uint16_t,4> colors{red,green,blue,yellow};
    for(unsigned slot=0;slot<4;++slot) upload(m,bank,slot*0x2000+3*512+2,static_cast<std::uint16_t>(0x8000|colors[slot]));
    upload(m,bank,0x7ffe,blue); // slot 3, palette 15, index 255 : dernière entrée valide
    m.write8(arm9,controls[bank],e?0x82:0x84); m.write16(arm9,bg(e)+0x8000,0x3000);
    for(unsigned layer=0;layer<4;++layer) {
        m.write32(arm9,port(e),0x40010000|(1U<<(8+layer)));
        check(pixel(m,e)==colors[layer],"slot étendu BG0..3 ou palette 3 incorrect");
    }
    for(unsigned layer=0;layer<2;++layer) {
        m.write16(arm9,port(e)+8+layer*2,0x3080); m.write32(arm9,port(e),0x40010000|(1U<<(8+layer)));
        check(pixel(m,e)==colors[layer+2],"BG0/1 bit13 ne sélectionne pas slot 2/3");
        m.write16(arm9,port(e)+8+layer*2,0x1080);
    }
    tile(m,bg(e),0xffff); m.write16(arm9,bg(e)+0x8000,0xf000); m.write32(arm9,port(e),0x40010800);
    check(pixel(m,e)==blue,"dernière entrée de palette BG étendue");
    tile(m,bg(e),0); upload(m,bank,0x7e00,red);
    check(pixel(m,e)==backdrop,"index 0 étendu doit rester transparent dans chaque palette");
    tile(m,bg(e),0x0101); m.write32(arm9,port(e),0x00010100);
    check(pixel(m,e)==red,"8 bpp sans palettes étendues doit ignorer la banque de palette du BG");
    tile(m,bg(e),0x1111); m.write16(arm9,bg(e)+0x8000,0x1000); m.write16(arm9,port(e)+8,0x1000);
    m.write32(arm9,port(e),0x40010100);
    check(pixel(m,e)==green,"BG 4 bpp doit conserver les palettes standard");
    tile(m,bg(e),0x0101); m.write16(arm9,bg(e)+0x8000,0); m.write16(arm9,port(e)+12,0x1000); identity(m,e);
    m.write32(arm9,port(e),0x40010402);
    check(pixel(m,e)==red,"affine à carte 8 bits doit conserver la palette standard");
    m.write16(arm9,bg(e)+0x8000,0x3000); m.write32(arm9,port(e),0x40010405);
    check(pixel(m,e)==blue,"affine à carte 16 bits doit utiliser le slot BG2 étendu");
    m.write16(arm9,port(e)+12,0x80);
    check(pixel(m,e)==red,"bitmap indexé doit conserver sa palette standard");
    m.write16(arm9,port(e)+12,0x84); m.write16(arm9,bg(e),static_cast<std::uint16_t>(0x8000|green));
    check(pixel(m,e)==green,"bitmap direct doit ignorer les palettes étendues");
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read16(arm9,0x06800000+starts[bank]));},"palette étendue garde un accès CPU LCDC");
    expect_failure<ravenemu::Error>([&]{m.write16(arm7,0x06800000+starts[bank],0);},"palette étendue accessible par ARM7");
}

void main_palette_routing_test() {
    Memory m; setup(m);
    upload(m,4,2,red); upload(m,5,2,green); upload(m,6,2,blue);
    m.write8(arm9,controls[4],0x84); m.write8(arm9,controls[5],0x84); m.write8(arm9,controls[6],0x84);
    m.write32(arm9,port(0),0x40010100);
    check(pixel(m,0)==0x7fff,"palettes BG E/F/G superposées doivent se combiner en OU");
    m.write8(arm9,controls[4],0);
    check(pixel(m,0)==0x7fe0,"désallocation E modifie la vue GPU sans effacer F/G");
    m.write8(arm9,controls[5],0x8c); // F : slots 2/3
    check(pixel(m,0)==blue,"OFS F ne doit pas couvrir les slots 0/1");
    m.write8(arm9,controls[6],0); m.write16(arm9,port(0)+8,0x3080);
    check(pixel(m,0)==green,"OFS=1 doit affecter F aux slots 2/3");
    for(const auto reserved : {0x94,0x9c}) {
        m.write8(arm9,controls[5],static_cast<std::uint8_t>(reserved));
        check(pixel(m,0)==0,"OFS 2/3 réservé a exposé une palette BG");
    }
    // Les deux moitiés de F et G sont des slots BG distincts.
    upload(m,5,2,red); upload(m,5,0x2002,green); upload(m,6,2,yellow); upload(m,6,0x2002,blue);
    m.write8(arm9,controls[5],0x84); m.write8(arm9,controls[6],0x8c); m.write16(arm9,port(0)+8,0x1080);
    for(unsigned layer=0;layer<4;++layer) {
        m.write32(arm9,port(0),0x40010000|(1U<<(8+layer)));
        check(pixel(m,0)==std::array<std::uint16_t,4>{red,green,yellow,blue}[layer],"F/G ne routent pas les quatre slots BG");
    }
    m.write8(arm9,controls[5],0x83); m.write32(arm9,port(0),0x40010100);
    check(pixel(m,0)==0,"texture palette confondue avec palette BG");
    m.write8(arm9,controls[5],0x80);
    check(m.read16(arm9,0x06890002)==red,"réaffectation LCDC a perdu la palette");
}

void object_palettes_test(unsigned e) {
    Memory m; setup(m); const unsigned bank=e?8:5;
    upload(m,bank,7*512+2,blue); upload(m,bank,0x2000+7*512+2,red); upload(m,bank,0x1ffe,yellow);
    m.write8(arm9,controls[bank],e?0x83:0x9d); // OFS ignoré en palette OBJ A
    if(!e) { upload(m,6,7*512+2,green); m.write8(arm9,controls[6],0x85); }
    m.write16(arm9,oam(e),0x2000); m.write16(arm9,oam(e)+4,0x7000); m.write32(arm9,port(e),0x80011010);
    check(pixel(m,e)==(e?blue:0x7fe0),"OBJ 8 bpp n'utilise pas la palette étendue OAM ou le OU F/G");
    if(!e) m.write8(arm9,controls[6],0);
    tile(m,obj(e),0xffff); m.write16(arm9,oam(e)+4,0xf000);
    check(pixel(m,e)==yellow,"index 255, palette 15 OBJ ou moitié basse 8 Kio incorrecte");
    tile(m,obj(e),0); upload(m,bank,0x1e00,red);
    check(pixel(m,e)==backdrop,"couleur zéro OBJ étendue doit être transparente");
    tile(m,obj(e),0x0101); m.write8(arm9,controls[bank],0);
    check(pixel(m,e)==0,"palette OBJ non allouée doit se lire zéro");
    tile(m,obj(e),0x1111); m.write16(arm9,oam(e),0); m.write16(arm9,oam(e)+4,0x1000);
    check(pixel(m,e)==green,"OBJ 4 bpp doit utiliser la palette standard malgré DISPCNT.31");
    m.write16(arm9,oam(e),0x0c00); m.write16(arm9,oam(e)+4,0xf000); m.write16(arm9,obj(e),static_cast<std::uint16_t>(0x8000|blue));
    m.write32(arm9,port(e),0x80011040);
    check(pixel(m,e)==blue,"OBJ bitmap doit ignorer les palettes étendues");
}

void master_registers_test() {
    Memory m;
    for(unsigned e=0;e<2;++e) {
        const auto address=port(e)+0x6c;
        m.write32(arm9,address,0xffffffff);
        check(m.read32(arm9,address)==0xc01f,"masque MASTER_BRIGHT ou écriture moteur éteint incorrect");
        m.write8(arm9,address,0xe8); m.write8(arm9,address+1,0xbf);
        m.write16(arm9,address+2,0xffff);
        check(m.read32(arm9,address)==0x8008,"accès octets et demi-mots MASTER_BRIGHT");
        const std::array<std::uint32_t,2> invalid{0x4010,0};
        expect_failure<ravenemu::Error>([&]{m.write_words(arm9,address,invalid);},"transfert MASTER_BRIGHT vers trou accepté");
        check(m.read32(arm9,address)==0x8008,"transfert refusé a modifié MASTER_BRIGHT");
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read16(arm7,address));},"ARM7 accède à MASTER_BRIGHT");
        expect_failure<ravenemu::Error>([&]{m.write16(arm7,address,0);},"ARM7 modifie MASTER_BRIGHT");
    }
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read32(arm9,0x04000060));},"trou 3D mappé par erreur");
    auto copy=m; copy.write16(arm9,port(0)+0x6c,0);
    check(m.read16(arm9,port(0)+0x6c)==0x8008&&copy.read16(arm9,port(1)+0x6c)==0x8008,"copie partage les registres maître");
    m.clear_ram();
    check(m.read32(arm9,port(0)+0x6c)==0&&m.read32(arm9,port(1)+0x6c)==0,"reset conserve MASTER_BRIGHT");
}

void master_render_test() {
    Memory m; setup(m);
    m.write32(arm9,port(0),0x10100); m.write32(arm9,port(1),0x10000); m.write16(arm9,pal(1),blue);
    m.write16(arm9,port(0)+0x6c,0x4008); m.write16(arm9,port(1)+0x6c,0x8008);
    auto frame=m.render_frame();
    check(frame.upper[0]==0x3dff&&frame.lower[0]==0x4000,"luminosité maître A/B indépendante");
    m.write16(arm9,port(0)+0x6c,0x001f); m.write16(arm9,port(1)+0x6c,0x8000);
    frame=m.render_frame();
    check(frame.upper[0]==red&&frame.lower[0]==blue,"mode nul ou facteur zéro modifie les pixels");
    m.write16(arm9,port(0)+0x6c,0x401f); m.write16(arm9,port(1)+0x6c,0x801f);
    frame=m.render_frame();
    check(frame.upper[0]==0x7fff&&frame.lower[0]==0,"facteur maître supérieur à 16 non saturé");
    // WINOUT autorise BG0 mais interdit les effets BLDCNT. MASTER_BRIGHT reste actif.
    m.write32(arm9,port(0),0x12100); m.write16(arm9,port(0)+0x4a,1);
    m.write16(arm9,port(0)+0x50,0x0081); m.write16(arm9,port(0)+0x54,16);
    m.write16(arm9,port(0)+0x6c,0x8008);
    check(pixel(m,0)==0x0010,"fenêtre a masqué la luminosité maître");
    m.write16(arm9,port(0)+0x4a,0x21);
    check(pixel(m,0)==0x4210,"luminosité maître doit suivre BLDCNT/BLDY");
    // L'affichage direct LCDC contourne les effets 2D et fonctionne moteur A éteint.
    m.write8(arm9,controls[0],0x80); m.write16(arm9,0x06800000,static_cast<std::uint16_t>(0x8000|blue));
    m.write32(arm9,port(0),0x20000); m.write16(arm9,power,0x8001);
    m.write16(arm9,port(0)+0x6c,0x8008);
    check(pixel(m,0)==0x4000,"affichage VRAM direct ne reçoit pas MASTER_BRIGHT moteur éteint");
    m.write32(arm9,port(0),0); m.write16(arm9,port(0)+0x6c,0x8010);
    check(pixel(m,0)==0,"écran blanc mode 0 doit passer par la luminosité maître");
    m.write32(arm9,port(0),0x20080); m.write16(arm9,port(0)+0x6c,0x8008);
    check(pixel(m,0)==0x4210,"forced blank doit passer par la luminosité maître");
    m.write32(arm9,port(0),0x10000);
    check(pixel(m,0)==0x4210,"moteur 2D éteint doit conserver l'étape de luminosité maître");
    m.write16(arm9,port(0)+0x6c,0xc000);
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"mode maître réservé accepté");
    m.write16(arm9,power,0);
    frame=m.render_frame();
    check(frame.upper[0]==0x7fff&&frame.lower[0]==0x7fff,"LCD éteint doit conserver les deux images blanches");
}

void put32(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8U));
}

void machine_palette_test() {
    std::vector<std::array<std::uint32_t,2>> commands;
    const auto half=[&](std::uint32_t address,std::uint32_t value){commands.push_back({address,value});};
    const auto write=[&](std::uint32_t address,std::uint32_t value){half(address,value&0xffffU);half(address+2,value>>16U);};
    write(power,0x8203); write(controls[0],0x84848281);
    half(controls[4],0x80); half(controls[7],0x80);
    write(0x06884000,static_cast<std::uint32_t>(red)<<16U); // slot 2, palette 0, index 1
    write(0x06898000,static_cast<std::uint32_t>(blue)<<16U);
    half(controls[4],0x84); half(controls[7],0x82);
    for(unsigned e=0;e<2;++e) {
        write(port(e)+8,e?0x1080:0x3080);
        for(unsigned offset=0;offset<64;offset+=4) write(bg(e)+offset,0x01010101);
        write(port(e),0x40010100); write(port(e)+0x6c,e?0x4008:0x8008);
    }
    const auto size=static_cast<std::uint32_t>(0x100+commands.size()*8);
    std::vector<std::uint8_t> rom(0x200+size+4);
    put32(rom,0x20,0x200); put32(rom,0x24,0x02000000); put32(rom,0x28,0x02000000); put32(rom,0x2c,size);
    put32(rom,0x30,0x200+size); put32(rom,0x34,0x03800000); put32(rom,0x38,0x03800000); put32(rom,0x3c,4);
    put32(rom,0x200+size,0xeafffffe); // ARM7 B .
    put32(rom,0x200,0xe59f0018); // LDR r0,[pc,#24] : commandes
    put32(rom,0x204,0xe59f3018); // LDR r3,[pc,#24] : nombre
    put32(rom,0x208,0xe8b00006); // LDMIA r0!,{r1,r2}
    put32(rom,0x20c,0xe1c120b0); // STRH r2,[r1] : respecte les ports VRAMCNT H/I sur 16 bits
    put32(rom,0x210,0xe2533001); // SUBS r3,r3,#1
    put32(rom,0x214,0x1afffffb); // BNE boucle
    put32(rom,0x218,0xeafffffe); // B .
    put32(rom,0x220,0x02000100); put32(rom,0x224,static_cast<std::uint32_t>(commands.size()));
    for(std::size_t i=0;i<commands.size();++i) {put32(rom,0x300+i*8,commands[i][0]);put32(rom,0x304+i*8,commands[i][1]);}
    ravenemu::nds::Machine machine;
    machine.load_cartridge(ravenemu::nds::Cartridge{rom}); machine.run_steps(arm9,2+commands.size()*4);
    const auto cpu_before=machine.cpu_state(arm9);
    const auto frame=machine.render_frame();
    check(std::all_of(frame.upper.begin(),frame.upper.end(),[](auto color){return color==0x0010;})&&
          std::all_of(frame.lower.begin(),frame.lower.end(),[](auto color){return color==0x7def;}),
          "programme ARM9 : palettes étendues et luminosités A/B incorrectes");
    check(machine.cpu_state(arm9)==cpu_before&&machine.memory().read16(arm9,0x04000006)==0,"rendu fait avancer CPU ou LCD");
    machine.memory().write16(arm9,power,0x0203);
    const auto swapped=machine.render_frame();
    check(swapped.upper[0]==0x7def&&swapped.lower[0]==0x0010,"luminosité maître ne suit pas la permutation des moteurs");
    auto copy=machine;
    upload(copy.memory(),4,0x4002,green); copy.memory().write16(arm9,port(0)+0x6c,0);
    check(copy.render_frame().lower[0]==green&&machine.render_frame().lower[0]==0x0010,"copie partage palettes ou luminosité");
    auto invalid=rom; put32(invalid,0x34,0x04000000); put32(invalid,0x38,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&]{machine.load_cartridge(ravenemu::nds::Cartridge{invalid});},"ROM invalide acceptée");
    check(machine.render_frame().lower[0]==0x0010,"échec de chargement perd palettes ou luminosité");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    check(machine.memory().read32(arm9,port(0)+0x6c)==0&&machine.memory().read32(arm9,port(1)+0x6c)==0&&
          machine.memory().read8(arm9,controls[4])==0&&machine.memory().read8(arm9,controls[7])==0,"chargement conserve l'état étendu");
    machine.memory().write8(arm9,controls[4],0x80); machine.memory().write8(arm9,controls[7],0x80);
    check(machine.memory().read16(arm9,0x06884002)==0&&machine.memory().read16(arm9,0x06898002)==0,"chargement conserve les couleurs étendues");
}
} // namespace

int main() {
    for(unsigned e=0;e<2;++e) {bg_slots_test(e);object_palettes_test(e);}
    main_palette_routing_test(); master_registers_test(); master_render_test(); machine_palette_test();
}
