// SPDX-License-Identifier: GPL-3.0-or-later
// A fake syntheon-gpu for tst_gpu_client (through GpuClient::setWorkerForTests):
// it speaks the worker's JSON lines, or a planted defect. No Qt, no GPU.
//   fake-gpuworker <mode> <the worker's usual arguments>
//     ok                 ready, then (plan) a result per item, done; (serve) a result and idle per `run`
//     junk               a line that isn't JSON, then waits
//     hang               ready, then ignores everything but EOF
//     crash              ready, then an access violation
//     crash-after-ready  (serve) ready, then an access violation at the first `run`
//     na                 an error event with na true, exit 5 (--probe still answers)
//     protocol-error     ready, then an error event "protocol: unknown command 'x'" and exit 2
//     mismatch           like ok, but the opengl worker is faster (200 fps) and draws something else (its signature differs)
//     mismatch-fill      like ok, but the opengl worker draws the fill workloads (quads, sprites) differently (200 fps, another signature)
//     crash-at-quads     like ok, but the opengl worker crashes at `run quads`
//     crash-at-shading   like ok, but the opengl worker crashes at `run shading` (the second API of the GPU test's case)
//     crash-all-at-shading  like ok, but every 3D API's worker crashes at `run shading` (no API reaches the fills)
//     crash-all-at-geometry  like ok, but every 3D API's worker crashes at `run geometry` (nothing is measured, only `ready` is known)
//     vk-fails-gl-on-screen  like ok, but vulkan crashes at `run geometry` and opengl's results carry the on-screen window's real plan
//     crash-d2d / crash-qt   like ok, but the native path with that --api crashes right after ready
//     d2d-fast           like ok, but the d2d worker's native results are four times faster (more frames)
//     exit-after-finish  like ok, but the native paths (d2d, qt) exit with code 1 right after their last result
//     warn-d2d           like ok, but the d2d worker sends a `warning` event after ready
//     few-frames         like ok, but the opengl worker measures only 19 frames in `shading` (§9: too few)
//     implausible        like ok, but the opengl worker's `drawcalls` has a 0.004 ms frame (§9: implausible frame rate)
//     fast-fps           like ok, but the opengl worker's `sprites` runs at 150,000 fps (§9: implausible frame rate)
//     implausible-qt / implausible-d2d   like ok, but that native path's `text` has a 0.004 ms frame (§9 on native 2D)
//     few-native         like ok, but the native paths (d2d, qt) measure only 5 frames per workload (no floor on native 2D)
//     bad-gpu-time       like ok, with GPU times: vulkan's median is 16 ms against a 10 ms p50 (§9: dropped), opengl's 9 ms
// Every result is 20 frames in 0.2 s (100 fps, p50 10 ms, fastest frame 9 ms) unless a mode says otherwise:
// §9 needs at least 20 frames.
// --probe answers one fake GPU (Vulkan and OpenGL OK, DirectX Windows only) and done, in every mode.
// SB_FAKE_LOG=<file> (through SYNTHEON_GPU_WORKER_ENV): "<api> <workload>" appended per `run` command.
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

std::string g_api;  // the worker's --api, for the modes that differ by API
bool g_mismatch = false, g_mismatchFill = false;

bool g_d2dFast = false;
std::string g_mode;

