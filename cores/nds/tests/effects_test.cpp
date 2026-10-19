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
constexpr std::uint16_t red = 0x001f, green = 0x03e0, blue = 0x7c00;

namespace {
std::uint32_t port(unsigned e) { return 0x04000000 + e * 0x1000; }
std::uint32_t bg(unsigned e) { return 0x06000000 + e * 0x200000; }
std::uint32_t obj(unsigned e) { return 0x06400000 + e * 0x200000; }
std::uint32_t pal(unsigned e) { return 0x05000000 + e * 0x400; }
std::uint32_t oam(unsigned e) { return 0x07000000 + e * 0x400; }
const auto& screen(const VideoFrame& frame, unsigned e) { return e == 0 ? frame.upper : frame.lower; }
void sprite(Memory& m, unsigned e, unsigned n, std::uint16_t a0, std::uint16_t a1, std::uint16_t a2) {
    m.write16(arm9,oam(e)+n*8,a0); m.write16(arm9,oam(e)+n*8+2,a1); m.write16(arm9,oam(e)+n*8+4,a2);
}
void setup(Memory& m) {
    m.write16(arm9,0x04000304,0x8203); m.write32(arm9,0x04000240,0x84848281);
    for (unsigned e=0;e<2;++e) {
        m.write16(arm9,pal(e),blue); m.write16(arm9,pal(e)+2,red); m.write16(arm9,pal(e)+4,green);
        m.write16(arm9,pal(e)+6,blue); m.write16(arm9,pal(e)+0x202,blue); m.write16(arm9,pal(e)+0x204,green);
        for (unsigned i=0;i<128;++i) sprite(m,e,i,0x200,0,0);
        for (unsigned i=0;i<32;i+=2) {
            m.write16(arm9,bg(e)+i,0x1111); m.write16(arm9,bg(e)+32+i,0x2222); m.write16(arm9,bg(e)+64+i,0x3333);
            m.write16(arm9,obj(e)+i,0x1111); m.write16(arm9,obj(e)+32+i,0x2222);
        }
        for (unsigned i=0;i<0x800;i+=2) {
            m.write16(arm9,bg(e)+0x8000+i,0); m.write16(arm9,bg(e)+0x8800+i,1); m.write16(arm9,bg(e)+0x9000+i,2);
        }
        m.write16(arm9,port(e)+8,0x1000); m.write16(arm9,port(e)+10,0x1101); m.write16(arm9,port(e)+12,0x1202);
        m.write32(arm9,port(e),0x00010300);
    }
}
std::uint16_t pixel(Memory& m, unsigned e, unsigned x=0, unsigned y=0) { return screen(m.render_frame(),e)[y*256+x]; }

void registers_test() {
    Memory m;
    for (unsigned e=0;e<2;++e) {
        m.write32(arm9,port(e)+0x48,0xffffffff);
        check(m.read32(arm9,port(e)+0x48)==0,"fenêtres écrites moteur éteint");
    }
    setup(m);
    for (unsigned e=0;e<2;++e) {
        m.write32(arm9,port(e)+0x40,0x12345678); m.write32(arm9,port(e)+0x44,0xabcdef01);
        check(m.read32(arm9,port(e)+0x40)==0x12345678 && m.read32(arm9,port(e)+0x44)==0xabcdef01,"dimensions tronquées");
        m.write32(arm9,port(e)+0x48,0xffffffff); m.write32(arm9,port(e)+0x50,0xffffffff); m.write16(arm9,port(e)+0x54,0xffff);
        check(m.read32(arm9,port(e)+0x48)==0x3f3f3f3f && m.read32(arm9,port(e)+0x50)==0x1f1f3fff &&
              m.read16(arm9,port(e)+0x54)==31,"masques WININ/OUT, BLDCNT/ALPHA/Y");
        m.write8(arm9,port(e)+0x51,0x81); m.write8(arm9,port(e)+0x53,0x42);
        check(m.read32(arm9,port(e)+0x50)==0x021f01ff,"voies d'octets des effets");
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read16(Processor::arm7,port(e)+0x50));},"effets accessibles ARM7");
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read16(arm9,port(e)+0x60));},"port 3D mappé par erreur");
    }
    m.write16(arm9,0x04000304,0x8201); m.write16(arm9,port(0)+0x50,0); m.write16(arm9,port(1)+0x50,0);
    check(m.read16(arm9,port(0)+0x50)==0x01ff && m.read16(arm9,port(1)+0x50)==0,"protection indépendante des effets A/B");
    m.write16(arm9,0x04000304,0x8203);
    const std::array<std::uint32_t,2> invalid{0,0xffffffff};
    expect_failure<ravenemu::Error>([&]{m.write_words(arm9,port(0)+0x50,invalid);},"écriture multiple vers ports non mappés acceptée");
    check(m.read16(arm9,port(0)+0x50)==0x01ff,"validation préalable des transferts multiples vers les effets");
    m.clear_ram(); m.write16(arm9,0x04000304,0x8203);
    check(m.read32(arm9,port(0)+0x48)==0 && m.read32(arm9,port(1)+0x50)==0,"reset des effets/fenêtres");
}

