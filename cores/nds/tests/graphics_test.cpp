#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

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

namespace {
std::uint32_t port(unsigned e) { return 0x04000000 + e * 0x1000; }
std::uint32_t bg(unsigned e) { return 0x06000000 + e * 0x200000; }
std::uint32_t obj(unsigned e) { return 0x06400000 + e * 0x200000; }
std::uint32_t pal(unsigned e) { return 0x05000000 + e * 0x400; }
std::uint32_t oam(unsigned e) { return 0x07000000 + e * 0x400; }
const auto& screen(const VideoFrame& frame, unsigned e) { return e == 0 ? frame.upper : frame.lower; }
void palette(Memory& m, unsigned e, bool object, unsigned index, std::uint16_t color) {
    m.write16(arm9, pal(e) + (object ? 0x200 : 0) + index * 2, color);
}
void tile(Memory& m, std::uint32_t address, bool depth8, unsigned index) {
    for (unsigned i = 0; i < (depth8 ? 64U : 32U); i += 2)
        m.write16(arm9, address + i, static_cast<std::uint16_t>(index * (depth8 ? 0x101U : 0x1111U)));
}
void setup(Memory& m) {
    m.write16(arm9, power, 0x8203);
    m.write32(arm9, 0x04000240, 0x84848281); // A/B : BG/OBJ A ; C/D : BG/OBJ B
    for (unsigned e = 0; e < 2; ++e) {
        palette(m, e, false, 0, backdrop);
        for (const bool object : {false, true}) {
            palette(m, e, object, 1, red); palette(m, e, object, 2, green);
            palette(m, e, object, 3, blue); palette(m, e, object, 4, yellow);
        }
        for (unsigned i = 0; i < 128; ++i) m.write16(arm9, oam(e) + i * 8, 0x0200);
    }
}
void identity(Memory& m, unsigned e, unsigned layer) {
    const auto base = port(e) + 0x20 + (layer - 2) * 0x10;
    m.write32(arm9, base, 0x100); m.write32(arm9, base + 4, 0x01000000);
    m.write32(arm9, base + 8, 0); m.write32(arm9, base + 12, 0);
}
void sprite(Memory& m, unsigned e, unsigned n, std::uint16_t a0, std::uint16_t a1, std::uint16_t a2) {
    m.write16(arm9, oam(e) + n * 8, a0); m.write16(arm9, oam(e) + n * 8 + 2, a1); m.write16(arm9, oam(e) + n * 8 + 4, a2);
}

void registers_and_memory_test() {
    Memory m;
    for (unsigned e = 0; e < 2; ++e) {
        m.write16(arm9, port(e) + 8, 0xffff);
        m.write16(arm9, pal(e), red);
        check(m.read16(arm9, port(e) + 8) == 0 && m.read16(arm9, pal(e)) == 0, "moteur éteint : ports et palette protégés");
    }
    setup(m);
    m.write32(arm9, port(1), 0xffffffff);
    check(m.read32(arm9, port(1)) == 0xc0b1fff7, "bits DISPCNT propres au moteur B");
    for (unsigned e = 0; e < 2; ++e) {
        m.write16(arm9, port(e) + 8, 0xabcd);
        m.write32(arm9, port(e) + 0x10, 0xffffffff);
        m.write32(arm9, port(e) + 0x28, 0xffffffff);
        check(m.read16(arm9, port(e) + 8) == 0xabcd && m.read32(arm9, port(e) + 0x10) == 0x01ff01ff &&
              m.read32(arm9, port(e) + 0x28) == 0x0fffffff, "masques BGxCNT, scroll 9 bits et référence 28 bits");
        m.write32(arm9, pal(e), 0x12345678); m.write8(arm9, pal(e), 0);
        m.write32(arm9, oam(e), 0xaabbccdd); m.write8(arm9, oam(e), 0);
        check(m.read32(arm9, pal(e)) == 0x12345678 && m.read32(arm9, oam(e)) == 0xaabbccdd, "STRB palettes/OAM doit être ignoré");
        expect_failure<ravenemu::Error>([&] { static_cast<void>(m.read16(arm7, pal(e))); }, "palette ARM7 accessible");
        expect_failure<ravenemu::Error>([&] { m.write16(arm7, oam(e), 0); }, "OAM ARM7 accessible");
    }
    m.write16(arm9, power, 0x8201); // A éteint, B conservé
    m.write16(arm9, pal(0), 0); m.write16(arm9, port(0) + 8, 0);
    check(m.read16(arm9, pal(0)) == 0 && m.read16(arm9, pal(1)) == 0x5678 && m.read16(arm9, port(0) + 8) == 0xabcd,
          "extinction d'un moteur a modifié l'autre ou ses anciens registres");
    m.write16(arm9, power, 0x8203);
    check(m.read16(arm9, pal(0)) == 0x5678, "extinction a effacé la palette");
    const std::array<std::uint32_t, 2> invalid{1, 2};
    expect_failure<ravenemu::Error>([&] { m.write_words(arm9, 0x050007fc, invalid); }, "palette puis trou accepté");
    check(m.read32(arm9, 0x050007fc) == 0, "palette partiellement écrite sur échec");
    auto copy = m;
    copy.write16(arm9, pal(0), green); copy.write16(arm9, oam(1), 0);
    check(m.read16(arm9, pal(0)) == 0x5678 && m.read16(arm9, oam(1)) == 0xccdd, "copie partage palettes/OAM");
    m.clear_ram(); m.write16(arm9, power, 0x8203);
    check(m.read16(arm9, pal(0)) == 0 && m.read16(arm9, oam(1)) == 0 && m.read16(arm9, port(1) + 8) == 0 &&
          m.read32(arm9, port(1)) == 0, "reset incomplet des moteurs A/B");
}

void text_priority_test(unsigned e) {
    Memory m; setup(m);
    for (unsigned layer = 0; layer < 4; ++layer) {
        tile(m, bg(e) + (layer + 1) * 32, false, layer + 1);
        m.write16(arm9, bg(e) + 0x8000 + layer * 0x800, static_cast<std::uint16_t>(layer + 1));
        m.write16(arm9, port(e) + 8 + layer * 2, static_cast<std::uint16_t>((16 + layer) << 8U));
    }
    m.write16(arm9, bg(e) + 32, 0x1110); // premier pixel BG0 transparent
    m.write32(arm9, port(e), 0x00010f00);
    auto frame = m.render_frame();
    check(screen(frame,e)[0] == green && screen(frame,e)[1] == red && screen(frame,e)[8] == backdrop,
          "priorité BG0..3, transparence et backdrop");
    m.write32(arm9, port(e), 0x00010d00);
    check(screen(m.render_frame(),e)[0] == blue, "BG2 doit apparaître quand BG0 est transparent et BG1 masqué");
    for (unsigned layer = 0; layer < 4; ++layer)
        m.write16(arm9, port(e) + 8 + layer * 2, static_cast<std::uint16_t>(((16 + layer) << 8U) | (3 - layer)));
    m.write32(arm9, port(e), 0x00010f00);
    check(screen(m.render_frame(),e)[1] == yellow, "priorité programmable BG3 devant BG0");
    palette(m,e,false,4,0); // noir opaque, indice non nul
    check(screen(m.render_frame(),e)[1] == 0, "couleur noire confondue avec transparence");
}

void text_addressing_test(unsigned e) {
    Memory m; setup(m);
    m.write32(arm9, port(e), 0x00010100);
    m.write16(arm9, port(e) + 8, 0x1000);
    tile(m,bg(e)+32,false,1);
    m.write32(arm9,bg(e)+32,0x87654321);
    for (unsigned x=0;x<8;x+=4) m.write16(arm9,bg(e)+32+28+x/2,0x9999);
    palette(m,e,false,49,red); palette(m,e,false,56,green); palette(m,e,false,57,blue);
    m.write16(arm9,bg(e)+0x8000,0x3001);
    check(screen(m.render_frame(),e)[0] == red, "palette 4bpp et nibble bas");
    m.write16(arm9,bg(e)+0x8000,0x3401);
    check(screen(m.render_frame(),e)[0] == green, "retournement horizontal de tuile");
    m.write16(arm9,bg(e)+0x8000,0x3801);
    check(screen(m.render_frame(),e)[0] == blue, "retournement vertical de tuile");
    for (unsigned i=1;i<=4;++i) tile(m,bg(e)+i*32,false,i);
    for (unsigned block=0;block<4;++block) m.write16(arm9,bg(e)+0x8000+block*0x800,static_cast<std::uint16_t>(block+1));
    m.write16(arm9,port(e)+8,0xd000); // 512x512, map à 8000
    m.write16(arm9,port(e)+0x10,256);
    check(screen(m.render_frame(),e)[0]==green,"bloc de carte à droite");
    m.write16(arm9,port(e)+0x10,0); m.write16(arm9,port(e)+0x12,256);
    check(screen(m.render_frame(),e)[0]==blue,"bloc de carte en bas");
    m.write16(arm9,port(e)+0x10,256);
    check(screen(m.render_frame(),e)[0]==yellow,"quatrième bloc de carte");
    m.write16(arm9,port(e)+8,0x9000); m.write16(arm9,port(e)+0x10,0);
    check(screen(m.render_frame(),e)[0]==green,"carte 256x512 utilise le second bloc vertical");
    m.write16(arm9,port(e)+8,0x5000); m.write16(arm9,port(e)+0x10,256); m.write16(arm9,port(e)+0x12,0);
    check(screen(m.render_frame(),e)[0]==green,"carte 512x256 utilise le second bloc horizontal");
    m.write16(arm9,port(e)+8,0x1000); m.write16(arm9,port(e)+0x10,256); // wrap 256
    check(screen(m.render_frame(),e)[0]==red,"défilement texte doit boucler sur la taille");
    m.write16(arm9,port(e)+0x10,0);
    palette(m,e,false,200,yellow);
    tile(m,bg(e)+64,true,200); m.write16(arm9,bg(e)+0x8000,0xf001);
    m.write16(arm9,port(e)+8,0x1080);
    check(screen(m.render_frame(),e)[0]==yellow,"tuile 8bpp doit ignorer la banque de palette standard");
    tile(m,bg(e)+0x10000+32,false,2); m.write16(arm9,bg(e)+0x8000,1);
    m.write16(arm9,port(e)+8,0x1010);
    check(screen(m.render_frame(),e)[0]==green,"bits NDS supplémentaires de CharBase");
    if (e==0) {
        m.write16(arm9,port(e)+8,0x1000);
        m.write16(arm9,bg(e)+0x18000,1);
        m.write32(arm9,port(e),0x09010100);
        check(screen(m.render_frame(),e)[0]==green,"bases caractère/carte étendues DISPCNT A");
    }
}

void affine_test(unsigned e) {
    for (const auto layer : {2U,3U}) {
        Memory m; setup(m); identity(m,e,layer);
        m.write32(arm9,port(e),0x00010000U | (1U << (8+layer)) | (layer==2?2U:1U));
        m.write16(arm9,port(e)+8+layer*2,0x1000);
        tile(m,bg(e)+64,true,1); tile(m,bg(e)+128,true,2);
        m.write16(arm9,bg(e)+0x8000,1); // carte 8 bits : première entrée 1, seconde 0
        const auto matrix=port(e)+0x20+(layer-2)*0x10;
        auto frame=m.render_frame();
        check(screen(frame,e)[0]==red && screen(frame,e)[8]==backdrop && screen(frame,e)[128]==backdrop,"fond affine identité et clipping");
        m.write16(arm9,bg(e)+0x8000+14,0x0200); // entrée 15=2
        m.write32(arm9,matrix+8,0x0fffffff); // -1/256, arrondi vers -inf
        check(screen(m.render_frame(),e)[0]==backdrop,"référence négative sans overflow doit être transparente");
        m.write16(arm9,port(e)+8+layer*2,0x3000);
        frame=m.render_frame();
        check(screen(frame,e)[0]==green && screen(frame,e)[1]==red,"overflow affine et fraction négative");
        m.write32(arm9,matrix+8,0); m.write16(arm9,matrix,0x200);
        frame=m.render_frame();
        check(screen(frame,e)[3]==red && screen(frame,e)[4]==backdrop,"mise à l'échelle affine");
        m.write16(arm9,bg(e)+0x8000+16,2); // rangée suivante
        m.write32(arm9,matrix,0x01000000); m.write32(arm9,matrix+4,0x00000100);
        check(screen(m.render_frame(),e)[8]==green,"rotation affine des coordonnées x/y");
    }
}

void bitmap_and_extended_test(unsigned e) {
    Memory m; setup(m); identity(m,e,3);
    m.write32(arm9,port(e),0x00010803); // BG3 extended tile
    m.write16(arm9,port(e)+14,0x1000);
    tile(m,bg(e)+300*64,true,2); m.write16(arm9,bg(e)+0x8000,300);
    check(screen(m.render_frame(),e)[0]==green,"carte affine 16 bits et numéro de tuile supérieur à 255");
    identity(m,e,2);
    m.write32(arm9,port(e),0x40010405); // palette étendue n'affecte pas bitmap indexé
    m.write16(arm9,port(e)+12,0x0180);
    m.write16(arm9,bg(e)+0x4000,0x0201);
    auto frame=m.render_frame();
    check(screen(frame,e)[0]==red && screen(frame,e)[1]==green && screen(frame,e)[2]==backdrop,"bitmap indexé et indice zéro transparent");
    m.write16(arm9,port(e)+12,0x0184);
    m.write32(arm9,bg(e)+0x4000,0x03e0801f); m.write32(arm9,bg(e)+0x4004,0x8000fc00);
    frame=m.render_frame();
    check(screen(frame,e)[0]==red && screen(frame,e)[1]==backdrop && screen(frame,e)[2]==blue && screen(frame,e)[3]==0,
          "bitmap RGB555 : alpha bit15 et noir opaque");
    if(e==1) {
        m.write16(arm9,port(e)+12,0xc184);
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"bitmap trop grand pour B accepté");
        m.write32(arm9,port(e),0x00010406);
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"mode 6 réservé sur B accepté");
    } else {
        m.write32(arm9,0x04000240,0x99918981); // quatre banques BG A, 512 Kio
        m.write16(arm9,port(e)+12,0x6000); // grand bitmap 1024x512, overflow
        m.write32(arm9,port(e),0x00010406);
        m.write16(arm9,bg(e)+0x7fffe,0x0102);
        m.write32(arm9,port(e)+0x28,1023*256); m.write32(arm9,port(e)+0x2c,511*256);
        check(screen(m.render_frame(),e)[0]==red,"grand bitmap 512 Kio et dernière adresse");
    }
}

