#pragma once

#include "apu/noise_channel.hpp"
#include "apu/sampling.hpp"
#include "apu/square_channel.hpp"
#include "apu/wave_channel.hpp"
#include "support/bits.hpp"
#include <ravenemu/gbc/hardware_mode.hpp>

namespace ravenemu::cgb {

class Apu {
public:
    static constexpr int sample_rate = 32768;

    explicit Apu(gbc::HardwareMode hardware_mode = gbc::HardwareMode::dmg)
        : hardware_mode_(hardware_mode) {}

    /**
     * Avance de `cycles` points (4,19 MHz), en différé.
     *
     * Le bus appelle tick(1) à chaque point, soit 70 224 fois par image : y
     * faire avancer les quatre canaux coûtait le quart du temps du cœur. Entre
     * deux accès, l'APU ne dépend que du temps écoulé, et avancer de N points
     * d'un coup donne exactement l'état de N pas d'un point : les tranches ne
     * se coupent qu'aux expirations des minuteurs et des fenêtres
     * d'échantillonnage, les accumulateurs s'additionnent. Le temps s'entasse
     * donc ici, et catch_up() le rattrape avant tout ce qui l'observe ou le
     * modifie : registres, PCM, échantillons, séquenceur, états, réinitialisations.
     */
    void tick(int cycles) noexcept {
        pending_cycles_ += cycles;
        // Sans accès ni front du séquenceur (un jeu qui remet DIV à zéro sans
        // cesse), le temps ne s'entasse pas indéfiniment.
        if (pending_cycles_ >= max_pending_cycles) catch_up();
    }

    /** Rattrape le temps en attente, tel que l'auraient fait les tick() un à un. */
    void catch_up() noexcept {
        int remaining = pending_cycles_;
        pending_cycles_ = 0;
        while (remaining > 0) {
            const int slice = std::min(sample_timer_, remaining);
            if (power_on_) {
                square1_.step(slice); square2_.step(slice); wave_.step(slice); noise_.step(slice);
            }
            sample_timer_ -= slice;
            remaining -= slice;
            if (sample_timer_ <= 0) { sample_timer_ += cycles_per_sample; emit_sample(); }
        }
    }

    /**
     * Front descendant du bit DIV sélectionné par la vitesse CPU. APU coupé,
     * le séquenceur ne compte pas : le rallumage le remet au pas 0.
     */
    void clock_divider_falling_edge() noexcept {
        catch_up();
        if (!power_on_) return;
        if (skip_next_divider_edge_) {
            skip_next_divider_edge_ = false;
            return;
        }
        clock_frame_sequencer();
    }

    /**
     * Niveau du bit de DIV qui cadence le séquenceur, vu par l'APU au moment
     * où une écriture l'allume. Allumé pendant que ce bit vaut 1, l'APU saute
     * le premier front qui suit (SameSuite, div_write_trigger_10).
     */
    void set_divider_signal(bool high) noexcept { divider_signal_high_ = high; }

    [[nodiscard]] std::size_t read_samples(std::span<std::int16_t> destination) noexcept {
        catch_up();
        std::size_t copied{};
        const auto maximum = destination.size() & ~std::size_t{1};
        while (copied < maximum && ring_count_ > 0) {
            destination[copied++] = ring_[ring_read_];
            ring_read_ = (ring_read_ + 1) & (ring_capacity - 1);
            --ring_count_;
        }
        return copied;
    }

    [[nodiscard]] int read_pcm12() noexcept {
        catch_up();
        return square1_.output() | (square2_.output() << 4);
    }

    [[nodiscard]] int read_pcm34() noexcept {
        catch_up();
        return wave_.output() | (noise_.output() << 4);
    }

    void reset_for_boot_rom() noexcept {
        catch_up();
        power_down();
        frame_step_ = 0;
        sample_timer_ = cycles_per_sample;
        ring_read_ = 0; ring_write_ = 0; ring_count_ = 0;
        capacitor_left_ = 0; capacitor_right_ = 0;
    }

