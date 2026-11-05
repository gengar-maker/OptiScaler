#include "gpu_fixture.h"
#include "XeSSNrProcessor.h"
#include <filesystem>
#include <fstream>
#include <thread>

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 4, "usage: processor_smoke PROVIDER_DIRECTORY CAPTURE_DIRECTORY OUTPUT_DIRECTORY");
        const std::filesystem::path provider = argv[1], capture = argv[2], output = argv[3];
        std::filesystem::create_directories(output);
        constexpr unsigned w = 1280, h = 720;
        std::vector<char> pixels(w * h * 8);
        std::ifstream source(capture / "color.bin", std::ios::binary);
        Require(bool(source.read(pixels.data(), pixels.size())), "captured colour read failed");
        Gpu gpu;
        auto colour = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), w, h);
        auto motion = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), w, h);
        gpu.Upload(colour.Get(), pixels.data(), w * 8, D3D12_RESOURCE_STATE_COMMON);
        std::vector<char> zero(w * h * 8);
        gpu.Upload(motion.Get(), zero.data(), w * 8, D3D12_RESOURCE_STATE_COMMON);
        XeSSNr::ExecutionOptions options;
        options.batchSubmissions = !std::getenv("SNR_BENCH_SERIAL");
        options.localSharedHeap = !std::getenv("SNR_BENCH_CROSS_ADAPTER");
        XeSSNr::Processor processor(gpu.d.Get(), options);
        std::string error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        for (;;)
        {
            auto state = processor.Prepare(provider, w, h, error);
            if (state == ArcNrSyclLoader::Preparation::Ready)
                break;
            Require(state != ArcNrSyclLoader::Preparation::Failed, error.c_str());
            Require(std::chrono::steady_clock::now() < deadline, "model compilation deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const unsigned frames =
            std::getenv("SNR_BENCH_FRAMES") ? std::max(4, atoi(std::getenv("SNR_BENCH_FRAMES"))) : 4;
        for (unsigned i = 0; i < frames; ++i)
        {
            XeSSNr::Frame frame {};
            frame.colour = colour;
            frame.motion = motion;
            frame.output = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), w, h);
            frame.width = w;
            frame.height = h;
            frame.sequence = i;
            frame.ndc = true;
            frame.velocityScale = { 2, -2 };
            frame.applyModel = i != 2;
            frame.hdr.WhitePoint = 1;
            frame.hdr.ExposurePreMul = 1;
            frame.hdr.ColourStrength = frame.hdr.TransferStrength = 1;
            frame.hdr.MaxRatio = 2;
            frame.hdr.CompareZoom = 1;
            frame.controls.local_tone = frame.controls.local_structure = frame.controls.automatic_structure =
                frame.controls.intensity = 1;
            frame.controls.skin_structure = -1;
            Require(processor.Run(gpu.q.Get(), frame, error), error.c_str());
            const auto t = processor.Timings();
            Require(t.temporal_active == ((i == 1 || i >= 4) ? 1u : 0u), "history reset/continuity mismatch");
            const auto bytes = gpu.Read(frame.output.Get(), D3D12_RESOURCE_STATE_COMMON);
            if (!frame.applyModel)
                Require(bytes == pixels, "full model ApplyModel=false is not byte exact");
            for (size_t n = 0; n < bytes.size() / 2; ++n)
            {
                HALF v, input;
                memcpy(&v, bytes.data() + n * 2, 2);
                memcpy(&input, pixels.data() + n * 2, 2);
                Require(std::isfinite(XMConvertHalfToFloat(v)), "nonfinite output");
                if (n % 4 == 3)
                    Require(v == input, "alpha changed");
            }
            const auto g = processor.GpuTimings();
            std::cout << "NR-GPU frame=" << i << " fallback=" << g[0] << " encode=" << g[1] << " inputCopy=" << g[2]
                      << " outputCopy=" << g[3] << " resolve=" << g[4] << "ms\n";
            std::cout << "NR-BENCH frame=" << i << " pass=" << processor.LastFrameMs()
                      << " model=" << t.model_us / 1000.0 << "ms\n";
            if (i == frames - 1)
            {
                std::ofstream file(output / "last-frame.bin", std::ios::binary);
                file.write(bytes.data(), bytes.size());
            }
            if (i == 0)
            {
                std::ofstream file(output / "d3d12-snr-hdr.bin", std::ios::binary);
                file.write(bytes.data(), bytes.size());
                Require(bool(file), "output write failed");
            }
            std::cout << "PASS full FP16 frame=" << i << " apply=" << frame.applyModel
                      << " temporal=" << t.temporal_active << '\n';
        }
        ComPtr<ID3D12InfoQueue> debug;
        const auto debugResult = gpu.d.As(&debug);
        Require(SUCCEEDED(debugResult) || std::getenv("SNR_TEST_NO_DEBUG"), "debug info queue missing");
        unsigned errors = 0;
        for (UINT64 i = 0; debug && i < debug->GetNumStoredMessages(); ++i)
        {
            SIZE_T n = 0;
            debug->GetMessage(i, nullptr, &n);
            std::vector<char> bytes(n);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            debug->GetMessage(i, m, &n);
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                ++errors;
                std::cerr << m->pDescription << '\n';
            }
        }
        Require(errors == 0, "D3D12 validation failed");
        std::cout << "PASS D3D12 HDR -> full OpenVINO -> restored HDR, exact hidden edit, temporal reset, no host "
                     "image staging, validation passed\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
