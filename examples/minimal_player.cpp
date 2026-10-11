#include <ravenemu/sdk.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: ravenemu_minimal_player gb|gbc|gba jeu.gb|gbc|gba\n";
        return 2;
    }

    try {
        const std::string system_name = argv[1];
        const auto system = system_name == "gb" ? ravenemu::sdk::System::game_boy :
            system_name == "gbc" ? ravenemu::sdk::System::game_boy_color :
            system_name == "gba" ? ravenemu::sdk::System::game_boy_advance :
            throw std::invalid_argument("Console attendue : gb, gbc ou gba");
        std::ifstream input(argv[2], std::ios::binary);
        if (!input) throw std::runtime_error("ROM inaccessible");
        const std::vector<std::uint8_t> rom{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        auto core = ravenemu::sdk::make_core(system);
        core->load_rom(rom, {});
        const auto video = core->video_spec();
        const auto audio = core->audio_spec();
        std::vector<std::int32_t> pixels(video.pixel_count());
        std::vector<std::int16_t> samples(4096);
        core->run_frame(pixels, true);
        const auto produced = core->read_audio(samples);
        std::cout << system_name << ": " << video.width << "x" << video.height
                  << ", " << video.refresh_rate_hz << " FPS, audio "
                  << audio.sample_rate_hz << " Hz, "
                  << produced / 2 << " stereo frames after first video frame\n";
    } catch (const std::exception& error) {
        std::cerr << "RavenEmu Cores: " << error.what() << '\n';
        return 1;
    }
}
