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
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t power = 0x04000304;
constexpr std::uint16_t backdrop = 0x6b5a, red = 0x001f, green = 0x03e0, blue = 0x7c00;

namespace {
std::uint32_t port(unsigned e) { return 0x04000000 + e * 0x1000; }
std::uint32_t bg(unsigned e) { return 0x06000000 + e * 0x200000; }
std::uint32_t obj(unsigned e) { return 0x06400000 + e * 0x200000; }
std::uint32_t pal(unsigned e) { return 0x05000000 + e * 0x400; }
std::uint32_t oam(unsigned e) { return 0x07000000 + e * 0x400; }
const auto& screen(const VideoFrame& frame, unsigned e) { return e ? frame.lower : frame.upper; }
unsigned pattern(unsigned x, unsigned y) { return 1 + (x & 7U) + (y & 7U) * 8; }
void sprite(Memory& m, unsigned e, unsigned n, std::uint16_t a0, std::uint16_t a1, std::uint16_t a2) {
    m.write16(arm9,oam(e)+n*8,a0); m.write16(arm9,oam(e)+n*8+2,a1); m.write16(arm9,oam(e)+n*8+4,a2);
}
template<class Pattern>
void tile(Memory& m, std::uint32_t address, Pattern source) {
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;x+=2)
        m.write16(arm9,address+y*8+x,static_cast<std::uint16_t>(source(x,y)|(source(x+1,y)<<8U)));
}
void setup(Memory& m) {
    m.write16(arm9,power,0x8203); m.write32(arm9,0x04000240,0x84848281);
    for(unsigned e=0;e<2;++e) {
        m.write16(arm9,pal(e),backdrop);
        for(unsigned i=1;i<256;++i) {
            m.write16(arm9,pal(e)+i*2,static_cast<std::uint16_t>(i));
            m.write16(arm9,pal(e)+0x200+i*2,static_cast<std::uint16_t>(i));
        }
        for(unsigned n=0;n<128;++n) sprite(m,e,n,0x200,0,0);
        tile(m,bg(e),pattern); tile(m,obj(e),pattern);
        m.write16(arm9,port(e)+8,0x10c0);
    }
}
template<class Expected>
void check_image(const VideoFrame& frame, unsigned e, Expected expected, const char* message) {
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x)
        check(screen(frame,e)[y*256+x]==expected(x,y),message);
}
void identity(Memory& m, unsigned e) {
    m.write32(arm9,port(e)+0x20,0x100); m.write32(arm9,port(e)+0x24,0x01000000);
}

