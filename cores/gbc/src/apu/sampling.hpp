#pragma once

namespace ravenemu::cgb {

/**
 * Fenêtre d'échantillonnage de l'APU : 128 points (4,19 MHz) par échantillon,
 * soit 32 768 Hz. Chaque canal y moyenne la sortie de son DAC.
 */
inline constexpr int apu_cycles_per_sample = 128;

} // namespace ravenemu::cgb
