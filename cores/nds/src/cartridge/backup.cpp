#include <ravenemu/nds/backup.hpp>
#include <ravenemu/core.hpp>

#include <utility>

namespace ravenemu::nds {
namespace {
// Cycle d'écriture de 5 ms sur l'horloge commune 33 513 982 Hz.
constexpr std::uint32_t write_cycles = 167570;
}

void Backup::load(BackupType type, std::span<const std::uint8_t> bytes, std::uint8_t status) {
    std::size_t size;
    switch (type) {
    case BackupType::none: size = 0; break;
    case BackupType::eeprom_512: size = 512; break;
    case BackupType::eeprom_8k: size = 8192; break;
    case BackupType::eeprom_64k: size = 65536; break;
    default: throw Error("NDS : profil de sauvegarde inconnu");
    }
    const auto status_mask = type == BackupType::eeprom_512 ? 0x0cU : (size ? 0x8cU : 0U);
    if ((!bytes.empty() && bytes.size() != size) || (status & ~status_mask))
        throw Error("NDS : taille ou état de sauvegarde incompatible avec le profil EEPROM");
    Backup prepared;
    prepared.type_ = type;
    prepared.bytes_.assign(size,0xff);
    if (!bytes.empty()) prepared.bytes_.assign(bytes.begin(),bytes.end());
    prepared.status_ = status;
    prepared.generation_ = prepared.saved_generation_ = generation_ + 1U;
    *this = std::move(prepared);
}

BackupSnapshot Backup::snapshot() const {
    if (type_ == BackupType::none) throw Error("NDS : aucune sauvegarde cartouche configurée");
    if (busy()) throw Error("NDS : écriture EEPROM en cours");
    return {bytes_,generation_,status_};
}

unsigned Backup::page_size() const noexcept {
    return type_ == BackupType::eeprom_512 ? 16U : (type_ == BackupType::eeprom_8k ? 32U : 128U);
}

bool Backup::protected_address(std::uint32_t address) const noexcept {
    const auto protection = (status_ >> 2U) & 3U;
    if (!protection) return false;
    const auto size = static_cast<std::uint32_t>(bytes_.size());
    return address >= size - (protection == 1 ? size / 4U : (protection == 2 ? size / 2U : size));
}

std::uint8_t Backup::exchange(Session& session, std::uint8_t output) const {
    if (bytes_.empty()) return 0xff;
    using Phase = Session::Phase;
    switch (session.phase) {
    case Phase::command:
        session.command = output;
        if (busy() && output != 0x05) { session.command = 0; session.phase = Phase::ignore; break; }
        switch (output) {
        case 0x06: case 0x04: case 0x9f: session.phase = Phase::ignore; break;
        case 0x05: session.phase = Phase::status; break;
        case 0x01: session.phase = Phase::write_status; break;
        case 0x0a: case 0x0b:
            if (type_ != BackupType::eeprom_512) throw Error("NDS : commande EEPROM 9 bits sur un autre profil");
            [[fallthrough]];
        case 0x02: case 0x03:
            session.phase = Phase::address;
            session.address = (output & 8U) ? 1U : 0U;
            session.address_bytes = type_ == BackupType::eeprom_512 ? 1U : 2U;
            break;
        default: throw Error("NDS : commande EEPROM cartouche non prise en charge");
        }
        break;
    case Phase::address:
        session.address = ((session.address << 8U) | output) & static_cast<std::uint32_t>(bytes_.size() - 1U);
        if (--session.address_bytes == 0)
            session.phase = (session.command & 1U) ? Phase::read : Phase::write;
        break;
    case Phase::read: {
        const auto value = bytes_[session.address];
        session.address = (session.address + 1U) & static_cast<std::uint32_t>(bytes_.size() - 1U);
        return value;
    }
    case Phase::write: {
        const auto mask = page_size() - 1U;
        const auto index = session.address & mask;
        session.page[index] = output;
        session.written.set(index);
        session.address = (session.address & ~mask) | ((index + 1U) & mask);
        break;
    }
    case Phase::status:
        return static_cast<std::uint8_t>(status_ | (busy() ? 1U : 0U) | (write_enabled_ ? 2U : 0U) |
                                        (type_ == BackupType::eeprom_512 ? 0xf0U : 0U));
    case Phase::write_status:
        session.status = output & (type_ == BackupType::eeprom_512 ? 0x0cU : 0x8cU);
        session.has_status = true;
        session.phase = Phase::ignore;
        break;
    case Phase::ignore: break;
    }
    return 0xff;
}

void Backup::deselect(Session& session) noexcept {
    if (!bytes_.empty() && !busy()) {
        if (session.command == 0x06) write_enabled_ = true;
        else if (session.command == 0x04) write_enabled_ = false;
        else if (write_enabled_ && (session.written.any() || session.has_status)) {
            const auto address = session.address & ~(page_size() - 1U);
            // /W est câblé haut sur les EEPROM des cartouches DS : SRWD ne bloque pas WRSR.
            if (session.has_status || !protected_address(address)) {
                operation_ = {address,write_cycles,session.page,session.written,session.status,session.has_status};
            } else write_enabled_ = false;
        }
    }
    session = {};
}

void Backup::advance_cycles(std::uint64_t cycles) noexcept {
    if (!busy()) return;
    if (cycles < operation_.remaining) { operation_.remaining -= static_cast<std::uint32_t>(cycles); return; }
    bool changed = false;
    if (operation_.write_status) {
        changed = status_ != operation_.status;
        status_ = operation_.status;
    } else for (unsigned i = 0; i < page_size(); ++i) if (operation_.written.test(i)) {
        auto& byte = bytes_[operation_.address + i];
        changed |= byte != operation_.page[i];
        byte = operation_.page[i];
    }
    if (changed) ++generation_;
    write_enabled_ = false;
    operation_ = {};
}

} // namespace ravenemu::nds
