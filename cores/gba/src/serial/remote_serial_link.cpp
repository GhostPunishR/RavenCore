#include "serial/remote_serial_link.hpp"

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

} // namespace

void RemoteSerialLink::attach(Serial& serial) noexcept {
    if (serial_ == &serial && serial.link() == this) return;
    // Nouvelle machine (ROM, état chargé) : son transfert n'est pas celui en attente.
    if (serial_ != &serial) pending_.reset();
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
    // Un transfert parti vers l'ancien partenaire finira comme sans câble.
    pending_.reset();
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
    pending_ = Pending{id, generation_, mode, data, std::nullopt};
    try {
        link_.send(RemoteLink::Transfer{id, to_remote(mode), data, static_cast<std::uint32_t>(duration)});
    } catch (...) {
        pending_.reset();
        return 0;
    }
    return id;
}

void RemoteSerialLink::capture(Serial& participant) {
    // Le partenaire distant entre dans un transfert dès sa lecture du message :
    // il n'y a pas de départ à atteindre plus tard.
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
    std::uint32_t sent = 0xffff'ffffU;
    if (serial_ != nullptr) {
        if (!serial_->ready_for(mode)) {
            sent = serial_->idle_level(mode);
        } else {
            sent = serial_->outgoing(mode);
            // Elle entre dans le transfert maintenant, pour sa durée.
            serial_->join(0, static_cast<int>(transfer.duration), transfer.id);
            Serial::Received received{0xffff, 0xffff, 0xffff, 0xffff};
            if (mode == Serial::Mode::multiplayer) {
                received[0] = low(transfer.data);
                received[1] = low(sent);
            } else {
                received[0] = low(transfer.data);
                received[1] = high(transfer.data);
            }
            serial_->receive(transfer.id, received);
            serial_->mark_busy();
        }
    }
    link_.send(RemoteLink::Reply{transfer.id, sent});
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
