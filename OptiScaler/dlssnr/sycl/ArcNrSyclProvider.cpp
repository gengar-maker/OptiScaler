#include "ArcNrSyclApi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
#include <sycl/ext/oneapi/bindless_images.hpp>

#include "arc_nr/device_buffer.hpp"
#if defined(ARC_NR_USE_OPENVINO)
#include "runtime.hpp"
#else
#include "arc_nr/device_context.hpp"
#include "arc_nr/mlx_model.hpp"
#endif
#include "arc_nr/mlx_frame_pipeline.hpp"

#if defined(_WIN32)
#define ARC_NR_EXPORT __declspec(dllexport)
#else
#define ARC_NR_EXPORT __attribute__((visibility("default")))
#endif

namespace {

namespace external = sycl::ext::oneapi::experimental;

static_assert(sizeof(sycl::half) == sizeof(uint16_t));

void Error(char* destination, uint32_t capacity, const char* message) {
    if (destination == nullptr || capacity == 0) return;
    const std::size_t length = std::min<std::size_t>(
        std::strlen(message), static_cast<std::size_t>(capacity - 1));
    std::memcpy(destination, message, length);
    destination[length] = '\0';
}

sycl::queue IntelQueue() {
#if defined(ARC_NR_USE_OPENVINO)
    return arc_nr::ov_backend::MakeQueue();
#else
    // Tekken recreates its D3D11 device during startup. Keep the selected
    // Level Zero device so each new SNR session does not enumerate adapters
    // while the game and another SYCL client are using the GPU.
    static const sycl::device intel_device = []() -> sycl::device {
        for (const sycl::platform& platform : sycl::platform::get_platforms()) {
            if (platform.get_backend() != sycl::backend::ext_oneapi_level_zero)
                continue;
            for (const sycl::device& device : platform.get_devices()) {
                if (device.is_gpu() &&
                    device.get_info<sycl::info::device::vendor_id>() == 0x8086u)
                    return device;
            }
        }
        throw std::runtime_error("no Intel Level Zero GPU is available");
    }();
    return sycl::queue(intel_device, sycl::property_list{
        sycl::property::queue::enable_profiling{}});
#endif
}

#if defined(ARC_NR_USE_OPENVINO)
using ProviderContext = arc_nr::ov_backend::DeviceContext;
using ProviderModel = arc_nr::ov_backend::Model;
#else
using ProviderContext = arc_nr::DeviceContext;
using ProviderModel = arc_nr::MlxModelPlan;
#endif

struct Session {
    std::mutex mutex;
    ProviderContext context;
    arc_nr::MlxFramePipeline frame;
    ProviderModel model;
    const uint32_t image_width;
    const uint32_t image_height;
    const std::size_t pixels;
    const std::size_t network_pixels;
    arc_nr::DeviceBuffer<sycl::half> color;
    arc_nr::DeviceBuffer<sycl::half> history;
    arc_nr::DeviceBuffer<sycl::half> motion;
    arc_nr::DeviceBuffer<sycl::half> previous_color;
    arc_nr::DeviceBuffer<sycl::half> motion_confidence;
    arc_nr::DeviceBuffer<int> tile_motion;
    arc_nr::DeviceBuffer<sycl::half> features;
    arc_nr::DeviceBuffer<sycl::half> head;
    arc_nr::DeviceBuffer<sycl::half> output;
    // Allocated once on first use; not needed by the default or HDR path.
    arc_nr::DeviceBuffer<float> base_tone_scratch;
    bool gpu_history_valid = false;
    bool previous_color_valid = false;
    uint32_t last_gpu_frame_index = 0;

