#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Affichage depuis la RAM principale (GBATEK, « DS Video Capture and Main
// Memory Display Mode ») : en mode 3 de DISPCNT, le moteur A montre une image
// de 256 × 192 points en 15 bits envoyée à DISP_MMEM_FIFO (0x04000068), mot
// par mot, deux points par mot, le premier dans les bits bas.
// La DMA de l'ARM9 en mode 4 (« Main memory display ») l'alimente par rafales
// (4 mots, 8 points, d'après GBATEK) ; un canal activé attend la ligne 0 de
// l'image suivante pour commencer.
// RavenCore remplit la file au début de chaque ligne visible, jusqu'à la
// ligne entière (128 mots), puis en retire la ligne. La profondeur réelle du
// FIFO n'est pas donnée par GBATEK : la file garde ici au plus deux lignes ;
// un point qui manque s'affiche noir.
namespace ravenemu::nds {

class MainMemoryDisplay {
public:
    static constexpr std::size_t line_words = 128;
    static constexpr std::size_t capacity = 2 * line_words;

    void push(std::uint32_t word) noexcept {
        if (size_ == capacity) return;
        words_[(head_ + size_) % capacity] = word;
        ++size_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Les 256 points d'une ligne, retirés de la file.
    [[nodiscard]] std::array<std::uint16_t, 2 * line_words> take_line() noexcept {
        std::array<std::uint16_t, 2 * line_words> pixels{};
        for (std::size_t index = 0; index < line_words && size_ > 0; ++index) {
            const auto word = words_[head_];
            head_ = (head_ + 1) % capacity;
            --size_;
            pixels[index * 2] = static_cast<std::uint16_t>(word & 0x7fffU);
            pixels[index * 2 + 1] = static_cast<std::uint16_t>((word >> 16U) & 0x7fffU);
        }
        return pixels;
    }

    // Canaux DMA de l'ARM9 autorisés à alimenter la file pendant l'image :
    // ceux qui étaient actifs en mode 4 à sa ligne 0.
    [[nodiscard]] bool armed(unsigned channel) const noexcept { return ((armed_ >> channel) & 1U) != 0; }
    void arm(unsigned channel) noexcept { armed_ |= 1U << channel; }
    // Un canal (ré)activé attend l'image suivante.
    void disarm(unsigned channel) noexcept { armed_ &= ~(1U << channel); }

private:
    std::array<std::uint32_t, capacity> words_{};
    std::size_t head_{};
    std::size_t size_{};
    std::uint32_t armed_{};
};

} // namespace ravenemu::nds
