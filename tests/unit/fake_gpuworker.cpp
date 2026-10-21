// SPDX-License-Identifier: GPL-3.0-or-later
// A fake synbench-gpu for tst_gpu_client (through GpuClient::setWorkerForTests):
// it speaks the worker's JSON lines, or a planted defect. No Qt, no GPU.
//   fake-gpuworker <mode> <the worker's usual arguments>
//     ok                 ready, then (plan) a result per item, done; (serve) a result and idle per `run`
//     junk               a line that isn't JSON, then waits
//     hang               ready, then ignores everything but EOF
//     crash              ready, then an access violation
//     crash-after-ready  (serve) ready, then an access violation at the first `run`
//     na                 an error event with na true, exit 5
// "stop" on stdin ends it with exit 4 (not in `hang`), "quit" with exit 0, EOF with exit 3.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex g_mutex;
std::deque<std::string> g_commands;
std::atomic<bool> g_stop{false}, g_quit{false};

void out(const std::string &line)
{
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

void ready() { out("{\"ev\":\"ready\",\"api\":\"fake\",\"device\":\"Fake GPU\",\"renderW\":320,\"renderH\":180,\"windowW\":0,\"windowH\":0}"); }

void result(const std::string &workload)
{
    out("{\"ev\":\"result\",\"workload\":\"" + workload + "\",\"frames\":10,\"seconds\":0.1,\"fps\":100,"
        "\"ftMs\":{\"p1\":9,\"p50\":10,\"p99\":11},\"cpuMs\":1,\"sig\":\"\"}");
}

// Sleeps ms in small steps; false when "stop" arrived (the caller exits 4).
bool sleepMs(int ms, bool honourStop)
{
    for (int t = 0; t < ms; t += 10) {
        if (honourStop && g_stop)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

[[noreturn]] void crash()
{
    volatile int *nowhere = nullptr;
    *nowhere = 1;
    std::abort();
}

// "geometry:100:300" -> the sum of both times, in ms.
int millis(const std::string &item)
{
    const std::size_t a = item.find(':'), b = item.rfind(':');
    if (a == std::string::npos || a == b)
        return 0;
    return std::atoi(item.c_str() + a + 1) + std::atoi(item.c_str() + b + 1);
}

} // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "ok";
    bool serve = false;
    std::string plan;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--serve"))
            serve = true;
        else if (!std::strcmp(argv[i], "--plan") && i + 1 < argc)
            plan = argv[++i];
    }
    if (mode == "na") {
        out("{\"ev\":\"error\",\"workload\":\"\",\"reason\":\"No Vulkan driver is installed\",\"na\":true}");
        return 5;
    }
    std::thread([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "stop") {
                g_stop = true;
            } else if (line == "quit") {
                g_quit = true;
            } else {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_commands.push_back(line);
            }
        }
        std::_Exit(3);  // EOF: the lifeline
    }).detach();

    if (mode == "junk") {
        out("this is not json");
        sleepMs(60000, true);
        return g_stop ? 4 : 0;
    }
    ready();
    if (mode == "crash")
        crash();
    if (mode == "hang") {
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!serve) {
        // Plan mode: each item takes its warm-up + measure time.
        std::string rest = plan;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string item = rest.substr(0, comma);
            rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
            if (!sleepMs(millis(item), true))
                return 4;
            result(item.substr(0, item.find(':')));
        }
        out("{\"ev\":\"done\"}");
        return 0;
    }
    for (;;) {
        if (g_stop)
            return 4;
        if (g_quit) {
            out("{\"ev\":\"done\"}");
            return 0;
        }
        std::string command;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_commands.empty()) {
                command = g_commands.front();
                g_commands.pop_front();
            }
        }
        if (command.rfind("run ", 0) == 0) {
            if (mode == "crash-after-ready")
                crash();
            const std::string item = command.substr(4);
            if (!sleepMs(millis(item), true))
                return 4;
            result(item.substr(0, item.find(':')));
            out("{\"ev\":\"idle\"}");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
