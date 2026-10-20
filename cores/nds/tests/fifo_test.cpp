#include <ravenemu/nds/machine.hpp>
#include "check.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

using ravenemu::nds::Memory;
using ravenemu::nds::Processor;
using ravenemu::nds::DmaController;
using ravenemu::nds::LcdTiming;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;
constexpr auto arm9 = Processor::arm9, arm7 = Processor::arm7;
constexpr std::uint32_t fifo = 0x04000068, capture = 0x04000064, power = 0x04000304;
constexpr std::uint32_t dma0 = 0x040000b0, source = 0x02010000, destination = 0x06860000;
constexpr std::uint32_t enabled = 0x80000000, word = 0x04000000, fixed = 0x00400000, display_dma = 0x20000000;
constexpr auto line_cycles = LcdTiming::line_cycles, frame_cycles = LcdTiming::frame_cycles;
constexpr std::uint16_t red = 0x001f, green = 0x03e0, blue = 0x7c00;

namespace {
void setup(Memory& m) {
    m.write16(arm9,power,0x8201); // FIFO A indépendante du moteur 2D A éteint
    m.write32(arm9,0x04000000,0x30000); m.write32(arm9,0x04001000,0x10000);
    m.write16(arm9,0x05000400,green); m.write8(arm9,0x04000243,0x80);
}
void configure(Memory& m,unsigned channel,std::uint32_t src,std::uint32_t dst,std::uint32_t control) {
    const std::array<std::uint32_t,3> values{src,dst,control}; m.write_words(arm9,dma0+channel*12,values);
}
void service(Memory& m) { unsigned units=0; while(m.dma_pending(arm9)) {m.step_dma(arm9);++units;} check(units==4,"une demande vidéo doit fournir quatre mots"); }
template<class Service>
void stream_lines(Memory& m,unsigned lines,Service provide) {
    for(unsigned y=0;y<lines;++y) {
        m.advance_cycles(1); provide(); // première demande de la ligne
        m.advance_cycles(47); // premier groupe de huit pixels
        for(unsigned group=1;group<32;++group) {provide();m.advance_cycles(48);}
        m.advance_cycles(line_cycles-1536);
    }
}
std::uint16_t pixel(unsigned x,unsigned y) {
    return static_cast<std::uint16_t>(((x+y*257)&0x7fffU)|(((x+y)&1U)?0x8000U:0));
}
void source_image(Memory& m,std::uint32_t address=source) {
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;x+=2)
        m.write32(arm9,address+(y*256+x)*2,pixel(x,y)|(static_cast<std::uint32_t>(pixel(x+1,y))<<16U));
}
std::uint16_t dark_half(std::uint16_t color) {
    unsigned result=0;
    for(unsigned shift=0;shift<15;shift+=5) {
        const auto channel=(static_cast<unsigned>(color)>>shift)&31U;
        result|=(channel-channel/2)<<shift;
    }
    return static_cast<std::uint16_t>(result);
}

