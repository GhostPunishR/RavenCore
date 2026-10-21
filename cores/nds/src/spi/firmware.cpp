#include <ravenemu/nds/firmware.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {
namespace {
constexpr bool writes(std::uint8_t command) noexcept {
    return command == 0x02 || command == 0x0a || command == 0xdb || command == 0xd8;
}
constexpr std::uint32_t cycles_for_us(std::uint64_t microseconds) noexcept {
    return static_cast<std::uint32_t>((microseconds * timer_clock_hz + 999999U) / 1000000U);
}
} // namespace

void Firmware::deselect(Session& session) noexcept {
    // CS haut termine une commande complète ; WIP vit indépendamment du bus.
    if (!busy()) {
        if (session.command == 0x06) write_enabled_ = true;
        else if (session.command == 0x04) write_enabled_ = false;
        else if (session.command == 0xb9) asleep_ = true;
        else if (session.command == 0xab) asleep_ = false;
        else if (writes(session.command) && write_enabled_ &&
                 ((session.phase == Session::Phase::program && session.count) || session.phase == Session::Phase::erase)) {
            operation_.command = session.command;
            operation_.address = session.address;
            if (session.phase == Session::Phase::program) {
                operation_.length = session.count;
                operation_.page = session.page;
                operation_.remaining = cycles_for_us(session.command == 0x02 ? 1200 : 11000);
            } else {
                operation_.length = session.command == 0xdb ? 0x100U : 0x10000U;
                operation_.address &= ~(operation_.length - 1U);
                operation_.remaining = cycles_for_us(session.command == 0xdb ? 10000 : 1000000);
            }
        }
    }
    session = {};
}

void Firmware::advance_cycles(std::uint64_t cycles) noexcept {
    if (!busy()) return;
    if (cycles < operation_.remaining) { operation_.remaining -= static_cast<std::uint32_t>(cycles); return; }
    operation_.remaining = 0;
    bool changed = false;
    for (std::uint32_t i = 0; i < operation_.length; ++i) {
        auto& destination = bytes_[operation_.address + i];
        const auto value = operation_.command == 0x02 ? static_cast<std::uint8_t>(destination & operation_.page[i]) :
                           operation_.command == 0x0a ? operation_.page[i] : std::uint8_t{0xff};
        changed |= destination != value;
        destination = value;
    }
    write_enabled_ = false;
    if (changed) { ++generation_; refresh_settings(); }
}

std::uint8_t Firmware::exchange(Session& session, std::uint8_t output) const {
    if (bytes_.empty()) throw Error("NDS : firmware absent pour le transfert SPI");
    using Phase = Session::Phase;
    switch (session.phase) {
    case Phase::command:
        if ((asleep_ && output != 0xab) || (busy() && output != 0x05)) {
            session.phase = Phase::ignore;
            return 0xff;
        }
        switch (output) {
        case 0x03: case 0x0b: session.phase = Phase::address; break;
        case 0x02: case 0x0a: case 0xdb: case 0xd8:
            session.phase = write_enabled_ ? Phase::address : Phase::ignore;
            break;
        case 0x05: session.phase = Phase::status; break;
        case 0x9f: session.phase = Phase::id; break;
        case 0x06: case 0x04: case 0xb9: case 0xab: session.phase = Phase::ignore; break;
        default: throw Error("NDS : commande flash firmware SPI non prise en charge");
        }
        session.command = output;
        break;
    case Phase::address:
        session.address = (session.address << 8U) | output;
        if (++session.address_bytes == 3) {
            if (writes(session.command)) {
                session.address %= static_cast<std::uint32_t>(bytes_.size());
                // Modèle /W bas : les 256 premières pages (64 Kio) sont protégées.
                if (session.address < 0x10000) { session.command = 0; session.phase = Phase::ignore; }
                else session.phase = (session.command == 0x02 || session.command == 0x0a) ? Phase::program : Phase::erase;
            } else session.phase = session.command == 0x0b ? Phase::dummy : Phase::data;
        }
        break;
    case Phase::dummy: session.phase = Phase::data; break;
    case Phase::data: {
        const auto value = bytes_[session.address % bytes_.size()];
        session.address = (session.address + 1U) & 0xffffffU;
        return value;
    }
    case Phase::program:
        if (session.count >= 0x100U - (session.address & 0xffU))
            throw Error("NDS : écriture firmware au-delà de la page de 256 octets non prise en charge");
        session.page[session.count++] = output;
        break;
    case Phase::erase:
        session.command = 0; session.phase = Phase::ignore; // effacement : exactement trois octets d'adresse
        break;
    case Phase::id: {
        // Modèles matériels documentés : M45PE20 (256 Kio), M25PE40 (512 Kio).
        const std::array<std::uint8_t, 3> id = bytes_.size() == 0x40000 ?
            std::array<std::uint8_t, 3>{0x20,0x40,0x12} : std::array<std::uint8_t, 3>{0x20,0x80,0x13};
        return session.id_index < id.size() ? id[session.id_index++] : 0xff;
    }
    case Phase::status: return static_cast<std::uint8_t>((write_enabled_ ? 2U : 0U) | (busy() ? 1U : 0U));
    case Phase::ignore: break;
    }
    return 0xff; // MISO sans donnée dans ce modèle, pas une mesure d'open-bus
}

} // namespace ravenemu::nds