    /** État observable à l'entrée $0100 sans redistribuer de boot ROM. */
    void initialize_hle_post_boot() noexcept {
        catch_up();
        square1_.reset(); square2_.reset(); wave_.reset(); noise_.reset();
        power_on_ = true;
        raw_registers_.fill(0);
        nr50_ = 0x77;
        nr51_ = 0xf3;
        raw_registers_[0x01] = 0x80; // NR11 : duty 2, longueur masquée en lecture
        raw_registers_[0x02] = 0xf3; // NR12
        raw_registers_[0x14] = nr50_;
        raw_registers_[0x15] = nr51_;
        square1_.duty = 2;
        square1_.sample_duty = 2;
        square1_.dac_enabled = true;
        square1_.enabled = true;
        square1_.envelope_initial = 15;
        square1_.envelope_period = 3;
        // **Zéro, et non quinze.** Quinze est le volume *initial* que NR12
        // déclare ; ce n'est pas celui du canal quand le jeu prend la main.
        //
        // La ROM d'amorçage déclenche le canal 1 pour son carillon pendant le
        // défilement du logo. L'enveloppe décroissante de période trois retire
        // un cran tous les trois tops du séquenceur à 64 Hz : quinze crans en
        // sept dixièmes de seconde, quand le défilement en dure plusieurs. À
        // l'entrée en $0100, le carillon est donc éteint depuis longtemps.
        //
        // Le canal reste *signalé* allumé dans NR52 — sa longueur n'a jamais
        // été armée, NR14 bit 6 étant à zéro — mais il ne produit plus rien.
        // Reproduire le volume initial faisait sonner un créneau à pleine
        // amplitude pendant les huit premiers dixièmes de seconde de chaque
        // partie, avant la musique du titre.
        square1_.volume = 0;
        square1_.envelope_timer = 3;
        square1_.length = 1;
        square1_.timer_running = true;
        square1_.force_first_step_zero = false;
        frame_step_ = 0;
        sample_timer_ = cycles_per_sample;
        ring_read_ = 0; ring_write_ = 0; ring_count_ = 0;
        prime_capacitors();
    }

    [[nodiscard]] int read(int address) noexcept {
        catch_up();
        if (address >= 0xff30 && address <= 0xff3f) {
            return read_wave_ram(address - 0xff30);
        }
        const int offset = address - 0xff10;
        if (offset < 0 || offset >= static_cast<int>(raw_registers_.size())) return 0xff;
        if (address == 0xff26) {
            int status = power_on_ ? 0x80 : 0;
            if (square1_.enabled) status |= 1;
            if (square2_.enabled) status |= 2;
            if (wave_.enabled) status |= 4;
            if (noise_.enabled) status |= 8;
            return status | 0x70;
        }
        return raw_registers_[static_cast<std::size_t>(offset)] | read_masks[static_cast<std::size_t>(offset)];
    }