void registers_and_dma_test() {
    Memory m;
    for(unsigned e=0;e<2;++e) m.write32(arm9,port(e)+0x4c,0xffffffff); // moteurs éteints : ignoré
    setup(m);
    for(unsigned e=0;e<2;++e) {
        m.write32(arm9,port(e),0x10100);
        check(screen(m.render_frame(),e)[1]==2,"écriture MOSAIC moteur éteint a été conservée");
        m.write32(arm9,port(e)+0x4c,0xffffffff);
        check(m.read32(arm9,port(e)+0x4c)==0&&m.peek16(arm9,port(e)+0x4c)==0,"MOSAIC en écriture seule doit se lire zéro");
        check(screen(m.render_frame(),e)[15*256+15]==1,"taille maximale 16x16 non appliquée");
        m.write8(arm9,port(e)+0x4c,0x32); m.write8(arm9,port(e)+0x4d,0);
        m.write16(arm9,port(e)+0x4e,0xffff);
        check(screen(m.render_frame(),e)[3*256+2]==1&&screen(m.render_frame(),e)[4*256+3]==36,"voies d'octets BG et bits réservés MOSAIC");
        expect_failure<ravenemu::Error>([&]{m.write16(arm7,port(e)+0x4c,0);},"ARM7 peut écrire MOSAIC");
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read16(arm7,port(e)+0x4c));},"ARM7 peut lire MOSAIC");
        const std::array<std::uint32_t,3> invalid{0,0,0};
        expect_failure<ravenemu::Error>([&]{m.write_words(arm9,port(e)+0x4c,invalid);},"transfert MOSAIC vers trou accepté");
        check(screen(m.render_frame(),e)[2]==1,"transfert refusé a modifié la mosaïque");
    }
    m.write16(arm9,power,0x8201); m.write16(arm9,port(0)+0x4c,0); m.write16(arm9,port(1)+0x4c,0);
    m.write16(arm9,power,0x8203);
    auto frame=m.render_frame();
    check(frame.upper[1]==1&&frame.lower[1]==2,"protection POWCNT1 des MOSAIC A/B indépendante");
    auto copy=m; copy.write16(arm9,port(0)+0x4c,0);
    check(copy.render_frame().upper[1]==2&&m.render_frame().upper[1]==1,"copie partage MOSAIC");
    m.write16(arm9,0x02001000,3); // DMA immédiat, un demi-mot vers MOSAIC A
    const std::array<std::uint32_t,3> upload{0x02001000,port(0)+0x4c,0x80000001};
    m.write_words(arm9,0x040000b0,upload); m.step_dma(arm9);
    check(m.render_frame().upper[3]==1&&m.render_frame().upper[4]==5,"DMA 16 bits vers MOSAIC");
    // Un DMA 32 bits vers B doit ignorer la moitié réservée sans toucher A.
    m.write32(arm9,0x02001000,0xffff0042);
    const std::array<std::uint32_t,3> upload_b{0x02001000,port(1)+0x4c,0x84000001};
    m.write_words(arm9,0x040000b0,upload_b); m.step_dma(arm9);
    frame=m.render_frame();
    check(frame.upper[3]==1&&frame.lower[2]==1&&frame.lower[3]==4,"DMA 32 bits vers MOSAIC B ou indépendance de A");
    m.clear_ram(); setup(m); m.write32(arm9,port(0),0x10100); m.write32(arm9,port(1),0x10100);
    frame=m.render_frame();
    check(frame.upper[1]==2&&frame.lower[1]==2,"reset conserve une taille de mosaïque");
}

void text_test(unsigned e) {
    Memory m; setup(m); m.write32(arm9,port(e),0x10100);
    m.write16(arm9,port(e)+0x10,3); m.write16(arm9,port(e)+0x12,5);
    for(const auto size : {std::array{1U,1U},std::array{3U,5U},std::array{16U,16U},std::array{16U,1U},std::array{1U,16U}}) {
        m.write16(arm9,port(e)+0x4c,static_cast<std::uint16_t>((size[0]-1)|((size[1]-1)<<4U)));
        check_image(m.render_frame(),e,[&](unsigned x,unsigned y){return pattern(x/size[0]*size[0]+3,y/size[1]*size[1]+5);},
                    "mosaïque texte : grille écran, taille plus un et défilement");
    }
    m.write16(arm9,port(e)+8,0x1080);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return pattern(x+3,y+5);},"BG sans bit mosaïque affecté par MOSAIC");
    m.write16(arm9,port(e)+8,0x10c0); m.write16(arm9,port(e)+0x4c,0x42);
    for(unsigned i=0;i<0x800;i+=2) m.write16(arm9,bg(e)+0x8000+i,0xc00);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return pattern(7-((x/3*3+3)&7U),7-((y/5*5+5)&7U));},
                "mosaïque doit précéder flips de tuile");
    // Les tuiles 4 bpp gardent leur banque standard de palette.
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;x+=4) {
        unsigned word=0;
        for(unsigned i=0;i<4;++i) word|=(1+(x+i+y*3)%15)<<(i*4U);
        m.write16(arm9,bg(e)+y*4+x/2,static_cast<std::uint16_t>(word));
    }
    for(unsigned i=0;i<0x800;i+=2) m.write16(arm9,bg(e)+0x8000+i,0x2000);
    m.write16(arm9,port(e)+8,0x1040);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return 33+(((x/3*3+3)&7U)+((y/5*5+5)&7U)*3)%15;},
                "mosaïque 4 bpp : adresse de texel et banque de palette");
}