void cpu_port_and_ring_test() {
    Memory m; setup(m);
    for(const auto address : {fifo,0x04001068U}) {
        expect_failure<ravenemu::Error>([&]{m.write32(arm7,address,0);},"FIFO vidéo accessible en écriture ARM7");
        expect_failure<ravenemu::Error>([&]{static_cast<void>(m.read32(arm7,address));},"FIFO vidéo accessible en lecture ARM7");
    }
    expect_failure<ravenemu::Error>([&]{m.write32(arm9,0x04001068,0);},"FIFO vidéo B inexistante mappée");
    expect_failure<ravenemu::Error>([&]{m.write16(arm9,fifo,0xffff);},"écriture FIFO vidéo 16 bits non prise en charge acceptée");
    expect_failure<ravenemu::Error>([&]{m.write8(arm9,fifo,0xff);},"écriture FIFO vidéo 8 bits non prise en charge acceptée");
    expect_failure<ravenemu::Error>([&]{m.write32(arm9,fifo+1,0);},"écriture FIFO vidéo non alignée acceptée");
    m.write32(arm9,fifo,0xfc00801f);
    check(m.read32(arm9,fifo)==0&&m.peek16(arm9,fifo+2)==0&&m.read8(arm9,fifo)==0,"lecture du port vidéo doit être sans retrait et à zéro");
    const std::array<std::uint32_t,3> invalid{0,0,0}; // FIFO, MASTER_BRIGHT, puis trou 70h
    expect_failure<ravenemu::Error>([&]{m.write_words(arm9,fifo,invalid);},"transfert FIFO vers trou accepté");
    check(m.render_frame().upper[0]==0,"rendu a consommé la FIFO sans horloge");
    m.advance_cycles(0); m.advance_cycles(47);
    check(m.render_frame().upper[0]==0,"groupe vidéo consommé avant ses 48 cycles");
    m.advance_cycles(1);
    auto frame=m.render_frame();
    check(frame.upper[0]==red&&frame.upper[1]==blue&&frame.upper[2]==0&&frame.lower[0]==green,"ordre little-endian, bit15 ignoré ou indépendance B");
    check(m.render_frame().upper==frame.upper,"inspection retire des pixels de la FIFO");
    m.advance_cycles(48); m.advance_cycles(48);
    frame=m.render_frame();
    check(frame.upper[8]==0&&frame.upper[16]==red&&frame.upper[17]==blue,"sous-alimentation doit relire les mots retenus dans l'anneau");
    m.clear_ram(); setup(m);
    for(unsigned i=0;i<9;++i) m.write32(arm9,fifo,(i+1)*0x10001U);
    m.advance_cycles(96); frame=m.render_frame();
    check(frame.upper[0]==9&&frame.upper[2]==2&&frame.upper[14]==8,"débordement de l'anneau vidéo ou capacité de seize pixels incorrects");
    m.write16(arm9,0x0400006c,0x8010); check(m.render_frame().upper[0]==0,"FIFO doit traverser MASTER_BRIGHT");
    m.write16(arm9,power,0x0201); check(m.render_frame().upper[0]==green&&m.render_frame().lower[0]==0,"FIFO doit suivre la permutation du moteur A");
    m.write16(arm9,power,0); check(m.render_frame().upper[0]==0x7fff,"LCD éteint doit conserver la sortie blanche du modèle");
}

void copying_and_vblank_test() {
    Memory m; setup(m); for(unsigned i=0;i<4;++i) m.write32(arm9,fifo,0x001f001f);
    m.advance_cycles(48); auto copy=m; copy.write32(arm9,fifo,0x03e003e0);
    copy.advance_cycles(48); m.advance_cycles(48);
    check(copy.render_frame().upper[8]==green&&m.render_frame().upper[8]==0,"copie partage l'anneau vidéo ou l'image consommée");
    m.clear_ram(); setup(m); for(unsigned i=0;i<8;++i) m.write32(arm9,fifo,(i+1)*0x10001U);
    m.advance_cycles(192*line_cycles); m.write32(arm9,fifo,0x03e003e0);
    m.advance_cycles(71*line_cycles+48);
    check(m.render_frame().upper[0]==green&&m.render_frame().upper[2]==2,"VBlank doit réinitialiser les pointeurs sans effacer les mots retenus");
    m.clear_ram(); setup(m); m.advance_cycles(48);
    check(m.render_frame().upper[0]==0&&m.render_frame().upper.back()==0,"reset conserve des pixels ou des mots FIFO");
    // Activer en milieu d'image attend le prochain démarrage de ligne zéro.
    m.clear_ram(); m.advance_cycles(10*line_cycles); setup(m); m.write32(arm9,fifo,0x001f001f);
    m.advance_cycles(182*line_cycles);
    check(m.render_frame().upper[0]==0,"FIFO a commencé au milieu d'une image");
}

