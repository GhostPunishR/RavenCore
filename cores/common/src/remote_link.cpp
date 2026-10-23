#include "ravenemu/remote_link.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace ravenemu {

namespace {

// Messages, petit-boutistes. Le premier octet donne leur nature.
enum Kind : std::uint8_t { hello = 1, transfer = 2, reply = 3, status = 4 };

void put32(std::uint8_t* out, std::uint32_t value) noexcept {
    for (int index = 0; index < 4; ++index) out[index] = static_cast<std::uint8_t>(value >> (8 * index));
}

std::uint32_t get32(std::span<const std::uint8_t> in, std::size_t at) noexcept {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(in[at + static_cast<std::size_t>(index)]) << (8 * index);
    return value;
}

bool valid_mode(std::uint8_t mode, RemoteLink::Family family) noexcept {
    if (family == RemoteLink::Family::game_boy) return mode == static_cast<std::uint8_t>(RemoteLink::Mode::game_boy);
    return mode >= static_cast<std::uint8_t>(RemoteLink::Mode::normal8) && mode <= static_cast<std::uint8_t>(RemoteLink::Mode::other);
}

} // namespace

RemoteLink::RemoteLink(Family family, Send send) : family_(family), send_(std::move(send)) {}

bool RemoteLink::receive(std::span<const std::uint8_t> message) {
    if (message.empty()) return false;
    std::optional<Message> parsed;
    switch (message[0]) {
    case hello: {
        if (message.size() != 3) return false;
        std::lock_guard lock(mutex_);
        // Une autre version du protocole ou une autre famille de consoles :
        // le câble reste sans effet, comme branché à un appareil étranger.
        const auto compatible = message[1] == protocol_version && message[2] == static_cast<std::uint8_t>(family_);
        if (!connected_) {
            // L'autre côté s'est branché le premier : son annonce vaut au branchement d'ici.
            early_hello_ = compatible;
            return true;
        }
        accept_hello(compatible);
        return true;
    }
    case transfer: {
        if (message.size() != 18 || !valid_mode(message[5], family_)) return false;
        // Le plus long transfert GBA (multijoueur à 9600 bauds) dure 125 829 cycles.
        const auto duration = get32(message, 10);
        if (duration > 1'000'000) return false;
        parsed = Transfer{get32(message, 1), static_cast<Mode>(message[5]), get32(message, 6), duration,
                          get32(message, 14)};
        break;
    }
    case reply:
        if (message.size() != 9) return false;
        parsed = Reply{get32(message, 1), get32(message, 5)};
        break;
    case status:
        if (message.size() != 3 || !valid_mode(message[1], family_) || message[2] > 1) return false;
        parsed = Status{static_cast<Mode>(message[1]), message[2] != 0};
        break;
    default:
        return false;
    }
    std::lock_guard lock(mutex_);
    // Hors branchement, ou avant l'annonce du partenaire, le câble ne porte rien.
    if (!connected_ || !partner_ok_) return true;
    queue_.push_back(*parsed);
    pending_.store(true, std::memory_order_release);
    arrived_.notify_all();
    return true;
}

void RemoteLink::accept_hello(bool compatible) {
    partner_ok_ = compatible;
    incompatible_ = !compatible;
    plugged_.store(compatible, std::memory_order_release);
    arrived_.notify_all();
}

void RemoteLink::set_connected(bool connected) {
    {
        std::lock_guard lock(mutex_);
        const auto early = std::exchange(early_hello_, std::nullopt);
        if (connected == connected_) return;
        connected_ = connected;
        partner_ok_ = false;
        incompatible_ = false;
        plugged_.store(false, std::memory_order_release);
        queue_.clear();
        pending_.store(false, std::memory_order_release);
        if (connected) generation_.fetch_add(1, std::memory_order_acq_rel);
        if (connected && early) accept_hello(*early);
        arrived_.notify_all();
    }
    if (connected) {
        const std::array<std::uint8_t, 3> bytes{hello, protocol_version, static_cast<std::uint8_t>(family_)};
        send_bytes(bytes);
    }
}

RemoteLink::State RemoteLink::state() const {
    std::lock_guard lock(mutex_);
    if (!connected_) return State::unplugged;
    if (incompatible_) return State::incompatible;
    return partner_ok_ ? State::plugged : State::waiting;
}

std::optional<RemoteLink::Message> RemoteLink::take() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    auto message = queue_.front();
    queue_.pop_front();
    pending_.store(!queue_.empty(), std::memory_order_release);
    return message;
}

bool RemoteLink::gone(std::uint32_t generation) const noexcept {
    return !connected_ || !partner_ok_ || generation_.load(std::memory_order_acquire) != generation;
}

std::optional<RemoteLink::Message> RemoteLink::wait(std::uint32_t generation) {
    std::unique_lock lock(mutex_);
    arrived_.wait(lock, [&] { return !queue_.empty() || gone(generation); });
    if (gone(generation)) return std::nullopt;
    auto message = queue_.front();
    queue_.pop_front();
    pending_.store(!queue_.empty(), std::memory_order_release);
    return message;
}

bool RemoteLink::wait_until(std::uint32_t generation, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    arrived_.wait_until(lock, deadline, [&] { return !queue_.empty() || gone(generation); });
    return !queue_.empty() && !gone(generation);
}