void affine_and_bitmap_test(unsigned e) {
    Memory m; setup(m); m.write16(arm9,port(e)+0xc,0x10c0); m.write16(arm9,port(e)+0x4c,0x21);
    m.write32(arm9,port(e),0x10402);
    m.write32(arm9,port(e)+0x20,0x00800100); m.write32(arm9,port(e)+0x24,0x01000080);
    m.write32(arm9,port(e)+0x28,0x0fffff00); m.write32(arm9,port(e)+0x2c,0x100);
    for(const bool wrap : {false,true}) {
        m.write16(arm9,port(e)+0xc,wrap?0x30c0:0x10c0);
        check_image(m.render_frame(),e,[&](unsigned x,unsigned y){
            const auto sx=static_cast<int>(x/2*2+y/3*3/2)-1, sy=static_cast<int>(x/2+y/3*3+1);
            return !wrap&&(sx<0||sx>=128||sy>=128) ? backdrop : pattern(static_cast<unsigned>(sx),static_cast<unsigned>(sy));
        },"affine mosaïque : quantification avant matrice, référence négative et wrap");
    }
    m.write16(arm9,port(e)+0xc,0x3040);
    m.write32(arm9,port(e),0x10405); // même image avec carte étendue 16 bits
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return pattern(x/2*2+y/3*3/2-1,x/2+y/3*3+1);},"mosaïque des tuiles affines étendues");
    identity(m,e); m.write32(arm9,port(e)+0x28,0); m.write32(arm9,port(e)+0x2c,0);
    m.write16(arm9,port(e)+0xc,0x20c0);
    for(unsigned y=0;y<128;++y) for(unsigned x=0;x<128;x+=2)
        m.write16(arm9,bg(e)+y*128+x,static_cast<std::uint16_t>((1+(x+y*5)%255)|((1+(x+1+y*5)%255)<<8U)));
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return 1+((x/2*2%128)+(y/3*3%128)*5)%255;},"mosaïque bitmap indexé");
    m.write16(arm9,port(e)+0xc,0x20c4);
    for(unsigned y=0;y<128;++y) for(unsigned x=0;x<128;++x)
        m.write16(arm9,bg(e)+(y*128+x)*2,static_cast<std::uint16_t>((x+y)%5?0x8000|x|(y<<7U):0));
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){
        const auto sx=x/2*2%128, sy=y/3*3%128;
        return (sx+sy)%5 ? sx+(sy<<7U) : backdrop;
    },"mosaïque RGB555 : réplication du bit de visibilité");
}

void large_bitmap_test() {
    Memory m; setup(m); identity(m,0); m.write16(arm9,port(0)+0xc,0x40); m.write16(arm9,port(0)+0x4c,0x42);
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;x+=2)
        m.write16(arm9,bg(0)+y*512+x,static_cast<std::uint16_t>((1+(x+y*5)%255)|((1+(x+1+y*5)%255)<<8U)));
    m.write32(arm9,port(0),0x10406);
    check_image(m.render_frame(),0,[](unsigned x,unsigned y){return 1+(x/3*3+y/5*5*5)%255;},"mosaïque grand bitmap A");
}