void display_dma_and_shared_capture_test() {
    Memory m; setup(m); source_image(m); m.write16(arm9,0x0400006c,0x8008);
    m.write32(arm9,capture,0xa2330000); // B=FIFO, D, 256x192
    configure(m,0,source,fifo,enabled|0x40000000U|word|fixed|display_dma|24576);
    check(!m.dma_pending(arm9)&&!m.step_dma(arm9),"DMA vidéo déclenché avant la première avance LCD");
    stream_lines(m,192,[&]{service(m);});
    const auto frame=m.render_frame();
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x) {
        check(frame.upper[y*256+x]==dark_half(pixel(x,y)),"image DMA FIFO ou luminosité maître incorrecte");
        check(m.read16(arm9,destination+(y*256+x)*2)==pixel(x,y),"affichage et capture ont consommé deux flux FIFO distincts");
    }
    check(!(m.read32(arm9,capture)&enabled)&&!(m.read32(arm9,dma0+8)&enabled)&&m.read32(arm9,0x04000214)==0x100,
          "fin capture, compteur DMA ou IRQ de fin d'image incorrects");
    m.advance_cycles(71*line_cycles+1);
    check(!m.dma_pending(arm9),"DMA vidéo ponctuel redémarré à l'image suivante");
    // Le chemin recommandé count=4 + repeat réarme les rafales sans rembobiner SAD.
    m.clear_ram(); setup(m); source_image(m); m.write32(arm9,capture,0xa2030000); // 128x128
    configure(m,0,source,fifo,enabled|word|fixed|display_dma|0x02000000U|4);
    stream_lines(m,2,[&]{service(m);});
    for(unsigned x=0;x<128;++x)
        check(m.read16(arm9,destination+256+x*2)==pixel(x,1),"capture 128 pixels doit consommer toute la ligne FIFO de 256");
    check(m.read32(arm9,dma0+8)&enabled,"repeat vidéo a désactivé le canal");
    m.advance_cycles(1); check(m.dma_pending(arm9),"rafale répétée suivante absente");
    m.advance_cycles(190*line_cycles-1);
    check(!m.dma_pending(arm9),"VBlank n'a pas suspendu une rafale vidéo non servie");
    m.advance_cycles(71*line_cycles+1); service(m);
    m.advance_cycles(47); check(m.render_frame().upper[0]==(pixel(0,2)&0x7fffU),"reprise DMA ne conserve pas le pointeur source après VBlank");
}

void dma_bursts_errors_and_cpu_test() {
    DmaController dma{arm9};
    const auto put=[&](unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i) dma.write_byte(offset+i,static_cast<std::uint8_t>(value>>(i*8U)));};
    put(0,source);put(4,fifo);put(8,enabled|word|fixed|display_dma|0x40000000U|9);
    for(unsigned burst=0;burst<3;++burst) {
        dma.trigger(DmaController::Event::display_fifo);
        check(dma.next()->source==source+burst*16&&dma.next()->remaining==9-burst*4,"reprise de quota DMA vidéo incorrecte");
        const auto count=burst==2?1U:4U;
        for(unsigned i=0;i<count;++i) check(dma.complete_unit()==(burst==2?0x100U:0U),"IRQ émise à la fin d'une rafale au lieu de la longueur DMA");
        check(!dma.next(),"rafale vidéo dépasse quatre mots sans nouvelle demande");
    }
    check(!dma.listening(DmaController::Event::display_fifo),"DMA terminé reste armé");
    for(const auto invalid : {std::array<std::uint32_t,3>{source,fifo,enabled|fixed|display_dma|4},
                             std::array<std::uint32_t,3>{0x03000000,fifo,enabled|word|fixed|display_dma|4},
                             std::array<std::uint32_t,3>{source,0x02001000,enabled|word|fixed|display_dma|4}}) {
        Memory m; setup(m); configure(m,0,invalid[0],invalid[1],invalid[2]); m.advance_cycles(1);
        expect_failure<ravenemu::nds::DmaTransferError>([&]{m.step_dma(arm9);},"DMA vidéo invalide a été exécuté");
        check(m.dma_pending(arm9)&&m.read32(arm9,0x04000214)==0,"échec DMA a avancé le canal ou levé une IRQ");
        m.write32(arm9,dma0+8,0); m.write32(arm9,fifo,0x001f001f); m.advance_cycles(47);
        check(m.render_frame().upper[0]==red,"DMA refusé a modifié l'anneau vidéo");
    }
    Memory m; setup(m); m.write32(arm9,source,0x001f001f); m.write32(arm9,0x02000000,0xeafffffe);
    configure(m,0,source,fifo,enabled|word|fixed|display_dma|4);
    configure(m,1,source,0x02001000,enabled|word|1);
    m.advance_cycles(1); ravenemu::nds::Cpu cpu{arm9}; cpu.reset(0x02000000,false);
    for(unsigned i=0;i<5;++i) cpu.step(m);
    check(cpu.state().instructions==0&&m.read32(arm9,0x02001000)==0x001f001f,"CPU non suspendu ou priorité DMA vidéo incorrecte");
    cpu.step(m); check(cpu.state().instructions==1,"CPU ne reprend pas après la rafale");
}

