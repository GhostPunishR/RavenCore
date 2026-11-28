#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

// File des commandes de géométrie (GBATEK, « DS 3D Geometry Commands ») :
// un FIFO de 256 entrées et un PIPE de 4 ; une entrée porte une commande de
// 8 bits et un paramètre de 32 bits. Une entrée va au PIPE si le FIFO est
// vide et le PIPE pas plein, sinon au FIFO ; quand le PIPE tombe sous 3
// entrées, 2 entrées passent du FIFO au PIPE. Une commande sans paramètre
// occupe une entrée, une commande à N paramètres en occupe N.
// Commandes valides : 10h-1Ch, 20h-2Bh, 30h-34h, 40h-41h, 50h, 60h, 70h-72h ;
// les autres sont ignorées sans prendre de paramètre.
namespace ravenemu::nds::geometry {

struct Entry {
    std::uint8_t command{};
    std::uint32_t parameter{};
};

// Nombre de paramètres d'une commande, -1 si elle n'existe pas.
constexpr int parameter_count(unsigned command) noexcept {
    switch (command) {
    case 0x10: return 1;   // MTX_MODE
    case 0x11: return 0;   // MTX_PUSH
    case 0x12:             // MTX_POP
    case 0x13:             // MTX_STORE
    case 0x14: return 1;   // MTX_RESTORE
    case 0x15: return 0;   // MTX_IDENTITY
    case 0x16: return 16;  // MTX_LOAD_4x4
    case 0x17: return 12;  // MTX_LOAD_4x3
    case 0x18: return 16;  // MTX_MULT_4x4
    case 0x19: return 12;  // MTX_MULT_4x3
    case 0x1a: return 9;   // MTX_MULT_3x3
    case 0x1b:             // MTX_SCALE
    case 0x1c: return 3;   // MTX_TRANS
    case 0x20:             // COLOR
    case 0x21:             // NORMAL
    case 0x22: return 1;   // TEXCOORD
    case 0x23: return 2;   // VTX_16
    case 0x24:             // VTX_10
    case 0x25:             // VTX_XY
    case 0x26:             // VTX_XZ
    case 0x27:             // VTX_YZ
    case 0x28:             // VTX_DIFF
    case 0x29:             // POLYGON_ATTR
    case 0x2a:             // TEXIMAGE_PARAM
    case 0x2b: return 1;   // PLTT_BASE
    case 0x30:             // DIF_AMB
    case 0x31:             // SPE_EMI
    case 0x32:             // LIGHT_VECTOR
    case 0x33: return 1;   // LIGHT_COLOR
    case 0x34: return 32;  // SHININESS
    case 0x40: return 1;   // BEGIN_VTXS
    case 0x41: return 0;   // END_VTXS
    case 0x50:             // SWAP_BUFFERS
    case 0x60: return 1;   // VIEWPORT
    case 0x70: return 3;   // BOX_TEST
    case 0x71: return 2;   // POS_TEST
    case 0x72: return 1;   // VEC_TEST
    default: return -1;
    }
}

// Entrées occupées par une commande valide.
constexpr std::size_t entries_of(unsigned command) noexcept {
    const auto count = parameter_count(command);
    return count <= 0 ? 1U : static_cast<std::size_t>(count);
}

class CommandFifo {
public:
    static constexpr std::size_t fifo_capacity = 256;
    static constexpr std::size_t pipe_capacity = 4;

    void push(Entry entry) {
        if (fifo_.empty() && pipe_.size() < pipe_capacity) pipe_.push_back(entry);
        else fifo_.push_back(entry);
    }
    [[nodiscard]] Entry front() const { return pipe_.front(); }
    void pop() {
        pipe_.pop_front();
        if (pipe_.size() < 3)
            for (unsigned moved = 0; moved < 2 && !fifo_.empty(); ++moved) {
                pipe_.push_back(fifo_.front());
                fifo_.pop_front();
            }
    }
    [[nodiscard]] bool empty() const noexcept { return pipe_.empty(); }
    [[nodiscard]] std::size_t total() const noexcept { return pipe_.size() + fifo_.size(); }
    // Entrées du FIFO seul (GXSTAT bits 16-24), au plus 256.
    [[nodiscard]] std::size_t fifo_count() const noexcept { return fifo_.size() < fifo_capacity ? fifo_.size() : fifo_capacity; }
    [[nodiscard]] bool full() const noexcept { return fifo_.size() >= fifo_capacity; }

private:
    std::deque<Entry> pipe_;
    std::deque<Entry> fifo_;
};

// GXFIFO (0x04000400) : un mot de commandes groupées (jusqu'à quatre, 00h
// ensuite pour aucune), puis les paramètres de chacune, dans l'ordre.
// `emit` reçoit chaque entrée prête.
class PackedCommands {
public:
    template <typename Emit>
    void write(std::uint32_t word, Emit&& emit) {
        if (remaining_ > 0) {
            emit(Entry{commands_[index_], word});
            if (--remaining_ == 0) advance(emit);
            return;
        }
        count_ = 0;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const auto command = static_cast<std::uint8_t>(word >> (8U * slot));
            if (slot > 0 && command == 0) break;
            if (parameter_count(command) >= 0) commands_[count_++] = command;
        }
        index_ = 0;
        start(emit);
    }

private:
    // Les commandes sans paramètre partent aussitôt ; la première qui en
    // attend arrête la file.
    template <typename Emit>
    void start(Emit&& emit) {
        while (index_ < count_) {
            const auto count = parameter_count(commands_[index_]);
            if (count > 0) {
                remaining_ = static_cast<unsigned>(count);
                return;
            }
            emit(Entry{commands_[index_], 0});
            ++index_;
        }
    }
    template <typename Emit>
    void advance(Emit&& emit) {
        ++index_;
        start(emit);
    }

    std::array<std::uint8_t, 4> commands_{};
    unsigned count_{};
    unsigned index_{};
    unsigned remaining_{};
};

} // namespace ravenemu::nds::geometry
