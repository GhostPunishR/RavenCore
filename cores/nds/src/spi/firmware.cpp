#include <ravenemu/nds/firmware.hpp>
#include <ravenemu/core.hpp>

namespace ravenemu::nds {

void Firmware::Session::deselect() noexcept {
    // Les commandes sans données prennent effet quand CS remonte.
    if (command == 0x06) write_enabled = true;
    else if (command == 0x04) write_enabled = false;
    else if (command == 0xb9) asleep = true;
    else if (command == 0xab) asleep = false;
    phase = Phase::command;
    address = address_bytes = id_index = 0;
    command = 0;
}

std::uint8_t Firmware::exchange(Session& session, std::uint8_t output) const {
    if (bytes_.empty()) throw Error("NDS : firmware absent pour le transfert SPI");
    using Phase = Session::Phase;
    switch (session.phase) {
    case Phase::command:
        if (session.asleep && output != 0xab) { session.phase = Phase::ignore; return 0xff; }
        switch (output) {
        case 0x03: case 0x0b: session.phase = Phase::address; break;
        case 0x05: session.phase = Phase::status; break;
        case 0x9f: session.phase = Phase::id; break;
        case 0x06: case 0x04: case 0xb9: case 0xab: session.phase = Phase::ignore; break;
        default: throw Error("NDS : commande flash firmware SPI non prise en charge (écriture/effacement inclus)");
        }
        session.command = output;
        break;
    case Phase::address:
        session.address = (session.address << 8U) | output;
        if (++session.address_bytes == 3) session.phase = session.command == 0x0b ? Phase::dummy : Phase::data;
        break;
    case Phase::dummy: session.phase = Phase::data; break;
    case Phase::data: {
        const auto value = bytes_[session.address % bytes_.size()];
        session.address = (session.address + 1U) & 0xffffffU;
        return value;
    }
    case Phase::id: {
        // Modèles matériels documentés : M45PE20 (256 Kio), M25PE40 (512 Kio).
        const std::array<std::uint8_t, 3> id = bytes_.size() == 0x40000 ?
            std::array<std::uint8_t, 3>{0x20,0x40,0x12} : std::array<std::uint8_t, 3>{0x20,0x80,0x13};
        return session.id_index < id.size() ? id[session.id_index++] : 0xff;
    }
    case Phase::status: return session.write_enabled ? 2 : 0;
    case Phase::ignore: break;
    }
    return 0xff; // MISO sans donnée dans ce modèle, pas une mesure d'open-bus
}

} // namespace ravenemu::nds
