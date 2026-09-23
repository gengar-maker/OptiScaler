#include "ArcNrSyclApi.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <sycl/sycl.hpp>

#include "arc_nr/device_buffer.hpp"
#include "arc_nr/device_context.hpp"
#include "arc_nr/mlx_frame_pipeline.hpp"
#include "arc_nr/mlx_model.hpp"

#if defined(_WIN32)
#define ARC_NR_EXPORT __declspec(dllexport)
#else
#define ARC_NR_EXPORT __attribute__((visibility("default")))
#endif

namespace {

static_assert(sizeof(sycl::half) == sizeof(uint16_t));

void Error(char* destination, uint32_t capacity, const char* message) {
    if (destination == nullptr || capacity == 0) return;
    const std::size_t length = std::min<std::size_t>(
        std::strlen(message), static_cast<std::size_t>(capacity - 1));
    std::memcpy(destination, message, length);
    destination[length] = '\0';
}

sycl::queue IntelQueue() {
    for (const sycl::platform& platform : sycl::platform::get_platforms()) {
        if (platform.get_backend() != sycl::backend::ext_oneapi_level_zero)
            continue;
        for (const sycl::device& device : platform.get_devices()) {
            if (device.is_gpu() &&
                device.get_info<sycl::info::device::vendor_id>() == 0x8086u) {
                return sycl::queue(
                    device, sycl::property_list{
                                sycl::property::queue::enable_profiling{}});
            }
        }
    }
    throw std::runtime_error("no Intel Level Zero GPU is available");
}

struct Session {
    std::mutex mutex;
    arc_nr::DeviceContext context;
    arc_nr::MlxFramePipeline frame;
    arc_nr::MlxModelPlan model;
    const std::size_t pixels;
    const std::size_t network_pixels;
    arc_nr::DeviceBuffer<sycl::half> color;
    arc_nr::DeviceBuffer<sycl::half> history;
    arc_nr::DeviceBuffer<sycl::half> motion;
    arc_nr::DeviceBuffer<sycl::half> features;
    arc_nr::DeviceBuffer<sycl::half> head;
    arc_nr::DeviceBuffer<sycl::half> output;

    Session(uint32_t width, uint32_t height,
            const std::filesystem::path& logical,
            const std::filesystem::path& derived)
        : context(IntelQueue()),
          frame(context.queue(), width, height),
          model(context, frame.network_height(), frame.network_width(),
                logical, derived),
          pixels(static_cast<std::size_t>(width) * height),
          network_pixels(static_cast<std::size_t>(frame.network_width()) *
                         frame.network_height()),
          color(context.queue(), pixels * 4),
          history(context.queue(), pixels * 4),
          motion(context.queue(), pixels * 2),
          features(context.queue(), network_pixels * 16),
          head(context.queue(), network_pixels * 4),
          output(context.queue(), pixels * 4) {}
};

void* Create(const ArcNrSyclCreate* create, char* error,
             uint32_t error_bytes) {
    try {
        if (create == nullptr || create->struct_size != sizeof(*create) ||
            create->width == 0 || create->height == 0 ||
            create->width > 8192 || create->height > 8192 ||
            create->logical_weights_directory == nullptr ||
            create->derived_weights_directory == nullptr) {
            Error(error, error_bytes, "invalid SYCL session configuration");
            return nullptr;
        }
        const auto logical =
            std::filesystem::u8path(create->logical_weights_directory);
        const auto derived =
            std::filesystem::u8path(create->derived_weights_directory);
        if (!std::filesystem::is_directory(logical) ||
            !std::filesystem::is_directory(derived)) {
            Error(error, error_bytes, "SYCL model weight directory is missing");
            return nullptr;
        }
        auto session = std::make_unique<Session>(create->width, create->height,
                                                 logical, derived);
        Error(error, error_bytes, "");
        return session.release();
    } catch (const std::exception& e) {
        Error(error, error_bytes, e.what());
        return nullptr;
    } catch (...) {
        Error(error, error_bytes, "unknown SYCL session creation failure");
        return nullptr;
    }
}

ArcNrSyclStatus Evaluate(void* opaque, const ArcNrSyclFrame* input,
                          char* error, uint32_t error_bytes) {
    if (opaque == nullptr || input == nullptr ||
        input->struct_size != sizeof(*input) ||
        input->color_rgba_f16 == nullptr ||
        input->output_rgba_f16 == nullptr ||
        (input->previous_output_rgba_f16 == nullptr) !=
            (input->motion_rg_f16 == nullptr)) {
        Error(error, error_bytes, "invalid SYCL frame buffers");
        return ARC_NR_SYCL_INVALID_ARGUMENT;
    }
    auto& session = *static_cast<Session*>(opaque);
    std::lock_guard<std::mutex> lock(session.mutex);
    try {
        auto& queue = session.context.queue();
        queue.memcpy(session.color.data(), input->color_rgba_f16,
                     session.color.size_bytes()).wait_and_throw();
        const bool temporal = input->reset_history == 0 &&
                              input->previous_output_rgba_f16 != nullptr;
        if (temporal) {
            queue.memcpy(session.history.data(),
                         input->previous_output_rgba_f16,
                         session.history.size_bytes()).wait_and_throw();
            queue.memcpy(session.motion.data(), input->motion_rg_f16,
                         session.motion.size_bytes()).wait_and_throw();
        }
        arc_nr::MlxFrameControls controls;
        controls.frame_index = input->frame_index;
        controls.normalized_style = input->normalized_style;
        controls.local_tone = input->local_tone;
        controls.local_structure = input->local_structure;
        controls.skin_structure = input->skin_structure;
        controls.automatic_structure = input->automatic_structure;
        controls.intensity = input->intensity;
        session.frame.Prepare(
            session.color.data(), temporal ? session.history.data() : nullptr,
            temporal ? session.motion.data() : nullptr, nullptr, controls,
            session.features.data()).wait_and_throw();
        session.model.Execute(session.features.data(), session.head.data());
        queue.wait_and_throw();
        session.frame.Compose(session.color.data(), session.head.data(),
                              session.features.data(), nullptr, temporal,
                              controls, session.output.data()).wait_and_throw();
        queue.memcpy(input->output_rgba_f16, session.output.data(),
                     session.output.size_bytes()).wait_and_throw();
        Error(error, error_bytes, "");
        return ARC_NR_SYCL_OK;
    } catch (const std::exception& e) {
        Error(error, error_bytes, e.what());
        return ARC_NR_SYCL_INFERENCE_FAILED;
    } catch (...) {
        Error(error, error_bytes, "unknown SYCL inference failure");
        return ARC_NR_SYCL_INFERENCE_FAILED;
    }
}

void Destroy(void* session) { delete static_cast<Session*>(session); }

}  // namespace

extern "C" ARC_NR_EXPORT int arc_nr_sycl_get_api(
    uint32_t requested_version, ArcNrSyclApi* out_api) {
    if (requested_version != ARC_NR_SYCL_API_VERSION || out_api == nullptr ||
        out_api->struct_size != sizeof(ArcNrSyclApi))
        return 0;
    out_api->version = ARC_NR_SYCL_API_VERSION;
    out_api->create = Create;
    out_api->evaluate = Evaluate;
    out_api->destroy = Destroy;
    return 1;
}
