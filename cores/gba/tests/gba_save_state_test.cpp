#include "save/save_factory.hpp"
#include "check.hpp"

using namespace ravenemu;
using namespace ravenemu::gba;
using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

std::vector<std::uint8_t> snapshot(const SaveMemory& memory) {
    BinaryWriter out;
    memory.save_state(out);
    return std::move(out).take();
}

std::unique_ptr<SaveMemory> restore(const SaveMemory& memory) {
    auto restored = make_save(memory.type());
    const auto bytes = snapshot(memory);
    BinaryReader in(bytes);
    restored->load_state(in);
    check(in.exhausted(), "données de contrôleur restantes");
    return restored;
}

void flash_transactions() {
    // Sélection de banque, programmation, identification, effacement secteur
    // puis puce. Chaque frontière de commande doit pouvoir être restaurée.
    const std::vector<std::pair<int, int>> writes{
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x5555, 0xb0}, {0, 1},
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x5555, 0xa0}, {0x123, 0x42},
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x5555, 0x90},
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x5555, 0xf0},
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x5555, 0x80},
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x100, 0x30},
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x5555, 0x80},
        {0x5555, 0xaa}, {0x2aaa, 0x55}, {0x5555, 0x10},
    };
    for (const auto type : {GbaSaveType::flash_64k, GbaSaveType::flash_128k}) {
        for (std::size_t split = 0; split <= writes.size(); ++split) {
            Flash original(type);
            for (std::size_t i = 0; i < split; ++i) original.write(writes[i].first, writes[i].second);
            auto restored = restore(original);
            for (const int address : {0, 1, 0x123}) {
                check(original.read(address) == restored->read(address), "banque/mode Flash perdu");
            }
            for (std::size_t i = split; i < writes.size(); ++i) {
                original.write(writes[i].first, writes[i].second);
                restored->write(writes[i].first, writes[i].second);
                check(snapshot(original) == snapshot(*restored), "transaction Flash divergente");
            }
        }
    }
    Flash memory(GbaSaveType::flash_128k);
    auto bytes = snapshot(memory);
    bytes.back() = 2; // banque hors limites (i32 big endian)
    BinaryReader invalid(bytes);
    expect_failure<SaveStateError>([&] { memory.load_state(invalid); }, "banque Flash invalide acceptée");
}

void eeprom_transactions() {
    for (const auto type : {GbaSaveType::eeprom_512, GbaSaveType::eeprom_8k}) {
        const int address_bits = type == GbaSaveType::eeprom_512 ? 6 : 14;
        std::vector<int> write_bits{1, 0};
        for (int bit = address_bits - 1; bit >= 0; --bit) write_bits.push_back((3 >> bit) & 1);
        for (int bit = 0; bit < 64; ++bit) write_bits.push_back(bit % 3 == 0 ? 1 : 0);
        write_bits.push_back(0);
        for (std::size_t split = 0; split <= write_bits.size(); ++split) {
            Eeprom original(type);
            original.hint_transfer_length(address_bits == 6 ? 73 : 81);
            for (std::size_t i = 0; i < split; ++i) original.write(0, write_bits[i]);
            auto restored = restore(original);
            for (std::size_t i = split; i < write_bits.size(); ++i) {
                original.write(0, write_bits[i]); restored->write(0, write_bits[i]);
            }
            check(snapshot(original) == snapshot(*restored), "écriture EEPROM divergente");
        }
        for (int split = 0; split < 68; ++split) {
            Eeprom original(type);
            for (const int bit : write_bits) original.write(0, bit);
            original.write(0, 1); original.write(0, 1);
            for (int bit = address_bits - 1; bit >= 0; --bit) original.write(0, (3 >> bit) & 1);
            original.write(0, 0);
            for (int i = 0; i < split; ++i) static_cast<void>(original.read(0));
            auto restored = restore(original);
            for (int i = split; i < 70; ++i) {
                check(original.read(0) == restored->read(0), "lecture EEPROM divergente");
            }
        }
        Eeprom memory(type);
        auto bytes = snapshot(memory);
        bytes[memory.data().size() + 7] = 5; // State inexistant
        BinaryReader invalid(bytes);
        expect_failure<SaveStateError>([&] { memory.load_state(invalid); }, "état EEPROM invalide accepté");
    }
}

int main() {
    flash_transactions();
    eeprom_transactions();
}
