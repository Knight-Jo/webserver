#include "AsyncLogging.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Options {
    int threads = 8;
    int durationSec = 20;
    int bytes = 512;
    int burst = 2000;
    int pauseUs = 2000;
    int flush = 1;
    long roll = 1L << 26;
    std::string basename = "logs/stress";
};

bool parseInt(const std::string &s, int &out) {
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool parseLong(const std::string &s, long &out) {
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

Options parseArgs(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        auto takeValue = [&](std::string &val) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            val = argv[++i];
            return true;
        };

        std::string key;
        std::string value;
        auto eq = arg.find('=');
        if (eq != std::string::npos) {
            key = arg.substr(0, eq);
            value = arg.substr(eq + 1);
        } else {
            key = arg;
        }

        if (key == "--threads") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.threads);
        } else if (key == "--duration") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.durationSec);
        } else if (key == "--bytes") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.bytes);
        } else if (key == "--burst") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.burst);
        } else if (key == "--pause-us") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.pauseUs);
        } else if (key == "--flush") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.flush);
        } else if (key == "--roll") {
            if (value.empty() && !takeValue(value)) continue;
            parseLong(value, opt.roll);
        } else if (key == "--basename") {
            if (value.empty() && !takeValue(value)) continue;
            opt.basename = value;
        }
    }
    if (opt.threads < 1) opt.threads = 1;
    if (opt.durationSec < 1) opt.durationSec = 1;
    if (opt.bytes < 32) opt.bytes = 32;
    if (opt.burst < 1) opt.burst = 1;
    return opt;
}

struct Barrier {
    explicit Barrier(int count) : total(count), waiting(0) {}
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        ++waiting;
        if (waiting == total) {
            ready = true;
            cv.notify_all();
            return;
        }
        cv.wait(lock, [&] { return ready; });
    }

    int total;
    int waiting;
    bool ready = false;
    std::mutex mutex;
    std::condition_variable cv;
};
}

int main(int argc, char **argv) {
    Options opt = parseArgs(argc, argv);

    AsyncLogging logger(opt.basename, opt.roll, opt.flush);
    logger.start();

    std::string payload(opt.bytes, 'Z');
    payload.back() = '\n';

    Barrier barrier(opt.threads + 1);
    std::atomic<long long> totalBytes(0);
    std::atomic<long long> totalMessages(0);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(opt.threads));

    auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(opt.durationSec);

    for (int t = 0; t < opt.threads; ++t) {
        workers.emplace_back([&] {
            barrier.wait();
            while (std::chrono::steady_clock::now() < endTime) {
                for (int i = 0; i < opt.burst; ++i) {
                    logger.append(payload.data(), static_cast<int>(payload.size()));
                }
                totalMessages.fetch_add(opt.burst, std::memory_order_relaxed);
                totalBytes.fetch_add(static_cast<long long>(opt.burst) * payload.size(), std::memory_order_relaxed);
                if (opt.pauseUs > 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(opt.pauseUs));
                }
            }
        });
    }

    barrier.wait();
    auto begin = std::chrono::steady_clock::now();
    for (auto &worker : workers) {
        worker.join();
    }
    auto end = std::chrono::steady_clock::now();

    logger.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    long long bytes = totalBytes.load();
    long long messages = totalMessages.load();
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    double mbps = mb / seconds;

    std::cout << "threads=" << opt.threads
              << " duration_s=" << seconds
              << " messages=" << messages
              << " payload_bytes=" << opt.bytes
              << " burst_messages=" << opt.burst
              << " pause_us=" << opt.pauseUs
              << " flush_s=" << opt.flush
              << " roll_bytes=" << opt.roll
              << " total_mib=" << mb
              << " throughput_mibps=" << mbps
              << std::endl;
    return 0;
}
