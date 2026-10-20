#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::nds::LcdTiming;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t capture = 0x04000064, power = 0x04000304, enabled = 0x80000000;
constexpr auto line_cycles = LcdTiming::line_cycles, frame_cycles = LcdTiming::frame_cycles;
constexpr std::uint16_t red = 0x001f, green = 0x03e0, blue = 0x7c00, sentinel = 0xdace;

namespace {
std::uint32_t vram(unsigned bank) { return 0x06800000 + bank * 0x20000; }
std::uint16_t pattern(unsigned bank,unsigned pixel) {
    return static_cast<std::uint16_t>(((pixel*37+11+bank*0x111)&0x7fffU)|((pixel&4U)?0x8000U:0));
}
void setup(Memory& m) {
    m.write16(arm9,power,0x8203); m.write32(arm9,0x04000240,0x80808080);
    m.write16(arm9,0x05000000,red); m.write16(arm9,0x05000400,green);
    for(unsigned n=0;n<128;++n) m.write16(arm9,0x07000000+n*8,0x200);
}
void fill(Memory& m,unsigned bank,std::uint16_t color) {
    const auto word=static_cast<std::uint32_t>(color)*0x10001U;
    for(unsigned offset=0;offset<0x20000;offset+=4) m.write32(arm9,vram(bank)+offset,word);
}
void source_pattern(Memory& m,unsigned bank) {
    for(unsigned pixel=0;pixel<65536;pixel+=2)
        m.write32(arm9,vram(bank)+pixel*2,pattern(bank,pixel)|(static_cast<std::uint32_t>(pattern(bank,pixel+1))<<16U));
}
void registers_test() {
    Memory m; m.write32(arm9,capture,0xffffffff);
    check(m.read32(arm9,capture)==0xef3f1f1f,"masque DISPCAPCNT ou écriture moteur éteint incorrect");
    m.write8(arm9,capture,0xa8); m.write8(arm9,capture+1,0xf0); m.write16(arm9,capture+2,0x1234);
    check(m.peek32(arm9,capture)==0x02341008,"voies d'octets et demi-mots DISPCAPCNT");
    const std::array<std::uint32_t,2> invalid{0,0};
    expect_failure<ravenemu::Error>([&]{m.write_words(arm9,capture,invalid);},"capture puis FIFO non implémentée acceptée");
    check(m.read32(arm9,capture)==0x02341008,"transfert refusé a modifié DISPCAPCNT");
    for(const auto address : {capture,0x04001064U,0x04000060U,0x04000068U}) {
        if(address!=capture) expect_failure<ravenemu::Error>([&]{m.write32(arm9,address,0);},"port 3D/FIFO/B mappé par erreur");
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read32(arm7,address));},"capture accessible ARM7");
    }
    auto copy=m; copy.write32(arm9,capture,0);
    check(m.read32(arm9,capture)==0x02341008,"copie partage le registre de capture");
    m.clear_ram(); check(m.read32(arm9,capture)==0,"reset conserve DISPCAPCNT");
    setup(m); m.write32(arm9,0x02001000,0x80330000);
    const std::array<std::uint32_t,3> upload{0x02001000,capture,0x84000001};
    m.write_words(arm9,0x040000b0,upload); m.step_dma(arm9);
    check(m.read32(arm9,capture)==0x80330000,"DMA vers DISPCAPCNT");
}

