#pragma once

#include "support/bits.hpp"

namespace ravenemu::gba {

/**
 * Multiplieur de l'ARM7TDMI, rangée par rangée, pour le drapeau C qu'il
 * laisse. ARM le dit « sans signification » après une multiplication ; il est
 * pourtant fixé par les entrées : c'est un bit de la rangée des retenues que
 * l'ALU additionne en dernier. Le matériel décrit ici vient de l'analyse de
 * zayd, « Solving the Mystery of ARM7TDMI Multiply Carry Flag » (2024),
 * appuyée sur les brevets ARM de G. Larri et de D. Seal, G. Larri et
 * D. Jaggar (1994), et vérifiée sur console.
 *
 * - Rs est lu par chiffres de Booth en base 4 (de −2 à 2), quatre par cycle,
 *   soit 8 bits. Le multiplieur s'arrête dès que les bits restants sont tous
 *   à 0, ou tous à 1 si les opérandes sont signés (MUL, MLA, SMULL, SMLAL).
 * - Chaque chiffre donne un multiple de Rm ; une négation s'écrit ~x, le 1
 *   manquant entrant dans le bit 0 libre de la rangée des retenues. Quatre
 *   additionneurs à retenue conservée par cycle réduisent somme, retenues et
 *   multiple à deux rangées de 33 bits. Chacun rend ses deux bits bas, qui ne
 *   changeront plus, et passe la suite décalée de deux bits.
 * - Avant le premier cycle, la rangée des sommes reçoit l'accumulateur, et
 *   celle des retenues le chiffre qui précède le premier : ~Rm si Rs est
 *   impair, le 1 de la négation entrant par l'ALU. Le bit 0 de chaque rangée
 *   sort aussitôt. Les 30 bits hauts d'un accumulateur de 64 bits entrent
 *   ensuite deux par deux, en haut des rangées.
 * - Les extensions de signe sont remplacées par des constantes : en haut des
 *   rangées, la somme reçoit le bit d'accumulateur suivant plus les bits de
 *   signe inversés des retenues et du multiple, et les retenues le bit
 *   d'accumulateur d'après, inversé.
 *
 * L'ALU additionne ensuite les deux rangées par mots de 32 bits ; le décaleur
 * qui les réaligne rend en retenue le bit 31 de la rangée des retenues, ou son
 * bit 63 pour une multiplication longue menée sur les quatre cycles : seul ce
 * cas fait passer le mot haut par le décaleur.
 */
class BoothMultiplier {
public:
    // `accumulate` : Rn pour MLA, RdHi:RdLo pour UMLAL et SMLAL, 0 sinon.
    BoothMultiplier(std::uint32_t multiplicand, std::uint32_t multiplier, std::uint64_t accumulate,
                    bool signed_operands, bool long_result) noexcept
        : long_result_(long_result), carry_in_((multiplier & 1U) != 0) {
        const auto widen = [signed_operands](std::uint32_t value) -> std::uint64_t {
            return signed_operands && (value >> 31U) != 0 ? value | (operand_mask & ~std::uint64_t{0xffff'ffffU})
                                                          : value;
        };
        const auto rm = widen(multiplicand);
        auto rs = widen(multiplier) & row_mask;
        auto sum = accumulate;
        auto carry = carry_in_ ? ~rm : 0;
        low_sum_ = sum & 1U;
        low_carry_ = carry & 1U;
        sum >>= 1U;
        carry >>= 1U;
        auto pending = accumulate >> 34U;  // bits d'accumulateur pas encore entrés
        auto weight = 1U;                  // poids du bit 0 des rangées
        do {
            for (auto digit = 0U; digit < 4U; ++digit) {
                sum &= row_mask;
                carry &= row_mask;
                const auto [multiple, negated] = booth_multiple(rm, (rs >> (2U * digit)) & 7U);
                const auto addend = multiple & row_mask;
                const auto next_sum = sum ^ addend ^ carry;
                const auto next_carry = ((sum & addend) | (sum & carry) | (addend & carry)) << 1U | (negated ? 1U : 0U);
                low_sum_ |= (next_sum & 3U) << weight;
                low_carry_ |= (next_carry & 3U) << weight;
                weight += 2U;
                const auto top = (pending & 1U) + ((~carry >> 32U) & 1U) + ((~multiple >> 33U) & 1U);
                sum = next_sum >> 2U | top << 31U;
                carry = next_carry >> 2U | ((~pending >> 1U) & 1U) << 32U;
                pending >>= 2U;
            }
            ++cycles_;
            // Décalage arithmétique de Rs sur 33 bits.
            rs = rs >> 8U | ((rs & sign_bit) != 0 ? 0x1'fe00'0000ULL : 0U);
        } while (rs != 0 && !(signed_operands && rs == row_mask));
        sum_ = sum;
        carry_ = carry;
        pending_ = pending;
    }

    [[nodiscard]] bool carry_flag() const noexcept {
        if (long_result_ && cycles_ == 4) return ((carry_ >> 30U) & 1U) != 0;
        return ((carry_row() >> 31U) & 1U) != 0;
    }
    // Cycles de la boucle de Booth : le m des tables de GBATEK.
    [[nodiscard]] int cycles() const noexcept { return cycles_; }
    // Ce que l'ALU tire des deux rangées : le produit, plus l'accumulateur.
    // Au-dessus des rangées, la somme se poursuit par les bits d'accumulateur
    // restants, et les retenues sont un nombre signé de 33 bits.
    [[nodiscard]] std::uint64_t result() const noexcept {
        const auto carry = (carry_ & sign_bit) != 0 ? carry_ | ~row_mask : carry_;
        const auto high = sum_ + (pending_ << 33U) + carry;
        return low_sum_ + low_carry_ + (carry_in_ ? 1U : 0U) + (high << shift());
    }

private:
    static constexpr std::uint64_t row_mask = 0x1'ffff'ffffULL;      // 33 bits
    static constexpr std::uint64_t operand_mask = 0x3'ffff'ffffULL;  // 34 bits
    static constexpr std::uint64_t sign_bit = 0x1'0000'0000ULL;

    struct Multiple {
        std::uint64_t value;
        bool negated;
    };
    // Multiple de Rm pour trois bits de Rs : le bit bas est le haut du chiffre précédent.
    static Multiple booth_multiple(std::uint64_t rm, std::uint64_t bits) noexcept {
        switch (bits) {
        case 1:
        case 2: return {rm, false};
        case 3: return {(rm << 1U) & operand_mask, false};
        case 4: return {~(rm << 1U) & operand_mask, true};
        case 5:
        case 6: return {~rm & operand_mask, true};
        default: return {0, false};
        }
    }
    [[nodiscard]] unsigned shift() const noexcept { return 8U * static_cast<unsigned>(cycles_) + 1U; }
    // Rangée des retenues en entier, tronquée à 64 bits.
    [[nodiscard]] std::uint64_t carry_row() const noexcept { return low_carry_ | carry_ << shift(); }

    bool long_result_;
    bool carry_in_;
    int cycles_{};
    std::uint64_t low_sum_{};    // bits sortis des rangées, à leur poids
    std::uint64_t low_carry_{};
    std::uint64_t sum_{};        // rangées restantes, de poids 2^shift()
    std::uint64_t carry_{};
    std::uint64_t pending_{};
};

} // namespace ravenemu::gba
