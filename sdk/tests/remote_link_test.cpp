// Câble link distant par l'API C : deux consoles sur deux fils, reliées par
// un faux réseau qui livre les messages dans l'ordre, avec une latence.
#include <ravenemu/bridge.h>

#include "synthetic_roms.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Handle = std::unique_ptr<ravenemu_bridge, decltype(&ravenemu_destroy)>;
using Clock = std::chrono::steady_clock;

// Un sens de la connexion : chaque message arrive `latency` plus tard, dans l'ordre.
class Wire {
public:
    explicit Wire(std::chrono::milliseconds latency) : latency_(latency) {}
    ~Wire() { stop(); }

    static void send(void* user, const uint8_t* message, size_t size) {
        auto& wire = *static_cast<Wire*>(user);
        std::lock_guard lock(wire.mutex_);
        wire.queue_.push_back({Clock::now() + wire.latency_, std::vector<std::uint8_t>(message, message + size)});
        wire.changed_.notify_all();
    }
    void start(ravenemu_remote_link* target) {
        target_ = target;
        thread_ = std::thread([this] { deliver(); });
    }
    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            changed_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    struct Parcel {
        Clock::time_point due;
        std::vector<std::uint8_t> bytes;
    };
    void deliver() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            if (queue_.empty()) {
                changed_.wait(lock);
                continue;
            }
            const auto due = queue_.front().due;
            if (Clock::now() < due) {
                changed_.wait_until(lock, due);
                continue;
            }
            auto parcel = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            if (!ravenemu_remote_link_receive(target_, parcel.bytes.data(), parcel.bytes.size())) bad_.store(true);
            lock.lock();
        }
    }

public:
    std::atomic<bool> bad_{false};

private:
    std::chrono::milliseconds latency_;
    ravenemu_remote_link* target_{};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Parcel> queue_;
    bool stopping_{};
    std::thread thread_;
};

using Remote = std::unique_ptr<ravenemu_remote_link, decltype(&ravenemu_remote_link_destroy)>;

// Deux consoles et leur câble distant, branché des deux côtés.
struct Pair {
    Handle first;
    Handle second;
    Wire to_second;
    Wire to_first;
    Remote first_link{nullptr, ravenemu_remote_link_destroy};
    Remote second_link{nullptr, ravenemu_remote_link_destroy};