void windows_test(unsigned e) {
    Memory m; setup(m);
    m.write32(arm9,port(e),0x00016300); // WIN0, WIN1
    m.write16(arm9,port(e)+0x40,0x0a1e); m.write16(arm9,port(e)+0x44,0x1428); // [10,30) x [20,40)
    m.write16(arm9,port(e)+0x42,0x1428); m.write16(arm9,port(e)+0x46,0x1e32); // [20,40) x [30,50)
    m.write16(arm9,port(e)+0x48,0x0201); m.write16(arm9,port(e)+0x4a,0);
    auto frame=m.render_frame();
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x) {
        const auto expected=x>=10&&x<30&&y>=20&&y<40 ? red : (x>=20&&x<40&&y>=30&&y<50 ? green : blue);
        check(screen(frame,e)[y*256+x]==expected,"bornes exclusives et priorité WIN0/WIN1");
    }
    m.write16(arm9,port(e)+0x40,0xfa05); m.write16(arm9,port(e)+0x44,0xbe02);
    m.write32(arm9,port(e),0x00012300);
    frame=m.render_frame();
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x)
        check(screen(frame,e)[y*256+x]==(((x>=250||x<5)&&(y>=190||y<2))?red:blue),"fenêtre traversant les limites 8 bits");
    m.write16(arm9,port(e)+0x40,0x0505);
    check(pixel(m,e,5,0)==blue,"fenêtre de largeur nulle");
    m.write32(arm9,port(e),0x00010300);
    check(pixel(m,e)==red,"WININ/OUT ne doivent pas agir sans fenêtre active");
}

void blending_test(unsigned e) {
    Memory m; setup(m);
    m.write16(arm9,port(e)+0x50,0x0241); m.write16(arm9,port(e)+0x52,0x0808);
    check(pixel(m,e)==0x01ef,"mélange BG rouge/vert 8:8");
    m.write16(arm9,port(e)+0x52,0x1f1f);
    check(pixel(m,e)==0x03ff,"coefficients 17..31 saturés à 16");
    m.write16(arm9,pal(e)+4,red);
    check(pixel(m,e)==red,"saturation de la somme des canaux");
    m.write16(arm9,pal(e)+4,green); m.write16(arm9,port(e)+0x52,0);
    check(pixel(m,e)==0,"mélange de coefficients nuls");
    m.write16(arm9,port(e)+0x52,0x0808); m.write16(arm9,port(e)+0x50,0x2041);
    check(pixel(m,e)==red,"ne pas sauter une couche non sélectionnée comme seconde cible");
    m.write16(arm9,port(e)+0x50,0x0242);
    check(pixel(m,e)==red,"première cible masquée par une couche non sélectionnée");
    m.write16(arm9,port(e)+0x50,0x0241); m.write16(arm9,port(e)+10,0x1102); m.write16(arm9,port(e)+12,0x1201);
    m.write32(arm9,port(e),0x00010700);
    check(pixel(m,e)==red,"BG2 bloque le mélange BG0 vers BG1");
    m.write16(arm9,port(e)+0x50,0x0441);
    check(pixel(m,e)==0x3c0f,"seconde couche BG2 conservée malgré ordre des passes");
    m.write32(arm9,port(e),0x00010100); m.write16(arm9,port(e)+0x50,0x2041);
    check(pixel(m,e)==0x3c0f,"backdrop comme seconde cible");
    m.write16(arm9,port(e)+0x50,0x0081); m.write16(arm9,port(e)+0x54,8);
    check(pixel(m,e)==0x3dff,"éclaircissement RGB555");
    m.write16(arm9,port(e)+0x50,0x00c1);
    check(pixel(m,e)==0x0010,"assombrissement RGB555");
    m.write16(arm9,port(e)+0x54,31);
    check(pixel(m,e)==0,"luminosité saturée à 16");
    m.write16(arm9,port(e)+0x50,0x0081);
    check(pixel(m,e)==0x7fff,"éclaircissement maximum");
    m.write32(arm9,port(e),0x00010000); m.write16(arm9,port(e)+0x50,0x00a0);
    check(pixel(m,e)==0x7fff,"backdrop comme première cible luminosité");
    m.write16(arm9,port(e)+0x50,0x2060); m.write16(arm9,port(e)+0x52,0x0808);
    check(pixel(m,e)==blue,"ne pas mélanger le backdrop avec lui-même");
    m.write32(arm9,port(e),0x00012300); m.write16(arm9,port(e)+0x50,0x0241);
    m.write16(arm9,port(e)+0x40,0x0010); m.write16(arm9,port(e)+0x44,0x0010);
    m.write16(arm9,port(e)+0x48,3); m.write16(arm9,port(e)+0x4a,0x23);
    check(pixel(m,e)==red && pixel(m,e,16)==0x01ef,"fenêtre désactive uniquement les effets");
}