void object_test(unsigned e) {
    Memory m; setup(m); m.write32(arm9,port(e),0x11010);
    for(const auto size : {std::array{1U,1U},std::array{3U,4U},std::array{16U,16U}}) for(const unsigned flips : {0U,0x1000U,0x2000U,0x3000U}) {
        sprite(m,e,0,0x3003,static_cast<std::uint16_t>(5|flips),0);
        m.write16(arm9,port(e)+0x4c,static_cast<std::uint16_t>(((size[0]-1)<<8U)|((size[1]-1)<<12U)));
        check_image(m.render_frame(),e,[&](unsigned x,unsigned y){
            if(x<5||x>=13||y<3||y>=11) return static_cast<unsigned>(backdrop);
            auto sx=std::max(5U,x/size[0]*size[0])-5, sy=std::max(3U,y/size[1]*size[1])-3;
            if(flips&0x1000U) sx=7-sx;
            if(flips&0x2000U) sy=7-sy;
            return pattern(sx,sy);
        },"OBJ : grille écran, blocs partiels, flips et clipping de boîte");
    }
    // 4 bpp, banque standard 1 ; le filtre doit transporter la couleur déjà décodée.
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;x+=4) {
        unsigned word=0;
        for(unsigned i=0;i<4;++i) word|=(1+(x+i+y*3)%15)<<(i*4U);
        m.write16(arm9,obj(e)+y*4+x/2,static_cast<std::uint16_t>(word));
    }
    sprite(m,e,0,0x1003,5,0x1000); m.write16(arm9,port(e)+0x4c,0x3200);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){
        return x>=5&&x<13&&y>=3&&y<11?17+(std::max(5U,x/3*3)-5+(std::max(3U,y/4*4)-3)*3)%15:backdrop;
    },"mosaïque OBJ 4 bpp et banque standard");
    tile(m,obj(e),pattern);
    sprite(m,e,0,0x2003,5,0); // attribut mosaïque désactivé
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return x>=5&&x<13&&y>=3&&y<11?pattern(x-5,y-3):backdrop;},"OBJ sans bit mosaïque affecté");
    sprite(m,e,0,0x30fe,509,0); m.write16(arm9,port(e)+0x4c,0x3200);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return x<5&&y<6?pattern(x/3*3+3,y/4*4+2):backdrop;},"OBJ mosaïque traversant le coin haut gauche");
    sprite(m,e,0,0x3303,4,0); // affine double taille, identité
    m.write16(arm9,oam(e)+6,0x100); m.write16(arm9,oam(e)+14,0); m.write16(arm9,oam(e)+22,0); m.write16(arm9,oam(e)+30,0x100);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return x>=8&&x<16&&y>=8&&y<16?pattern(std::max(8U,x/3*3)-8,y/4*4-7):backdrop;},
                "OBJ affine : ligne mosaïque avant transformation et boîte double");
    sprite(m,e,0,0x3103,5,0); // rotation 90°, pas de double taille
    m.write16(arm9,oam(e)+6,0); m.write16(arm9,oam(e)+14,0xff00); m.write16(arm9,oam(e)+22,0x100); m.write16(arm9,oam(e)+30,0);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){
        if(x<5||x>=13||y<4||y>=11) return static_cast<unsigned>(backdrop);
        return pattern(11-y/4*4,std::max(5U,x/3*3)-5);
    },"OBJ affine : mosaïque globale sur sprite tourné");
    // Palettes OBJ étendues : F sur A, I sur B, palette 3.
    const auto bank_control=e?0x04000249U:0x04000245U, bank_start=e?0x068a0000U:0x06890000U;
    m.write8(arm9,bank_control,0x80);
    for(unsigned index=0;index<66;index+=2)
        m.write32(arm9,bank_start+3*512+index*2,(index+0x400)|((index+0x401)<<16U));
    m.write8(arm9,bank_control,e?0x83:0x85);
    sprite(m,e,0,0x3003,5,0x3000); m.write32(arm9,port(e),0x80011010);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){
        return x>=5&&x<13&&y>=3&&y<11?0x400+pattern(std::max(5U,x/3*3)-5,std::max(3U,y/4*4)-3):backdrop;
    },"mosaïque OBJ 8 bpp avec palette étendue");
}

