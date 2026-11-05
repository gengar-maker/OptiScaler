#include <Windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "../../OptiScaler/dlssnr/sycl/ArcNrSyclLoader.h"
using Clock = std::chrono::steady_clock;
using State = ArcNrSyclLoader::Preparation;
void Require(bool pass, const char* reason) { if (!pass) throw std::runtime_error(reason); }
template<class Predicate> void Until(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        Require(Clock::now() < deadline, "test deadline exceeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "supply fake_provider.dll");
        HMODULE module = LoadLibraryA(argv[1]);
        Require(module != nullptr, "load test provider");
        const auto release = reinterpret_cast<void(*)()>(GetProcAddress(module, "release_build"));
        const auto creates = reinterpret_cast<int(*)()>(GetProcAddress(module, "create_count"));
        const auto destroys = reinterpret_cast<int(*)()>(GetProcAddress(module, "destroy_count"));
        ArcNrSyclLoader loader;
        std::string error;
        Require(loader.Load(argv[1], error), error.c_str());
        ArcNrSyclCreate args{sizeof(args), 1280, 720, "logical", "derived"};
        auto poll = [&] {
            const auto start = Clock::now();
            const auto state = loader.PrepareAsync(args, error);
            Require(Clock::now() - start < std::chrono::milliseconds(100), "render-thread poll blocked");
            return state;
        };
        Require(poll() == State::Pending, "first prepare must be asynchronous");
        Until([&] { return creates() == 1; });
        for (int i = 0; i < 100; ++i) Require(poll() == State::Pending, "gated worker must stay pending");
        args.width = 1360; args.height = 768;
        Require(poll() == State::Pending, "resize must not wait on old build");
        release();
        Until([&] { poll(); return creates() == 2; });
        Require(destroys() == 1, "obsolete model must be destroyed");
        args.width = 1366;
        Require(poll() == State::Pending, "second resize must not block");
        release();
        Until([&] { poll(); return creates() == 3; });
        release();
        Until([&] { return poll() == State::Ready; });
        Require(loader.Generation() == 1 && loader.Ready(), "only latest result is adopted");
        args.width = 13;
        Require(poll() == State::Pending, "failure preparation begins asynchronously");
        Until([&] { return creates() == 4; });
        release();
        Until([&] { return poll() == State::Failed; });
        for (int i = 0; i < 100; ++i) Require(poll() == State::Failed, "failure must be cached");
        Require(creates() == 4 && error == "unsupported test size", "no repeated failed compilation");
        loader.RetryPreparation();
        Require(poll() == State::Pending, "explicit retry should restart build");
        Until([&] { return creates() == 5; }); release();
        Until([&] { return poll() == State::Failed; });
        args.width = 1280; args.height = 720;
        Require(poll() == State::Pending, "return to supported size after failure");
        Until([&] { return creates() == 6; }); release();
        Until([&] { return poll() == State::Ready; });
        Require(loader.Generation() == 2, "new model resets generation/history");
        loader.Reset();
        Require(destroys() == 4, "all successful model instances are released exactly once");
        Require(loader.Load(argv[1], error), "reload for shutdown test");
        Require(poll() == State::Pending, "shutdown test must start a worker");
        Until([&] { return creates() == 7; });
        const auto shutdown_start = Clock::now();
        ArcNrSyclLoader::OnProcessShutdown();
        loader.Reset();
        Require(Clock::now() - shutdown_start < std::chrono::milliseconds(100),
                "process shutdown must not join a blocked worker under loader lock");
        release();
        FreeLibrary(module);
        std::cout << "PASS: nonblocking preparation, rapid resize, failure, retry, recovery and lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