void object_effects_test(unsigned e) {
    Memory m; setup(m);
    m.write32(arm9,port(e),0x00011110); sprite(m,e,0,0x0400,0,0);
    m.write16(arm9,port(e)+0x50,0x0100); m.write16(arm9,port(e)+0x52,0x0808);
    check(pixel(m,e)==0x3c0f,"OBJ semi-transparent force alpha sans effet/1re cible dans BLDCNT");
    m.write16(arm9,port(e)+0x50,0x0190); m.write16(arm9,port(e)+0x54,16);
    check(pixel(m,e)==0x3c0f,"semi-transparence prioritaire sur luminosité");
    m.write16(arm9,port(e)+0x50,0x0090);
    check(pixel(m,e)==0x7fff,"luminosité OBJ quand aucune seconde cible n'est visible");
    sprite(m,e,0,0,0,0); m.write16(arm9,port(e)+0x50,0x0150);
    check(pixel(m,e)==0x3c0f,"OBJ normal comme première cible alpha");
    sprite(m,e,0,0x0400,0,0);
    sprite(m,e,1,0,0,1); m.write16(arm9,port(e)+0x50,0x1100);
    check(pixel(m,e)==0x3c0f,"OBJ inférieur exclu du mélange");
    m.write16(arm9,port(e)+0x50,0x1000);
    check(pixel(m,e)==blue,"pas de mélange OBJ vers OBJ");
    sprite(m,e,0,0,0,0x0400); sprite(m,e,1,0x200,0,0);
    m.write16(arm9,port(e)+0x50,0x1041);
    check(pixel(m,e)==0x3c0f,"mélange BG vers OBJ situé dessous");
    m.write32(arm9,port(e),0x00013110); sprite(m,e,0,0x0400,0,0);
    m.write16(arm9,port(e)+0x40,0x0004); m.write16(arm9,port(e)+0x44,0x0008);
    m.write16(arm9,port(e)+0x48,0x11); m.write16(arm9,port(e)+0x4a,0x31);
    m.write16(arm9,port(e)+0x50,0x0100);
    check(pixel(m,e)==blue && pixel(m,e,4)==0x3c0f,"masque d'effets sur OBJ semi-transparent");
    m.write16(arm9,port(e)+0x48,0x21);
    check(pixel(m,e)==red,"masque de visibilité OBJ");
}

