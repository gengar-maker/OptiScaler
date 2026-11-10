#include "gpu_fixture.h"
int main()
{
    try
    {
        Gpu gpu;
        constexpr unsigned w = 64, h = 32, tw = 80, th = 48;
        auto colour = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), tw, th);
        auto motion = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), tw, th); // RGBA input: shader reads RG only.
        std::vector<HALF> pixels(tw * th * 4), vectors(tw * th * 4);
        for (unsigned y = 0; y < th; ++y)
            for (unsigned x = 0; x < tw; ++x)
            {
                const auto i = (y * tw + x) * 4;
                pixels[i] = XMConvertFloatToHalf(float(x % 31) * .13f);
                pixels[i + 1] = XMConvertFloatToHalf(float(y % 23) * .37f);
                pixels[i + 2] = XMConvertFloatToHalf(float((x + y) % 47) * .91f);
                pixels[i + 3] = XMConvertFloatToHalf(.25f + float(x % 7) * .125f);
                vectors[i] = XMConvertFloatToHalf(float(int(x) - 40) / 128.f);
                vectors[i + 1] = XMConvertFloatToHalf(float(int(y) - 24) / 256.f);
            }
        gpu.Upload(colour.Get(), pixels.data(), tw * 8, D3D12_RESOURCE_STATE_COMMON);
        gpu.Upload(motion.Get(), vectors.data(), tw * 8, D3D12_RESOURCE_STATE_COMMON);
        XeSSNr::InputAdapter adapter(gpu.d.Get(), w, h);
        std::vector<char> expected(w * h * 8);
        for (unsigned y = 0; y < h; ++y)
            memcpy(expected.data() + y * w * 8, pixels.data() + ((y + 5) * tw + 3) * 4, w * 8);
        for (bool ndc : { false, true })
            for (bool jittered : { false, true })
                for (bool apply : { false, true })
                {
                    auto out = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), w, h);
                    XeSSNr::InputAdapter::CopyOriginal(gpu.l.Get(), colour.Get(), 3, 5, w, h, out.Get());
                    gpu.Submit();
                    Require(gpu.Read(out.Get(), D3D12_RESOURCE_STATE_COMMON) == expected,
                            "colour crop bypass changed bytes");
                    const auto t = XeSSNr::MotionToRenderPixels(w, h, { 2, -2 }, ndc, jittered, { .25f, -.125f },
                                                                { -.125f, .25f });
                    DlssNrConstants c {};
                    c.WhitePoint = 1;
                    c.ExposurePreMul = 1;
                    c.TransferStrength = c.ColourStrength = 1;
                    c.MaxRatio = 2;
                    c.CompareZoom = 1;
                    adapter.Encode(gpu.l.Get(), out.Get(), motion.Get(), 7, 2, t, c);
                    gpu.Submit();
                    const auto converted = gpu.Read(adapter.Motion(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    for (unsigned y = 0; y < h; ++y)
                        for (unsigned x = 0; x < w; ++x)
                        {
                            const auto i = ((y + 2) * tw + x + 7) * 4;
                            float actual[2];
                            memcpy(actual, converted.data() + (y * w + x) * 8, 8);
                            const float vx =
                                XMConvertHalfToFloat(vectors[i]) * (ndc ? float(w) : 2.f) + (jittered ? 0.f : -.375f);
                            const float vy = XMConvertHalfToFloat(vectors[i + 1]) * (ndc ? float(h) : -2.f) +
                                             (jittered ? 0.f : .375f);
                            Require(std::abs(actual[0] - vx) < 1e-6f && std::abs(actual[1] - vy) < 1e-6f,
                                    "motion coordinate/sign mismatch");
                        }
                    // Identity model output, independent of shader arithmetic.
                    Barrier(gpu.l.Get(), adapter.Proxy(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(gpu.l.Get(), adapter.Neural(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    gpu.l->CopyResource(adapter.Neural(), adapter.Proxy());
                    Barrier(gpu.l.Get(), adapter.Proxy(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    Barrier(gpu.l.Get(), adapter.Neural(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    gpu.Submit();
                    adapter.Resolve(gpu.l.Get(), out.Get(), apply);
                    gpu.Submit();
                    const auto actual = gpu.Read(out.Get(), D3D12_RESOURCE_STATE_COMMON);
                    if (!apply)
                        Require(actual == expected, "ApplyModel=false changed HDR/alpha");
                    for (size_t i = 0; i < actual.size() / 2; ++i)
                    {
                        HALF ah, bh;
                        memcpy(&ah, actual.data() + i * 2, 2);
                        memcpy(&bh, expected.data() + i * 2, 2);
                        if (i % 4 == 3)
                            Require(ah == bh, "alpha changed");
                        else
                            Require(std::abs(XMConvertHalfToFloat(ah) - XMConvertHalfToFloat(bh)) <
                                        .003f * std::max(1.f, XMConvertHalfToFloat(bh)),
                                    "neutral HDR error");
                    }
                }
        // Projection-based independent sign check: jitter moves geometry by J;
        // sample locations are p-J. The previous pixel is p+Jprevious-Jcurrent.
        const XeSSNr::Vec2 previous { -.2f, .3f }, current { .4f, -.1f };
        const auto t = XeSSNr::MotionToRenderPixels(w, h, { 2, -2 }, true, false, current, previous);
        Require(std::abs((12.f + t.bias.x - previous.x) - (12.f - current.x)) < 1e-5f,
                "static jitter x reprojection failed");
        Require(std::abs((9.f + t.bias.y - previous.y) - (9.f - current.y)) < 1e-5f,
                "static jitter y reprojection failed");
        Require(XeSSNr::ScalarWhitePoint(2, 4) == .125f, "scalar exposure multiplier contract");

        // Ordinary game textures retain their exact combined read states. Exposure
        // is sampled once on the GPU, converted from R8_UNORM and reused for resolve.
        auto ordinary = [&](UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_STATES state)
        {
            D3D12_RESOURCE_DESC d {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = width;
            d.Height = height;
            d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
            d.Format = format;
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> resource;
            Check(gpu.d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                 IID_PPV_ARGS(&resource)));
            return resource;
        };
        const auto readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
        auto gameColour = ordinary(tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, readState);
        auto gameMotion = ordinary(tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, readState);
        auto gameExposure = ordinary(1, 1, DXGI_FORMAT_R8_UNORM, readState);
        gpu.Upload(gameColour.Get(), pixels.data(), tw * 8, readState);
        gpu.Upload(gameMotion.Get(), vectors.data(), tw * 8, readState);
        XeSSNr::ReadableCopy snapshot(gpu.d.Get());
        XeSSNr::InputAdapter scalarAdapter(gpu.d.Get(), w, h);
        auto frozenExposure = XeSSNr::InputAdapter::MakeExposure(gpu.d.Get());
        for (unsigned char exposureByte : { 64, 128, 192 })
            for (bool apply : { false, true })
            {
                gpu.Upload(gameExposure.Get(), &exposureByte, 1, readState);
                auto out = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), w, h);
                snapshot.Record(gpu.l.Get(), 0, gameColour.Get(), 3, 5, w, h, out.Get());
                snapshot.Record(gpu.l.Get(), 1, gameExposure.Get(), 0, 0, 1, 1, frozenExposure.Get());
                gpu.Submit();
                Require(gpu.Read(out.Get(), D3D12_RESOURCE_STATE_COMMON) == expected,
                        "ordinary colour snapshot changed bytes");
                const auto exposureBytes = gpu.Read(frozenExposure.Get(), D3D12_RESOURCE_STATE_COMMON);
                float e = 0;
                memcpy(&e, exposureBytes.data(), sizeof(e));
                Require(std::abs(e - float(exposureByte) / 255.f) < 1e-7f, "R8 exposure normalization mismatch");
                DlssNrConstants c {};
                c.WhitePoint = .8f;
                c.ExposurePreMul = .8f;
                c.TransferStrength = c.ColourStrength = 1;
                c.MaxRatio = 2;
                c.CompareZoom = 1;
                const auto t = XeSSNr::MotionToRenderPixels(w, h, { 2, -2 }, false, false, { 0, 0 }, { 0, 0 });
                adapter.Encode(gpu.l.Get(), out.Get(), gameMotion.Get(), 7, 2, t, c, frozenExposure.Get());
                gpu.Submit();
                const auto proxy = gpu.Read(adapter.Proxy(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                // Independent scalar exposure is equivalent to (multiplier*texture)^-1.
                auto scalarOut = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), w, h);
                snapshot.Record(gpu.l.Get(), 0, gameColour.Get(), 3, 5, w, h, scalarOut.Get());
                gpu.Submit();
                auto scalar = c;
                scalar.WhitePoint = .8f / e;
                scalar.ExposurePreMul = 1;
                scalarAdapter.Encode(gpu.l.Get(), scalarOut.Get(), gameMotion.Get(), 7, 2, t, scalar);
                gpu.Submit();
                const auto reference = gpu.Read(scalarAdapter.Proxy(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                for (size_t i = 0; i < proxy.size() / 2; ++i)
                {
                    HALF a, b;
                    memcpy(&a, proxy.data() + i * 2, 2);
                    memcpy(&b, reference.data() + i * 2, 2);
                    Require(std::abs(XMConvertHalfToFloat(a) - XMConvertHalfToFloat(b)) < .002f,
                            "GPU texture/scalar HDR proxy differs");
                }
                // Rewriting the game's texture during inference cannot change resolve.
                unsigned char nextExposure = 32;
                gpu.Upload(gameExposure.Get(), &nextExposure, 1, readState);
                for (auto* a : { &adapter, &scalarAdapter })
                {
                    Barrier(gpu.l.Get(), a->Proxy(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(gpu.l.Get(), a->Neural(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    gpu.l->CopyResource(a->Neural(), a->Proxy());
                    Barrier(gpu.l.Get(), a->Proxy(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    Barrier(gpu.l.Get(), a->Neural(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                gpu.Submit();
                adapter.Resolve(gpu.l.Get(), out.Get(), apply, frozenExposure.Get());
                scalarAdapter.Resolve(gpu.l.Get(), scalarOut.Get(), apply);
                gpu.Submit();
                const auto actual = gpu.Read(out.Get(), D3D12_RESOURCE_STATE_COMMON);
                const auto scalarResult = gpu.Read(scalarOut.Get(), D3D12_RESOURCE_STATE_COMMON);
                if (!apply)
                    Require(actual == expected, "texture exposure hidden edit changed colour/alpha");
                for (size_t i = 0; i < actual.size() / 2; ++i)
                {
                    HALF a, b, src;
                    memcpy(&a, actual.data() + i * 2, 2);
                    memcpy(&b, scalarResult.data() + i * 2, 2);
                    memcpy(&src, expected.data() + i * 2, 2);
                    Require(std::isfinite(XMConvertHalfToFloat(a)), "texture exposure produced nonfinite colour");
                    if (i % 4 == 3)
                        Require(a == src, "texture exposure changed alpha");
                    else
                        Require(std::abs(XMConvertHalfToFloat(a) - XMConvertHalfToFloat(b)) <
                                    .003f * std::max(1.f, XMConvertHalfToFloat(b)),
                                "texture exposure resolve mismatch");
                }
            }
        const auto sourcePixels = gpu.Read(gameColour.Get(), readState);
        Require(sourcePixels.size() == pixels.size() * 2 &&
                    memcmp(sourcePixels.data(), pixels.data(), sourcePixels.size()) == 0,
                "game colour source modified");
        auto promoted = ordinary(tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
        gpu.Upload(promoted.Get(), pixels.data(), tw * 8, D3D12_RESOURCE_STATE_COMMON);
        auto promotedCopy = XeSSNr::InputAdapter::MakeOutput(gpu.d.Get(), w, h);
        snapshot.Record(gpu.l.Get(), 0, promoted.Get(), 3, 5, w, h, promotedCopy.Get());
        gpu.Submit();
        Require(gpu.Read(promotedCopy.Get(), D3D12_RESOURCE_STATE_COMMON) == expected,
                "implicit COMMON promotion snapshot mismatch");
        std::cout << "PASS: ordinary combined-read/implicit-promoted sources, R8 GPU exposure snapshot, scalar "
                     "equivalence, stable resolve after source rewrite\n";

        ComPtr<ID3D12InfoQueue> debug;
        Check(gpu.d.As(&debug));
        for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i)
        {
            SIZE_T n = 0;
            debug->GetMessage(i, nullptr, &n);
            std::vector<char> bytes(n);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            debug->GetMessage(i, m, &n);
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                std::cerr << m->pDescription << '\n';
                throw std::runtime_error("D3D12 validation failed");
            }
        }
        std::cout << "PASS: D3D12 HDR adapter, nonzero bases, exact off/copy, neutral HDR, alpha, pixel/NDC motion, "
                     "jitter signs, repeated frames; debug clean\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