void capture_blend_and_rollback_test() {
    Memory m; setup(m); m.write16(arm9,power,0x8203); m.write16(arm9,0x05000000,red);
    m.write32(arm9,capture,0xc2130808); // A 2D + B FIFO, D, 256x64
    for(unsigned i=0;i<8;++i) m.write32(arm9,fifo,0x03e083e0); // vert valide puis invalide
    m.advance_cycles(1536);
    check(m.read16(arm9,destination)==0x81ef&&m.read16(arm9,destination+2)==0x800f,"validité FIFO ou coefficients de mélange capture");
    m.clear_ram(); setup(m); m.write32(arm9,capture,0xc3130808); // A=3D refusée, B=FIFO
    for(unsigned i=0;i<8;++i) m.write32(arm9,fifo,(i+1)*0x10001U);
    m.write32(arm9,0x04000100,0x00800000); m.advance_cycles(1488);
    const auto before=m.render_frame().upper;
    expect_failure<ravenemu::Error>([&]{m.advance_cycles(48);},"capture 3D absente a été ignorée");
    check(m.read16(arm9,0x04000100)==1488&&m.render_frame().upper==before&&m.read16(arm9,destination)==0,
          "échec de capture a retiré le dernier groupe ou modifié les horloges/VRAM");
    m.write32(arm9,capture,0xa2130000); m.advance_cycles(48);
    check(m.render_frame().upper[248]==5&&m.read16(arm9,destination+496)==5,"reprise après erreur n'a pas conservé le dernier groupe FIFO");
}

void large_intervals_test() {
    Memory m; setup(m); for(unsigned i=0;i<8;++i) m.write32(arm9,fifo,(i+1)*0x10001U);
    m.write32(arm9,0x04000100,0x00800000); m.write16(arm9,0x04000004,0x38); m.write32(arm9,capture,0xa2330000);
    configure(m,0,source,fifo,enabled|word|fixed|display_dma|24576);
    m.advance_cycles(7*line_cycles+53); auto split=m, clock_only=m;
    clock_only.write32(arm9,0x04000000,0); clock_only.write32(arm9,capture,0); clock_only.write32(arm9,dma0+8,0);
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    m.advance_cycles(maximum); clock_only.advance_cycles(maximum);
    split.advance_cycles(frame_cycles+12345); split.advance_cycles(maximum-frame_cycles-12345);
    check(m.render_frame().upper==split.render_frame().upper&&m.dma_pending(arm9)==split.dma_pending(arm9),"FIFO groupée et fractionnée divergent");
    for(const auto address : {0x04000004U,0x04000006U,0x04000100U,0x04000214U})
        check(m.read16(arm9,address)==clock_only.read16(arm9,address)&&m.read16(arm9,address)==split.read16(arm9,address),"FIFO a changé la durée ou les événements des grandes avances");
    for(unsigned i=0;i<256*192;i+=31) check(m.read16(arm9,destination+i*2)==split.read16(arm9,destination+i*2),"capture FIFO groupée et fractionnée divergent");
    m.write32(arm9,fifo,0x001f001f); split.write32(arm9,fifo,0x001f001f);
    m.advance_cycles(48); split.advance_cycles(48);
    check(m.render_frame().upper==split.render_frame().upper,"coalescence a perdu les pointeurs de l'anneau");
}