void RemoteLink::send(const Transfer& value) const {
    std::array<std::uint8_t, 18> bytes{transfer};
    put32(&bytes[1], value.id);
    bytes[5] = static_cast<std::uint8_t>(value.mode);
    put32(&bytes[6], value.data);
    put32(&bytes[10], value.duration);
    put32(&bytes[14], value.interval);
    send_bytes(bytes);
}

void RemoteLink::send(const Reply& value) const {
    std::array<std::uint8_t, 9> bytes{reply};
    put32(&bytes[1], value.id);
    put32(&bytes[5], value.data);
    send_bytes(bytes);
}

void RemoteLink::send(const Status& value) const {
    const std::array<std::uint8_t, 3> bytes{status, static_cast<std::uint8_t>(value.mode), static_cast<std::uint8_t>(value.so_low ? 1 : 0)};
    send_bytes(bytes);
}

void RemoteLink::send_bytes(std::span<const std::uint8_t> bytes) const {
    if (send_) send_(bytes);
}

bool RemoteLinkEndpoint::exchange_bit(LinkPort& source, bool) noexcept {
    const int remaining = source.transfer_bits_remaining();
    // Premier bit : l'octet entier part, la réponse de l'autre console revient.
    if (remaining >= 8 || remaining <= 0) reply_ = transfer(source, source.transfer_data());
    const int bit = remaining > 0 && remaining <= 8 ? remaining - 1 : 7;
    return ((reply_ >> bit) & 1) != 0;
}

std::uint8_t RemoteLinkEndpoint::transfer(LinkPort& source, std::uint8_t outgoing) noexcept {
    try {
        const auto generation = link_.generation();
        if (!link_.plugged()) return 0xff;  // ligne SIN au repos, comme sans câble
        follow_generation();
        // L'écart depuis l'octet précédent : l'autre console laissera autant
        // de temps à son jeu avant de sortir le sien.
        const auto now = source.link_clock();
        const auto interval = last_start_ ? std::min<std::uint64_t>(now - *last_start_, max_interval) : 0;
        last_start_ = now;
        const auto id = ++last_id_;
        link_.send(RemoteLink::Transfer{id, RemoteLink::Mode::game_boy, outgoing, 0,
                                        static_cast<std::uint32_t>(interval)});
        while (auto message = link_.wait(generation)) {
            if (const auto* answer = std::get_if<RemoteLink::Reply>(&*message)) {
                if (answer->id == id) return static_cast<std::uint8_t>(answer->data);
                continue;  // réponse d'un échange déjà tranché
            }
            if (const auto* other = std::get_if<RemoteLink::Transfer>(&*message)) {
                // Les deux consoles cadencent en même temps : chacune reçoit
                // l'octet de l'autre, et la réponse à celui-ci sera ignorée.
                link_.send(RemoteLink::Reply{other->id, outgoing});
                return static_cast<std::uint8_t>(other->data);
            }
        }
    } catch (...) {
        // Envoi impossible : le transfert finit comme sans câble.
    }
    return 0xff;
}

void RemoteLinkEndpoint::poll() noexcept {
    try {
        follow_generation();
        // Le cycle d'un octet lu trop tôt est atteint : il est servi maintenant.
        if (deferred_ && (port_ == nullptr || port_->link_clock() >= due_)) {
            const auto transfer = *deferred_;
            deferred_.reset();
            clock_in(transfer);
        }
        while (!deferred_ && link_.pending()) {
            const auto message = link_.take();
            if (!message) break;
            if (const auto* transfer = std::get_if<RemoteLink::Transfer>(&*message)) serve(*transfer);
        }
    } catch (...) {
    }
}

// Branché, débranché ou rebranché : l'horloge du partenaire n'est plus la même.
void RemoteLinkEndpoint::follow_generation() noexcept {
    const auto generation = link_.generation();
    if (generation == generation_) return;
    generation_ = generation;
    forget_timing();
}

void RemoteLinkEndpoint::serve(const RemoteLink::Transfer& transfer) noexcept {
    // Pas plus tôt, sur cette horloge, que l'écart laissé par l'autre depuis
    // l'octet précédent : le jeu a pu y répondre.
    if (port_ != nullptr && last_serve_) {
        const auto due = *last_serve_ + std::min<std::uint64_t>(transfer.interval, max_interval);
        if (due > port_->link_clock()) {
            deferred_ = transfer;
            due_ = due;
            return;
        }
    }
    clock_in(transfer);
}

void RemoteLinkEndpoint::clock_in(const RemoteLink::Transfer& transfer) noexcept {
    // L'autre console cadence : celle-ci reçoit ses huit bits d'un coup et
    // renvoie ceux qu'elle sortait, comme pendant un échange bit à bit.
    if (port_ != nullptr) last_serve_ = port_->link_clock();
    std::uint8_t sent = 0xff;
    if (port_ != nullptr) {
        sent = 0;
        for (int bit = 7; bit >= 0; --bit) {
            sent = static_cast<std::uint8_t>((sent << 1) | (port_->outgoing_bit_high() ? 1 : 0));
            port_->clock_external_bit(((transfer.data >> bit) & 1) != 0);
        }
    }
    try {
        link_.send(RemoteLink::Reply{transfer.id, sent});
    } catch (...) {
    }
}

} // namespace ravenemu
