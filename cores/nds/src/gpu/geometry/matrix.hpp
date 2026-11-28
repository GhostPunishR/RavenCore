#pragma once

#include <array>
#include <cstdint>
#include <span>

// Matrices du moteur de géométrie (GBATEK, « DS 3D Matrix Types », « DS 3D
// Matrix Examples (Maths Basics) ») : 4 × 4 éléments de 32 bits signés, 12
// bits après la virgule, rangés ligne par ligne (m[0] à m[3] : première
// ligne). Un produit garde ses fractions de 24 bits pendant les additions,
// puis décale le résultat de 12 bits.
namespace ravenemu::nds::geometry {

using Matrix = std::array<std::int32_t, 16>;

inline constexpr std::int32_t one = 1 << 12;

constexpr Matrix identity() noexcept {
    Matrix matrix{};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = one;
    return matrix;
}

// a × b, ligne de a par colonne de b.
constexpr Matrix multiply(const Matrix& a, const Matrix& b) noexcept {
    Matrix result{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            std::int64_t sum = 0;
            for (unsigned k = 0; k < 4; ++k) sum += std::int64_t{a[row * 4 + k]} * b[k * 4 + column];
            result[row * 4 + column] = static_cast<std::int32_t>(sum >> 12);
        }
    return result;
}

// Vecteur ligne (x, y, z, w) × matrice.
constexpr std::array<std::int32_t, 4> transform(const std::array<std::int32_t, 4>& vector, const Matrix& matrix) noexcept {
    std::array<std::int32_t, 4> result{};
    for (unsigned column = 0; column < 4; ++column) {
        std::int64_t sum = 0;
        for (unsigned k = 0; k < 4; ++k) sum += std::int64_t{vector[k]} * matrix[k * 4 + column];
        result[column] = static_cast<std::int32_t>(sum >> 12);
    }
    return result;
}

// Matrices données par leurs paramètres (« DS 3D Matrix Types ») : 4 × 3 et
// 3 × 3 complétées de 0 et de 1,0 ; échelle et translation.
inline Matrix from_4x4(std::span<const std::uint32_t> m) noexcept {
    Matrix matrix{};
    for (unsigned index = 0; index < 16; ++index) matrix[index] = static_cast<std::int32_t>(m[index]);
    return matrix;
}

inline Matrix from_4x3(std::span<const std::uint32_t> m) noexcept {
    Matrix matrix{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 3; ++column) matrix[row * 4 + column] = static_cast<std::int32_t>(m[row * 3 + column]);
    matrix[15] = one;
    return matrix;
}

inline Matrix from_3x3(std::span<const std::uint32_t> m) noexcept {
    Matrix matrix{};
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column) matrix[row * 4 + column] = static_cast<std::int32_t>(m[row * 3 + column]);
    matrix[15] = one;
    return matrix;
}

inline Matrix scale(std::span<const std::uint32_t> m) noexcept {
    Matrix matrix{};
    for (unsigned axis = 0; axis < 3; ++axis) matrix[axis * 5] = static_cast<std::int32_t>(m[axis]);
    matrix[15] = one;
    return matrix;
}

inline Matrix translation(std::span<const std::uint32_t> m) noexcept {
    auto matrix = identity();
    for (unsigned axis = 0; axis < 3; ++axis) matrix[12 + axis] = static_cast<std::int32_t>(m[axis]);
    return matrix;
}

} // namespace ravenemu::nds::geometry