void result(const std::string &workload)
{
    const bool fill = workload == "quads" || workload == "sprites";
    const bool gl = g_api == "opengl";
    const bool other = (g_mismatch || (g_mismatchFill && fill)) && gl;
    int frames = g_d2dFast && g_api == "d2d" ? 80 : (g_mismatchFill && fill && other ? 40 : 20);  // the other opengl is faster
    std::string seconds = "0.2", fps = other ? "200" : "100", min = "9", gpu;
    if (g_mode == "few-frames" && gl && workload == "shading") {
        frames = 19;
        seconds = "0.19";
    } else if (g_mode == "implausible" && gl && workload == "drawcalls") {
        min = "0.004";
    } else if (g_mode == "fast-fps" && gl && workload == "sprites") {
        frames = 30000;
        fps = "150000";
        min = "0.006";
    } else if ((g_mode == "implausible-qt" && g_api == "qt" && workload == "text") || (g_mode == "implausible-d2d" && g_api == "d2d" && workload == "text")) {
        min = "0.004";
    } else if (g_mode == "few-native" && (g_api == "qt" || g_api == "d2d")) {
        frames = 5;
        seconds = "0.05";
    } else if (g_mode == "bad-gpu-time") {
        gpu = gl ? ",\"gpuMs\":9" : ",\"gpuMs\":16";
    }
    const std::string sig(864, other ? 'f' : '0');
    // "hybrid" (M2): the window landed on an output of another GPU than the one rendering.
    const std::string shownBy = g_mode == "hybrid" ? ",\"displayGpu\":\"Fake iGPU\"" : "";
    // "on-screen": where the window really was (§10's `screen`, with the screen's physical size).
    const std::string screen = (g_mode == "on-screen" || (g_mode == "vk-fails-gl-on-screen" && gl)) ? ",\"screen\":\"eDP-1\",\"renderW\":2400,\"renderH\":1350,\"windowW\":1294,\"windowH\":728,"
                                                       "\"scale\":1.25,\"fit\":true,\"screenW\":2560,\"screenH\":1600"
                                                     : "";
    out("{\"ev\":\"result\",\"workload\":\"" + workload + "\",\"frames\":" + std::to_string(frames) + ",\"seconds\":" + seconds + ",\"fps\":" + fps + ","
        "\"ftMs\":{\"p1\":9,\"p50\":10,\"p99\":11,\"min\":" + min + "},\"cpuMs\":1" + gpu + ",\"series\":[[0.05,10.0],[0.15,10.0]],\"sig\":\"" + sig + "\"" + shownBy + screen + "}");
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

int fakeMain(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "ok";
    bool serve = false, probe = false;
    std::string plan, api;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--serve"))
            serve = true;
        else if (!std::strcmp(argv[i], "--probe"))
            probe = true;
        else if (!std::strcmp(argv[i], "--plan") && i + 1 < argc)
            plan = argv[++i];
        else if (!std::strcmp(argv[i], "--api") && i + 1 < argc && api.empty())
            api = argv[++i];  // the first one: the GPU test's own, not a dev hook's override
    }
    g_api = api;
    g_mismatch = mode == "mismatch";
    g_mismatchFill = mode == "mismatch-fill";
    g_d2dFast = mode == "d2d-fast";
    g_mode = mode;
    if (probe) {
        out("{\"ev\":\"gpu\",\"id\":\"10de:2c05@0000:01:00.0\",\"name\":\"Fake GPU\",\"vendorId\":4318,\"deviceId\":11269,"
            "\"kind\":\"discrete\",\"vramBytes\":17179869184,\"pciSlot\":\"0000:01:00.0\",\"apis\":{"
            "\"vulkan\":{\"ok\":true,\"version\":\"Vulkan 1.4.351\",\"driver\":\"fake 1\",\"note\":\"\"},"
            "\"opengl\":{\"ok\":true,\"version\":\"OpenGL 4.6\",\"driver\":\"fake 1\",\"note\":\"\"},"
            "\"dx12\":{\"ok\":false,\"reason\":\"DirectX is Windows only\"},"
            "\"dx11\":{\"ok\":false,\"reason\":\"DirectX is Windows only\"}}}");
        out("{\"ev\":\"done\"}");
        return 0;
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
    if (mode == "crash-start-gl" && api == "opengl")
        crash();  // before "ready": the API fails during start-up (AP-5 L3)
    ready();
    if ((mode == "crash-d2d" && api == "d2d") || (mode == "crash-qt" && api == "qt"))
        crash();
    if (mode == "warn-d2d" && api == "d2d")
        out("{\"ev\":\"warning\",\"text\":\"the bundled font IBM Plex Sans isn't in the worker; the text workload uses Segoe UI instead of IBM Plex Sans\"}");
    if (mode == "protocol-error") {
        out("{\"ev\":\"error\",\"workload\":\"\",\"reason\":\"protocol: unknown command 'x'\",\"na\":false}");
        return 2;
    }
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
        if (mode == "exit-after-finish" && (api == "d2d" || api == "qt")) {
            out("{\"ev\":\"error\",\"workload\":\"\",\"reason\":\"the worker failed while closing\",\"na\":false}");
            return 1;
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
            const std::string item = command.substr(4);
            if (const char *log = std::getenv("SB_FAKE_LOG")) {
                if (std::FILE *f = std::fopen(log, "ab")) {  // binary: no CRLF on Windows
                    std::fprintf(f, "%s %s\n", api.c_str(), item.substr(0, item.find(':')).c_str());
                    std::fclose(f);
                }
            }
            if (mode == "crash-at-quads" && api == "opengl" && item.rfind("quads", 0) == 0)
                crash();
            if (mode == "crash-after-ready" || (mode == "crash-at-shading" && api == "opengl" && item.rfind("shading", 0) == 0)
                || (mode == "crash-all-at-shading" && item.rfind("shading", 0) == 0)
                || (mode == "crash-all-at-geometry" && item.rfind("geometry", 0) == 0)
                || (mode == "vk-fails-gl-on-screen" && api == "vulkan" && item.rfind("geometry", 0) == 0))
                crash();
            if (!sleepMs(millis(item), true))
                return 4;
            result(item.substr(0, item.find(':')));
            out("{\"ev\":\"idle\"}");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Leaves with _Exit: the detached stdin thread blocks in std::getline, and returning from main
// with it running can hang on the exit-time locks of the standard streams.
int main(int argc, char **argv)
{
    const int code = fakeMain(argc, argv);
    std::fflush(stdout);
    std::_Exit(code);
}
