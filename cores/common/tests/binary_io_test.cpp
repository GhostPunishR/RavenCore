#include "ravenemu/binary_io.hpp"

#include "check.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using ravenemu::testing::check;
using ravenemu::testing::expect_failure;

namespace {

/**
 * Le format des états instantanés est big-endian et lu octet par octet.
 * Ces valeurs fixent l'ordre de lecture : un lecteur qui évaluerait ses
 * octets dans un autre ordre rendrait des valeurs inversées, et tous les
 * états existants deviendraient illisibles.
 */
void big_endian_reads_test() {
    const std::array<std::uint8_t, 14> bytes{
        0x12, 0x34,                                     // u16
        0x89, 0xab, 0xcd, 0xef,                         // u32
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, // u64
    };
    ravenemu::detail::BinaryReader in(bytes);
    check(in.u16() == 0x1234U, "u16 lu dans le mauvais ordre");
    check(in.u32() == 0x89ab'cdefU, "u32 lu dans le mauvais ordre");
    check(in.u64() == 0x0123'4567'89ab'cdefULL, "u64 lu dans le mauvais ordre");
    check(in.exhausted(), "octets non consommés après lecture");
}

void writer_round_trip_test() {
    ravenemu::detail::BinaryWriter out;
    out.u16(0xbeef); out.u32(0xdead'beefU); out.u64(0x0011'2233'4455'6677ULL);
    out.i32(-2); out.i64(std::numeric_limits<std::int64_t>::min()); out.f64(-0.5);
    const auto bytes = std::move(out).take();
    check(bytes[0] == 0xbe && bytes[1] == 0xef, "u16 écrit en petit-boutiste");

    ravenemu::detail::BinaryReader in(bytes);
    check(in.u16() == 0xbeefU, "aller-retour u16");
    check(in.u32() == 0xdead'beefU, "aller-retour u32");
    check(in.u64() == 0x0011'2233'4455'6677ULL, "aller-retour u64");
    check(in.i32() == -2, "aller-retour i32");
    check(in.i64() == std::numeric_limits<std::int64_t>::min(), "aller-retour i64");
    check(in.f64() == -0.5, "aller-retour f64");
    check(in.exhausted(), "octets non consommés après aller-retour");
}

void bounds_test() {
    const std::array<std::uint8_t, 3> short_input{1, 2, 3};
    ravenemu::detail::BinaryReader truncated(short_input);
    expect_failure<ravenemu::SaveStateError>([&] { static_cast<void>(truncated.u32()); },
                                             "lecture tronquée acceptée");

    const std::array<std::uint8_t, 4> value{0x00, 0x00, 0x01, 0x00};
    ravenemu::detail::BinaryReader bounded(value);
    expect_failure<ravenemu::SaveStateError>([&] { static_cast<void>(bounded.bounded_i32(0, 0xff)); },
                                             "valeur hors limites acceptée par bounded_i32");
}

} // namespace

int main() {
    big_endian_reads_test();
    writer_round_trip_test();
    bounds_test();
    return 0;
}