void object_windows_test(unsigned e) {
    Memory m; setup(m);
    m.write32(arm9,port(e),0x00019310); // OBJ et OBJWIN
    sprite(m,e,0,0x0800,0,0x0c00); // fenêtre de priorité basse
    sprite(m,e,1,0,0,1); // OBJ normal devant, ne doit pas effacer la fenêtre
    m.write16(arm9,port(e)+0x4a,0x0132); // intérieur rouge sans OBJ, extérieur vert + OBJ
    m.write16(arm9,obj(e),0x1110); // trou transparent dans le masque
    auto frame=m.render_frame();
    check(screen(frame,e)[0]==green && screen(frame,e)[1]==red && screen(frame,e)[8]==green,"OBJWIN : couverture, transparence, absence de couleur et priorité indépendante");
    m.write16(arm9,port(e)+0x40,0x0004); m.write16(arm9,port(e)+0x44,0x0008); m.write16(arm9,port(e)+0x48,2);
    m.write32(arm9,port(e),0x0001b310);
    check(pixel(m,e,1)==green && pixel(m,e,4)==red,"WIN0 prioritaire sur OBJWIN");
    m.write16(arm9,port(e)+0x42,0x0408); m.write16(arm9,port(e)+0x46,0x0008);
    m.write16(arm9,port(e)+0x48,0x0202); m.write32(arm9,port(e),0x0001f310);
    check(pixel(m,e,5)==green,"WIN1 prioritaire sur OBJWIN");
    sprite(m,e,0,0x0900,0x0010,0x0c00); // fenêtre affine identité à x=16
    m.write16(arm9,oam(e)+6,0x100); m.write16(arm9,oam(e)+30,0x100);
    check(pixel(m,e,17)==red && pixel(m,e,16)==green,"OBJWIN affine et trou de source");
    m.write32(arm9,port(e),0x00018310); // OBJ désactivés : pas de masque
    m.write16(arm9,port(e)+0x4a,0x0102);
    check(pixel(m,e,17)==green,"OBJWIN nécessite OBJ et OBJWIN activés");
    m.write32(arm9,port(e),0x80019310); sprite(m,e,0,0x2800,16,2); m.write16(arm9,obj(e)+64,1);
    check(pixel(m,e,16)==red && pixel(m,e,17)==green,"OBJWIN 8 bpp ignore la palette étendue et les indices transparents");
}

void bitmap_objects_test(unsigned e) {
    Memory m; setup(m);
    m.write32(arm9,port(e),0x00011140); // bitmap 1D, tuiles 2D indépendantes
    sprite(m,e,0,0x0c00,0,0x7001); // alpha 8/16, tile 1 = +128
    for(unsigned i=0;i<64;++i) m.write16(arm9,obj(e)+128+i*2,static_cast<std::uint16_t>(0x8000|blue));
    m.write16(arm9,obj(e)+128,blue); m.write16(arm9,port(e)+0x50,0x0100); m.write16(arm9,port(e)+0x52,0x1010);
    check(pixel(m,e)==red && pixel(m,e,1)==0x3c0f && pixel(m,e,7,7)==0x3c0f,"bitmap OBJ : bit15, stride 1D et alpha OAM indépendant de BLDALPHA");
    sprite(m,e,0,0x0c00,0,0x0001);
    check(pixel(m,e,1)==red,"alpha OAM zéro masque tout le bitmap");
    sprite(m,e,0,0x0c00,0,0xf001);
    check(pixel(m,e,1)==blue,"alpha OAM 15 devient opaque");
    m.write16(arm9,port(e)+0x50,0);
    sprite(m,e,0,0x0c00,0,0x1001);
    check(pixel(m,e,1)==blue,"bitmap sans seconde cible visible reste opaque");
    // Les bits de frontière des OBJ tuilés ne modifient pas la source bitmap.
    m.write32(arm9,port(e),0x00211150);
    check(pixel(m,e,1)==blue,"mappings tuilé et bitmap indépendants");
    if(e==0) {
        m.write32(arm9,port(e),0x00411140); m.write16(arm9,obj(e)+258,static_cast<std::uint16_t>(0x8000|green));
        check(pixel(m,e,1)==green,"frontière bitmap 1D 256 octets moteur A");
    }
    for(unsigned dimension=0;dimension<2;++dimension) {
        m.write32(arm9,port(e),0x00011100|(dimension?0x20U:0));
        const unsigned tile=dimension?33:17, base=dimension?0x1010:0x810, stride=dimension?256:128;
        sprite(m,e,0,0x0c00,0,static_cast<std::uint16_t>(0xf000|tile));
        m.write16(arm9,obj(e)+base,static_cast<std::uint16_t>(0x8000|green));
        m.write16(arm9,obj(e)+base+stride*2+14,static_cast<std::uint16_t>(0x8000|blue));
        check(pixel(m,e)==green && pixel(m,e,7,1)==blue,"bitmap OBJ 2D : adresse tile X/Y et stride 128/256");
        sprite(m,e,0,0x0c00,0x3000,static_cast<std::uint16_t>(0xf000|tile));
        check(pixel(m,e,0,6)==blue && pixel(m,e,7,7)==green,"flips H/V bitmap OBJ");
    }
    m.write32(arm9,port(e),0x00011140); sprite(m,e,0,0x0d00,0,0xf001);
    m.write16(arm9,oam(e)+6,0x100); m.write16(arm9,oam(e)+30,0x100);
    check(pixel(m,e,1)==blue,"bitmap OBJ affine identité");
    sprite(m,e,0,0x0c00,0x4000,0xf002); // bitmap 16x16 contigu
    m.write16(arm9,obj(e)+256+16,static_cast<std::uint16_t>(0x8000|green));
    m.write16(arm9,obj(e)+256+256,static_cast<std::uint16_t>(0x8000|blue));
    check(pixel(m,e,8)==green && pixel(m,e,0,8)==blue,"bitmap 1D 16x16 contigu, sans organisation en tuiles");
    m.write32(arm9,port(e),0x00011160);
    expect_failure<ravenemu::Error>([&]{static_cast<void>(m.render_frame());},"mapping bitmap réservé silencieux");
}

