#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

// Horloge et événements du DS. Le temps se compte en cycles de l'horloge
// système, 33,513982 MHz (GBATEK, « DS Memory Timings ») : celle de l'ARM7 et
// du bus. L'ARM9 tourne au double et compte ses propres cycles.
namespace ravenemu::nds {

using Cycles = std::uint64_t;

inline constexpr Cycles system_clock_hz = 33'513'982;

enum class Event : unsigned {
    lcd,      // début de ligne, puis H-Blank de l'ARM9 et de l'ARM7
    timers9,  // prochain débordement d'un timer de l'ARM9
    timers7,  // prochain débordement d'un timer de l'ARM7
    count,
};

// Une date par événement, au plus une en attente pour chacun ; une table
// courte suffit, parcourue en entier.
class Scheduler {
public:
    static constexpr Cycles never = std::numeric_limits<Cycles>::max();

    void schedule(Event event, Cycles at) noexcept { times_[index(event)] = at; }
    void cancel(Event event) noexcept { times_[index(event)] = never; }
    [[nodiscard]] Cycles time_of(Event event) const noexcept { return times_[index(event)]; }
    [[nodiscard]] Cycles next() const noexcept { return *std::min_element(times_.begin(), times_.end()); }

    // Retire et rend l'événement échu le plus ancien, s'il y en a un.
    [[nodiscard]] std::optional<Event> take_due(Cycles now) noexcept {
        const auto earliest = std::min_element(times_.begin(), times_.end());
        if (*earliest > now) return std::nullopt;
        *earliest = never;
        return static_cast<Event>(earliest - times_.begin());
    }

private:
    static constexpr std::size_t index(Event event) noexcept { return static_cast<std::size_t>(event); }
    std::array<Cycles, static_cast<std::size_t>(Event::count)> times_ = [] {
        std::array<Cycles, static_cast<std::size_t>(Event::count)> times{};
        times.fill(never);
        return times;
    }();
};

} // namespace ravenemu::nds
