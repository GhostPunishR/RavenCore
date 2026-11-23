#pragma once

#include "interrupts/interrupt_controller.hpp"

#include <array>
#include <cstdint>

// Communication entre l'ARM9 (côté 0) et l'ARM7 (côté 1) : IPCSYNC, et deux
// FIFO de 16 mots, une par sens (GBATEK, « DS Inter Process Communication »).
namespace ravenemu::nds {

class Ipc {
public:
    Ipc(InterruptController& arm9, InterruptController& arm7) noexcept : irq_{&arm9, &arm7} {}

    // 0x04000180 : bits 0-3 venus de l'autre CPU, 8-11 envoyés, 14 IRQ permise.
    [[nodiscard]] std::uint32_t read_sync(unsigned side) const noexcept {
        const auto& own = sides_[side];
        return sides_[other(side)].output | (std::uint32_t{own.output} << 8U) | (own.sync_irq ? 0x4000U : 0U);
    }
    void write_sync(unsigned side, std::uint32_t value, std::uint32_t mask) noexcept {
        auto& own = sides_[side];
        const auto bits = (mask >> 8U) & 15U;
        own.output = static_cast<std::uint8_t>((own.output & ~bits) | ((value >> 8U) & bits));
        if ((mask & 0x4000U) != 0) own.sync_irq = (value & 0x4000U) != 0;
        // Bit 13 : IRQ vers l'autre CPU, s'il l'a permise.
        if ((mask & value & 0x2000U) != 0 && sides_[other(side)].sync_irq) irq_[other(side)]->request(irq::ipc_sync);
    }

    // 0x04000184 : état des deux FIFO vues de ce côté, IRQ, erreur, activation.
    [[nodiscard]] std::uint32_t read_control(unsigned side) const noexcept {
        const auto& own = sides_[side];
        const auto& send = fifos_[side];
        const auto& receive = fifos_[other(side)];
        std::uint32_t value = 0;
        if (send.empty()) value |= 0x0001U;
        if (send.full()) value |= 0x0002U;
        if (own.send_irq) value |= 0x0004U;
        if (receive.empty()) value |= 0x0100U;
        if (receive.full()) value |= 0x0200U;
        if (own.receive_irq) value |= 0x0400U;
        if (own.error) value |= 0x4000U;
        if (own.enabled) value |= 0x8000U;
        return value;
    }
    void write_control(unsigned side, std::uint32_t value, std::uint32_t mask) noexcept {
        auto& own = sides_[side];
        value &= mask;
        if ((mask & 0x0004U) != 0) own.send_irq = (value & 0x0004U) != 0;
        if ((value & 0x0008U) != 0) {  // vide la FIFO d'envoi
            fifos_[side].clear();
            sides_[other(side)].last = 0;
        }
        if ((mask & 0x0400U) != 0) own.receive_irq = (value & 0x0400U) != 0;
        if ((value & 0x4000U) != 0) own.error = false;  // acquittement
        if ((mask & 0x8000U) != 0) own.enabled = (value & 0x8000U) != 0;
        update_irqs();
    }

    // 0x04000188. FIFO désactivée : écriture ignorée ; pleine : erreur.
    void send(unsigned side, std::uint32_t value) noexcept {
        auto& own = sides_[side];
        if (!own.enabled) return;
        if (fifos_[side].full()) {
            own.error = true;
            return;
        }
        fifos_[side].push(value);
        update_irqs();
    }
    // 0x04100000. Vide : le dernier mot reçu (ou 0) et l'erreur ; FIFO
    // désactivée : le plus ancien mot, laissé en place.
    std::uint32_t receive(unsigned side) noexcept {
        auto& own = sides_[side];
        auto& fifo = fifos_[other(side)];
        if (fifo.empty()) {
            own.error = true;
            return own.last;
        }
        if (!own.enabled) return fifo.front();
        own.last = fifo.pop();
        update_irqs();
        return own.last;
    }

private:
    class Fifo {
    public:
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] bool full() const noexcept { return size_ == words_.size(); }
        [[nodiscard]] std::uint32_t front() const noexcept { return words_[head_]; }
        void push(std::uint32_t value) noexcept {
            words_[(head_ + size_) % words_.size()] = value;
            ++size_;
        }
        std::uint32_t pop() noexcept {
            const auto value = words_[head_];
            head_ = (head_ + 1) % words_.size();
            --size_;
            return value;
        }
        void clear() noexcept { head_ = size_ = 0; }

    private:
        std::array<std::uint32_t, 16> words_{};
        std::size_t head_{};
        std::size_t size_{};
    };

    struct Side {
        std::uint8_t output{};
        bool sync_irq{};
        bool send_irq{};
        bool receive_irq{};
        bool error{};
        bool enabled{};
        std::uint32_t last{};
        // Conditions des IRQ de FIFO, déclenchées sur front montant.
        bool send_condition{};
        bool receive_condition{};
    };

    static constexpr unsigned other(unsigned side) noexcept { return side ^ 1U; }

    void update_irqs() noexcept {
        for (unsigned side = 0; side < 2; ++side) {
            auto& own = sides_[side];
            const auto send = own.send_irq && fifos_[side].empty();
            const auto receive = own.receive_irq && !fifos_[other(side)].empty();
            if (send && !own.send_condition) irq_[side]->request(irq::ipc_send_empty);
            if (receive && !own.receive_condition) irq_[side]->request(irq::ipc_receive);
            own.send_condition = send;
            own.receive_condition = receive;
        }
    }

    std::array<InterruptController*, 2> irq_;
    std::array<Side, 2> sides_{};
    std::array<Fifo, 2> fifos_{};  // fifos_[côté] : écrite par ce côté
};

} // namespace ravenemu::nds
