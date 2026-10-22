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
    const std::uint8_t gba_hello[] = {1, 1, 2};
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