void integration_test() {
    Memory m; setup(m);
    for(unsigned e=0;e<2;++e) {
        m.write32(arm9,port(e),0x00012300); m.write16(arm9,port(e)+0x40,0x0820); m.write16(arm9,port(e)+0x44,0x1028);
        m.write16(arm9,port(e)+0x48,0x23); m.write16(arm9,port(e)+0x4a,2);
        // Un programme ARM publie BLDCNT et BLDALPHA en une écriture 32 bits.
        m.write32(arm9,0x02000000,0xe5801000); m.write32(arm9,0x02000004,0xeafffffe);
        ravenemu::nds::Cpu cpu{arm9}; cpu.reset(0x02000000,false);
        cpu.set_register(0,port(e)+0x50); cpu.set_register(1,e?0x040c0241:0x08080241);
        cpu.step(m);
        check(cpu.state().instructions==1 && m.read32(arm9,port(e)+0x50)==(e?0x040c0241U:0x08080241U),"STR ARM vers registres de mélange");
    }
    const auto frame=m.render_frame();
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x) {
        const bool inside=x>=8&&x<32&&y>=16&&y<40;
        check(frame.upper[y*256+x]==(inside?0x01ef:green) && frame.lower[y*256+x]==(inside?0x00f7:green),"composition complète A/B configurée par ARM");
    }
    auto copy=m; copy.write16(arm9,port(1)+0x52,0x0010);
    check(pixel(copy,1,8,16)==red && pixel(m,1,8,16)==0x00f7,"copie indépendante des effets");
    m.write16(arm9,0x04000304,0x0203);
    const auto swapped=m.render_frame();
    check(swapped.upper==frame.lower && swapped.lower==frame.upper,"permutation des écrans après composition");
    m.write32(arm9,port(0),0x00012380);
    check(m.render_frame().lower[16*256+8]==0x7fff,"forced blank prioritaire sur fenêtres et mélange");
    m.write8(arm9,0x04000240,0x80); m.write16(arm9,0x06800000,green); m.write32(arm9,port(0),0x00022300);
    check(m.render_frame().lower[0]==green,"affichage VRAM direct indépendant des fenêtres et effets 2D");
}
} // namespace

int main() {
    registers_test();
    for(unsigned e=0;e<2;++e) {
        windows_test(e); blending_test(e); object_effects_test(e); object_windows_test(e); bitmap_objects_test(e);
    }
    integration_test();
}