void sprites_test(unsigned e) {
    Memory m; setup(m);
    tile(m,bg(e)+32,false,1); m.write16(arm9,bg(e)+0x8000,1);
    m.write16(arm9,port(e)+8,0x1001); // BG priorité 1
    m.write32(arm9,port(e),0x00011110); // BG0 + OBJ 1D
    tile(m,obj(e)+32,false,2); tile(m,obj(e)+64,false,3); tile(m,obj(e)+96,false,4);
    sprite(m,e,0,4,static_cast<std::uint16_t>(4|0x4000),static_cast<std::uint16_t>(1|0x400));
    auto frame=m.render_frame();
    check(screen(frame,e)[4*256+4]==green && screen(frame,e)[4*256+12]==blue && screen(frame,e)[12*256+4]==yellow &&
          screen(frame,e)[0]==red,"OBJ 16x16 1D et priorité égale devant BG");
    m.write16(arm9,obj(e)+32,0x2220);
    check(screen(m.render_frame(),e)[4*256+4]==red,"pixel OBJ transparent doit révéler BG");
    sprite(m,e,0,4,4,static_cast<std::uint16_t>(1|0x800));
    sprite(m,e,1,4,4,2);
    check(screen(m.render_frame(),e)[4*256+5]==blue,"priorité OBJ NDS doit précéder l'indice OAM");
    sprite(m,e,0,4,4,1);
    check(screen(m.render_frame(),e)[4*256+5]==green,"égalité de priorité OBJ : plus petit indice OAM");
    sprite(m,e,1,0x0200,0,0);
    tile(m,obj(e)+32,false,1); palette(m,e,true,81,yellow);
    sprite(m,e,0,4,4,0x5001);
    check(screen(m.render_frame(),e)[4*256+4]==yellow,"banque de palette OBJ 4bpp");
    // H/V flip et coordonnées négatives par wrap.
    m.write32(arm9,obj(e)+32,0x22222221); m.write16(arm9,obj(e)+32+28,0x3333);
    sprite(m,e,0,0,0x1000,1);
    check(screen(m.render_frame(),e)[0]==green,"retournement horizontal OBJ");
    sprite(m,e,0,0,0x2000,1);
    check(screen(m.render_frame(),e)[0]==blue,"retournement vertical OBJ");
    sprite(m,e,0,255,511,1);
    frame=m.render_frame();
    check(screen(frame,e)[0]==red && screen(frame,e)[7]==red,"clipping et wrap de position OBJ");
    // 8bpp, frontière 1D de 64 octets : numéro 3 commence à 192.
    m.write32(arm9,port(e),0x00111010);
    tile(m,obj(e)+192,true,200); palette(m,e,true,200,yellow);
    sprite(m,e,0,0x2000,0,0xf003);
    check(screen(m.render_frame(),e)[0]==yellow,"OBJ 8bpp et frontière 1D 64");
    // En 2D, le bit bas du numéro 8bpp est ignoré et les lignes de tuiles ont 1024 octets.
    m.write32(arm9,port(e),0x00011000);
    tile(m,obj(e)+64,true,2); tile(m,obj(e)+64+1024,true,3);
    sprite(m,e,0,0x2000,0x4000,3);
    frame=m.render_frame();
    check(screen(frame,e)[0]==green && screen(frame,e)[8*256]==blue,"OBJ 8bpp 2D : numéro pair et stride 1024");
}