    Pair(Handle a, Handle b, std::chrono::milliseconds latency)
        : first(std::move(a)), second(std::move(b)), to_second(latency), to_first(latency) {}
    ~Pair() {
        // Débrancher d'abord : plus rien n'attend, puis les fils de livraison s'arrêtent.
        if (first_link) ravenemu_remote_link_set_connected(first_link.get(), 0);
        if (second_link) ravenemu_remote_link_set_connected(second_link.get(), 0);
        to_second.stop();
        to_first.stop();
    }
    // Un côté s'arrête : comme l'hôte qui perd la connexion, on débranche les deux.
    void unplug() {
        ravenemu_remote_link_set_connected(first_link.get(), 0);
        ravenemu_remote_link_set_connected(second_link.get(), 0);
    }
    bool plug(int first_player, int second_player) {
        first_link.reset(ravenemu_remote_link_create(first.get(), first_player, Wire::send, &to_second));
        second_link.reset(ravenemu_remote_link_create(second.get(), second_player, Wire::send, &to_first));
        if (!first_link || !second_link) return false;
        to_second.start(second_link.get());
        to_first.start(first_link.get());
        ravenemu_remote_link_set_connected(first_link.get(), 1);
        ravenemu_remote_link_set_connected(second_link.get(), 1);
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline) {
            if (ravenemu_remote_link_state(first_link.get()) == RAVENEMU_REMOTE_PLUGGED &&
                ravenemu_remote_link_state(second_link.get()) == RAVENEMU_REMOTE_PLUGGED)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

Handle load(int system, const std::vector<std::uint8_t>& rom) {
    Handle handle(ravenemu_create(system), ravenemu_destroy);
    if (handle && !ravenemu_load(handle.get(), rom.data(), rom.size(), nullptr, 0)) handle.reset();
    return handle;
}

// Programme Game Boy : écrit `data` dans SB, `control` dans SC, attend la fin
// du transfert, puis range SB en C000 et 1 en C001.
std::vector<std::uint8_t> game_boy_transfer_rom(std::uint8_t data, std::uint8_t control) {
    auto rom = ravenemu::testing::minimal_game_boy_rom();
    const std::uint8_t entry[] = {0x00, 0xc3, 0x50, 0x01};  // NOP ; JP 0150
    std::memcpy(&rom[0x100], entry, sizeof entry);
    const std::uint8_t program[] = {
        0xf3,              // DI
        0x3e, data,        // LD A, data
        0xe0, 0x01,        // LDH (SB), A
        0x3e, control,     // LD A, control
        0xe0, 0x02,        // LDH (SC), A
        0xf0, 0x02,        // attente : LDH A, (SC)
        0xcb, 0x7f,        // BIT 7, A
        0x20, 0xfa,        // JR NZ, attente
        0xf0, 0x01,        // LDH A, (SB)
        0xea, 0x00, 0xc0,  // LD (C000), A
        0x3e, 0x01,        // LD A, 1
        0xea, 0x01, 0xc0,  // LD (C001), A
        0x18, 0xfe,        // JR .
    };
    std::memcpy(&rom[0x150], program, sizeof program);
    return rom;
}

// Le programme multijoueur GBA de bridge_test.cpp : le parent envoie 0xAA00,
// l'enfant 0xBB00, et chacun range SIOMULTI0 et SIOMULTI1 au début de l'IWRAM.
std::vector<std::uint8_t> gba_multiplayer_rom(bool slow) {
    // 115200 bauds (ORR r1, r1, #3), ou 9600 bauds : 63 427 cycles, la réponse
    // de l'enfant arrive alors avant la fin du transfert chez le parent.
    const std::uint32_t baud = slow ? 0xe3811000 : 0xe3811003;
    const std::uint32_t code[] = {
        0xe59f208c, 0xe3a01000, 0xe1c211b4, 0xe3a01a02, baud, 0xe1c210b8, 0xe1d230b8, 0xe3130004,
        0x03a04caa, 0x13a04cbb, 0xe1c240ba, 0x1a000008, 0xe1d230b8, 0xe3130008, 0x0afffffc, 0xe3833080,
        0xe1c230b8, 0xe1d230b8, 0xe3130080, 0x1afffffc, 0xea000005, 0xe1d230b8, 0xe3130080, 0x0afffffc,
        0xe1d230b8, 0xe3130080, 0x1afffffc, 0xe3a05403, 0xe1d260b0, 0xe1c560b0, 0xe1d260b2, 0xe1c560b2,
        0xe1d260b8, 0xe1c560b4, 0xe1d260b4, 0xe1c560b6, 0xeafffffe, 0x04000120};
    auto rom = ravenemu::testing::minimal_gba_rom();
    std::memcpy(rom.data(), code, sizeof code);
    return rom;
}

// Rafale Game Boy, comme un échange Pokémon : le maître envoie 0xA0 + n
// pour n de 1 à 16, une courte pause entre chaque, et range l'octet reçu en
// C000 + n - 1 ; l'esclave compte chaque octet (IF bit 3, sans
// interruption), range celui du maître de la même façon et prépare n + 1.
// Chacun écrit 1 en C100 une fois fini.
std::vector<std::uint8_t> game_boy_burst_rom(bool master) {
    static const std::uint8_t master_code[] = {
        0xf3, 0xaf, 0xe0, 0x0f, 0xe0, 0xff, 0x06, 0x01, 0x21, 0x00, 0xc0, 0x78, 0xf6, 0xa0, 0xe0, 0x01,
        0x3e, 0x81, 0xe0, 0x02, 0xf0, 0x02, 0xcb, 0x7f, 0x20, 0xfa, 0xf0, 0x01, 0x22, 0x16, 0x00, 0x15,
        0x20, 0xfd, 0x04, 0x78, 0xfe, 0x11, 0x20, 0xe3, 0x3e, 0x01, 0xea, 0x00, 0xc1, 0x18, 0xfe};
    static const std::uint8_t slave_code[] = {
        0xf3, 0xaf, 0xe0, 0x0f, 0xe0, 0xff, 0x06, 0x01, 0x21, 0x00, 0xc0, 0x3e, 0x01, 0xe0, 0x01, 0x3e,
        0x80, 0xe0, 0x02, 0xf0, 0x0f, 0xcb, 0x5f, 0x28, 0xfa, 0xaf, 0xe0, 0x0f, 0xf0, 0x01, 0x22, 0x04,
        0x78, 0xe0, 0x01, 0x3e, 0x80, 0xe0, 0x02, 0x78, 0xfe, 0x11, 0x20, 0xe7, 0x3e, 0x01, 0xea, 0x00,
        0xc1, 0x18, 0xfe};
    auto rom = ravenemu::testing::minimal_game_boy_rom();
    const std::uint8_t entry[] = {0x00, 0xc3, 0x50, 0x01};  // NOP ; JP 0150
    std::memcpy(&rom[0x100], entry, sizeof entry);
    if (master) std::memcpy(&rom[0x150], master_code, sizeof master_code);
    else std::memcpy(&rom[0x150], slave_code, sizeof slave_code);
    return rom;
}

// Rafale multijoueur, comme les échanges Pokémon : le parent lance 16
// transferts, une pause d'environ 40 000 cycles entre chacun, et note le mot
// de l'enfant ; l'enfant compte chaque transfert (IF bit 7, sans
// interruption), note le mot du parent et offre le numéro suivant. Chacun
// écrit 1 en IWRAM+0x40 une fois fini ; les mots vont en IWRAM+2n.
std::vector<std::uint8_t> gba_burst_rom() {
    const std::uint32_t code[] = {
        0xe59f20b8, 0xe3a01000, 0xe1c211b4, 0xe3a07001, 0xe1c270ba, 0xe59f10ac, 0xe1c210b8, 0xe3a05403,
        0xe1d230b8, 0xe3130004, 0x1a000014, 0xe1d230b8, 0xe3130008, 0x0afffffc, 0xe3874caa, 0xe1c240ba,
        0xe1d230b8, 0xe3833080, 0xe1c230b8, 0xe1d230b8, 0xe3130080, 0x1afffffc, 0xe1d260b2, 0xe0858087,
        0xe1c860b0, 0xe3a09a01, 0xe2599001, 0x1afffffd, 0xe2877001, 0xe3570011, 0x1affffee, 0xea00000c,
        0xe59fa03c, 0xe1da30b0, 0xe3130080, 0x0afffffc, 0xe3a03080, 0xe1ca30b0, 0xe1d260b0, 0xe0858087,
        0xe1c860b0, 0xe2877001, 0xe1c270ba, 0xe3570011, 0x1afffff3, 0xe3a06001, 0xe1c564b0, 0xeafffffe,
        0x04000120, 0x04000202, 0x00006003};
    // Après l'en-tête de la cartouche : B 0xC0 à l'entrée.
    auto rom = ravenemu::testing::minimal_gba_rom();
    const std::uint32_t entry = 0xea00002e;
    std::memcpy(rom.data(), &entry, sizeof entry);
    std::memcpy(rom.data() + 0xc0, code, sizeof code);
    return rom;
}

// Rafales multijoueur des salles Pokémon : 12 images du parent, chacune
// ouverte au VBlank par 9 transferts. L'enfant compte ses VBlank entre le
// premier transfert et le dernier, et offre le numéro suivant à chacun.
// Chacun écrit 1 en IWRAM+0x40 une fois fini ; l'enfant, son compte en +0x44.
std::vector<std::uint8_t> gba_pace_rom() {
    const std::uint32_t code[] = {
        0xe59f20fc, 0xe3a01000, 0xe1c211b4, 0xe3a00301, 0xe3a01008, 0xe1c010b4, 0xe3a07001, 0xe1c270ba,
        0xe59f10e4, 0xe1c210b8, 0xe3a05403, 0xe59fa0d4, 0xe3a0b000, 0xe1d230b8, 0xe3130004, 0x1a00001c,
        0xe1d230b8, 0xe3130008, 0x0afffffc, 0xe3a03001, 0xe1ca30b0, 0xe1da30b0, 0xe3130001, 0x0afffffc,
        0xe3a03001, 0xe1ca30b0, 0xe28bb001, 0xe3a0c009, 0xe3874caa, 0xe1c240ba, 0xe1d230b8, 0xe3833080,
        0xe1c230b8, 0xe1d230b8, 0xe3130080, 0x1afffffc, 0xe3a09c01, 0xe2599001, 0x1afffffd, 0xe2877001,
        0xe25cc001, 0x1afffff1, 0xe35b000c, 0x1affffe6, 0xea00000f, 0xe1da30b0, 0xe3130001, 0x0a000002,
        0xe3a04001, 0xe1ca40b0, 0xe28bb001, 0xe3130080, 0x0afffff7, 0xe3a04080, 0xe1ca40b0, 0xe3570001,
        0x03a0b000, 0xe2877001, 0xe1c270ba, 0xe357006d, 0x1affffef, 0xe1c5b4b4, 0xe3a06001, 0xe1c564b0,
        0xeafffffe, 0x04000120, 0x04000202, 0x00006003};
    auto rom = ravenemu::testing::minimal_gba_rom();
    const std::uint32_t entry = 0xea00002e;  // B 0xC0, après l'en-tête
    std::memcpy(rom.data(), &entry, sizeof entry);
    std::memcpy(rom.data() + 0xc0, code, sizeof code);
    return rom;
}

// Fait tourner `handle` sur son fil jusqu'à `done` (vrai), ou 600 images.
bool run_until(ravenemu_bridge* handle, ravenemu_remote_link* link, int pixels,
               bool (*done)(ravenemu_bridge*), const std::atomic<bool>* also_wait = nullptr) {
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(pixels));
    for (int index = 0; index < 600; ++index) {
        if (done(handle) && (also_wait == nullptr || also_wait->load())) return true;
        if (!ravenemu_run_frame(handle, frame.data(), frame.size())) {
            std::cerr << "run_frame failed: " << ravenemu_last_error() << "\n";
            return false;
        }
        ravenemu_remote_link_service(link);
    }
    return done(handle) && (also_wait == nullptr || also_wait->load());
}

bool game_boy_done(ravenemu_bridge* handle) {
    std::uint8_t flag = 0;
    return ravenemu_read_achievement_memory(handle, 0xc001, &flag, 1) == 1 && flag == 1;
}

bool gba_done(ravenemu_bridge* handle) {
    std::uint8_t received[4]{};
    return ravenemu_read_achievement_memory(handle, 0, received, sizeof received) == sizeof received &&
        received[1] != 0 && received[3] != 0;
}

int game_boy_remote_test(std::chrono::milliseconds latency) {
    Pair pair(load(RAVENEMU_GBC, game_boy_transfer_rom(0xa5, 0x81)),
              load(RAVENEMU_GB, game_boy_transfer_rom(0x3c, 0x80)), latency);
    if (!pair.first || !pair.second) return 10;
    if (!pair.plug(0, 0)) return 11;
    // L'esclave attend d'abord l'horloge du maître, comme un joueur arrivé le premier.
    std::vector<std::uint32_t> frame(160 * 144);
    if (!ravenemu_run_frame(pair.second.get(), frame.data(), frame.size())) return 12;
    if (!ravenemu_link_wanted(pair.second.get())) return 13;
    std::atomic<bool> master_done{false};
    std::atomic<bool> slave_ok{false};
    std::thread slave([&] {
        slave_ok = run_until(pair.second.get(), pair.second_link.get(), 160 * 144, game_boy_done, &master_done);
        if (!slave_ok) pair.unplug();
    });
    const auto master_ok = run_until(pair.first.get(), pair.first_link.get(), 160 * 144, game_boy_done);
    master_done = true;
    if (!master_ok) pair.unplug();
    slave.join();
    if (!master_ok) return 14;
    if (!slave_ok) return 18;
    std::uint8_t master_received = 0;
    std::uint8_t slave_received = 0;
    ravenemu_read_achievement_memory(pair.first.get(), 0xc000, &master_received, 1);
    ravenemu_read_achievement_memory(pair.second.get(), 0xc000, &slave_received, 1);
    if (master_received != 0x3c || slave_received != 0xa5) return 15;
    if (pair.to_first.bad_ || pair.to_second.bad_) return 16;
    if (!ravenemu_link_wanted(pair.first.get())) return 17;
    return 0;
}

int gba_remote_test(std::chrono::milliseconds latency, bool slow) {
    const auto rom = gba_multiplayer_rom(slow);
    Pair pair(load(RAVENEMU_GBA, rom), load(RAVENEMU_GBA, rom), latency);
    if (!pair.first || !pair.second) return 20;
    if (!pair.plug(0, 1)) return 21;
    std::atomic<bool> parent_done{false};
    std::atomic<bool> child_ok{false};
    std::thread child([&] {
        child_ok = run_until(pair.second.get(), pair.second_link.get(), 240 * 160, gba_done, &parent_done);
        if (!child_ok) pair.unplug();
    });
    const auto parent_ok = run_until(pair.first.get(), pair.first_link.get(), 240 * 160, gba_done);
    parent_done = true;
    if (!parent_ok) pair.unplug();
    child.join();
    if (!parent_ok) return 22;
    if (!child_ok) return 26;
    for (auto* handle : {pair.first.get(), pair.second.get()}) {
        std::uint8_t received[4]{};
        ravenemu_read_achievement_memory(handle, 0, received, sizeof received);
        // Comme par le câble local : SIOMULTI0 = 0xAA00 (parent), SIOMULTI1 = 0xBB00.
        if (received[0] != 0x00 || received[1] != 0xaa || received[2] != 0x00 || received[3] != 0xbb) return 23;
    }
    if (!ravenemu_link_wanted(pair.first.get()) || !ravenemu_link_wanted(pair.second.get())) return 24;
    if (pair.to_first.bad_ || pair.to_second.bad_) return 25;
    return 0;
}

bool burst_done(ravenemu_bridge* handle) {
    std::uint8_t flag = 0;
    return ravenemu_read_achievement_memory(handle, 0x40, &flag, 1) == 1 && flag == 1;
}

bool game_boy_burst_done(ravenemu_bridge* handle) {
    std::uint8_t flag = 0;
    return ravenemu_read_achievement_memory(handle, 0xc100, &flag, 1) == 1 && flag == 1;
}

// Une console qui attend, comme dans une application : une image, puis elle
// dort, et son câble est servi entre-temps.
bool run_like_an_app(ravenemu_bridge* handle, ravenemu_remote_link* link, int pixels, bool (*done)(ravenemu_bridge*),
                     const std::atomic<bool>& other_done) {
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(pixels));
    for (int index = 0; index < 300 && !(done(handle) && other_done.load()); ++index) {
        if (!ravenemu_run_frame(handle, frame.data(), frame.size())) return false;
        const auto until = Clock::now() + std::chrono::milliseconds(12);
        while (Clock::now() < until) {
            ravenemu_remote_link_service(link);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    return done(handle);
}

// Deux octets lus pendant le sommeil de l'esclave ne reçoivent pas la même
// réponse : le second attend que son jeu ait préparé la suivante.
int game_boy_burst_test(std::chrono::milliseconds latency) {
    Pair pair(load(RAVENEMU_GB, game_boy_burst_rom(true)), load(RAVENEMU_GBC, game_boy_burst_rom(false)), latency);
    if (!pair.first || !pair.second) return 60;
    if (!pair.plug(0, 0)) return 61;
    // L'esclave arme son port avant que le maître ne cadence : sans quoi le
    // premier octet se perd, comme entre deux consoles.
    std::vector<std::uint32_t> first(160 * 144);
    if (!ravenemu_run_frame(pair.second.get(), first.data(), first.size())) return 67;
    std::atomic<bool> master_done{false};
    std::atomic<bool> slave_ok{false};
    std::thread slave([&] {
        slave_ok = run_like_an_app(pair.second.get(), pair.second_link.get(), 160 * 144, game_boy_burst_done,
                                   master_done);
        if (!slave_ok) pair.unplug();
    });
    const auto master_ok = run_until(pair.first.get(), pair.first_link.get(), 160 * 144, game_boy_burst_done);
    master_done = true;
    if (!master_ok) pair.unplug();
    slave.join();
    if (!master_ok) return 62;
    if (!slave_ok) return 63;
    std::uint8_t master_log[16]{};
    std::uint8_t slave_log[16]{};
    ravenemu_read_achievement_memory(pair.first.get(), 0xc000, master_log, sizeof master_log);
    ravenemu_read_achievement_memory(pair.second.get(), 0xc000, slave_log, sizeof slave_log);
    for (int number = 1; number <= 16; ++number) {
        if (master_log[number - 1] != number) return 64;
        if (slave_log[number - 1] != (0xa0 | number)) return 65;
    }
    if (pair.to_first.bad_ || pair.to_second.bad_) return 66;
    return 0;
}

// L'enfant tourne comme dans une application. Deux transferts lus pendant
// son sommeil ne reçoivent pas la même donnée : le second attend que le jeu
// de l'enfant ait répondu au premier, comme sur deux consoles.
int gba_burst_test(std::chrono::milliseconds latency) {
    const auto rom = gba_burst_rom();
    Pair pair(load(RAVENEMU_GBA, rom), load(RAVENEMU_GBA, rom), latency);
    if (!pair.first || !pair.second) return 50;
    if (!pair.plug(0, 1)) return 51;
    std::atomic<bool> parent_done{false};
    std::atomic<bool> child_ok{false};
    std::thread child([&] {
        child_ok = run_like_an_app(pair.second.get(), pair.second_link.get(), 240 * 160, burst_done, parent_done);
        if (!child_ok) pair.unplug();
    });
    const auto parent_ok = run_until(pair.first.get(), pair.first_link.get(), 240 * 160, burst_done);
    parent_done = true;
    if (!parent_ok) pair.unplug();
    child.join();
    if (!parent_ok) return 52;
    if (!child_ok) return 53;
    std::uint8_t parent_log[34]{};
    std::uint8_t child_log[34]{};
    ravenemu_read_achievement_memory(pair.first.get(), 0, parent_log, sizeof parent_log);
    ravenemu_read_achievement_memory(pair.second.get(), 0, child_log, sizeof child_log);
    for (int number = 1; number <= 16; ++number) {
        const auto at = static_cast<std::size_t>(2 * number);
        // Le parent reçoit chaque numéro une fois, dans l'ordre ; l'enfant, chaque mot du parent.
        if ((parent_log[at] | parent_log[at + 1] << 8) != number) return 54;
        if ((child_log[at] | child_log[at + 1] << 8) != (0xaa00 | number)) return 55;
    }
    if (pair.to_first.bad_ || pair.to_second.bad_) return 56;
    return 0;
}

// Le parent attend un aller-retour par transfert et ralentit : l'enfant,
// qui tourne comme dans une application, ne le devance pas de plus de trois
// images. Sans cela, il en prend plusieurs par image du parent, et un jeu
// comme Pokémon y voit une liaison rompue.
int gba_pace_test(std::chrono::milliseconds latency) {
    const auto rom = gba_pace_rom();
    Pair pair(load(RAVENEMU_GBA, rom), load(RAVENEMU_GBA, rom), latency);
    if (!pair.first || !pair.second) return 70;
    if (!pair.plug(0, 1)) return 71;
    std::atomic<bool> parent_done{false};
    std::atomic<bool> child_ok{false};
    std::thread child([&] {
        child_ok = run_like_an_app(pair.second.get(), pair.second_link.get(), 240 * 160, burst_done, parent_done);
        if (!child_ok) pair.unplug();
    });
    const auto parent_ok = run_until(pair.first.get(), pair.first_link.get(), 240 * 160, burst_done);
    parent_done = true;
    if (!parent_ok) pair.unplug();
    child.join();
    if (!parent_ok) return 72;
    if (!child_ok) return 73;
    std::uint8_t frames[2]{};
    ravenemu_read_achievement_memory(pair.second.get(), 0x44, frames, sizeof frames);
    // Onze images du parent entre son premier transfert et son dernier, plus
    // trois d'avance au plus, plus celle en cours.
    if ((frames[0] | frames[1] << 8) > 15) {
        std::cerr << "child frames: " << (frames[0] | frames[1] << 8) << "\n";
        return 74;
    }
    if (pair.to_first.bad_ || pair.to_second.bad_) return 75;
    return 0;
}

void ignore(void*, const uint8_t*, size_t) {}

int api_test() {
    const auto idle_rom = ravenemu::testing::minimal_game_boy_rom();
    auto game_boy = load(RAVENEMU_GB, idle_rom);
    auto color = load(RAVENEMU_GBC, idle_rom);
    auto advance = load(RAVENEMU_GBA, ravenemu::testing::minimal_gba_rom());
    if (!game_boy || !color || !advance) return 30;

    // Paramètres refusés.
    if (ravenemu_remote_link_create(nullptr, 0, ignore, nullptr) ||
        ravenemu_remote_link_create(game_boy.get(), 0, nullptr, nullptr) ||
        ravenemu_remote_link_create(advance.get(), 2, ignore, nullptr))
        return 31;

    // Câble local et câble distant s'excluent.
    ravenemu_bridge* const pair[] = {game_boy.get(), color.get()};
    std::unique_ptr<ravenemu_link, decltype(&ravenemu_link_destroy)> local(ravenemu_link_create(pair, 2), ravenemu_link_destroy);
    if (!local) return 32;
    if (ravenemu_remote_link_create(game_boy.get(), 0, ignore, nullptr)) return 33;
    local.reset();
    Remote remote(ravenemu_remote_link_create(game_boy.get(), 0, ignore, nullptr), ravenemu_remote_link_destroy);
    if (!remote) return 34;
    if (ravenemu_link_create(pair, 2)) return 35;
    if (ravenemu_remote_link_create(game_boy.get(), 0, ignore, nullptr)) return 36;

    // Débranché, puis branché sans partenaire : la console joue seule.
    if (ravenemu_remote_link_state(remote.get()) != RAVENEMU_REMOTE_UNPLUGGED) return 37;
    ravenemu_remote_link_set_connected(remote.get(), 1);
    if (ravenemu_remote_link_state(remote.get()) != RAVENEMU_REMOTE_WAITING) return 38;
    const std::uint8_t malformed[] = {7, 1};
    if (ravenemu_remote_link_receive(remote.get(), malformed, sizeof malformed) ||
        ravenemu_remote_link_receive(remote.get(), nullptr, 0) || ravenemu_remote_link_receive(nullptr, malformed, 2))
        return 39;
    // Une GBA en face : incompatible.
    const std::uint8_t gba_hello[] = {1, 2, 2};  // protocole 2, famille GBA
    if (!ravenemu_remote_link_receive(remote.get(), gba_hello, sizeof gba_hello) ||
        ravenemu_remote_link_state(remote.get()) != RAVENEMU_REMOTE_INCOMPATIBLE)
        return 40;
    std::vector<std::uint32_t> frame(160 * 144);
    for (int index = 0; index < 4; ++index)
        if (!ravenemu_run_frame(game_boy.get(), frame.data(), frame.size()) || !ravenemu_remote_link_service(remote.get()))
            return 41;
    // Ce jeu ne touche pas à son port link.
    if (ravenemu_link_wanted(game_boy.get()) || ravenemu_link_wanted(nullptr)) return 42;

    // Le câble retiré, la console joue seule ; détruite avant son câble, rien ne casse.
    remote.reset();
    if (!ravenemu_run_frame(game_boy.get(), frame.data(), frame.size())) return 43;
    Remote orphan(ravenemu_remote_link_create(advance.get(), 1, ignore, nullptr), ravenemu_remote_link_destroy);
    if (!orphan) return 44;
    advance.reset();
    if (ravenemu_remote_link_service(orphan.get())) return 45;
    ravenemu_remote_link_set_connected(orphan.get(), 1);
    orphan.reset();
    ravenemu_remote_link_destroy(nullptr);
    ravenemu_remote_link_set_connected(nullptr, 1);
    return ravenemu_remote_link_state(nullptr) == RAVENEMU_REMOTE_UNPLUGGED ? 0 : 46;
}

} // namespace

int main() {
    if (const int failure = api_test()) {
        std::cerr << "Remote link API failed (" << failure << "): " << ravenemu_last_error() << "\n";
        return failure;
    }
    for (const auto latency : {std::chrono::milliseconds(0), std::chrono::milliseconds(5), std::chrono::milliseconds(20)}) {
        if (const int failure = game_boy_remote_test(latency)) {
            std::cerr << "Game Boy remote link failed at " << latency.count() << " ms (" << failure
                      << "): " << ravenemu_last_error() << "\n";
            return failure;
        }
        if (const int failure = game_boy_burst_test(latency)) {
            std::cerr << "Game Boy remote burst failed at " << latency.count() << " ms (" << failure
                      << "): " << ravenemu_last_error() << "\n";
            return failure;
        }
        if (const int failure = gba_burst_test(latency)) {
            std::cerr << "GBA remote burst failed at " << latency.count() << " ms (" << failure
                      << "): " << ravenemu_last_error() << "\n";
            return failure;
        }
        if (latency.count() < 20) {
            if (const int failure = gba_pace_test(latency)) {
                std::cerr << "GBA remote pace failed at " << latency.count() << " ms (" << failure
                          << "): " << ravenemu_last_error() << "\n";
                return failure;
            }
        }
        for (const bool slow : {false, true}) {
            if (const int failure = gba_remote_test(latency, slow)) {
                std::cerr << "GBA remote link failed at " << latency.count() << " ms"
                          << (slow ? ", 9600 bauds" : "") << " (" << failure << "): " << ravenemu_last_error() << "\n";
                return failure;
            }
        }
    }
    return 0;
}