void object_overlap_test(unsigned e) {
    Memory m; setup(m); m.write32(arm9,port(e),0x11010); m.write16(arm9,port(e)+0x4c,0x0300);
    m.write16(arm9,pal(e)+0x202,red); m.write16(arm9,pal(e)+0x204,green);
    tile(m,obj(e),[](unsigned x,unsigned){return x==1||x==4?0U:1U;});
    sprite(m,e,0,0x3000,0,0);
    auto frame=m.render_frame();
    check(screen(frame,e)[0]==red&&screen(frame,e)[1]==red&&screen(frame,e)[3]==red&&screen(frame,e)[4]==backdrop&&screen(frame,e)[7]==backdrop,
          "mosaïque OBJ doit répéter aussi les texels transparents");
    tile(m,obj(e)+64,[](unsigned,unsigned){return 2U;}); sprite(m,e,1,0x2000,0,0x402);
    frame=m.render_frame();
    check(screen(frame,e)[0]==red&&screen(frame,e)[1]==green&&screen(frame,e)[2]==red&&screen(frame,e)[3]==red&&
          screen(frame,e)[4]==green&&screen(frame,e)[5]==red,"OBJ sans mosaïque révélé par transparence doit interrompre la répétition");
    tile(m,obj(e),[](unsigned,unsigned){return 1U;});
    sprite(m,e,0,0x3000,0,0x400); sprite(m,e,1,0x3000,8,0x402); m.write16(arm9,port(e)+0x4c,0x0500);
    frame=m.render_frame();
    check(screen(frame,e)[8]==red&&screen(frame,e)[11]==red&&screen(frame,e)[12]==green&&screen(frame,e)[16]==backdrop,
          "mosaïque X doit agir après sélection de tous les OBJ, sans dépasser leurs couvertures");
    sprite(m,e,1,0x3000,8,2); frame=m.render_frame();
    check(screen(frame,e)[7]==red&&screen(frame,e)[8]==green,"priorité BG plus haute doit interrompre le bloc OBJ");
    sprite(m,e,1,0x2000,8,0x402);
    check(screen(m.render_frame(),e)[8]==green,"désactivation mosaïque du gagnant OBJ ignorée");
    // Le pixel mémorisé transporte également son alpha bitmap.
    for(unsigned i=0;i<64;++i) {m.write16(arm9,obj(e)+i*2,0x801f);m.write16(arm9,obj(e)+128+i*2,0x83e0);}
    sprite(m,e,0,0x1c00,0,0x7000); sprite(m,e,1,0x1c00,8,0xf001);
    m.write16(arm9,pal(e),blue); m.write32(arm9,port(e),0x11040); m.write16(arm9,port(e)+0x50,0x2000);
    frame=m.render_frame();
    check(screen(frame,e)[8]==0x3c0f&&screen(frame,e)[11]==0x3c0f&&screen(frame,e)[12]==green,"mosaïque OBJ bitmap doit conserver alpha et couleur du pixel source");
}

void windows_and_effects_test(unsigned e) {
    Memory m; setup(m); m.write16(arm9,pal(e)+2,red); m.write16(arm9,pal(e)+4,green); m.write16(arm9,pal(e)+6,blue);
    tile(m,bg(e),[](unsigned,unsigned){return 1U;}); tile(m,bg(e)+64,[](unsigned x,unsigned){return x&1U?3U:2U;});
    for(unsigned i=0;i<0x800;i+=2) m.write16(arm9,bg(e)+0x8800+i,1);
    m.write16(arm9,port(e)+10,0x1181); m.write32(arm9,port(e),0x10300); m.write16(arm9,port(e)+0x4c,0x12);
    m.write16(arm9,port(e)+0x50,0x0241); m.write16(arm9,port(e)+0x52,0x0808);
    auto frame=m.render_frame();
    check(screen(frame,e)[0]==0x01ef&&screen(frame,e)[1]==0x3c0f,"BLDALPHA doit composer chaque pixel après la mosaïque de sa couche");
    m.write32(arm9,port(e),0x12100); m.write16(arm9,port(e)+0x40,0x0104); m.write16(arm9,port(e)+0x44,2);
    m.write16(arm9,port(e)+0x48,1); m.write16(arm9,port(e)+0x4a,0);
    frame=m.render_frame();
    check(screen(frame,e)[0]==backdrop&&screen(frame,e)[1]==red&&screen(frame,e)[3]==red&&screen(frame,e)[4]==backdrop,
          "fenêtre ne doit pas supprimer l'origine de mosaïque d'un pixel autorisé");
    m.write16(arm9,port(e)+0x6c,0x8008);
    check(screen(m.render_frame(),e)[1]==0x0010,"MASTER_BRIGHT doit suivre fenêtre et mosaïque");
    m.write16(arm9,port(e)+0x6c,0); m.write32(arm9,port(e),0x19110); m.write16(arm9,port(e)+0x4a,0x0100);
    tile(m,obj(e),[](unsigned x,unsigned y){return (x&1U)||(y&1U)?0U:1U;}); sprite(m,e,0,0x3803,5,0);
    m.write16(arm9,port(e)+0x4c,0xffff);
    check_image(m.render_frame(),e,[](unsigned x,unsigned y){return x>=5&&x<13&&y>=3&&y<11&&!((x-5)&1U)&&!((y-3)&1U)?red:backdrop;},
                "OBJWIN doit ignorer la mosaïque, y compris l'axe vertical");
}