    Session(uint32_t width, uint32_t height,
            const std::filesystem::path& logical,
            const std::filesystem::path& derived)
        : context(IntelQueue()),
          frame(context.queue(), width, height
#if defined(ARC_NR_USE_OPENVINO)
                , 16
#endif
          ),
          model(context, frame.network_height(), frame.network_width(),
                logical, derived),
          image_width(width),
          image_height(height),
          pixels(static_cast<std::size_t>(width) * height),
          network_pixels(static_cast<std::size_t>(frame.network_width()) *
                         frame.network_height()),
          color(context.queue(), pixels * 4),
          history(context.queue(), pixels * 4),
          motion(context.queue(), pixels * 2),
          previous_color(context.queue(), pixels * 4),
          motion_confidence(context.queue(), pixels),
          features(context.queue(), network_pixels * 16),
          head(context.queue(), network_pixels * 4),
          output(context.queue(), pixels * 4) {}
    ~Session() {
#if defined(ARC_NR_USE_OPENVINO)
        // Release remote tensors before the externally owned USM buffers.
        model.Reset();
#endif
    }
};

// Object vectors do not describe lighting and disocclusions. Compare the
// reprojected input with the previous input before exposing temporal history
// to the model. This also rejects vectors pointing outside the frame.
void ConfidenceForExternalMotion(Session& session) {
    const uint32_t w = session.image_width, h = session.image_height;
    const auto* current = session.color.data();
    const auto* previous = session.previous_color.data();
    const auto* motion = session.motion.data();
    auto* confidence = session.motion_confidence.data();
    session.context.queue().parallel_for(sycl::range<1>(session.pixels),
        [=](sycl::id<1> id) {
            const size_t i = id[0];
            const float x = static_cast<float>(i % w);
            const float y = static_cast<float>(i / w);
            const float px = x + static_cast<float>(motion[i * 2]) * w;
            const float py = y + static_cast<float>(motion[i * 2 + 1]) * h;
            if (!sycl::isfinite(px) || !sycl::isfinite(py) ||
                px < 0.0f || py < 0.0f ||
                px > static_cast<float>(w - 1) ||
                py > static_cast<float>(h - 1)) {
                confidence[i] = sycl::half(0.0f);
                return;
            }
            const uint32_t x0 = static_cast<uint32_t>(px);
            const uint32_t y0 = static_cast<uint32_t>(py);
            const uint32_t x1 = sycl::min(x0 + 1, w - 1);
            const uint32_t y1 = sycl::min(y0 + 1, h - 1);
            const float tx = px - static_cast<float>(x0);
            const float ty = py - static_cast<float>(y0);
            float mismatch = 0.0f;
            for (uint32_t c = 0; c < 3; ++c) {
                const float a = static_cast<float>(previous[
                    (static_cast<size_t>(y0) * w + x0) * 4 + c]);
                const float b = static_cast<float>(previous[
                    (static_cast<size_t>(y0) * w + x1) * 4 + c]);
                const float d = static_cast<float>(previous[
                    (static_cast<size_t>(y1) * w + x0) * 4 + c]);
                const float e = static_cast<float>(previous[
                    (static_cast<size_t>(y1) * w + x1) * 4 + c]);
                const float warped = (a + tx * (b - a)) * (1.0f - ty) +
                                     (d + tx * (e - d)) * ty;
                mismatch += sycl::fabs(static_cast<float>(current[i * 4 + c]) -
                                       warped);
            }
            const float quality = sycl::clamp((0.12f - mismatch / 3.0f) /
                                               0.09f, 0.0f, 1.0f);
            confidence[i] = sycl::half(quality);
        }).wait_and_throw();
}

// Backward flow: for each current tile, find the matching location in the
// previous pre-NR frame. Both passes stay on the Intel GPU. Pixel confidence
// rejects disocclusions and bad matches before history reaches the model.
void EstimateMotion(Session& session) {
    auto& queue = session.context.queue();
    const int w = static_cast<int>(session.image_width);
    const int h = static_cast<int>(session.image_height);
    const int tiles_x = (w + 15) / 16;
    const size_t tiles = static_cast<size_t>(tiles_x) * ((h + 15) / 16);
    const auto* current = session.color.data();
    const auto* previous = session.previous_color.data();
    auto* tile_motion = session.tile_motion.data();
    queue.parallel_for(sycl::range<1>(tiles), [=](sycl::id<1> id) {
        const int tx = static_cast<int>(id[0] % tiles_x);
        const int ty = static_cast<int>(id[0] / tiles_x);
        float best = 1.0e20f;
        int best_x = 0, best_y = 0;
        for (int pass = 0; pass < 2; ++pass) {
            const int step = pass == 0 ? 4 : 1;
            const int radius = pass == 0 ? 16 : 3;
            const int center_x = pass == 0 ? 0 : best_x;
            const int center_y = pass == 0 ? 0 : best_y;
            float pass_best = 1.0e20f;
            int pass_x = center_x, pass_y = center_y;
            for (int dy = -radius; dy <= radius; dy += step) {
                for (int dx = -radius; dx <= radius; dx += step) {
                    const int mx = center_x + dx, my = center_y + dy;
                    float score = 0.0f;
                    int count = 0;
                    for (int sy = 2; sy < 16; sy += 4) {
                        const int y = ty * 16 + sy;
                        if (y >= h || y + my < 0 || y + my >= h) continue;
                        for (int sx = 2; sx < 16; sx += 4) {
                            const int x = tx * 16 + sx;
                            if (x >= w || x + mx < 0 || x + mx >= w) continue;
                            const size_t a = (static_cast<size_t>(y) * w + x) * 4;
                            const size_t b = (static_cast<size_t>(y + my) * w +
                                              x + mx) * 4;
                            const float ac = 0.25f * static_cast<float>(current[a]) +
                                0.5f * static_cast<float>(current[a + 1]) +
                                0.25f * static_cast<float>(current[a + 2]);
                            const float bc = 0.25f * static_cast<float>(previous[b]) +
                                0.5f * static_cast<float>(previous[b + 1]) +
                                0.25f * static_cast<float>(previous[b + 2]);
                            score += sycl::fabs(ac - bc);
                            ++count;
                        }
                    }
                    if (count < 8) continue;
                    score = score / static_cast<float>(count) +
                        0.0005f * static_cast<float>(sycl::abs(mx) + sycl::abs(my));
                    if (score < pass_best) {
                        pass_best = score;
                        pass_x = mx;
                        pass_y = my;
                    }
                }
            }
            best = pass_best;
            best_x = pass_x;
            best_y = pass_y;
        }
        tile_motion[id[0] * 2] = best_x;
        tile_motion[id[0] * 2 + 1] = best_y;
    }).wait_and_throw();

    auto* motion = session.motion.data();
    auto* confidence = session.motion_confidence.data();
    queue.parallel_for(sycl::range<1>(session.pixels), [=](sycl::id<1> id) {
        const int x = static_cast<int>(id[0] % w);
        const int y = static_cast<int>(id[0] / w);
        const size_t tile = static_cast<size_t>(y / 16) * tiles_x + x / 16;
        const int dx = tile_motion[tile * 2], dy = tile_motion[tile * 2 + 1];
        float quality = 0.0f;
        if (x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h) {
            const size_t a = id[0] * 4;
            const size_t b = (static_cast<size_t>(y + dy) * w + x + dx) * 4;
            float error = 0.0f;
            for (int c = 0; c < 3; ++c)
                error += sycl::fabs(static_cast<float>(current[a + c]) -
                                    static_cast<float>(previous[b + c]));
            quality = sycl::clamp((0.12f - error / 3.0f) / 0.09f, 0.0f, 1.0f);
        }
        motion[id[0] * 2] = sycl::half(static_cast<float>(dx) / w);
        motion[id[0] * 2 + 1] = sycl::half(static_cast<float>(dy) / h);
        confidence[id[0]] = sycl::half(quality);
    }).wait_and_throw();
}

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
#if !defined(ARC_NR_USE_OPENVINO)
        if (!std::filesystem::is_directory(logical) ||
            !std::filesystem::is_directory(derived)) {
            Error(error, error_bytes, "SYCL model weight directory is missing");
            return nullptr;
        }
#endif
#if defined(ARC_NR_USE_OPENVINO)
        arc_nr::ov_backend::SetPreparationStatus("Preparing SNR resources");
        // Check/export before allocating any GPU resources for the new size.
        arc_nr::ov_backend::EnsureProfile(
            arc_nr::MlxFramePipeline::AlignedExtent(create->width, 16),
            arc_nr::MlxFramePipeline::AlignedExtent(create->height, 16));
#endif
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
    session.gpu_history_valid = false;
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

#if defined(ARC_NR_USE_OPENVINO)
using ImportedHeap = arc_nr::ov_backend::ImportedHeap;
#else
class ImportedHeap {
public:
    ImportedHeap(ProviderContext& context, void* handle, uint64_t bytes,
                 bool read_input = true)
        : queue_(context.queue()) {
        external::external_mem_descriptor<external::resource_win32_handle>
            descriptor {{handle}, external::external_mem_handle_type::win32_nt_handle,
                        static_cast<size_t>(bytes)};
        memory_ = external::import_external_memory(descriptor, queue_);
        imported_ = true;
        data_ = static_cast<sycl::half*>(external::map_external_linear_memory(
            memory_, 0, bytes, queue_));
    }
    ~ImportedHeap() {
        try {
            if (data_ != nullptr) external::unmap_external_linear_memory(data_, queue_);
            if (imported_) external::release_external_memory(memory_, queue_);
        } catch (...) {}
    }
    ImportedHeap(const ImportedHeap&) = delete;
    ImportedHeap& operator=(const ImportedHeap&) = delete;
    sycl::half* data() const { return data_; }
    void Flush() {}
private:
    sycl::queue& queue_;
    external::external_mem memory_ {};
    bool imported_ = false;
    sycl::half* data_ = nullptr;
};
#endif

ArcNrSyclStatus EvaluateGpu(void* opaque, const ArcNrSyclGpuFrame* input,
                             char* error, uint32_t error_bytes) {
    if (opaque == nullptr || input == nullptr ||
        input->struct_size != sizeof(*input) ||
        input->controls.struct_size != sizeof(ArcNrSyclFrame) ||
        input->color_heap_nt_handle == nullptr ||
        input->output_heap_nt_handle == nullptr ||
        input->row_pitch_bytes == 0 ||
        (input->row_pitch_bytes & 7u) != 0 ||
        input->pixel_format > 3) {
        Error(error, error_bytes, "invalid shared GPU frame");
        return ARC_NR_SYCL_INVALID_ARGUMENT;
    }
    auto& session = *static_cast<Session*>(opaque);
    const uint32_t pixel_bytes = input->pixel_format == 0 ? 8u : 4u;
    const uint64_t minimum_bytes =
        static_cast<uint64_t>(session.pixels) * pixel_bytes;
    if (input->heap_bytes < minimum_bytes) {
        Error(error, error_bytes, "shared GPU heap is too small");
        return ARC_NR_SYCL_INVALID_ARGUMENT;
    }
    const bool has_motion = input->motion_heap_nt_handle != nullptr;
    if (has_motion &&
        ((input->motion_format != 1 && input->motion_format != 2) ||
         input->motion_width == 0 || input->motion_height == 0 ||
         input->motion_width > 8192 || input->motion_height > 8192 ||
         input->motion_base_x >= input->motion_width ||
         input->motion_base_y >= input->motion_height ||
         input->motion_subrect_width == 0 ||
         input->motion_subrect_height == 0 ||
         input->motion_subrect_width >
             input->motion_width - input->motion_base_x ||
         input->motion_subrect_height >
             input->motion_height - input->motion_base_y ||
         input->motion_row_pitch_bytes <
             input->motion_width * (input->motion_format == 1 ? 4u : 8u) ||
         input->motion_heap_bytes <
             static_cast<uint64_t>(input->motion_row_pitch_bytes) *
                 input->motion_height ||
         !std::isfinite(input->motion_scale_x) ||
         !std::isfinite(input->motion_scale_y))) {
        Error(error, error_bytes, "invalid shared GPU motion texture");
        return ARC_NR_SYCL_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session.mutex);
    try {
        using Clock = std::chrono::steady_clock;
        const auto elapsed_us = [](Clock::time_point begin,
                                   Clock::time_point end) -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
        };
        const auto start = Clock::now();
        auto& queue = session.context.queue();
        Clock::time_point imported, copied_input, prepared, modeled, composed,
                          copied_output;
        bool temporal = false;
        {
        ImportedHeap color_heap(session.context, input->color_heap_nt_handle,
                                input->heap_bytes);
        ImportedHeap output_heap(session.context, input->output_heap_nt_handle,
                                 input->heap_bytes, false);
        const bool estimate_enabled =
            (input->controls.reserved & 1u) != 0;
        if (estimate_enabled && !has_motion && !session.tile_motion.data()) {
            session.tile_motion = arc_nr::DeviceBuffer<int>(queue,
                static_cast<size_t>((session.image_width + 15) / 16) *
                ((session.image_height + 15) / 16) * 2);
        }
        const bool estimated_motion = estimate_enabled && !has_motion &&
            input->controls.reset_history == 0 &&
            session.previous_color_valid;
        temporal = (has_motion || estimated_motion) &&
            session.previous_color_valid &&
            input->controls.reset_history == 0 &&
            session.gpu_history_valid &&
            input->controls.frame_index == session.last_gpu_frame_index + 1u;
        std::unique_ptr<ImportedHeap> motion_heap;
        if (temporal && has_motion)
            motion_heap = std::make_unique<ImportedHeap>(
                session.context, input->motion_heap_nt_handle,
                input->motion_heap_bytes);
        imported = Clock::now();
        const auto* source = color_heap.data();
        auto* destination = output_heap.data();
        auto* color = session.color.data();
        const size_t row_elements = input->row_pitch_bytes / sizeof(sycl::half);
        const size_t image_width = session.image_width;
        const size_t image_height = session.image_height;
        const size_t row_bytes = input->row_pitch_bytes;
        if (row_bytes < image_width * pixel_bytes ||
            input->heap_bytes < row_bytes * image_height)
            throw std::runtime_error("shared GPU heap footprint is too small");
        if (input->pixel_format == 0) {
            queue.parallel_for(sycl::range<1>(session.pixels * 4),
                               [=](sycl::id<1> id) {
                const size_t i = id[0];
                color[i] = source[(i / (image_width * 4)) * row_elements +
                                  i % (image_width * 4)];
            }).wait_and_throw();
        } else if (input->pixel_format == 3) {
            const auto* source32 = reinterpret_cast<const uint32_t*>(source);
            const size_t pitch32 = row_bytes / 4;
            queue.parallel_for(sycl::range<1>(session.pixels),
                               [=](sycl::id<1> id) {
                const size_t pixel = id[0];
                const uint32_t packed = source32[
                    (pixel / image_width) * pitch32 + pixel % image_width];
                color[pixel * 4 + 0] = sycl::half(
                    static_cast<float>(packed & 1023u) / 1023.0f);
                color[pixel * 4 + 1] = sycl::half(
                    static_cast<float>((packed >> 10) & 1023u) / 1023.0f);
                color[pixel * 4 + 2] = sycl::half(
                    static_cast<float>((packed >> 20) & 1023u) / 1023.0f);
                color[pixel * 4 + 3] = sycl::half(
                    static_cast<float>(packed >> 30) / 3.0f);
            }).wait_and_throw();
        } else {
            const auto* source32 = reinterpret_cast<const uint32_t*>(source);
            const bool bgra = input->pixel_format == 2;
            const size_t pitch32 = row_bytes / 4;
            queue.parallel_for(sycl::range<1>(session.pixels),
                               [=](sycl::id<1> id) {
                const size_t pixel = id[0];
                const uint32_t packed = source32[
                    (pixel / image_width) * pitch32 + pixel % image_width];
                const uint32_t c0 = packed & 255u;
                const uint32_t c1 = (packed >> 8) & 255u;
                const uint32_t c2 = (packed >> 16) & 255u;
                const uint32_t c3 = packed >> 24;
                color[pixel * 4 + 0] = sycl::half(
                    static_cast<float>(bgra ? c2 : c0) / 255.0f);
                color[pixel * 4 + 1] = sycl::half(
                    static_cast<float>(c1) / 255.0f);
                color[pixel * 4 + 2] = sycl::half(
                    static_cast<float>(bgra ? c0 : c2) / 255.0f);
                color[pixel * 4 + 3] = sycl::half(
                    static_cast<float>(c3) / 255.0f);
            }).wait_and_throw();
        }
        if (temporal) {
            // History already contains the unmodified neural reconstruction.
            // Presentation controls must never feed back into the next frame.
            if (estimated_motion) {
                EstimateMotion(session);
            } else {
            const auto* motion16 = motion_heap->data();
            const auto* motion32 =
                reinterpret_cast<const float*>(motion_heap->data());
            auto* normalized = session.motion.data();
            const uint32_t format = input->motion_format;
            const uint32_t pitch16 = input->motion_row_pitch_bytes / 2;
            const uint32_t pitch32 = input->motion_row_pitch_bytes / 4;
            const uint32_t base_x = input->motion_base_x;
            const uint32_t base_y = input->motion_base_y;
            const uint32_t sub_w = input->motion_subrect_width;
            const uint32_t sub_h = input->motion_subrect_height;
            const float scale_x = input->motion_scale_x /
                static_cast<float>(sub_w);
            const float scale_y = input->motion_scale_y /
                static_cast<float>(sub_h);
            queue.parallel_for(sycl::range<1>(session.pixels),
                               [=](sycl::id<1> id) {
                const size_t i = id[0];
                const uint32_t x = static_cast<uint32_t>(i % image_width);
                const uint32_t y = static_cast<uint32_t>(i / image_width);
                const float px = static_cast<float>(base_x) +
                    (static_cast<float>(x) + 0.5f) *
                        static_cast<float>(sub_w) /
                        static_cast<float>(image_width) - 0.5f;
                const float py = static_cast<float>(base_y) +
                    (static_cast<float>(y) + 0.5f) *
                        static_cast<float>(sub_h) /
                        static_cast<float>(image_height) - 0.5f;
                const int left = sycl::clamp(
                    static_cast<int>(sycl::floor(px)),
                    static_cast<int>(base_x),
                    static_cast<int>(base_x + sub_w - 1));
                const int top = sycl::clamp(
                    static_cast<int>(sycl::floor(py)),
                    static_cast<int>(base_y),
                    static_cast<int>(base_y + sub_h - 1));
                const int right = sycl::min(left + 1,
                    static_cast<int>(base_x + sub_w - 1));
                const int bottom = sycl::min(top + 1,
                    static_cast<int>(base_y + sub_h - 1));
                const float tx = sycl::clamp(px - static_cast<float>(left),
                                             0.0f, 1.0f);
                const float ty = sycl::clamp(py - static_cast<float>(top),
                                             0.0f, 1.0f);
                for (int c = 0; c < 2; ++c) {
                    const auto sample = [=](int sx, int sy) -> float {
                        if (format == 1)
                            return static_cast<float>(motion16[
                                static_cast<size_t>(sy) * pitch16 +
                                static_cast<size_t>(sx) * 2 + c]);
                        return motion32[static_cast<size_t>(sy) * pitch32 +
                                        static_cast<size_t>(sx) * 2 + c];
                    };
                    const float a = sample(left, top);
                    const float b = sample(right, top);
                    const float d = sample(left, bottom);
                    const float e = sample(right, bottom);
                    const float value = sycl::isfinite(a) &&
                        sycl::isfinite(b) && sycl::isfinite(d) &&
                        sycl::isfinite(e)
                        ? ((a + tx * (b - a)) * (1.0f - ty) +
                           (d + tx * (e - d)) * ty)
                        : 0.0f;
                    normalized[i * 2 + c] = sycl::half(value *
                        (c == 0 ? scale_x : scale_y));
                }
            }).wait_and_throw();
            ConfidenceForExternalMotion(session);
            }
        }
        if (has_motion || estimate_enabled) {
            queue.memcpy(session.previous_color.data(), session.color.data(),
                         session.color.size_bytes()).wait_and_throw();
            session.previous_color_valid = true;
        } else {
            session.previous_color_valid = false;
        }
        copied_input = Clock::now();
        arc_nr::MlxFrameControls controls;
        controls.frame_index = input->controls.frame_index;
        controls.normalized_style = input->controls.normalized_style;
        controls.local_tone = input->controls.local_tone;
        controls.local_structure = input->controls.local_structure;
        controls.skin_structure = input->controls.skin_structure;
        controls.automatic_structure = input->controls.automatic_structure;
        controls.intensity = input->controls.intensity;
        session.frame.Prepare(session.color.data(),
                              temporal ? session.history.data() : nullptr,
                              temporal ? session.motion.data() : nullptr,
                              nullptr,
                              controls, session.features.data(),
                              temporal ?
                                  session.motion_confidence.data() : nullptr)
            .wait_and_throw();
        prepared = Clock::now();
        session.model.Execute(session.features.data(), session.head.data());
        queue.wait_and_throw();
        modeled = Clock::now();
        const bool have_strengths = (input->controls.reserved & 2u) != 0;
        const float colour_strength = have_strengths
            ? ((input->controls.reserved >> 8) & 0xfffu) / 1024.0f : 1.0f;
        const float transfer_strength = have_strengths
            ? ((input->controls.reserved >> 20) & 0xfffu) / 1024.0f : 1.0f;
        session.frame.Compose(session.color.data(), session.head.data(),
                              session.features.data(), nullptr, temporal,
                              controls, session.output.data(),
                              temporal ?
                                  session.motion_confidence.data() : nullptr,
                              colour_strength, transfer_strength,
                              session.history.data())
            .wait_and_throw();
        if ((input->controls.reserved & 4u) != 0 && transfer_strength != 0.0f) {
            if (!session.base_tone_scratch.data())
                session.base_tone_scratch = arc_nr::DeviceBuffer<float>(queue, session.pixels * 4);
            session.frame.PreserveBaseTone(session.color.data(), session.output.data(),
                                           session.base_tone_scratch.data(),
                                           colour_strength == 0.0f).wait_and_throw();
        }
        composed = Clock::now();
        const auto* output = session.output.data();
        if (input->pixel_format == 0) {
            if (row_elements == image_width * 4) {
                queue.memcpy(destination, output, session.output.size_bytes())
                    .wait_and_throw();
            } else {
                queue.parallel_for(sycl::range<1>(session.pixels * 4),
                                   [=](sycl::id<1> id) {
                    const size_t i = id[0];
                    destination[(i / (image_width * 4)) * row_elements +
                                i % (image_width * 4)] = output[i];
                }).wait_and_throw();
            }
        } else if (input->pixel_format == 3) {
            auto* destination32 = reinterpret_cast<uint32_t*>(destination);
            const size_t pitch32 = row_bytes / 4;
            queue.parallel_for(sycl::range<1>(session.pixels),
                               [=](sycl::id<1> id) {
                const size_t pixel = id[0];
                const auto quantize = [=](size_t channel,
                                          float scale) -> uint32_t {
                    const float value = sycl::clamp(
                        static_cast<float>(output[pixel * 4 + channel]),
                        0.0f, 1.0f);
                    return static_cast<uint32_t>(sycl::round(value * scale));
                };
                const uint32_t r = quantize(0, 1023.0f);
                const uint32_t g = quantize(1, 1023.0f);
                const uint32_t b = quantize(2, 1023.0f);
                const uint32_t a = quantize(3, 3.0f);
                destination32[(pixel / image_width) * pitch32 +
                              pixel % image_width] = r | (g << 10) |
                                                     (b << 20) | (a << 30);
            }).wait_and_throw();
        } else {
            auto* destination32 = reinterpret_cast<uint32_t*>(destination);
            const bool bgra = input->pixel_format == 2;
            const size_t pitch32 = row_bytes / 4;
            queue.parallel_for(sycl::range<1>(session.pixels),
                               [=](sycl::id<1> id) {
                const size_t pixel = id[0];
                const auto quantize = [=](size_t channel) -> uint32_t {
                    const float value = sycl::clamp(
                        static_cast<float>(output[pixel * 4 + channel]),
                        0.0f, 1.0f);
                    return static_cast<uint32_t>(sycl::round(value * 255.0f));
                };
                const uint32_t r = quantize(0);
                const uint32_t g = quantize(1);
                const uint32_t b = quantize(2);
                const uint32_t a = quantize(3);
                destination32[(pixel / image_width) * pitch32 +
                              pixel % image_width] =
                    (bgra ? b : r) | (g << 8) |
                    ((bgra ? r : b) << 16) | (a << 24);
            }).wait_and_throw();
        }
        output_heap.Flush();
        copied_output = Clock::now();
        }
        const auto released = Clock::now();
        session.gpu_history_valid = true;
        session.last_gpu_frame_index = input->controls.frame_index;
        if (input->timings != nullptr) {
            *input->timings = ArcNrSyclGpuTimings {
                elapsed_us(start, imported),
                elapsed_us(imported, copied_input),
                elapsed_us(copied_input, prepared),
                elapsed_us(prepared, modeled),
                elapsed_us(modeled, composed),
                elapsed_us(composed, copied_output),
                elapsed_us(copied_output, released),
                elapsed_us(start, released),
                temporal ? 1u : 0u,
            };
        }
        Error(error, error_bytes, "");
        return ARC_NR_SYCL_OK;
    } catch (const std::exception& e) {
        session.gpu_history_valid = false;
        session.previous_color_valid = false;
        Error(error, error_bytes, e.what());
        return ARC_NR_SYCL_INFERENCE_FAILED;
    } catch (...) {
        session.gpu_history_valid = false;
        session.previous_color_valid = false;
        Error(error, error_bytes, "unknown shared GPU inference failure");
        return ARC_NR_SYCL_INFERENCE_FAILED;
    }
}

}  // namespace

#if defined(ARC_NR_USE_OPENVINO)
extern "C" ARC_NR_EXPORT void arc_nr_sycl_preparation_status(char* out, uint32_t bytes) {
    Error(out, bytes, arc_nr::ov_backend::PreparationStatus().c_str());
}
#endif

extern "C" ARC_NR_EXPORT int arc_nr_sycl_get_api(
    uint32_t requested_version, ArcNrSyclApi* out_api) {
    if (requested_version != ARC_NR_SYCL_API_VERSION || out_api == nullptr ||
        out_api->struct_size != sizeof(ArcNrSyclApi))
        return 0;
    out_api->version = ARC_NR_SYCL_API_VERSION;
    out_api->create = Create;
    out_api->evaluate = Evaluate;
    out_api->destroy = Destroy;
    out_api->evaluate_gpu = EvaluateGpu;
    return 1;
}
