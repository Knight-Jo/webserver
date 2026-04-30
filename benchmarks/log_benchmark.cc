#include "AsyncLogging.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Options {
    int threads = 4;
    int messages = 200000;
    int bytes = 256;
    int flush = 1;
    long roll = 1L << 28;
    int sampleRate = 1000;
    std::string basename = "logs/bench";
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
        } else if (key == "--messages") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.messages);
        } else if (key == "--bytes") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.bytes);
        } else if (key == "--flush") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.flush);
        } else if (key == "--roll") {
            if (value.empty() && !takeValue(value)) continue;
            parseLong(value, opt.roll);
        } else if (key == "--sample-rate") {
            if (value.empty() && !takeValue(value)) continue;
            parseInt(value, opt.sampleRate);
        } else if (key == "--basename") {
            if (value.empty() && !takeValue(value)) continue;
            opt.basename = value;
        }
    }
    if (opt.threads < 1) opt.threads = 1;
    if (opt.messages < 1) opt.messages = 1;
    if (opt.bytes < 32) opt.bytes = 32;
    if (opt.sampleRate < 0) opt.sampleRate = 0;
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

    std::string payload(opt.bytes, 'X');
    payload.back() = '\n';

    Barrier barrier(opt.threads + 1);
    std::atomic<long long> totalBytes(0);
    std::atomic<long long> totalSamples(0);
    std::atomic<long long> totalSampleNs(0);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(opt.threads));

    for (int t = 0; t < opt.threads; ++t) {
        workers.emplace_back([&, t] {
            barrier.wait();
            long long localBytes = 0;
            long long localSamples = 0;
            long long localSampleNs = 0;
            for (int i = 0; i < opt.messages; ++i) {
                if (opt.sampleRate > 0 && (i % opt.sampleRate == 0)) {
                    auto start = std::chrono::steady_clock::now();
                    logger.append(payload.data(), static_cast<int>(payload.size()));
                    auto end = std::chrono::steady_clock::now();
                    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                    localSamples += 1;
                    localSampleNs += ns;
                } else {
                    logger.append(payload.data(), static_cast<int>(payload.size()));
                }
                localBytes += static_cast<long long>(payload.size());
            }
            totalBytes.fetch_add(localBytes, std::memory_order_relaxed);
            totalSamples.fetch_add(localSamples, std::memory_order_relaxed);
            totalSampleNs.fetch_add(localSampleNs, std::memory_order_relaxed);
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
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    double mbps = mb / seconds;
    long long samples = totalSamples.load();
    long long sampleNs = totalSampleNs.load();
    double avgAppendUs = samples > 0 ? (sampleNs / 1000.0) / static_cast<double>(samples) : 0.0;

    std::cout << "threads=" << opt.threads << "\n"
              << " messages=" << opt.messages << "\n"
              << " payload_bytes=" << opt.bytes << "\n"
              << " flush_s=" << opt.flush << "\n"
              << " roll_bytes=" << opt.roll << "\n"
              << " duration_s=" << seconds << "\n" // 测试总运行时间
              << " total_mib=" << mb << "\n"  // 写入总量
              << " throughput_mibps=" << mbps << "\n"   // 吞吐量
              << " sample_rate=" << opt.sampleRate << "\n"  //采样间隔
              << " avg_append_us=" << avgAppendUs << "\n" // 平均每次append耗时
              << std::endl;
    return 0;
}
