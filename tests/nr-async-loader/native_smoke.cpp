#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include "../../OptiScaler/dlssnr/sycl/ArcNrSyclLoader.h"

int main(int argc, char** argv) {
    if (argc != 6) return 2;
    try {
        ArcNrSyclLoader loader;
        std::string error;
        if (!loader.Load(argv[1], error)) throw std::runtime_error(error);
        const uint32_t width = std::stoul(argv[2]), height = std::stoul(argv[3]);
        ArcNrSyclCreate args {sizeof(args), width, height, "unused", "unused"};
        const auto start = std::chrono::steady_clock::now();
        double worst_ms = 0;
        unsigned polls = 0;
        for (;;) {
            const auto before = std::chrono::steady_clock::now();
            const auto status = loader.PrepareAsync(args, error);
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - before).count();
            worst_ms = std::max(worst_ms, elapsed);
            if (status == ArcNrSyclLoader::Preparation::Ready) break;
            if (status == ArcNrSyclLoader::Preparation::Failed) throw std::runtime_error(error);
            if (++polls % 60 == 1) std::cout << "pending: " << loader.PreparationMessage() << std::endl;
            if (std::chrono::steady_clock::now() - start > std::chrono::minutes(11))
                throw std::runtime_error("preparation timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        std::cout << "ready; polls=" << polls << " max_poll_ms=" << worst_ms << std::endl;
        if (polls < 2 || worst_ms > 100) throw std::runtime_error("preparation blocked polling");
        const size_t count = size_t(width) * height * 4;
        std::vector<uint16_t> input(count), output(count);
        std::ifstream file(argv[4], std::ios::binary);
        if (!file.read(reinterpret_cast<char*>(input.data()), count * 2)) throw std::runtime_error("input read");
        ArcNrSyclFrame frame{};
        frame.struct_size = sizeof(frame);
        frame.color_rgba_f16 = input.data(); frame.output_rgba_f16 = output.data();
        frame.reset_history = 1;
        frame.normalized_style = 0.5f; frame.local_tone = frame.local_structure = frame.skin_structure = 0.5f;
        frame.automatic_structure = 1.0f; frame.intensity = 1.0f;
        for (int i = 0; i < 4; ++i) {
            frame.frame_index = i;
            if (loader.Evaluate(frame, error) != ARC_NR_SYCL_OK) throw std::runtime_error(error);
        }
        std::ofstream out(argv[5], std::ios::binary);
        out.write(reinterpret_cast<char*>(output.data()), count * 2);
        std::cout << "PASS: background profile build and full-resolution inference " << width << 'x' << height << std::endl;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}