    /**
     * Écriture d'un registre audio.
     *
     * Le mixeur peut s'y connecter ou s'en déconnecter — un DAC qu'on allume,
     * NR52 qu'on coupe. Quand cela survient au milieu d'une fenêtre déjà
     * entamée, la fenêtre est à cheval sur deux régimes et sa moyenne ne
     * représente ni l'un ni l'autre ; [emit_sample] la traite à part.
     */
    void write(int address, int value) {
        catch_up();
        const bool connected_before = power_on_ && any_dac_enabled();
        write_register(address, value);
        if (sample_timer_ < cycles_per_sample &&
            connected_before != (power_on_ && any_dac_enabled())) {
            mixer_changed_mid_window_ = true;
        }
    }

private:
    void write_register(int address, int value) {
        value = byte(value);
        if (address >= 0xff30 && address <= 0xff3f) {
            write_wave_ram(address - 0xff30, value);
            return;
        }
        const int offset = address - 0xff10;
        if (offset < 0 || offset >= static_cast<int>(raw_registers_.size())) return;
        if (!power_on_ && address != 0xff26) {
            write_length_while_powered_off(address, value);
            return;
        }
        raw_registers_[static_cast<std::size_t>(offset)] = value;
        switch (address) {
        case 0xff10: {
            const bool new_negate = (value & 8) != 0;
            if (square1_.sweep_negate && !new_negate && square1_.sweep_negate_used) {
                square1_.enabled = false;
            }
            square1_.sweep_period = (value >> 4) & 7; square1_.sweep_negate = new_negate;
            square1_.sweep_shift = value & 7; break;
        }
        case 0xff11: square1_.duty = (value >> 6) & 3; square1_.length = 64 - (value & 0x3f); break;
        case 0xff12: configure_envelope(square1_, value); break;
        case 0xff13: square1_.frequency = (square1_.frequency & 0x700) | value; break;
        case 0xff14:
            square1_.frequency = (square1_.frequency & 0xff) | ((value & 7) << 8);
            write_channel_control(square1_, value); break;
        case 0xff16: square2_.duty = (value >> 6) & 3; square2_.length = 64 - (value & 0x3f); break;
        case 0xff17: configure_envelope(square2_, value); break;
        case 0xff18: square2_.frequency = (square2_.frequency & 0x700) | value; break;
        case 0xff19:
            square2_.frequency = (square2_.frequency & 0xff) | ((value & 7) << 8);
            write_channel_control(square2_, value); break;
        case 0xff1a:
            wave_.dac_enabled = (value & 0x80) != 0;
            if (!wave_.dac_enabled) {
                // Le DAC coupé arrête le canal et vide son tampon : redémarré,
                // il sort 0 jusqu'à son premier échantillon, comme après la mise
                // sous tension (SameSuite, channel_3_restart_stop_delay).
                wave_.enabled = false;
                wave_.sample_buffer = 0;
            }
            break;
        case 0xff1b: wave_.length = 256 - value; break;
        case 0xff1c: wave_.volume_code = (value >> 5) & 3; break;
        case 0xff1d: wave_.frequency = (wave_.frequency & 0x700) | value; break;
        case 0xff1e:
            wave_.frequency = (wave_.frequency & 0xff) | ((value & 7) << 8);
            if ((value & 0x80) != 0 && !gbc::is_cgb_hardware(hardware_mode_)) {
                corrupt_dmg_wave_ram_on_retrigger();
            }
            write_channel_control(wave_, value); break;
        case 0xff20: noise_.length = 64 - (value & 0x3f); break;
        case 0xff21: configure_envelope(noise_, value); break;
        case 0xff22:
            noise_.clock_shift = (value >> 4) & 0x0f; noise_.width_mode7 = (value & 8) != 0;
            noise_.divisor_code = value & 7; break;
        case 0xff23:
            write_channel_control(noise_, value); break;
        case 0xff24: nr50_ = value; break;
        case 0xff25: nr51_ = value; break;
        case 0xff26: {
            const bool turn_on = (value & 0x80) != 0;
            if (power_on_ && !turn_on) power_down();
            else if (!power_on_ && turn_on) {
                // La mise sous tension remet le séquenceur à zéro : le prochain
                // front de DIV-APU est le pas 0, à sa date habituelle (DIV
                // n'est pas remis à zéro, d'où le « modulo 8192 » de Blargg).
                power_on_ = true;
                frame_step_ = 0;
                skip_next_divider_edge_ = divider_signal_high_;
            }
            break;
        }
        default: break;
        }
    }

public:
    /**
     * Disposition 1 : celle des états GB jusqu'au format 11. La 2 (format 12)
     * ajoute le rapport cyclique de l'échantillon en cours des deux canaux
     * carrés et le front du séquenceur à sauter.
     */
    void save(BinaryWriter& out, int layout = state_layout) {
        catch_up();
        out.i32(layout);
        out.boolean(power_on_); out.i32(nr50_); out.i32(nr51_);
        for (const int value : raw_registers_) out.i32(value);
        out.i32(frame_step_); out.i32(sample_timer_);
        out.f64(capacitor_left_); out.f64(capacitor_right_);
        square1_.save(out); square2_.save(out); wave_.save(out); noise_.save(out);
        if (layout < 2) return;
        out.i32(square1_.sample_duty); out.i32(square2_.sample_duty);
        out.boolean(skip_next_divider_edge_);
    }
    void load(BinaryReader& in) {
        pending_cycles_ = 0;
        const int layout = in.i32();
        if (layout != 1 && layout != state_layout) throw SaveStateError("État instantané corrompu (APU)");
        power_on_ = in.boolean(); nr50_ = in.i32(); nr51_ = in.i32();
        for (auto& value : raw_registers_) value = in.i32();
        frame_step_ = in.i32(); sample_timer_ = in.i32();
        const bool invalid_register = std::any_of(
            raw_registers_.begin(), raw_registers_.end(),
            [](int value) { return value < 0 || value > 0xff; });
        if (nr50_ < 0 || nr50_ > 0xff || nr51_ < 0 || nr51_ > 0xff ||
            frame_step_ < 0 || frame_step_ > 7 ||
            sample_timer_ <= 0 || sample_timer_ > cycles_per_sample ||
            invalid_register) {
            throw SaveStateError("État instantané corrompu (APU)");
        }
        capacitor_left_ = in.f64(); capacitor_right_ = in.f64();
        if (!std::isfinite(capacitor_left_) || !std::isfinite(capacitor_right_) ||
            std::abs(capacitor_left_) > 1'000'000 || std::abs(capacitor_right_) > 1'000'000) {
            throw SaveStateError("État instantané corrompu (filtre APU)");
        }
        square1_.load(in); square2_.load(in); wave_.load(in); noise_.load(in);
        square1_.sample_duty = square1_.duty; square2_.sample_duty = square2_.duty;
        skip_next_divider_edge_ = false;
        if (layout >= 2) {
            square1_.sample_duty = in.bounded_i32(0, 3); square2_.sample_duty = in.bounded_i32(0, 3);
            skip_next_divider_edge_ = in.boolean();
            if (skip_next_divider_edge_ && !power_on_) throw SaveStateError("État instantané corrompu (APU)");
        }
        ring_read_ = 0; ring_write_ = 0; ring_count_ = 0;
        mixer_changed_mid_window_ = false;
    }

private:
    [[nodiscard]] int read_wave_ram(int requested_index) const noexcept {
        if (!wave_.enabled) {
            return wave_.ram[static_cast<std::size_t>(requested_index & 0x0f)];
        }
        if (!gbc::is_cgb_hardware(hardware_mode_) && wave_.ram_access_cycles <= 0) return 0xff;
        return wave_.ram[static_cast<std::size_t>((wave_.position >> 1) & 0x0f)];
    }