void put32(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8U));
}
void machine_program_test() {
    constexpr std::array<std::array<std::uint32_t,2>,5> commands{{
        {power,0x8001},{0x04000000,0x30000},{dma0,0x02000200},{dma0+4,fifo},
        {dma0+8,enabled|0x40000000U|word|fixed|display_dma|24576}}};
    constexpr unsigned size=0x200+256*192*2;
    std::vector<std::uint8_t> rom(0x200+size+4);
    put32(rom,0x20,0x200);put32(rom,0x24,0x02000000);put32(rom,0x28,0x02000000);put32(rom,0x2c,size);
    put32(rom,0x30,0x200+size);put32(rom,0x34,0x03800000);put32(rom,0x38,0x03800000);put32(rom,0x3c,4);put32(rom,0x200+size,0xeafffffe);
    put32(rom,0x200,0xe59f0078);put32(rom,0x204,0xe59f3078);put32(rom,0x208,0xe8b00006);put32(rom,0x20c,0xe5812000);
    put32(rom,0x210,0xe2533001);put32(rom,0x214,0x1afffffb);put32(rom,0x218,0xe59f4068);put32(rom,0x21c,0xe5945000);
    put32(rom,0x220,0xe3150c01);put32(rom,0x224,0x0afffffc);put32(rom,0x228,0xe59f605c);put32(rom,0x22c,0xe5865000);put32(rom,0x230,0xeafffffe);
    put32(rom,0x280,0x02000100);put32(rom,0x284,commands.size());put32(rom,0x288,0x04000214);put32(rom,0x28c,0x02020000);
    for(unsigned i=0;i<commands.size();++i) {put32(rom,0x300+i*8,commands[i][0]);put32(rom,0x304+i*8,commands[i][1]);}
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;x+=2) put32(rom,0x400+(y*256+x)*2,pixel(x,y)|(static_cast<std::uint32_t>(pixel(x+1,y))<<16U));
    ravenemu::nds::Machine machine; machine.load_cartridge(ravenemu::nds::Cartridge{rom}); machine.run_steps(arm9,2+4*commands.size()+10);
    check(machine.memory().read32(arm9,0x02020000)==0&&!machine.memory().dma_pending(arm9),"programme ARM ne reste pas en attente de la demande vidéo");
    const auto provide=[&](ravenemu::nds::Machine& target){const auto instructions=target.cpu_state(arm9).instructions;target.run_steps(arm9,4);check(target.cpu_state(arm9).instructions==instructions,"DMA vidéo n'a pas suspendu ARM9 pendant quatre pas");};
    stream_lines(machine.memory(),1,[&]{provide(machine);}); auto copy=machine;
    copy.memory().write32(arm9,0x02000400,0x03e003e0); stream_lines(copy.memory(),1,[&]{provide(copy);});
    check(copy.render_frame().upper[256]==green&&machine.render_frame().upper[256]==0,"copie partage le DMA, RAM ou pixels FIFO");
    auto invalid=rom;put32(invalid,0x34,0x04000000);put32(invalid,0x38,0x04000000);
    expect_failure<ravenemu::RomLoadError>([&]{machine.load_cartridge(ravenemu::nds::Cartridge{invalid});},"programme invalide accepté");
    check(machine.render_frame().upper[1]==(pixel(1,0)&0x7fffU)&&machine.memory().read32(arm9,dma0+8)&enabled,"chargement refusé détruit le flux actif");
    stream_lines(machine.memory(),191,[&]{provide(machine);}); machine.run_steps(arm9,10);
    check(machine.memory().read32(arm9,0x02020000)==0x100,"programme ARM n'observe pas l'IRQ de fin du DMA vidéo");
    const auto frame=machine.render_frame();
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x) check(frame.upper[y*256+x]==(pixel(x,y)&0x7fffU),"image complète du programme ARM DMA/FIFO incorrecte");
    machine.load_cartridge(ravenemu::nds::Cartridge{rom}); machine.memory().write16(arm9,power,0x8001);machine.memory().write32(arm9,0x04000000,0x30000);
    check(machine.render_frame().upper[1]==0&&!machine.memory().dma_pending(arm9),"chargement conserve des pixels ou un DMA FIFO actif");
}
} // namespace

int main() {
    cpu_port_and_ring_test(); copying_and_vblank_test(); display_dma_and_shared_capture_test();
    dma_bursts_errors_and_cpu_test(); capture_blend_and_rollback_test(); large_intervals_test(); machine_program_test();
}