void sizes_banks_and_offsets_test() {
    constexpr std::array<unsigned,4> widths{128,256,256,256}, heights{128,64,128,192};
    for(unsigned size=0;size<4;++size) {
        Memory m; setup(m); const auto dst=size, src=(size+1)%4, write_slot=size==3?3:3-size, write_offset=write_slot*0x8000;
        fill(m,dst,sentinel); source_pattern(m,src);
        // La lecture garde un stride 256, même lorsque la destination est large de 128.
        const bool display_vram=(size&1U)!=0;
        const auto read_offset=display_vram?0U:0x18000U;
        m.write32(arm9,0x04000000,(src<<18U)|(display_vram?0x20000U:0U));
        m.write16(arm9,power,0); // la capture VRAM est indépendante des moteurs et des LCD
        m.write16(arm9,0x0400006c,0xc000); // MASTER_BRIGHT n'appartient pas au chemin de capture
        m.write32(arm9,capture,enabled|0x20000000U|0x0c000000U|(size<<20U)|(dst<<16U)|(write_slot<<18U));
        m.advance_cycles(static_cast<std::uint64_t>(heights[size])*line_cycles);
        check(static_cast<bool>(m.read32(arm9,capture)&enabled)==(heights[size]<192),"busy doit durer jusqu'à la ligne 192 indépendamment de la taille");
        std::vector<std::uint16_t> expected(65536,sentinel);
        for(unsigned y=0;y<heights[size];++y) for(unsigned x=0;x<widths[size];++x)
            expected[(write_offset/2+y*widths[size]+x)%65536]=pattern(src,(read_offset/2+y*256+x)%65536);
        for(unsigned pixel=0;pixel<expected.size();++pixel)
            check(m.read16(arm9,vram(dst)+pixel*2)==expected[pixel],"taille, banque, offset, stride ou wrap de capture incorrect");
        m.advance_cycles(static_cast<std::uint64_t>(192-heights[size])*line_cycles);
        check(!(m.read32(arm9,capture)&enabled),"busy non effacé à VBlank");
        m.write16(arm9,vram(dst),0x1234); m.advance_cycles(frame_cycles);
        check(m.read16(arm9,vram(dst))==0x1234,"capture ponctuelle réexécutée à l'image suivante");
    }
}

void scheduling_and_remapping_test() {
    Memory m; setup(m); fill(m,3,sentinel); source_pattern(m,0);
    m.advance_cycles(7*line_cycles+100); m.write32(arm9,capture,0xa0130000); // B, 256x64, destination D
    m.advance_cycles(185*line_cycles-100);
    check(m.read32(arm9,capture)&enabled,"capture armée en cours d'image effacée avant son démarrage");
    check(m.read16(arm9,vram(3))==sentinel,"capture a commencé en milieu d'image");
    m.advance_cycles(71*line_cycles); m.advance_cycles(0);
    const auto frame=m.render_frame(); static_cast<void>(frame);
    check(m.read16(arm9,vram(3))==sentinel,"inspection ou début de ligne zéro écrit des pixels trop tôt");
    m.advance_cycles(1535); check(m.read16(arm9,vram(3))==sentinel,"capture avant fin de ligne visible");
    m.advance_cycles(1); check(m.read16(arm9,vram(3))==pattern(0,0),"première ligne pas capturée à HBlank");
    m.write16(arm9,vram(0)+512,0x87e0); m.advance_cycles(line_cycles);
    check(m.read16(arm9,vram(3)+512)==0x87e0,"capture ne voit pas les données modifiées entre deux lignes");
    m.write8(arm9,0x04000243,0); m.advance_cycles(line_cycles); // destination non LCDC : aucune écriture
    m.write8(arm9,0x04000243,0x80);
    check(m.read16(arm9,vram(3)+1024)==sentinel,"capture écrit dans une banque destination non allouée LCDC");
    m.write8(arm9,0x04000240,0x81); m.advance_cycles(line_cycles);
    check(m.read16(arm9,vram(3)+1536)==0,"source VRAM non allouée LCDC doit fournir zéro");
    m.write32(arm9,capture,0); m.advance_cycles(10*line_cycles);
    check(m.read16(arm9,vram(3)+2048)==sentinel,"capture désactivée continue d'écrire");
    m.clear_ram(); setup(m); fill(m,3,sentinel);
    // Même banque en entrée et sortie, destination décalée : les anciennes lignes deviennent des sources futures.
    source_pattern(m,0); m.write16(arm9,vram(0)+0x10000,0x1234); m.write32(arm9,capture,0xa0340000);
    m.advance_cycles(192*line_cycles);
    check(m.read16(arm9,vram(0)+0x8000)==pattern(0,0)&&m.read16(arm9,vram(0)+0x10000)==pattern(0,0)&&m.read16(arm9,vram(0)+0x18000)==pattern(0,0),
          "capture doit lire la VRAM à chaque ligne, y compris en rétroaction");
}