    void write_wave_ram(int requested_index, int value) noexcept {
        int index = requested_index & 0x0f;
        if (wave_.enabled) {
            if (!gbc::is_cgb_hardware(hardware_mode_) && wave_.ram_access_cycles <= 0) return;
            index = (wave_.position >> 1) & 0x0f;
        }
        wave_.ram[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value);
    }

    template <typename Channel>
    void write_channel_control(Channel& channel, int value) noexcept {
        const bool trigger = (value & 0x80) != 0;
        const bool was_length_enabled = channel.length_enabled;
        channel.length_enabled = (value & 0x40) != 0;
        // Le front sauté après une mise sous tension ne cadence pas la
        // longueur : le prochain pas est alors de ceux qui ne la cadencent pas.
        const bool extra_length_clock = (frame_step_ & 1) != 0 || skip_next_divider_edge_;

        // Activer le compteur pendant une phase où la prochaine étape ne le
        // cadence pas produit immédiatement un clock supplémentaire. Si la
        // même écriture déclenche le canal, l'extinction éventuelle est
        // remplacée par le reload du trigger.
        if (!was_length_enabled && channel.length_enabled && extra_length_clock && channel.length > 0) {
            --channel.length;
            if (channel.length == 0 && !trigger) channel.enabled = false;
        }
        if (trigger) {
            channel.trigger(channel.length_enabled && extra_length_clock, frame_step_ == 7);
        }
    }

