#include "serial/remote_serial_link.hpp"

#include <algorithm>
#include <limits>
#include <variant>

namespace ravenemu::gba {

namespace {

RemoteLink::Mode to_remote(Serial::Mode mode) noexcept {
    switch (mode) {
    case Serial::Mode::normal8: return RemoteLink::Mode::normal8;
    case Serial::Mode::normal32: return RemoteLink::Mode::normal32;
    case Serial::Mode::multiplayer: return RemoteLink::Mode::multiplayer;
    case Serial::Mode::other: break;
    }
    return RemoteLink::Mode::other;
}

Serial::Mode from_remote(RemoteLink::Mode mode) noexcept {
    switch (mode) {
    case RemoteLink::Mode::normal8: return Serial::Mode::normal8;
    case RemoteLink::Mode::normal32: return Serial::Mode::normal32;
    case RemoteLink::Mode::multiplayer: return Serial::Mode::multiplayer;
    default: return Serial::Mode::other;
    }
}

std::uint16_t low(std::uint32_t value) noexcept { return static_cast<std::uint16_t>(value); }
std::uint16_t high(std::uint32_t value) noexcept { return static_cast<std::uint16_t>(value >> 16U); }

// Ce que reçoit la console cadencée : en multijoueur, le parent puis elle ;
// en mode normal, le mot du partenaire.
Serial::Received received_by_partner(Serial::Mode mode, std::uint32_t data, std::uint32_t sent) noexcept {
    if (mode == Serial::Mode::multiplayer) return {low(data), low(sent), 0xffff, 0xffff};
    return {low(data), high(data), 0xffff, 0xffff};
}

} // namespace

void RemoteSerialLink::attach(Serial& serial) noexcept {
    if (serial_ == &serial && serial.link() == this) return;
    // Nouvelle machine (ROM, état chargé) : son transfert n'est pas celui en
    // attente, et son horloge repart d'ailleurs.
    if (serial_ != &serial) {
        pending_.reset();
        forget_timing();
    }
    serial_ = &serial;
    serial.attach(this, player_);
    serial.refresh_status();
}

void RemoteSerialLink::detach() noexcept {
    if (serial_ != nullptr && serial_->link() == this) {
        serial_->attach(nullptr, 0);
        serial_->refresh_status();
    }
    serial_ = nullptr;
    pending_.reset();
    forget_timing();
}

bool RemoteSerialLink::all_multiplayer() const noexcept {
    return partner_mode_ == RemoteLink::Mode::multiplayer &&
        (serial_ == nullptr || serial_->mode() == Serial::Mode::multiplayer);
}

void RemoteSerialLink::service() {
    follow_plug();
    while (link_.pending()) {
        const auto message = link_.take();
        if (!message) break;
        handle(*message);
    }
}

// Branché, débranché ou rebranché : l'ancien partenaire et ses lignes
// d'état sont oubliés ; le nouveau apprend celles de cette console.
void RemoteSerialLink::follow_plug() {
    const auto plugged = link_.plugged();
    const auto generation = link_.generation();
    if (plugged == plugged_ && generation == generation_) return;
    plugged_ = plugged;
    generation_ = generation;
    // Un transfert parti vers l'ancien partenaire finira comme sans câble ;
    // celui qu'il avait lancé n'aura pas lieu.
    pending_.reset();
    if (deferred_ && serial_ != nullptr) serial_->leave(deferred_->id);
    forget_timing();
    partner_mode_ = RemoteLink::Mode::other;
    partner_so_low_ = false;
    published_.reset();
    if (serial_ != nullptr) serial_->refresh_status();
    if (plugged) publish_status();
}

void RemoteSerialLink::handle(const RemoteLink::Message& message) {
    if (const auto* transfer = std::get_if<RemoteLink::Transfer>(&message)) {
        serve(*transfer);
    } else if (const auto* status = std::get_if<RemoteLink::Status>(&message)) {
        partner_mode_ = status->mode;
        partner_so_low_ = status->so_low;
        if (serial_ != nullptr) serial_->refresh_status();
    } else if (const auto* reply = std::get_if<RemoteLink::Reply>(&message)) {
        // L'autre a répondu avant que ce transfert n'atteigne sa fin : la
        // réponse attend la fin. Toute autre réponse est celle d'un échange tranché.
        if (pending_ && reply->id == pending_->id) pending_->answer = reply->data;
    }
}

std::uint32_t RemoteSerialLink::start(Serial& master, Serial::Mode mode, int duration) {
    follow_plug();
    // Le câble ne porte qu'un transfert de cette console à la fois.
    if (!plugged_ || pending_) return 0;
    if (++counter_ > 0x7fff'ffffU) counter_ = 1;
    // Numéros pairs pour le parent, impairs pour l'autre : jamais confondus.
    const auto id = (counter_ << 1U) | static_cast<std::uint32_t>(player_ & 1);
    const auto data = master.outgoing(mode);
    // L'écart depuis le départ précédent : l'autre console laissera autant de
    // temps à son jeu avant d'entrer dans celui-ci, et suit ainsi cette horloge.
    const auto now = master.clock();
    const auto interval = last_start_
        ? std::clamp<std::int64_t>(now - *last_start_, 0, std::numeric_limits<std::uint32_t>::max())
        : 0;
    pending_ = Pending{id, generation_, mode, data, std::nullopt};
    try {
        link_.send(RemoteLink::Transfer{id, to_remote(mode), data, static_cast<std::uint32_t>(duration),
                                        static_cast<std::uint32_t>(interval)});
    } catch (...) {
        pending_.reset();
        return 0;
    }
    last_start_ = now;
    return id;
}

void RemoteSerialLink::capture(Serial& participant) {
    // Le cycle d'un transfert lu trop tôt est atteint : cette console y entre
    // maintenant, avec la donnée que son jeu a préparée entre-temps.
    if (deferred_ && participant.transfer_id() == deferred_->id && serial_ == &participant) {
        const auto mine = *deferred_;
        deferred_.reset();
        if (!participant.ready_for(mine.mode)) {
            participant.leave(mine.id);
            reply(mine.id, participant.idle_level(mine.mode));
            return;
        }
        last_join_ = participant.clock();
        settle_pace(*last_join_);
        const auto sent = participant.outgoing(mine.mode);
        participant.receive(mine.id, received_by_partner(mine.mode, mine.data, sent));
        participant.mark_busy();
        reply(mine.id, sent);
        return;
    }
    if (participant.transfer_id() != 0) participant.mark_busy();
}

void RemoteSerialLink::finish(const Serial& caller) {
    if (!pending_ || caller.transfer_id() != pending_->id || serial_ != &caller) return;
    const auto mine = *pending_;
    pending_.reset();
    auto answer = mine.answer;
    try {
        while (!answer) {
            const auto message = link_.wait(mine.generation);
            if (!message) break;
            if (const auto* reply = std::get_if<RemoteLink::Reply>(&*message); reply && reply->id == mine.id) {
                answer = reply->data;
                break;
            }
            // Le partenaire cadence lui aussi : cette console, occupée, ne l'écoute pas.
            handle(*message);
        }
    } catch (...) {
    }
    // Débranché pendant l'attente : le transfert finit comme sans câble.
    if (!answer) return;
    Serial::Received received{0xffff, 0xffff, 0xffff, 0xffff};
    if (mine.mode == Serial::Mode::multiplayer) {
        received[0] = low(mine.data);  // le parent, joueur 0
        received[1] = low(*answer);
    } else {
        received[0] = low(*answer);
        received[1] = high(*answer);
    }
    serial_->receive(mine.id, received);
}

void RemoteSerialLink::serve(const RemoteLink::Transfer& transfer) {
    const auto mode = from_remote(transfer.mode);
    if (serial_ == nullptr) {
        reply(transfer.id, 0xffff'ffffU);
        return;
    }
    advance_pace(transfer.interval);
    // Le parent attend chaque réponse avant de cadencer de nouveau : un
    // transfert encore différé ici n'a plus lieu.
    if (deferred_) {
        serial_->leave(deferred_->id);
        reply(deferred_->id, serial_->idle_level(deferred_->mode));
        deferred_.reset();
    }
    if (!serial_->ready_for(mode)) {
        reply(transfer.id, serial_->idle_level(mode));
        return;
    }
    // Pas plus tôt, sur cette horloge, que l'écart laissé par le parent depuis
    // le transfert précédent : le jeu a pu y répondre.
    const auto now = serial_->clock();
    if (last_join_) {
        const auto due = *last_join_ + std::min<std::int64_t>(transfer.interval, Serial::longest_transfer);
        if (due > now) {
            deferred_ = Deferred{transfer.id, mode, transfer.data};
            serial_->join(due - now, static_cast<int>(transfer.duration), transfer.id);
            return;
        }
    }
    last_join_ = now;
    settle_pace(now);
    // Elle entre dans le transfert maintenant, pour sa durée.
    const auto sent = serial_->outgoing(mode);
    serial_->join(0, static_cast<int>(transfer.duration), transfer.id);
    serial_->receive(transfer.id, received_by_partner(mode, transfer.data, sent));
    serial_->mark_busy();
    reply(transfer.id, sent);
}

// Tant que le parent cadence à son rythme, son horloge, rapportée à celle-ci,
// avance de l'écart annoncé à chaque transfert, que cette console y entre ou
// non. Au premier transfert, ou après un silence, elle repart de l'entrée de
// cette console : l'avance prise entre-temps ne compte pas.
void RemoteSerialLink::advance_pace(std::uint32_t interval) noexcept {
    // RavenCore 0.10.0 plafonne l'écart au plus long transfert : un écart
    // d'exactement cette valeur le trahit, et son horloge reste inconnue.
    if (interval == static_cast<std::uint32_t>(Serial::longest_transfer)) partner_caps_ = true;
    steady_ = interval > 0 && interval < pace_gap;
    if (steady_ && pace_) *pace_ += interval;
    else pace_.reset();
}

// Cette console entre dans un transfert : l'horloge du parent ne passe pas
// devant elle. En retard, cette console n'a donc jamais à l'attendre.
void RemoteSerialLink::settle_pace(std::int64_t joined) noexcept {
    if (partner_caps_) {
        pace_.reset();
        return;
    }
    pace_ = pace_ ? std::min(*pace_, joined) : joined;
}

void RemoteSerialLink::keep_pace() {
    if (!pace_ || !steady_ || serial_ == nullptr || deferred_ || pending_) return;
    if (serial_->mode() != Serial::Mode::multiplayer || partner_mode_ != RemoteLink::Mode::multiplayer) return;
    if (serial_->clock() - *pace_ < pace_lead) return;
    try {
        // Un message arrive (transfert, ligne d'état) : service() le lit.
        if (link_.wait_until(generation_, std::chrono::steady_clock::now() + pace_wait)) return;
    } catch (...) {
    }
    // Plus rien du parent, ou débranché : cette console reprend sa route.
    pace_.reset();
}

void RemoteSerialLink::reply(std::uint32_t id, std::uint32_t data) {
    try {
        link_.send(RemoteLink::Reply{id, data});
    } catch (...) {
        // Envoi impossible : l'autre console finira sans câble.
    }
}

void RemoteSerialLink::refresh() noexcept {
    if (serial_ == nullptr) return;
    serial_->refresh_status();
    try {
        follow_plug();
        publish_status();
    } catch (...) {
    }
}

void RemoteSerialLink::publish_status() {
    if (serial_ == nullptr || !plugged_) return;
    const RemoteLink::Status status{to_remote(serial_->mode()), serial_->so_low()};
    if (published_ && published_->mode == status.mode && published_->so_low == status.so_low) return;
    published_ = status;
    link_.send(status);
}

} // namespace ravenemu::gba
