#include "serial/serial.hpp"

namespace ravenemu::gba {

namespace {
constexpr std::uint32_t all_ones(Serial::Mode mode) noexcept {
    return mode == Serial::Mode::normal32 ? 0xffff'ffffU : mode == Serial::Mode::normal8 ? 0xffU : 0xffffU;
}
constexpr bool normal(Serial::Mode mode) noexcept {
    return mode == Serial::Mode::normal8 || mode == Serial::Mode::normal32;
}
} // namespace

Serial::Mode Serial::mode() const noexcept {
    if ((io_half(0x134) & 0x8000) != 0) return Mode::other;  // RCNT : modes GPIO / JOY Bus
    switch ((io_half(0x128) >> 12) & 3) {
    case 0: return Mode::normal8;
    case 1: return Mode::normal32;
    case 2: return Mode::multiplayer;
    default: return Mode::other;  // UART
    }
}

bool Serial::linked() const noexcept { return hub_ != nullptr && hub_->players() >= 2; }

void Serial::control_write(int value) {
    const auto requested = (value & 0x80) != 0;
    // Hors transfert, le bit Start suit l'écriture ; pendant un transfert,
    // il reste levé jusqu'à la fin.
    const auto in_flight = remaining_ != idle || join_in_ != idle;
    if (!in_flight) busy_ = false;
    switch (mode()) {
    case Mode::normal8:
    case Mode::normal32:
        if (requested && !in_flight) {
            busy_ = true;
            // Horloge interne : la console cadence. Horloge externe : elle
            // attend qu'un partenaire la cadence.
            if ((value & 1) != 0) start_normal();
        }
        break;
    case Mode::multiplayer:
        // Seul le parent lance un transfert ; chez un enfant, le bit est en
        // lecture seule et indique le transfert en cours.
        if (requested && !in_flight && player_ == 0) {
            busy_ = true;
            start_multiplayer();
        }
        break;
    case Mode::other:
        if (!in_flight) busy_ = requested;
        break;
    }
    write_io_half(0x128, (io_half(0x128) & ~0x80) | (busy_ ? 0x80 : 0));
    if (hub_) hub_->refresh();
    else refresh_status();
}

void Serial::rcnt_write() {
    if (hub_) hub_->refresh();
    else refresh_status();
}

void Serial::start_normal() {
    const auto current = mode();
    const auto bits = current == Mode::normal32 ? 32 : 8;
    remaining_ = bits * ((io_half(0x128) & 2) != 0 ? 8 : 64);  // 2 MHz ou 256 kHz
    linked_transfer_ = false;
    received_.reset();
    if (linked()) linked_transfer_ = hub_->start(*this, current, remaining_);
}

void Serial::start_multiplayer() {
    const auto players = linked() ? hub_->players() : 1;
    remaining_ = multiplayer_cycles[static_cast<std::size_t>(io_half(0x128) & 3)][static_cast<std::size_t>(players - 1)];
    // Pendant le transfert, le parent lit 0xFFFF dans SIOMULTI0..3 (mGBA).
    for (int offset = 0x120; offset <= 0x126; offset += 2) write_io_half(offset, 0xffff);
    linked_transfer_ = false;
    received_.reset();
    if (linked()) linked_transfer_ = hub_->start(*this, Mode::multiplayer, remaining_);
}

void Serial::join(std::int64_t start_in, int duration) {
    // Un départ précédent pas encore atteint a lieu maintenant : chaque
    // transfert se termine une fois, même quand cette console est en retard.
    if (join_in_ != idle) {
        join_in_ = idle;
        begin_joined(join_duration_);
        mark_busy();
    }
    if (start_in > 0) {
        join_in_ = static_cast<int>(std::min<std::int64_t>(start_in, longest_transfer));
        join_duration_ = duration;
        return;
    }
    // Cette console a déjà dépassé l'instant du départ (elle avançait en premier).
    begin_joined(start_in + duration);
}

void Serial::join_now(int late) {
    join_in_ = idle;
    begin_joined(join_duration_ - late);
    if (hub_) hub_->capture(*this);
    else mark_busy();
}

void Serial::begin_joined(std::int64_t remaining) {
    if (remaining_ != idle) complete();  // fin précédente pas encore atteinte : elle a lieu maintenant
    remaining_ = static_cast<int>(std::clamp<std::int64_t>(remaining, 1, longest_transfer));
    linked_transfer_ = true;
}

void Serial::leave() noexcept {
    remaining_ = idle;
    join_in_ = idle;
    linked_transfer_ = false;
    received_.reset();
}

void Serial::mark_busy() noexcept {
    busy_ = true;
    write_io_half(0x128, io_half(0x128) | 0x80);
}

void Serial::complete() {
    remaining_ = idle;
    if (linked_transfer_ && !received_ && hub_) hub_->finish(*this);
    linked_transfer_ = false;
    const auto received = std::exchange(received_, std::nullopt);
    const auto control = io_half(0x128);
    switch (mode()) {
    case Mode::multiplayer: {
        // Seule, la console garde sa donnée et lit 0xFFFF pour chaque enfant absent.
        const Received alone{static_cast<std::uint16_t>(io_half(0x12a)), 0xffff, 0xffff, 0xffff};
        const auto& values = received ? *received : alone;
        for (std::size_t index = 0; index < values.size(); ++index)
            write_io_half(0x120 + 2 * static_cast<int>(index), values[index]);
        break;
    }
    case Mode::normal32:
        write_io_half(0x120, received ? (*received)[0] : 0xffff);
        write_io_half(0x122, received ? (*received)[1] : 0xffff);
        break;
    case Mode::normal8:
        bus_.io[0x12a] = received ? static_cast<std::uint8_t>((*received)[0]) : 0xff;
        break;
    case Mode::other:
        break;
    }
    busy_ = false;
    write_io_half(0x128, control & ~0x80);
    refresh_status();
    if ((control & 0x4000) != 0) interrupts_.request(serial_interrupt);
}

void Serial::refresh_status() noexcept {
    auto control = io_half(0x128);
    const auto is_linked = linked();
    switch (mode()) {
    case Mode::multiplayer: {
        // SI : 0 pour le parent ; 1 pour un enfant, comme sans câble (ligne
        // tirée à 1). SD : toutes les consoles reliées sont en multijoueur.
        auto ready = true;
        if (is_linked)
            for (int player = 0; player < LinkHub::max_players; ++player)
                if (const auto* other = hub_->serial(player); other && other->mode() != Mode::multiplayer) ready = false;
        const auto child = !is_linked || player_ != 0;
        control = (control & ~0x7c) | (child ? 0x04 : 0) | (ready ? 0x08 : 0) | (player_ << 4);
        break;
    }
    case Mode::normal8:
    case Mode::normal32: {
        // SI : la ligne SO du partenaire, haute sans câble. Le mode normal ne
        // relie que deux consoles, les joueurs 0 et 1.
        const auto* partner = is_linked && player_ < 2 ? hub_->serial(player_ ^ 1) : nullptr;
        const auto low = partner != nullptr && partner->so_low();
        control = (control & ~0x04) | (low ? 0 : 0x04);
        break;
    }
    case Mode::other:
        return;
    }
    write_io_half(0x128, control);
}

std::uint32_t Serial::outgoing(Mode mode) const noexcept {
    switch (mode) {
    case Mode::normal8: return bus_.io[0x12a];
    case Mode::normal32: return static_cast<std::uint32_t>(io_half(0x120)) | static_cast<std::uint32_t>(io_half(0x122)) << 16U;
    default: return static_cast<std::uint32_t>(io_half(0x12a));
    }
}

// Ligne SO au repos : le bit 3 de SIOCNT en mode normal ; un esclave qui
// attend l'horloge la tient basse pour se dire prêt. Hors mode normal, haute.
bool Serial::so_low() const noexcept {
    if (!normal(mode())) return false;
    return busy_ || (io_half(0x128) & 0x08) == 0;
}

std::uint32_t Serial::idle_level(Mode mode) const noexcept { return so_low() ? 0 : all_ones(mode); }

bool Serial::ready_for(Mode mode) const noexcept {
    const auto current = this->mode();
    if (mode == Mode::multiplayer) return current == Mode::multiplayer;
    // Mode normal : l'esclave attend, horloge externe et bit Start levé.
    return normal(current) && (io_half(0x128) & 1) == 0 && busy_;
}

std::array<std::int32_t, Serial::link_state_words> Serial::export_link_state() const noexcept {
    std::array<std::int32_t, link_state_words> state{
        join_in_, join_duration_, linked_transfer_ ? 1 : 0, received_ ? 1 : 0, 0, 0, 0, 0};
    if (received_)
        for (std::size_t index = 0; index < received_->size(); ++index) state[4 + index] = (*received_)[index];
    return state;
}

void Serial::import_link_state(std::span<const std::int32_t> state) {
    if (state.size() != link_state_words) throw SaveStateError("État série GBA invalide");
    const auto join_in = state[0];
    const auto duration = state[1];
    const auto linked = state[2];
    const auto has_received = state[3];
    const auto in_range = [](std::int32_t value, std::int32_t low, std::int32_t high) { return value >= low && value <= high; };
    auto valid = (join_in == idle || in_range(join_in, 1, longest_transfer)) && in_range(duration, 0, longest_transfer) &&
                 in_range(linked, 0, 1) && in_range(has_received, 0, 1);
    for (std::size_t index = 4; index < link_state_words; ++index) valid = valid && in_range(state[index], 0, 0xffff);
    // Un départ attendu ou un résultat n'existent que pour un transfert relié.
    if (join_in != idle) valid = valid && duration >= 1;
    if (has_received != 0) valid = valid && linked != 0 && remaining_ != idle;
    if (linked != 0) valid = valid && (remaining_ != idle || join_in != idle);
    if (!valid) throw SaveStateError("État série GBA invalide");
    join_in_ = join_in;
    join_duration_ = duration;
    linked_transfer_ = linked != 0;
    received_.reset();
    if (has_received != 0) {
        Received value{};
        for (std::size_t index = 0; index < value.size(); ++index) value[index] = static_cast<std::uint16_t>(state[4 + index]);
        received_ = value;
    }
    busy_ = (io_half(0x128) & 0x80) != 0;
}

void LinkHub::attach(int player, Serial& serial, std::int64_t origin) noexcept {
    slots_[static_cast<std::size_t>(player)] = {&serial, origin};
    serial.attach(this, player);
}

void LinkHub::detach_all() noexcept {
    for (auto& slot : slots_) {
        if (slot.serial) slot.serial->attach(nullptr, 0);
        slot = {};
    }
}

int LinkHub::players() const noexcept {
    auto count = 0;
    for (const auto& slot : slots_) count += slot.serial ? 1 : 0;
    return count;
}

std::int64_t LinkHub::now(const Serial& serial) const noexcept {
    return serial.clock() - slots_[static_cast<std::size_t>(serial.player())].origin;
}

void LinkHub::refresh() noexcept {
    for (const auto& slot : slots_)
        if (slot.serial) slot.serial->refresh_status();
}

bool LinkHub::start(Serial& master, Serial::Mode mode, int duration) {
    const auto master_index = master.player();
    // Le câble ne porte qu'un transfert à la fois ; le mode normal ne relie
    // que les joueurs 0 et 1.
    if (transfer_.active || (mode != Serial::Mode::multiplayer && master_index > 1)) return false;
    transfer_ = {};
    transfer_.active = true;
    transfer_.mode = mode;
    transfer_.master = master_index;
    transfer_.sent.fill(all_ones(mode));
    transfer_.sent[static_cast<std::size_t>(master_index)] = master.outgoing(mode);
    transfer_.participates[static_cast<std::size_t>(master_index)] = true;
    transfer_.captured[static_cast<std::size_t>(master_index)] = true;
    first_ = master_index;
    const auto at = now(master);
    for (int player = 0; player < max_players; ++player) {
        auto* other = slots_[static_cast<std::size_t>(player)].serial;
        if (other == nullptr || player == master_index) continue;
        if (mode != Serial::Mode::multiplayer && player != (master_index ^ 1)) continue;
        const auto index = static_cast<std::size_t>(player);
        // Un partenaire qui cadence lui-même un transfert n'écoute pas.
        if (normal(mode) && other->transferring() && !other->ready_for(mode)) {
            transfer_.sent[index] = other->idle_level(mode);
            continue;
        }
        // join() peut d'abord clore un transfert précédent : la console ne
        // fait partie de celui-ci qu'ensuite.
        const auto start_in = at - now(*other);
        other->join(start_in, duration);
        transfer_.participates[index] = true;
        if (start_in <= 0) capture(*other);
    }
    return true;
}

void LinkHub::capture(Serial& participant) {
    const auto index = static_cast<std::size_t>(participant.player());
    if (!transfer_.active || !transfer_.participates[index] || transfer_.captured[index]) {
        // Données déjà relevées à la fin d'un transfert plus court qu'une tranche.
        if (participant.transferring()) participant.mark_busy();
        return;
    }
    transfer_.captured[index] = true;
    if (!participant.ready_for(transfer_.mode)) {
        transfer_.participates[index] = false;
        transfer_.sent[index] = participant.idle_level(transfer_.mode);
        participant.leave();
        return;
    }
    transfer_.sent[index] = participant.outgoing(transfer_.mode);
    participant.mark_busy();
}

void LinkHub::finish(const Serial& caller) {
    if (!transfer_.active || !transfer_.participates[static_cast<std::size_t>(caller.player())]) return;
    // Une console qui n'a pas encore atteint le départ (transfert plus court
    // qu'une tranche) est lue telle qu'elle est.
    for (int player = 0; player < max_players; ++player) {
        const auto index = static_cast<std::size_t>(player);
        if (transfer_.participates[index] && !transfer_.captured[index] && slots_[index].serial)
            capture(*slots_[index].serial);
    }
    transfer_.active = false;
    for (int player = 0; player < max_players; ++player) {
        const auto index = static_cast<std::size_t>(player);
        auto* serial = slots_[index].serial;
        if (!transfer_.participates[index] || serial == nullptr) continue;
        Serial::Received received{};
        if (transfer_.mode == Serial::Mode::multiplayer) {
            for (std::size_t slot = 0; slot < received.size(); ++slot)
                received[slot] = static_cast<std::uint16_t>(transfer_.sent[slot]);
        } else {
            // Mode normal : chacun reçoit ce que l'autre a envoyé.
            const auto from = player == transfer_.master ? transfer_.master ^ 1 : transfer_.master;
            const auto value = transfer_.sent[static_cast<std::size_t>(from)];
            received[0] = static_cast<std::uint16_t>(value);
            received[1] = static_cast<std::uint16_t>(value >> 16U);
        }
        serial->receive(received);
    }
}

} // namespace ravenemu::gba