    // Redéclencher le canal au moment où il va lire un octet (lecture due au
    // prochain top, 2 cycles plus tard) recopie cet octet, ou son bloc aligné
    // de quatre, en tête de wave RAM (Blargg, dmg_sound 10).
    void corrupt_dmg_wave_ram_on_retrigger() noexcept {
        if (!wave_.enabled || wave_.timer > 2) return;
        const int current_byte = ((wave_.position + 1) >> 1) & 0x0f;
        if (current_byte < 4) {
            wave_.ram[0] = wave_.ram[static_cast<std::size_t>(current_byte)];
            return;
        }
        const int source = current_byte & ~3;
        for (int index = 0; index < 4; ++index) {
            wave_.ram[static_cast<std::size_t>(index)] =
                wave_.ram[static_cast<std::size_t>(source + index)];
        }
    }

    void write_length_while_powered_off(int address, int value) noexcept {
        // Sur les modèles monochromes, les quatre compteurs de longueur sont
        // encore câblés lorsque NR52 coupe le reste de l'APU. Le CGB bloque
        // aussi ces écritures.
        if (gbc::is_cgb_hardware(hardware_mode_)) return;
        switch (address) {
        case 0xff11: square1_.length = 64 - (value & 0x3f); break;
        case 0xff16: square2_.length = 64 - (value & 0x3f); break;
        case 0xff1b: wave_.length = 256 - value; break;
        case 0xff20: noise_.length = 64 - (value & 0x3f); break;
        default: break;
        }
    }

    template <typename Channel>
    static void configure_envelope(Channel& channel, int value) noexcept {
        // Le seul comportement "zombie" commun aux révisions documentées :
        // réécrire $08 pendant une enveloppe croissante de période 0 avance
        // manuellement le volume d'un cran.
        if (channel.enabled && channel.envelope_add && channel.envelope_period == 0 &&
            (value & 0x0f) == 0x08) {
            channel.volume = (channel.volume + 1) & 0x0f;
        }
        channel.envelope_initial = (value >> 4) & 0x0f;
        channel.envelope_add = (value & 8) != 0;
        channel.envelope_period = value & 7;
        channel.dac_enabled = (value & 0xf8) != 0;
        if (!channel.dac_enabled) channel.enabled = false;
    }
    void clock_frame_sequencer() noexcept {
        if (frame_step_ == 0 || frame_step_ == 4) clock_lengths();
        else if (frame_step_ == 2 || frame_step_ == 6) { clock_lengths(); square1_.clock_sweep(); }
        else if (frame_step_ == 7) { square1_.clock_envelope(); square2_.clock_envelope(); noise_.clock_envelope(); }
        frame_step_ = (frame_step_ + 1) & 7;
    }
    void clock_lengths() noexcept {
        square1_.clock_length(); square2_.clock_length(); wave_.clock_length(); noise_.clock_length();
    }
    static std::int16_t clamp_short(double value) noexcept {
        return static_cast<std::int16_t>(std::clamp(static_cast<int>(value), -32768, 32767));
    }
    /**
     * Aiguillage NR51 et volumes maîtres NR50, pour quatre sorties de canaux.
     *
     * Partagé entre le prélèvement d'un échantillon et l'amorçage du
     * condensateur : les deux doivent voir exactement le même niveau, sans quoi
     * l'amorçage laisserait justement la marche qu'il vise à supprimer.
     */
    [[nodiscard]] std::pair<double, double> route(
        double c1, double c2, double c3, double c4) const noexcept {
        double left{}; double right{};
        if ((nr51_ & 0x10) != 0) left += c1;
        if ((nr51_ & 0x20) != 0) left += c2;
        if ((nr51_ & 0x40) != 0) left += c3;
        if ((nr51_ & 0x80) != 0) left += c4;
        if ((nr51_ & 0x01) != 0) right += c1;
        if ((nr51_ & 0x02) != 0) right += c2;
        if ((nr51_ & 0x04) != 0) right += c3;
        if ((nr51_ & 0x08) != 0) right += c4;
        left *= static_cast<double>(((nr50_ >> 4) & 7) + 1) * mix_gain;
        right *= static_cast<double>((nr50_ & 7) + 1) * mix_gain;
        return {left, right};
    }