void blend_test() {
    for(const unsigned mode : {2U,3U}) for(const auto factors : {std::array{8U,8U},std::array{0U,8U},std::array{0U,0U},std::array{31U,31U}}) {
        Memory m; setup(m); m.write16(arm9,vram(0),0x83e0); m.write16(arm9,vram(0)+2,0x03e0);
        m.write32(arm9,capture,enabled|(mode<<29U)|0x00130000U|factors[0]|(factors[1]<<8U));
        m.advance_cycles(1536);
        const auto first=m.read16(arm9,vram(3)), second=m.read16(arm9,vram(3)+2);
        if(factors[0]==8) check(first==0x81ef&&second==0x800f,"mélange de capture ou masque alpha B");
        else if(factors[1]==8) check(first==0x81e0&&second==0,"alpha destination avec coefficient A nul");
        else if(factors[0]==0) check(first==0&&second==0,"coefficients nuls doivent produire une capture transparente");
        else check(first==0x83ff&&second==0x801f,"coefficients capture supérieurs à 16 ou saturation RGB555");
    }
    Memory m; setup(m); m.write16(arm9,vram(0),0x03e0);
    m.write32(arm9,capture,0xa1130000); // B seul, drapeau 3D A inutilisé
    m.advance_cycles(1536);
    check(m.read16(arm9,vram(3))==0x03e0,"copie B doit conserver RGB même si alpha nul et ignorer les options A");
    m.clear_ram(); setup(m); m.write32(arm9,capture,0x82130000); // A seul, option FIFO B inutilisée
    m.advance_cycles(1536);
    check(m.read16(arm9,vram(3))==0x801f,"copie A doit ignorer coefficients et options B inutilisées");
}

void source_a_before_display_test() {
    Memory m; setup(m); m.write32(arm9,0x04000000,0x00060000); // VRAM B sur écran A
    m.write16(arm9,vram(1),0x801f); m.write16(arm9,0x05000000,blue); // composition 2D bleue
    m.write16(arm9,power,0x0203); // écran A en bas
    m.write16(arm9,0x0400006c,0x8010); m.write32(arm9,0x04001000,0x10007); // moteur B invalide, non évalué
    m.write32(arm9,capture,0x80130000); m.advance_cycles(1536);
    check(m.read16(arm9,vram(3))==0xfc00,"capture A doit précéder sélection VRAM, permutation et MASTER_BRIGHT et ignorer B");
    m.write32(arm9,capture,0); m.advance_cycles(frame_cycles-1536);
    m.write32(arm9,0x04000000,0x80); m.write32(arm9,capture,0x80130000); m.advance_cycles(1536);
    check(m.read16(arm9,vram(3))==0xffff,"forced blank doit fournir du blanc à la capture 2D");
}

