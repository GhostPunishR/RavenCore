#include "cartridge/save_memory.hpp"

#include <algorithm>

namespace ravenemu::nds {

namespace {

constexpr bool addressed_command(std::uint8_t command) noexcept {
    return command == 0x02 || command == 0x03 || command == 0x0a || command == 0x0b || command == 0xd8 || command == 0xdb;
}

// Identifiants JEDEC des puces Flash de GBATEK : ST M45PE20, 45PE40, 45PE80,
// Macronix MX25L6445E.
constexpr std::array<std::uint8_t, 3> flash_id(SaveType type) noexcept {
    switch (type) {
    case SaveType::flash_512k: return {0x20, 0x40, 0x13};
    case SaveType::flash_1m: return {0x20, 0x40, 0x14};
    case SaveType::flash_8m: return {0xc2, 0x20, 0x17};
    default: return {0x20, 0x40, 0x12};
    }
}

} // namespace

std::size_t save_size(SaveType type) noexcept {
    switch (type) {
    case SaveType::none:
    case SaveType::automatic: return 0;
    case SaveType::eeprom_512: return 512;
    case SaveType::eeprom_8k:
    case SaveType::fram_8k: return 8 * 1024;
    case SaveType::fram_32k: return 32 * 1024;
    case SaveType::eeprom_64k: return 64 * 1024;
    case SaveType::eeprom_128k: return 128 * 1024;
    case SaveType::flash_256k: return 256 * 1024;
    case SaveType::flash_512k: return 512 * 1024;
    case SaveType::flash_1m: return 1024 * 1024;
    case SaveType::flash_8m: return 8 * 1024 * 1024;
    }
    return 0;
}

SaveType save_type_for_size(std::size_t size) noexcept {
    for (const auto type : {SaveType::eeprom_512, SaveType::eeprom_8k, SaveType::fram_32k, SaveType::eeprom_64k,
                            SaveType::eeprom_128k, SaveType::flash_256k, SaveType::flash_512k, SaveType::flash_1m,
                            SaveType::flash_8m}) {
        if (save_size(type) == size) return type;
    }
    return SaveType::none;
}

void SaveMemory::set_type(SaveType type) {
    type_ = type;
    data_.assign(save_size(type), 0xff);
    selected_ = powered_down_ = write_enable_ = provisional_ = false;
    protect_ = 0;
    dirty_ = false;
}

// Change de puce au milieu d'une commande : WEL et la sélection restent.
void SaveMemory::become(SaveType type) {
    type_ = type;
    data_.assign(save_size(type), 0xff);
    provisional_ = false;
}

bool SaveMemory::load(std::span<const std::uint8_t> data) {
    if (type_ == SaveType::none || type_ == SaveType::automatic) {
        const auto type = save_type_for_size(data.size());
        if (type == SaveType::none) return false;
        set_type(type);
    }
    if (data.size() != data_.size()) return false;
    std::copy(data.begin(), data.end(), data_.begin());
    dirty_ = false;
    return true;
}

bool SaveMemory::flash() const noexcept {
    return type_ == SaveType::flash_256k || type_ == SaveType::flash_512k || type_ == SaveType::flash_1m ||
           type_ == SaveType::flash_8m;
}

unsigned SaveMemory::address_bytes() const noexcept {
    if (type_ == SaveType::eeprom_512) return 1;
    return type_ == SaveType::eeprom_128k || flash() ? 3U : 2U;
}

std::uint32_t SaveMemory::page_size() const noexcept {
    switch (type_) {
    case SaveType::eeprom_512: return 16;
    case SaveType::eeprom_8k: return 32;
    case SaveType::eeprom_64k: return 128;
    case SaveType::fram_8k:
    case SaveType::fram_32k: return static_cast<std::uint32_t>(data_.size());
    default: return 256;
    }
}

std::uint8_t SaveMemory::status() const noexcept {
    const auto latch = static_cast<std::uint8_t>(write_enable_ ? 2U : 0U);
    if (flash()) return latch;
    if (type_ == SaveType::eeprom_512) return static_cast<std::uint8_t>(0xf0U | latch | (protect_ & 0x0cU));
    return static_cast<std::uint8_t>(latch | protect_);
}

// WP : 1 = quart haut, 2 = moitié haute, 3 = toute la mémoire (EEPROM, FRAM).
bool SaveMemory::protected_address(std::uint32_t address) const noexcept {
    if (flash()) return false;
    const auto size = static_cast<std::uint32_t>(data_.size());
    switch ((protect_ >> 2U) & 3U) {
    case 1: return address >= size - size / 4U;
    case 2: return address >= size / 2U;
    case 3: return true;
    default: return false;
    }
}

void SaveMemory::start(std::uint8_t command) noexcept {
    selected_ = true;
    command_ = command;
    received_ = 0;
    address_ = 0;
    pending_count_ = 0;
    if (powered_down_) return;
    // Flash choisie par une lecture : la première écriture tranche.
    if (provisional_ && (command == 0x02 || command == 0x0a || command == 0xdb || command == 0xd8)) {
        if (command == 0x02) become(SaveType::eeprom_128k);
        provisional_ = false;
    }
    if (command == 0x06) write_enable_ = true;
    if (command == 0x04) write_enable_ = false;
    if (command == 0xb9 && flash()) powered_down_ = true;
}

std::uint8_t SaveMemory::transfer(std::uint8_t value, std::uint32_t instruction) {
    if (type_ == SaveType::none) return 0xff;
    if (!selected_) {
        start(value);
        command_instruction_ = instruction;
        return 0xff;
    }
    if (powered_down_) return 0xff;
    if (type_ == SaveType::automatic) return detect(value, instruction);
    ++received_;
    return data_byte(value);
}

// Type inconnu : les octets d'adresse s'accumulent tant que l'instruction de
// la commande les écrit ; le premier octet venu d'ailleurs fixe la largeur
// d'adresse, puis la commande est rejouée sur la puce choisie.
std::uint8_t SaveMemory::detect(std::uint8_t value, std::uint32_t instruction) {
    if (!addressed_command(command_)) {
        ++received_;
        return data_byte(value);  // RDSR, RDID, WRSR : réponses communes
    }
    if (instruction == command_instruction_ && pending_count_ < pending_.size()) {
        pending_[pending_count_++] = value;
        return 0xff;
    }
    if (!decide()) return 0xff;  // largeur inconnue : rien n'est choisi
    ++received_;
    return data_byte(value);
}

// Choisit la puce d'après la largeur d'adresse retenue, puis rejoue la
// commande et son adresse. Rend false si la largeur ne dit rien.
bool SaveMemory::decide() {
    const auto width = pending_count_;
    if (width == 0 || width > 3) return false;
    const auto address = pending_;
    if (width == 1) become(SaveType::eeprom_512);
    else if (width == 2) become(SaveType::eeprom_64k);
    else if (command_ == 0x02) become(SaveType::eeprom_128k);
    else {
        become(SaveType::flash_1m);
        provisional_ = command_ == 0x03 || command_ == 0x0b;
    }
    for (unsigned index = 0; index < width; ++index) {
        ++received_;
        static_cast<void>(data_byte(address[index]));
    }
    return true;
}

// Octet suivant la commande : adresse, octet muet, puis données.
std::uint8_t SaveMemory::data_byte(std::uint8_t value) {
    switch (command_) {
    case 0x05: return status();
    case 0x9f: return flash() && received_ <= 3 ? flash_id(type_)[received_ - 1] : 0xff;
    case 0x01:
        if (received_ == 1 && write_enable_ && !flash())
            protect_ = static_cast<std::uint8_t>(value & (type_ == SaveType::eeprom_512 ? 0x0cU : 0x8cU));
        return 0xff;
    default: break;
    }
    // 0Bh et 0Ah n'existent que sur l'EEPROM de 0,5 Kio et la Flash.
    const auto extended = type_ == SaveType::eeprom_512 || flash();
    const auto reading = command_ == 0x03 || (command_ == 0x0b && extended);
    const auto writing = command_ == 0x02 || (command_ == 0x0a && extended);
    const auto erasing = flash() && (command_ == 0xdb || command_ == 0xd8);
    if (!reading && !writing && !erasing) return 0xff;
    if (received_ <= address_bytes()) {
        address_ = (address_ << 8U) | value;
        if (received_ == address_bytes()) {
            // EEPROM de 0,5 Kio : 0Bh et 0Ah visent 100h-1FFh.
            if (type_ == SaveType::eeprom_512 && (command_ == 0x0b || command_ == 0x0a)) address_ |= 0x100U;
            address_ %= static_cast<std::uint32_t>(data_.size());
            if (erasing && write_enable_) erase(command_ == 0xdb ? 0x100U : 0x1'0000U);
        }
        return 0xff;
    }
    if (reading) {
        if (flash() && command_ == 0x0b && received_ == address_bytes() + 1) return 0xff;  // octet muet
        const auto byte = data_[address_];
        address_ = (address_ + 1U) % static_cast<std::uint32_t>(data_.size());
        return byte;
    }
    if (writing) write_byte(value, flash() && command_ == 0x02);
    return 0xff;
}

void SaveMemory::write_byte(std::uint8_t value, bool program_only) {
    if (!write_enable_ || protected_address(address_)) return;
    auto& byte = data_[address_];
    byte = program_only ? static_cast<std::uint8_t>(byte & value) : value;
    dirty_ = true;
    const auto page = page_size();
    address_ = (address_ & ~(page - 1U)) | ((address_ + 1U) & (page - 1U));
}

void SaveMemory::erase(std::uint32_t size) {
    const auto begin = address_ & ~(size - 1U);
    const auto end = std::min<std::size_t>(begin + size, data_.size());
    std::fill(data_.begin() + begin, data_.begin() + static_cast<std::ptrdiff_t>(end), std::uint8_t{0xff});
    dirty_ = true;
}

// La fin d'une écriture, d'un effacement ou de WRSR remet WEL à 0 ; RDP
// réveille la Flash.
void SaveMemory::deselect() noexcept {
    if (!selected_) return;
    if (type_ == SaveType::automatic && (command_ == 0xdb || command_ == 0xd8)) static_cast<void>(decide());
    selected_ = false;
    if (powered_down_) {
        if (command_ == 0xab) powered_down_ = false;
        return;
    }
    switch (command_) {
    case 0x01:
    case 0x02:
    case 0x0a:
    case 0xdb:
    case 0xd8: write_enable_ = false; break;
    default: break;
    }
}

} // namespace ravenemu::nds