void put32(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8U));
}
void machine_mosaic_test() {
    std::vector<std::array<std::uint32_t,2>> commands;
    const auto write=[&](std::uint32_t address,std::uint32_t value){commands.push_back({address,value});};
    write(power,0x8203); write(0x04000240,0x84848281);
    for(unsigned e=0;e<2;++e) {
        write(port(e)+8,0x10c0);
        for(unsigned i=0;i<256;i+=2) write(pal(e)+i*2,(i?i:backdrop)|((i+1)<<16U));
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;x+=4) {
            unsigned value=0;
            for(unsigned i=0;i<4;++i) value|=pattern(x+i,y)<<(i*8U);
            write(bg(e)+y*8+x,value);
        }
        write(port(e)+0x4c,e?0x42:0x21); write(port(e),0x10100);
    }
    const auto size=static_cast<std::uint32_t>(0x100+commands.size()*8);
    std::vector<std::uint8_t> rom(0x200+size+4);
    put32(rom,0x20,0x200);put32(rom,0x24,0x02000000);put32(rom,0x28,0x02000000);put32(rom,0x2c,size);
    put32(rom,0x30,0x200+size);put32(rom,0x34,0x03800000);put32(rom,0x38,0x03800000);put32(rom,0x3c,4);
    put32(rom,0x200+size,0xeafffffe); // ARM7 B .
    put32(rom,0x200,0xe59f0018);put32(rom,0x204,0xe59f3018); // LDR r0/r3 : table et nombre
    put32(rom,0x208,0xe8b00006);put32(rom,0x20c,0xe5812000); // LDMIA r0!,{r1,r2}, STR r2,[r1]
    put32(rom,0x210,0xe2533001);put32(rom,0x214,0x1afffffb);put32(rom,0x218,0xeafffffe); // SUBS, BNE, B .
    put32(rom,0x220,0x02000100);put32(rom,0x224,static_cast<std::uint32_t>(commands.size()));
    for(std::size_t i=0;i<commands.size();++i) {put32(rom,0x300+i*8,commands[i][0]);put32(rom,0x304+i*8,commands[i][1]);}
    ravenemu::nds::Machine machine; machine.load_cartridge(ravenemu::nds::Cartridge{rom}); machine.run_steps(arm9,2+commands.size()*4);
    const auto cpu=machine.cpu_state(arm9); const auto frame=machine.render_frame();
    check_image(frame,0,[](unsigned x,unsigned y){return pattern(x/2*2,y/3*3);},"programme ARM : mosaïque A");
    check_image(frame,1,[](unsigned x,unsigned y){return pattern(x/3*3,y/5*5);},"programme ARM : mosaïque B");
    check(machine.cpu_state(arm9)==cpu&&machine.memory().read16(arm9,0x04000006)==0,"rendu avance CPU ou LCD");
    machine.memory().write16(arm9,power,0x0203);
    check(machine.render_frame().lower[2]==3&&machine.render_frame().upper[2]==1,"mosaïque ne suit pas le moteur après permutation");
    auto copy=machine; copy.memory().write16(arm9,port(0)+0x4c,0);
    check(copy.render_frame().lower[1]==2&&machine.render_frame().lower[1]==1,"copie de machine partage la mosaïque");
    auto invalid=rom; put32(invalid,0x34,0x04000000);put32(invalid,0x38,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&]{machine.load_cartridge(ravenemu::nds::Cartridge{invalid});},"chargement invalide accepté");
    check(machine.render_frame().lower[1]==1,"chargement refusé perd MOSAIC");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    setup(machine.memory()); machine.memory().write32(arm9,port(0),0x10100); machine.memory().write32(arm9,port(1),0x10100);
    check(machine.render_frame().upper[1]==2&&machine.render_frame().lower[1]==2,"chargement réussi conserve MOSAIC");
}
} // namespace

int main() {
    registers_and_dma_test(); large_bitmap_test();
    for(unsigned e=0;e<2;++e) {text_test(e);affine_and_bitmap_test(e);object_test(e);object_overlap_test(e);windows_and_effects_test(e);}
    machine_mosaic_test();
}