    /**
     * Charge le condensateur au niveau continu courant.
     *
     * Le filtre ne laisse passer que l'écart entre l'entrée et la charge : un
     * condensateur à zéro devant un convertisseur qui repose déjà sur un
     * niveau non nul produit une marche de toute la hauteur de ce niveau, puis
     * sa décroissance — un claquement de quelques millisecondes.
     *
     * Sur le matériel, la question ne se pose pas : quand le jeu prend la main,
     * la ROM d'amorçage a alimenté le convertisseur depuis plusieurs secondes
     * et le condensateur est chargé depuis longtemps. C'est cet état-là que
     * l'entrée en $0100 doit installer, et non celui d'une machine dont on
     * vient de brancher le haut-parleur.
     */
    void prime_capacitors() noexcept {
        const auto [left, right] = route(
            square1_.dac_output(), square2_.dac_output(),
            wave_.dac_output(), noise_.dac_output());
        capacitor_left_ = left;
        capacitor_right_ = right;
    }

    void emit_sample() noexcept {
        const double c1 = square1_.drain_average(); const double c2 = square2_.drain_average();
        const double c3 = wave_.drain_average(); const double c4 = noise_.drain_average();
        if (!power_on_ || !any_dac_enabled()) {
            // Le condensateur est physiquement déconnecté lorsque les quatre
            // DAC sont coupés : sortie nulle et charge conservée.
            mixer_changed_mid_window_ = false;
            push_sample(0, 0);
            return;
        }
        if (mixer_changed_mid_window_) {
            // Fenêtre à cheval sur une reconnexion du mixeur. Une partie s'est
            // écoulée mixeur coupé — sortie nulle par la règle ci-dessus — et
            // l'autre mixeur rétabli, sur un niveau continu que la charge
            // gardée pendant la coupure rejoint exactement. Les deux moitiés
            // valent donc zéro séparément.
            //
            // Leur *moyenne*, elle, ne vaut zéro que par accident : elle mêle
            // les deux dans une proportion qui ne tient qu'à l'instant où le
            // jeu a écrit son registre. Filtrée contre la charge, elle sortait
            // une impulsion d'un seul échantillon, jusqu'à un sixième de la
            // dynamique — le tic entendu à l'ouverture d'une partie, quand le
            // jeu éteint puis rallume l'APU avant de jouer sa première note.
            //
            // On rend donc le zéro que valent les deux moitiés, et on reprend
            // la charge sur le niveau retrouvé : le régime établi commence à la
            // fenêtre suivante, sans marche.
            mixer_changed_mid_window_ = false;
            prime_capacitors();
            push_sample(0, 0);
            return;
        }
        const auto [left, right] = route(c1, c2, c3, c4);
        const double filtered_left = left - capacitor_left_;
        capacitor_left_ = left - filtered_left * charge_factor;
        const double filtered_right = right - capacitor_right_;
        capacitor_right_ = right - filtered_right * charge_factor;
        push_sample(clamp_short(filtered_left), clamp_short(filtered_right));
    }
    [[nodiscard]] bool any_dac_enabled() const noexcept {
        return square1_.dac_enabled || square2_.dac_enabled || wave_.dac_enabled || noise_.dac_enabled;
    }
    void push_sample(std::int16_t left, std::int16_t right) noexcept {
        if (ring_count_ > ring_capacity - 2) { ring_read_ = (ring_read_ + 2) & (ring_capacity - 1); ring_count_ -= 2; }
        ring_[ring_write_] = left; ring_[(ring_write_ + 1) & (ring_capacity - 1)] = right;
        ring_write_ = (ring_write_ + 2) & (ring_capacity - 1); ring_count_ += 2;
    }
    void power_down() noexcept {
        const int square1_length = square1_.length;
        const int square2_length = square2_.length;
        const int wave_length = wave_.length;
        const int noise_length = noise_.length;
        power_on_ = false;
        skip_next_divider_edge_ = false;
        square1_.reset(); square2_.reset(); wave_.reset(); noise_.reset();
        if (!gbc::is_cgb_hardware(hardware_mode_)) {
            square1_.length = square1_length;
            square2_.length = square2_length;
            wave_.length = wave_length;
            noise_.length = noise_length;
        }
        nr50_ = 0; nr51_ = 0; raw_registers_.fill(0);
    }