void affine_sprite_test(unsigned e) {
    Memory m; setup(m);
    m.write32(arm9,port(e),0x00011010);
    tile(m,obj(e),false,2);
    sprite(m,e,0,0x0300,0,0); // 8x8 affine dans une boîte 16x16
    m.write16(arm9,oam(e)+6,0x100); m.write16(arm9,oam(e)+14,0);
    m.write16(arm9,oam(e)+22,0); m.write16(arm9,oam(e)+30,0x100);
    auto frame=m.render_frame();
    check(screen(frame,e)[0]==backdrop && screen(frame,e)[4*256+4]==green && screen(frame,e)[11*256+11]==green &&
          screen(frame,e)[12*256+12]==backdrop,"OBJ affine double taille : centre et clipping");
    // 64x64 agrandi x2 : apparaît en bas ET en haut de l'écran sur NDS.
    for(unsigned i=0;i<64;++i) tile(m,obj(e)+i*64,true,2);
    sprite(m,e,0,static_cast<std::uint16_t>(0x2300|180),0xc000,0);
    m.write16(arm9,oam(e)+6,0x80); m.write16(arm9,oam(e)+30,0x80);
    frame=m.render_frame();
    check(screen(frame,e)[0]==green && screen(frame,e)[180*256]==green && screen(frame,e)[60*256]==backdrop,
          "wrap vertical OBJ affine NDS et échelle x2");
}