void graphics_line_and_redisplay_test() {
    Memory m; setup(m); m.write8(arm9,0x04000240,0x81); m.write8(arm9,0x04000241,0x82);
    // BG2 RGB555 256x256, avec transparences et mosaïque ; OBJ affine agrandi et wrappé.
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x)
        m.write16(arm9,0x06000000+(y*256+x)*2,static_cast<std::uint16_t>((x+y)%7?0x8000|((x+y*257)&0x7fffU):0));
    for(unsigned i=0;i<256;++i) m.write16(arm9,0x05000200+i*2,static_cast<std::uint16_t>(i*97&0x7fffU));
    for(unsigned i=0;i<4096;i+=2) m.write16(arm9,0x06400000+i,static_cast<std::uint16_t>((i/2)%5?0x0101:0x0202));
    m.write16(arm9,0x07000000,0x33b4); m.write16(arm9,0x07000002,0xc0fa);
    m.write16(arm9,0x07000006,0x80); m.write16(arm9,0x0700001e,0x80);
    m.write16(arm9,0x0400000c,0x40c4); m.write32(arm9,0x04000020,0x100); m.write32(arm9,0x04000024,0x01000000);
    m.write32(arm9,0x0400004c,0x3221); m.write16(arm9,0x04000040,0x1496); m.write16(arm9,0x04000044,0x0a96);
    m.write16(arm9,0x04000048,0x34); m.write16(arm9,0x0400004a,0x14);
    m.write16(arm9,0x04000050,0x84); m.write16(arm9,0x04000054,8); m.write32(arm9,0x04000000,0x13415);
    const auto expected=m.render_frame().upper;
    m.write16(arm9,0x0400006c,0x8010); m.write32(arm9,capture,0x80320000); // C, 256x192, capture avant assombrissement
    m.advance_cycles(192*line_cycles);
    for(unsigned pixel=0;pixel<expected.size();++pixel)
        check(m.read16(arm9,vram(2)+pixel*2)==(expected[pixel]|0x8000U),"composition ligne capturée différente de l'instantané 2D");
    // Réaffecter C au moteur B, puis afficher la capture en BG2 direct.
    m.write8(arm9,0x04000242,0x84); m.write16(arm9,0x0400100c,0x4084);
    m.write32(arm9,0x04001020,0x100); m.write32(arm9,0x04001024,0x01000000); m.write32(arm9,0x04001000,0x10405);
    check(m.render_frame().lower==expected,"réaffichage de capture sur moteur B après réaffectation VRAM");
}

void clocks_copy_and_failures_test() {
    Memory m; setup(m); fill(m,3,sentinel); m.write32(arm9,capture,0x81330000); // 3D non implémentée
    m.write32(arm9,0x04000100,0x00800000); m.advance_cycles(1535);
    expect_failure<ravenemu::Error>([&]{m.advance_cycles(1);},"source 3D de capture ignorée");
    check(m.read16(arm9,0x04000100)==1535&&m.read16(arm9,vram(3))==sentinel,"capture refusée a avancé l'horloge ou écrit des pixels");
    m.write32(arm9,capture,0xa2330000);
    expect_failure<ravenemu::Error>([&]{m.advance_cycles(1);},"source FIFO de capture ignorée");
    m.write32(arm9,capture,0xa0330000); source_pattern(m,0); m.advance_cycles(1);
    auto split=m, clock_only=m; clock_only.write32(arm9,capture,0);
    const auto count=std::numeric_limits<std::uint64_t>::max();
    m.advance_cycles(count); clock_only.advance_cycles(count);
    split.advance_cycles(17*line_cycles); split.advance_cycles(count-17*line_cycles);
    for(unsigned pixel=0;pixel<256*192;pixel+=29)
        check(m.read16(arm9,vram(3)+pixel*2)==split.read16(arm9,vram(3)+pixel*2),"capture groupée et fractionnée divergent");
    for(const auto address : {0x04000004U,0x04000006U,0x04000100U})
        check(m.read16(arm9,address)==clock_only.read16(arm9,address)&&m.read16(arm9,address)==split.read16(arm9,address),"capture a modifié les cycles timer/LCD sur grand intervalle");
    check(!(m.read32(arm9,capture)&enabled)&&split.read32(arm9,capture)==m.read32(arm9,capture),"busy ou copie du traitement de capture incorrect");
    m.clear_ram(); setup(m); m.write32(arm9,capture,0x80130000);
    const std::array<std::uint32_t,3> dma{vram(3),0x02001000,0x94000001};
    m.write_words(arm9,0x040000b0,dma); m.advance_cycles(1536);
    check(m.dma_pending(arm9)&&m.read32(arm9,0x02001000)==0,"capture a exécuté le DMA au lieu de l'armer");
    m.step_dma(arm9); check(m.read32(arm9,0x02001000)==0x801f801f,"DMA HBlank ne voit pas les pixels capturés");
}

