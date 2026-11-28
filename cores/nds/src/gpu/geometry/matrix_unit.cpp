#include "gpu/geometry/matrix_unit.hpp"

namespace ravenemu::nds::geometry {

namespace {

constexpr unsigned projection_mode = 0;
constexpr unsigned position_mode = 1;
constexpr unsigned both_mode = 2;

} // namespace

void MatrixUnit::load(const Matrix& matrix) noexcept {
    switch (mode_) {
    case projection_mode: projection_ = matrix; break;
    case position_mode: position_ = matrix; break;
    case both_mode:
        position_ = matrix;
        vector_ = matrix;
        break;
    default: texture_ = matrix; return;
    }
    changed();
}

void MatrixUnit::multiply(const Matrix& matrix, bool is_scale) noexcept {
    switch (mode_) {
    case projection_mode: projection_ = geometry::multiply(matrix, projection_); break;
    case position_mode: position_ = geometry::multiply(matrix, position_); break;
    case both_mode:
        position_ = geometry::multiply(matrix, position_);
        if (!is_scale) vector_ = geometry::multiply(matrix, vector_);
        break;
    default: texture_ = geometry::multiply(matrix, texture_); return;
    }
    changed();
}

void MatrixUnit::single_push(Matrix& stack, unsigned& pointer, const Matrix& current) noexcept {
    if (pointer != 0) error_ = true;
    stack = current;
    pointer = (pointer + 1) & 1U;
}

void MatrixUnit::single_pop(const Matrix& stack, unsigned& pointer, Matrix& current) noexcept {
    pointer = (pointer - 1) & 1U;
    if (pointer != 0) error_ = true;
    current = stack;
}

void MatrixUnit::push() noexcept {
    if (mode_ == projection_mode) return single_push(projection_stack_, projection_pointer_, projection_);
    if (mode_ == 3) return single_push(texture_stack_, texture_pointer_, texture_);
    if (coordinate_pointer_ >= 31) error_ = true;
    position_stack_[coordinate_pointer_ & 31U] = position_;
    vector_stack_[coordinate_pointer_ & 31U] = vector_;
    coordinate_pointer_ = (coordinate_pointer_ + 1) & 63U;
}

void MatrixUnit::pop(std::uint32_t parameter) noexcept {
    if (mode_ == projection_mode) {
        single_pop(projection_stack_, projection_pointer_, projection_);
        return changed();
    }
    if (mode_ == 3) return single_pop(texture_stack_, texture_pointer_, texture_);
    // Décalage signé de 6 bits, sur un pointeur de 6 bits : modulo 64.
    coordinate_pointer_ = (coordinate_pointer_ - (parameter & 0x3fU)) & 63U;
    if (coordinate_pointer_ >= 31) error_ = true;
    position_ = position_stack_[coordinate_pointer_ & 31U];
    vector_ = vector_stack_[coordinate_pointer_ & 31U];
    changed();
}

void MatrixUnit::store(std::uint32_t parameter) noexcept {
    if (mode_ == projection_mode) {
        projection_stack_ = projection_;
        return;
    }
    if (mode_ == 3) {
        texture_stack_ = texture_;
        return;
    }
    const auto index = parameter & 31U;
    if (index == 31) error_ = true;
    position_stack_[index] = position_;
    vector_stack_[index] = vector_;
}

void MatrixUnit::restore(std::uint32_t parameter) noexcept {
    if (mode_ == projection_mode) {
        projection_ = projection_stack_;
        return changed();
    }
    if (mode_ == 3) {
        texture_ = texture_stack_;
        return;
    }
    const auto index = parameter & 31U;
    if (index == 31) error_ = true;
    position_ = position_stack_[index];
    vector_ = vector_stack_[index];
    changed();
}

void MatrixUnit::acknowledge_error() noexcept {
    error_ = false;
    projection_pointer_ = 0;
    texture_pointer_ = 0;
}

} // namespace ravenemu::nds::geometry
