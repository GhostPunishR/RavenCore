#pragma once

#include "cartridge/cartridge_header.hpp"
#include <ravenemu/infrared_endpoint.hpp>

namespace ravenemu::cgb {

class Cartridge {
public:
    static constexpr int rom_bank_size = 0x4000;
    static constexpr int ram_bank_size = 0x2000;
    using Clock = std::function<std::int64_t()>;

    Cartridge(RomImage rom, CartridgeHeader header)
        : rom_(std::move(rom)), header_(header), ram_(static_cast<std::size_t>(header.ram_size)) {}
    virtual ~Cartridge() = default;

    [[nodiscard]] virtual int read_rom(int address) const = 0;
    /**
     * Banque de ROM que le CPU voit dans la fenêtre de 16 Kio de `address`
     * (0x0000-0x3FFF ou 0x4000-0x7FFF), ou -1 quand cette fenêtre n'est pas
     * une simple banque (demi-banques du MBC6, menu du MMM01).
     */
    [[nodiscard]] virtual int rom_bank_at(int) const noexcept { return -1; }
    /**
     * La ROM telle que le CPU la lit à partir de `address`, sans sortir de sa
     * fenêtre de 16 Kio : d'un bloc pour une simple banque, sinon octet par
     * octet. Sert aux lecteurs extérieurs (succès), qui relisent les deux
     * fenêtres à chaque image.
     */
    void read_rom_window(int address, std::span<std::uint8_t> out) const {
        const int bank = rom_bank_at(address);
        if (bank < 0) {
            for (std::size_t index = 0; index < out.size(); ++index)
                out[index] = static_cast<std::uint8_t>(read_rom(address + static_cast<int>(index)));
            return;
        }
        const auto start = static_cast<std::size_t>(bank) * rom_bank_size +
                           static_cast<std::size_t>(address & (rom_bank_size - 1));
        const auto available = start < rom_->size() ? std::min(out.size(), rom_->size() - start) : 0;
        std::copy_n(rom_->begin() + static_cast<std::ptrdiff_t>(start), available, out.begin());
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(available), out.end(), std::uint8_t{0xff});
    }
    virtual void write_control(int address, int value) = 0;
    [[nodiscard]] virtual int read_ram(int address) const = 0;
    virtual void write_ram(int address, int value) = 0;
    virtual void tick(int) {}
    [[nodiscard]] virtual bool rumble_active() const noexcept { return false; }
    virtual void set_acceleration(int, int) noexcept {}
    [[nodiscard]] virtual bool connect_infrared_endpoint(InfraredEndpoint*) noexcept {
        return true;
    }
    virtual void disconnect_infrared_endpoint() noexcept {}
    virtual void save_state(BinaryWriter& out) const = 0;
    virtual void load_state(BinaryReader& in) = 0;

    /**
     * Mémoire de cartouche qu'un lecteur extérieur peut inspecter.
     *
     * Virtuelle parce que `ram_` n'est pas de la mémoire de cartouche pour
     * tous les mappeurs : la classe de base l'alloue d'après l'en-tête, et un
     * mappeur dont les données vivent ailleurs le laisse à zéro. Le rendre
     * quand même ferait passer un tampon vierge pour de la mémoire lue, et un
     * lecteur ne pourrait plus distinguer une région qu'il ne sait pas lire
     * d'une région pleine de zéros. Un tel mappeur rend une étendue vide.
     */
    [[nodiscard]] virtual std::span<const std::uint8_t> achievement_ram() const noexcept { return ram_; }
    /**
     * Code GameShark : écrit dans la banque `bank` de la RAM de cartouche,
     * activée ou non et quelle que soit la banque que voit le CPU. Rien pour
     * un mappeur dont la mémoire n'est pas `ram_` (voir achievement_ram).
     */
    void write_ram_bank(int bank, int address, int value) {
        if (ram_.empty() || achievement_ram().data() != ram_.data()) return;
        const auto offset = (static_cast<std::size_t>(bank) * ram_bank_size +
                             static_cast<std::size_t>(address & (ram_bank_size - 1))) % ram_.size();
        const auto byte = static_cast<std::uint8_t>(value);
        if (ram_[offset] == byte) return;
        ram_[offset] = byte;
        mark_written();
    }
    [[nodiscard]] const CartridgeHeader& header() const noexcept { return header_; }
    [[nodiscard]] bool dirty() const noexcept { return generation_ != saved_generation_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bool has_persistent_data() const noexcept {
        return header_.has_battery && (!ram_.empty() || header_.has_rtc);
    }

    virtual std::optional<std::vector<std::uint8_t>> export_battery() {
        if (!header_.has_battery || ram_.empty()) return std::nullopt;
        return ram_;
    }

    virtual void import_battery(std::span<const std::uint8_t> data) {
        if (ram_.empty()) return;
        std::copy_n(data.begin(), static_cast<std::ptrdiff_t>(std::min(data.size(), ram_.size())), ram_.begin());
        mark_clean();
    }

    void acknowledge_saved(std::uint64_t generation) noexcept {
        if (generation == generation_) saved_generation_ = generation;
    }

    // Une restauration remplace la RAM, pas le fichier déjà écrit. Repartir
    // après l'ancienne génération invalide aussi ses acquittements en vol.
    void mark_restored_after(std::uint64_t previous_generation) noexcept {
        generation_ = previous_generation + 1;
        saved_generation_ = previous_generation;
    }
    // Reprend le compteur de la cartouche qu'elle remplace au reset : une
    // génération déjà rendue ne resservira jamais, et une écriture pas encore
    // enregistrée reste à enregistrer.
    void continue_after(std::uint64_t previous_generation, bool previous_dirty) noexcept {
        generation_ = previous_generation + (previous_dirty ? 1U : 0U);
        saved_generation_ = previous_generation;
    }

    [[nodiscard]] static std::unique_ptr<Cartridge> create(
        RomImage rom,
        Clock clock
    );

protected:
    [[nodiscard]] int rom_bank_count() const noexcept {
        return std::max(2, static_cast<int>(rom_->size() / rom_bank_size));
    }
    [[nodiscard]] int normalize_rom_bank(int bank) const noexcept { return bank % rom_bank_count(); }
    // Octet de `address` dans la banque `bank`, 0xFF au-delà de la ROM.
    [[nodiscard]] int rom_byte(int bank, int address) const noexcept {
        const auto offset = static_cast<std::size_t>(bank * rom_bank_size + (address & (rom_bank_size - 1)));
        return offset < rom_->size() ? (*rom_)[offset] : 0xff;
    }
    void mark_written() noexcept { ++generation_; }
    void mark_clean() noexcept { saved_generation_ = generation_; }

    RomImage rom_;
    CartridgeHeader header_;
    std::vector<std::uint8_t> ram_;

private:
    std::uint64_t generation_{};
    std::uint64_t saved_generation_{};
};

} // namespace ravenemu::cgb