    static constexpr int state_layout = 2;
    bool skip_next_divider_edge_{};
    // Non sauvegardé : le bus le donne juste avant chaque écriture de NR52.
    bool divider_signal_high_{};
    static constexpr int cycles_per_sample = apu_cycles_per_sample;
    static constexpr int max_pending_cycles = 1 << 16;
    static constexpr std::size_t ring_capacity = 8192;
    static constexpr int mix_gain = 64;
    /**
     * Le condensateur de sortie, par échantillon : 0,999958 par point
     * (4,19 MHz), soit 0,999958^128. Sa constante de temps, 5,7 ms (coupure
     * vers 28 Hz), lui fait retirer *lentement* la composante continue, comme
     * le décrit Pan Docs (Audio details).
     *
     * **La même pour la CGB.** Pan Docs donne 0,998943 pour la MGB et la CGB,
     * repris de l'ancien wiki gbdev, sans plus. Appliqué tel quel (0,8734 par
     * échantillon), il donne une constante de 0,23 ms, une coupure vers
     * 700 Hz : il ne retire plus le continu, mais les graves et la forme des
     * notes. Chaque front de créneau sortait en impulsion retombée en moins
     * d'une milliseconde ; une basse devenait un train de claquements. Ce sont
     * les parasites entendus en GBC : sur des jeux GBC, la part de l'énergie
     * sous 300 Hz tombait de 59 % à 15 % (Aevilia) et le facteur de crête
     * doublait. Le texte même de Pan Docs décrit un condensateur qui retire
     * lentement le continu : la CGB prend donc le facteur du DMG.
     */
    static constexpr double charge_factor = 0.9946383125332977;
    inline static constexpr std::array<int, 48> read_masks{{
        0x80, 0x3f, 0x00, 0xff, 0xbf, 0xff, 0x3f, 0x00, 0xff, 0xbf,
        0x7f, 0xff, 0x9f, 0xff, 0xbf, 0xff, 0xff, 0x00, 0x00, 0xbf,
        0x00, 0x00, 0x70, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    }};

    SquareChannel square1_{true};
    SquareChannel square2_{false};
    WaveChannel wave_{};
    NoiseChannel noise_{};
    gbc::HardwareMode hardware_mode_{gbc::HardwareMode::dmg};
    bool power_on_{true};
    int nr50_{0x77};
    int nr51_{0xf3};
    std::array<int, 48> raw_registers_ = [] { std::array<int, 48> values{}; values[0x14] = 0x77; values[0x15] = 0xf3; return values; }();
    int frame_step_{};
    int sample_timer_{cycles_per_sample};
    std::array<std::int16_t, ring_capacity> ring_{};
    std::size_t ring_read_{};
    std::size_t ring_write_{};
    std::size_t ring_count_{};
    double capacitor_left_{};
    double capacitor_right_{};

    /**
     * Le mixeur a changé d'état de connexion dans la fenêtre en cours.
     *
     * Transitoire : consommé par le prélèvement suivant. Rien n'en est
     * sérialisé, un état instantané reprenant toujours à une frontière de
     * fenêtre.
     */
    bool mixer_changed_mid_window_{};
    /** Points pas encore rattrapés : voir tick(). Jamais sérialisé, save() rattrape d'abord. */
    int pending_cycles_{};
};

} // namespace ravenemu::cgb
