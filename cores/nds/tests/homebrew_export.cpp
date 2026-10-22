#include <ravenemu/nds/core.hpp>
#include "support/interactive_fixture.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: nds_homebrew_demo fichier.nds\n"; return 1; }
    const auto rom = ravenemu::nds::test_support::interactive_cartridge();
    std::ofstream output(argv[1],std::ios::binary);
    output.write(reinterpret_cast<const char*>(rom.data()),static_cast<std::streamsize>(rom.size()));
    return output ? 0 : 1;
}
