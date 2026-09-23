#include "ArcNrSyclApi.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<uint16_t> Read(const char* path, std::size_t count) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() !=
                     static_cast<std::streamoff>(count * sizeof(uint16_t)))
        throw std::runtime_error("input is missing or has the wrong size");
    file.seekg(0);
    std::vector<uint16_t> data(count);
    file.read(reinterpret_cast<char*>(data.data()),
              count * sizeof(uint16_t));
    if (!file) throw std::runtime_error("input read failed");
    return data;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 7 && argc != 8) {
        std::cerr << "usage: arc_nr_sycl_provider_smoke LOGICAL_DIR "
                     "DERIVED_DIR WIDTH HEIGHT COLOR_RGBA.f16 "
                     "OUTPUT_RGBA.f16 [--temporal]\n";
        return 2;
    }
    try {
        const bool temporal = argc == 8 &&
                              std::string(argv[7]) == "--temporal";
        if (argc == 8 && !temporal)
            throw std::invalid_argument("unknown smoke-test option");
        ArcNrSyclApi api {};
        api.struct_size = sizeof(api);
        if (arc_nr_sycl_get_api(ARC_NR_SYCL_API_VERSION + 1, &api))
            throw std::runtime_error("SYCL provider accepted an unknown ABI");
        if (!arc_nr_sycl_get_api(ARC_NR_SYCL_API_VERSION, &api) ||
            api.version != ARC_NR_SYCL_API_VERSION ||
            !api.create || !api.evaluate || !api.destroy)
            throw std::runtime_error("SYCL provider ABI mismatch");
        const auto width = static_cast<uint32_t>(std::stoul(argv[3]));
        const auto height = static_cast<uint32_t>(std::stoul(argv[4]));
        const std::size_t values =
            static_cast<std::size_t>(width) * height * 4;
        auto color = Read(argv[5], values);
        std::vector<uint16_t> output(values);
        ArcNrSyclCreate create {};
        create.struct_size = sizeof(create);
        create.width = width;
        create.height = height;
        create.logical_weights_directory = argv[1];
        create.derived_weights_directory = argv[2];
        char error[512] {};
        void* session = api.create(&create, error, sizeof(error));
        if (!session) throw std::runtime_error(error);
        ArcNrSyclFrame frame {};
        frame.struct_size = sizeof(frame);
        frame.reset_history = 1;
        frame.color_rgba_f16 = color.data();
        frame.output_rgba_f16 = output.data();
        frame.local_tone = 1;
        frame.local_structure = 1;
        frame.skin_structure = -1;
        frame.automatic_structure = 1;
        frame.intensity = 1;
        auto status = api.evaluate(session, &frame, error,
                                   sizeof(error));
        if (status == ARC_NR_SYCL_OK && temporal) {
            auto previous = output;
            std::vector<uint16_t> motion(values / 2, 0);
            frame.frame_index = 1;
            frame.reset_history = 0;
            frame.previous_output_rgba_f16 = previous.data();
            frame.motion_rg_f16 = motion.data();
            status = api.evaluate(session, &frame, error, sizeof(error));
        }
        api.destroy(session);
        if (status != ARC_NR_SYCL_OK)
            throw std::runtime_error(error);
        std::ofstream file(argv[6], std::ios::binary);
        if (!file) throw std::runtime_error("cannot create output");
        file.write(reinterpret_cast<const char*>(output.data()),
                   values * sizeof(uint16_t));
        if (!file) throw std::runtime_error("cannot write output");
        std::cout << "SYCL provider evaluated " << width << 'x' << height
                  << " RGBA16F frame\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