void diagnostics_and_dma_test() {
    Memory m; setup(m);
    const std::array<std::uint32_t,3> upload{0x02001000,0x05000000,0x84000001};
    m.write32(arm9,0x02001000,0x03e0001f); m.write_words(arm9,0x040000b0,upload); m.step_dma(arm9);
    check(m.read32(arm9,0x05000000)==0x03e0001f,"DMA vers palettes");
    const std::array<std::uint32_t,3> oam_upload{0x02001000,0x07000400,0x84000001};
    m.write_words(arm9,0x040000b0,oam_upload); m.step_dma(arm9);
    check(m.read32(arm9,0x07000400)==0x03e0001f,"DMA vers OAM B");
    m.write32(arm9,port(0),0x00012100);
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"fenêtre 2D absente ignorée");
    m.write32(arm9,port(0),0x00010108);
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"BG0 3D ignoré");
    m.write32(arm9,port(0),0x40010100); m.write16(arm9,port(0)+8,0x80);
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"palette BG étendue absente ignorée");
    m.write32(arm9,port(0),0x00011010); sprite(m,0,0,0x0400,0,0);
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"OBJ semi-transparent ignoré");
}

void put32(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8U));
}

void machine_scene_test() {
    std::vector<std::array<std::uint32_t,2>> commands;
    const auto write=[&](std::uint32_t a,std::uint32_t v){commands.push_back({a,v});};
    write(power,0x8203); write(0x04000240,0x84848281);
    for(unsigned e=0;e<2;++e) {
        write(port(e)+8,0x1000);
        write(pal(e),static_cast<std::uint32_t>(e?blue:red)<<16U);
        write(pal(e)+0x200,static_cast<std::uint32_t>(e?yellow:green)<<16U);
        for(unsigned offset=0;offset<32;offset+=4) {write(bg(e)+offset,0x11111111);write(obj(e)+offset,0x11111111);}
        for(unsigned i=0;i<128;++i) write(oam(e)+i*8,0x200);
        write(oam(e),0x00140014); // OBJ0 à (20,20), autres objets désactivés
        write(port(e),0x00011110);
    }
    const auto size=static_cast<std::uint32_t>(0x100+commands.size()*8);
    std::vector<std::uint8_t> rom(0x200+size+4);
    put32(rom,0x20,0x200);put32(rom,0x24,0x02000000);put32(rom,0x28,0x02000000);put32(rom,0x2c,size);
    put32(rom,0x30,0x200+size);put32(rom,0x34,0x03800000);put32(rom,0x38,0x03800000);put32(rom,0x3c,4);
    put32(rom,0x200+size,0xeafffffe); // ARM7 B .
    put32(rom,0x200,0xe59f0018); // LDR r0,[pc,#24] -> +20, commandes
    put32(rom,0x204,0xe59f3018); // LDR r3,[pc,#24] -> +24, nombre
    put32(rom,0x208,0xe8b00006); // LDMIA r0!,{r1,r2}
    put32(rom,0x20c,0xe5812000); // STR r2,[r1]
    put32(rom,0x210,0xe2533001); // SUBS r3,r3,#1
    put32(rom,0x214,0x1afffffb); // BNE +8
    put32(rom,0x218,0xeafffffe); // B .
    put32(rom,0x220,0x02000100);put32(rom,0x224,static_cast<std::uint32_t>(commands.size()));
    for(std::size_t i=0;i<commands.size();++i){put32(rom,0x300+i*8,commands[i][0]);put32(rom,0x304+i*8,commands[i][1]);}
    ravenemu::nds::Machine machine;
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    machine.run_steps(arm9,2+commands.size()*4);
    const auto cpu_before=machine.cpu_state(arm9);
    const auto frame=machine.render_frame();
    for(unsigned y=0;y<192;++y)for(unsigned x=0;x<256;++x) {
        const bool sprite_pixel=x>=20&&x<28&&y>=20&&y<28;
        check(frame.upper[y*256+x]==(sprite_pixel?green:red)&&frame.lower[y*256+x]==(sprite_pixel?yellow:blue),
              "programme ARM : composition BG/OBJ A et B incorrecte");
    }
    check(machine.cpu_state(arm9)==cpu_before && machine.memory().read16(arm9,0x04000006)==0,"rendu a avancé CPU ou LCD");
    machine.memory().write16(arm9,power,0x0203);
    const auto swapped=machine.render_frame();
    check(swapped.upper[0]==blue&&swapped.lower[0]==red&&swapped.upper[20*256+20]==yellow,"permutation des moteurs A/B actifs");
    auto copy=machine;
    copy.memory().write16(arm9,pal(0)+2,green);
    check(copy.render_frame().lower[0]==green&&machine.render_frame().lower[0]==red,"copie partage le moteur 2D");
    auto invalid=rom;put32(invalid,0x34,0x04000000);put32(invalid,0x38,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&]{machine.load_cartridge(ravenemu::nds::Cartridge{invalid});},"chargement invalide accepté");
    check(machine.render_frame().upper[0]==blue,"chargement refusé a perdu le moteur B");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    machine.memory().write16(arm9,power,0x8203);
    check(machine.memory().read32(arm9,port(1))==0&&machine.memory().read16(arm9,pal(1)+2)==0&&
          machine.memory().read16(arm9,oam(1))==0&&machine.memory().read16(arm9,port(1)+8)==0,"chargement ne remet pas B à zéro");
}
} // namespace

int main() {
    registers_and_memory_test();
    for(unsigned e=0;e<2;++e) {
        text_priority_test(e); text_addressing_test(e); affine_test(e); bitmap_and_extended_test(e); sprites_test(e); affine_sprite_test(e);
    }
    diagnostics_and_dma_test();
    machine_scene_test();
}
