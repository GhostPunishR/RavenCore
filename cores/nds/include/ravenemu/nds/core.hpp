#pragma once

#include <ravenemu/core.hpp>
#include <ravenemu/nds/backup.hpp>
#include <ravenemu/nds/input.hpp>

namespace ravenemu::nds {

struct CoreOptions {
    BackupType backup_type{BackupType::none}; // sélection explicite, pas une détection de ROM
    std::span<const std::uint8_t> arm9_bios, arm7_bios, firmware;
};

/** Adaptateur à deux écrans empilés : haut puis tactile, 256 x 384 ARGB8888.
 * Démarrage direct expérimental. Audio PCM16 stéréo ; savestates indisponibles.
 */
class FrontendCore : public ravenemu::Core {
public:
    virtual void set_key(Key key, bool pressed) = 0;
    [[nodiscard]] virtual bool touch_available() const noexcept = 0;
    // Pixels du seul écran inférieur. Un contact exige un firmware étalonné.
    virtual void set_touch_position(std::uint16_t x, std::uint16_t y, bool pressed) = 0;
    virtual void set_lid_closed(bool closed) noexcept = 0;
    [[nodiscard]] virtual bool powered_off() const noexcept = 0;
};

// Les images de configuration sont copiées avant le retour de la fabrique.
[[nodiscard]] std::unique_ptr<FrontendCore> make_core(const CoreOptions& options = {});

} // namespace ravenemu::nds
