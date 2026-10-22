#pragma once

#include <array>
#include <cstdint>

namespace ravenemu {

/** Extrémité matérielle série exposée à un transport indépendant de l'hôte. */
class LinkPort {
public:
    virtual ~LinkPort() = default;
    [[nodiscard]] virtual bool outgoing_bit_high() const noexcept = 0;
    virtual void clock_external_bit(bool incoming_high) noexcept = 0;
    /** Bits encore à échanger dans le transfert en cours : 8 au premier. */
    [[nodiscard]] virtual int transfer_bits_remaining() const noexcept { return 0; }
    /** Octet du registre de données, entier tant que le premier bit n'est pas parti. */
    [[nodiscard]] virtual std::uint8_t transfer_data() const noexcept { return 0xff; }
};

/**
 * Transport d'un link cable ou d'un périphérique série (Printer, réseau…).
 * La durée reste entièrement décidée par le SerialPort émulé.
 */
class LinkEndpoint {
public:
    virtual ~LinkEndpoint() = default;
    [[nodiscard]] virtual bool attach(LinkPort& port) noexcept = 0;
    virtual void detach(LinkPort& port) noexcept = 0;
    [[nodiscard]] virtual bool exchange_bit(LinkPort& source, bool outgoing_high) noexcept = 0;
    /** Laisse un transport extérieur livrer ce qu'il a reçu ; rien pour un câble local. */
    virtual void poll() noexcept {}
};

/** Câble local déterministe reliant au plus deux machines dans un processus. */
class LocalLinkEndpoint final : public LinkEndpoint {
public:
    [[nodiscard]] bool attach(LinkPort& port) noexcept override {
        for (auto* existing : ports_) if (existing == &port) return true;
        for (auto& slot : ports_) {
            if (slot == nullptr) { slot = &port; return true; }
        }
        return false;
    }

    void detach(LinkPort& port) noexcept override {
        for (auto& slot : ports_) if (slot == &port) slot = nullptr;
    }

    [[nodiscard]] bool exchange_bit(LinkPort& source, bool outgoing_high) noexcept override {
        for (auto* port : ports_) {
            if (port == nullptr || port == &source) continue;
            const bool incoming = port->outgoing_bit_high();
            port->clock_external_bit(outgoing_high);
            return incoming;
        }
        return true; // ligne SIN au repos
    }

private:
    std::array<LinkPort*, 2> ports_{};
};

} // namespace ravenemu
