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
    received_.reset();
    transfer_id_ = linked() ? hub_->start(*this, current, remaining_) : 0;
}

void Serial::start_multiplayer() {
    const auto players = linked() ? hub_->players() : 1;
    remaining_ = multiplayer_cycles[static_cast<std::size_t>(io_half(0x128) & 3)][static_cast<std::size_t>(players - 1)];
    // Pendant le transfert, le parent lit 0xFFFF dans SIOMULTI0..3 (mGBA).
    for (int offset = 0x120; offset <= 0x126; offset += 2) write_io_half(offset, 0xffff);
    received_.reset();
    transfer_id_ = linked() ? hub_->start(*this, Mode::multiplayer, remaining_) : 0;
}

void Serial::join(std::int64_t start_in, int duration, std::uint32_t id) {
    // Un départ précédent pas encore atteint a lieu maintenant : chaque
    // transfert se termine une fois, même quand cette console est en retard.
    if (join_in_ != idle) {
        join_in_ = idle;
        begin_joined(join_duration_, join_id_);
        mark_busy();
    }
    join_id_ = id;
    next_received_.reset();
    if (start_in > 0) {
        join_in_ = static_cast<int>(std::min<std::int64_t>(start_in, longest_transfer));
        join_duration_ = duration;
        return;
    }
    // Cette console a déjà dépassé l'instant du départ (elle avançait en premier).
    begin_joined(start_in + duration, id);
}

void Serial::join_now(int late) {
    join_in_ = idle;
    begin_joined(join_duration_ - late, join_id_);
    if (hub_) hub_->capture(*this);
    else mark_busy();
}

void Serial::begin_joined(std::int64_t remaining, std::uint32_t id) {
    if (remaining_ != idle) complete();  // fin précédente pas encore atteinte : elle a lieu maintenant
    remaining_ = static_cast<int>(std::clamp<std::int64_t>(remaining, 1, longest_transfer));
    transfer_id_ = id;
    // Un transfert plus court qu'une tranche a pu finir avant ce départ.
    received_ = std::exchange(next_received_, std::nullopt);
    join_id_ = 0;
}

void Serial::leave(std::uint32_t id) noexcept {
    if (transfer_id_ == id) {
        remaining_ = idle;
        transfer_id_ = 0;
        received_.reset();
    }
    if (join_id_ == id) {
        join_in_ = idle;
        join_id_ = 0;
        next_received_.reset();
    }
}

void Serial::receive(std::uint32_t id, const Received& value) noexcept {
    if (transfer_id_ == id) received_ = value;
    else if (join_id_ == id) next_received_ = value;
}

void Serial::mark_busy() noexcept {
    busy_ = true;
    write_io_half(0x128, io_half(0x128) | 0x80);
}

void Serial::complete() {
    remaining_ = idle;
    if (transfer_id_ != 0 && !received_ && hub_) hub_->finish(*this);
    transfer_id_ = 0;
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
        join_in_, join_duration_, static_cast<std::int32_t>(join_id_), static_cast<std::int32_t>(transfer_id_)};
    const auto store = [&state](std::size_t at, const std::optional<Received>& value) {
        state[at] = value ? 1 : 0;
        if (value)
            for (std::size_t index = 0; index < value->size(); ++index) state[at + 1 + index] = (*value)[index];
    };
    store(4, received_);
    store(9, next_received_);
    return state;
}

void Serial::import_link_state(std::span<const std::int32_t> state) {
    if (state.size() != link_state_words) throw SaveStateError("État série GBA invalide");
    const auto in_range = [](std::int32_t value, std::int32_t low, std::int32_t high) { return value >= low && value <= high; };
    const auto join_in = state[0];
    const auto duration = state[1];
    const auto join_id = static_cast<std::uint32_t>(state[2]);
    const auto transfer_id = static_cast<std::uint32_t>(state[3]);
    auto valid = (join_in == idle || in_range(join_in, 1, longest_transfer)) && in_range(duration, 0, longest_transfer);
    const auto load = [&](std::size_t at, std::optional<Received>& value) {
        valid = valid && in_range(state[at], 0, 1);
        value.reset();
        if (state[at] != 1) return;
        Received result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            valid = valid && in_range(state[at + 1 + index], 0, 0xffff);
            result[index] = static_cast<std::uint16_t>(state[at + 1 + index]);
        }
        value = result;
    };
    std::optional<Received> received;
    std::optional<Received> next_received;
    load(4, received);
    load(9, next_received);
    // Un départ attendu a sa durée et son numéro ; un résultat, son transfert.
    valid = valid && (join_in == idle ? join_id == 0 && !next_received : duration >= 1 && join_id != 0);
    valid = valid && (transfer_id == 0 ? !received : remaining_ != idle);
    if (!valid) throw SaveStateError("État série GBA invalide");
    join_in_ = join_in;
    join_duration_ = duration;
    join_id_ = join_id;
    transfer_id_ = transfer_id;
    received_ = received;
    next_received_ = next_received;
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

std::uint32_t LinkHub::start(Serial& master, Serial::Mode mode, int duration) {
    const auto master_index = master.player();
    // Le câble ne porte qu'un transfert à la fois ; le mode normal ne relie
    // que les joueurs 0 et 1.
    if (transfer_.active || (mode != Serial::Mode::multiplayer && master_index > 1)) return 0;
    transfer_ = {};
    transfer_.active = true;
    // Numéros propres à ce câble, croissants : un état restauré d'un
    // transfert déjà terminé ne peut pas recevoir le résultat d'un autre.
    if (++last_id_ == 0) ++last_id_;
    transfer_.id = last_id_;
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
        other->join(start_in, duration, transfer_.id);
        transfer_.participates[index] = true;
        if (start_in <= 0) capture(*other);
    }
    return transfer_.id;
}

void LinkHub::capture(Serial& participant) {
    const auto index = static_cast<std::size_t>(participant.player());
    const auto mine = participant.transfer_id() == transfer_.id || participant.join_id() == transfer_.id;
    if (!transfer_.active || !mine || !transfer_.participates[index] || transfer_.captured[index]) {
        // Données déjà relevées à la fin d'un transfert plus court qu'une tranche.
        if (participant.transfer_id() != 0) participant.mark_busy();
        return;
    }
    transfer_.captured[index] = true;
    if (!participant.ready_for(transfer_.mode)) {
        transfer_.participates[index] = false;
        transfer_.sent[index] = participant.idle_level(transfer_.mode);
        participant.leave(transfer_.id);
        return;
    }
    transfer_.sent[index] = participant.outgoing(transfer_.mode);
    participant.mark_busy();
}

void LinkHub::finish(const Serial& caller) {
    // Un état restauré d'un transfert que le câble a déjà terminé se termine
    // comme sans câble : son numéro n'est plus celui du transfert en cours.
    if (!transfer_.active || caller.transfer_id() != transfer_.id ||
        !transfer_.participates[static_cast<std::size_t>(caller.player())])
        return;
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
        serial->receive(transfer_.id, received);
    }
}

} // namespace ravenemu::gba
