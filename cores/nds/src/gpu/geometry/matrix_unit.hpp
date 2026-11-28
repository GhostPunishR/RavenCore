#pragma once

#include "gpu/geometry/matrix.hpp"

#include <array>
#include <cstdint>

// Matrices courantes et piles (GBATEK, « DS 3D Matrix Load/Multiply », « DS
// 3D Matrix Stack ») :
// - MTX_MODE : 0 projection, 1 position, 2 position et vecteur ensemble,
//   3 texture. Les chargements et produits suivent le mode, sauf MTX_SCALE,
//   qui ne touche jamais la matrice des vecteurs ; PUSH, POP, STORE et
//   RESTORE agissent en mode 1 comme en mode 2.
// - Piles : projection et texture, une entrée et un pointeur de 1 bit ;
//   coordonnées et directions, 32 entrées et un pointeur partagé de 6 bits,
//   la moitié haute répétant la basse. Lire ou écrire l'entrée 1
//   (projection, texture) ou 31 à 63 (coordonnées) lève l'erreur.
// - Matrice de découpage : position × projection, recalculée à chaque
//   changement de l'une ou de l'autre.
namespace ravenemu::nds::geometry {

class MatrixUnit {
public:
    void set_mode(std::uint32_t parameter) noexcept { mode_ = parameter & 3U; }
    void load(const Matrix& matrix) noexcept;
    // C = m × C ; une échelle ne touche pas la matrice des vecteurs.
    void multiply(const Matrix& matrix, bool is_scale) noexcept;
    void push() noexcept;
    void pop(std::uint32_t parameter) noexcept;
    void store(std::uint32_t parameter) noexcept;
    void restore(std::uint32_t parameter) noexcept;

    [[nodiscard]] unsigned mode() const noexcept { return mode_; }
    [[nodiscard]] const Matrix& projection() const noexcept { return projection_; }
    [[nodiscard]] const Matrix& position() const noexcept { return position_; }
    [[nodiscard]] const Matrix& vector() const noexcept { return vector_; }
    [[nodiscard]] const Matrix& texture() const noexcept { return texture_; }
    [[nodiscard]] const Matrix& clip() const noexcept { return clip_; }

    [[nodiscard]] unsigned coordinate_level() const noexcept { return coordinate_pointer_; }  // 0 à 63
    [[nodiscard]] unsigned projection_level() const noexcept { return projection_pointer_; }  // 0 ou 1
    [[nodiscard]] bool error() const noexcept { return error_; }
    // GXSTAT bit 15 écrit à 1 : erreur effacée, pointeurs de projection et
    // de texture remis à 0.
    void acknowledge_error() noexcept;

private:
    void changed() noexcept { clip_ = geometry::multiply(position_, projection_); }
    // Pile d'une entrée (projection, texture) : l'entrée 1 répète la 0.
    void single_push(Matrix& stack, unsigned& pointer, const Matrix& current) noexcept;
    void single_pop(const Matrix& stack, unsigned& pointer, Matrix& current) noexcept;

    unsigned mode_{};
    Matrix projection_ = identity();
    Matrix position_ = identity();
    Matrix vector_ = identity();
    Matrix texture_ = identity();
    Matrix clip_ = identity();
    Matrix projection_stack_ = identity();
    Matrix texture_stack_ = identity();
    std::array<Matrix, 32> position_stack_{};
    std::array<Matrix, 32> vector_stack_{};
    unsigned projection_pointer_{};
    unsigned texture_pointer_{};
    unsigned coordinate_pointer_{};
    bool error_{};
};

} // namespace ravenemu::nds::geometry