void put32(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8U));
}
void machine_program_test() {
    constexpr std::array<std::array<std::uint32_t,2>,5> commands{{
        {power,0x8203},{0x04000240,0x80808080},{0x05000000,green},{0x04000000,0x10000},{capture,0x80330000}}};
    constexpr unsigned size=0x100+commands.size()*8;
    std::vector<std::uint8_t> rom(0x200+size+4);
    put32(rom,0x20,0x200); put32(rom,0x24,0x02000000); put32(rom,0x28,0x02000000); put32(rom,0x2c,size);
    put32(rom,0x30,0x200+size); put32(rom,0x34,0x03800000); put32(rom,0x38,0x03800000); put32(rom,0x3c,4);
    put32(rom,0x200+size,0xeafffffe);
    put32(rom,0x200,0xe59f0078); put32(rom,0x204,0xe59f3078); // LDR r0/r3 : table et nombre
    put32(rom,0x208,0xe8b00006); put32(rom,0x20c,0xe5812000); // LDMIA, STR : configuration
    put32(rom,0x210,0xe2533001); put32(rom,0x214,0x1afffffb); // SUBS, BNE
    put32(rom,0x218,0xe59f4068); put32(rom,0x21c,0xe5945000); // LDR r4,=DISPCAPCNT ; lecture état
    put32(rom,0x220,0xe3150102); put32(rom,0x224,0x1afffffc); // TST r5,#80000000 ; BNE lecture
    put32(rom,0x228,0xe59f605c); put32(rom,0x22c,0xe5865000); put32(rom,0x230,0xeafffffe); // publier état terminé puis B .
    put32(rom,0x280,0x02000100); put32(rom,0x284,commands.size()); put32(rom,0x288,capture); put32(rom,0x28c,0x02001000);
    for(unsigned i=0;i<commands.size();++i) {put32(rom,0x300+i*8,commands[i][0]);put32(rom,0x304+i*8,commands[i][1]);}
    ravenemu::nds::Machine machine; machine.load_cartridge(ravenemu::nds::Cartridge{rom}); machine.run_steps(arm9,2+4*commands.size()+10);
    check(machine.memory().read32(arm9,0x02001000)==0&&machine.memory().read32(arm9,capture)&enabled,"programme ARM ne reste pas en attente de capture");
    machine.advance_cycles(1536); auto copy=machine;
    copy.memory().write16(arm9,0x05000000,red); copy.advance_cycles(line_cycles);
    check(copy.memory().read16(arm9,vram(3)+512)==0x801f&&machine.memory().read16(arm9,vram(3)+512)==0,"copie partage capture active ou pixels");
    auto invalid=rom; put32(invalid,0x34,0x04000000); put32(invalid,0x38,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&]{machine.load_cartridge(ravenemu::nds::Cartridge{invalid});},"programme invalide accepté");
    check(machine.memory().read16(arm9,vram(3))==0x83e0&&machine.memory().read32(arm9,capture)&enabled,"chargement refusé détruit capture en cours");
    machine.advance_cycles(192*line_cycles-1536); machine.run_steps(arm9,10);
    check(machine.memory().read32(arm9,0x02001000)==0x00330000,"programme ARM n'observe pas l'effacement busy");
    for(unsigned pixel=0;pixel<256*192;++pixel) check(machine.memory().read16(arm9,vram(3)+pixel*2)==0x83e0,"capture du programme ARM incomplète");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom});
    check(machine.memory().read32(arm9,capture)==0,"chargement réussi conserve le contrôle de capture");
    machine.memory().write8(arm9,0x04000243,0x80); machine.advance_cycles(frame_cycles);
    check(machine.memory().read16(arm9,vram(3))==0,"chargement réussi conserve les pixels ou une capture active");
}
} // namespace

int main() {
    registers_test(); sizes_banks_and_offsets_test(); scheduling_and_remapping_test(); blend_test();
    source_a_before_display_test(); graphics_line_and_redisplay_test(); clocks_copy_and_failures_test(); machine_program_test();
}
