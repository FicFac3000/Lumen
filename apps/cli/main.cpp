// lumen-cli — the same engine, driven from the terminal.
//
// For working on FUNCTIONALITY rather than interface: every command the UI can
// send is typeable here, because the command layer is the only way into the
// model either way (§16.2). It is also the fastest way to reproduce something
// and paste it into a bug report.
#include "lumen/Command.hpp"
#include "lumen/Show.hpp"
#include "lumen/Simulated.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace lumen;

namespace {

void printResult(const Result& r) {
    if (!r.parsed) {
        std::printf("  ?  %s\n", r.message.c_str());
        return;
    }
    std::printf("  %s %s\n", r.ok ? "ok" : "!!", r.message.c_str());
    for (const std::string& row : r.rows) std::printf("     %s\n", row.c_str());
}

void banner(const Engine& e) {
    std::printf(
        "\n  Lumen — headless console. Simulated output, no hardware needed.\n"
        "  %zu fixtures patched. `Help` lists commands, `.quit` exits.\n"
        "  Lines starting with `.` are shell commands; everything else is a\n"
        "  console command and goes through the same path the UI uses.\n\n",
        e.patch.size());
}

} // namespace

int main(int argc, char** argv) {
    Engine engine;
    Console console(engine);
    auto output = std::make_unique<SimulatedOutput>();
    SimulatedOutput* dmx = output.get();
    engine.addOutput(std::move(output));

    std::string script;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--show" && i + 1 < argc) {
            const LoadReport r = ShowFile::load(engine, argv[++i]);
            std::printf("opened: ok=%d %s\n", int(r.ok),
                        r.ok ? "" : r.error.c_str());
        } else if (a == "--demo") {
            installStarterTypes(engine.patch);
            console.executeAll(
                "Fixture 1 Thru 8 Patch \"LED Par RGBW\" 8 1 1;"
                "Fixture 101 Thru 104 Patch \"Moving Spot\" 4 2 1");
        } else if (a == "-c" && i + 1 < argc) {
            script = argv[++i];
        } else if (a == "--help") {
            std::printf("lumen-cli [--demo] [--show file.lumen] [-c \"commands\"]\n");
            return 0;
        }
    }

    // One-shot mode, for scripting and for tests.
    if (!script.empty()) {
        const Result r = console.executeAll(script);
        engine.tick();
        printResult(r);
        return r.ok ? 0 : 1;
    }

    banner(engine);
    std::string line;
    while (true) {
        std::printf("lumen> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line == ".quit" || line == ".q" || line == ".exit") break;
        if (line == ".tick") {
            engine.tick();
            console.pumpBindings();
            std::printf("  ok tick; %llu DMX frames sent\n",
                        static_cast<unsigned long long>(dmx->totalFrames()));
            continue;
        }
        if (line == ".dmx") {
            engine.tick();
            for (const SimulatedOutput::Frame& f : dmx->frames()) {
                const int top = std::max(1, dmx->highestChannel(f.universe));
                std::printf("  universe %d (%d channels in use):\n", f.universe, top);
                for (int i = 0; i < top; ++i) {
                    if (i % 16 == 0) std::printf("   %3d:", i + 1);
                    std::printf(" %3d", f.data[static_cast<size_t>(i)]);
                    if (i % 16 == 15 || i == top - 1) std::printf("\n");
                }
            }
            continue;
        }
        if (line == ".help") {
            std::printf(
                "  .tick   advance the engine one pass and send DMX\n"
                "  .dmx    show the simulated DMX output\n"
                "  .quit   exit\n"
                "  anything else is a console command — try `Help`\n");
            continue;
        }

        printResult(console.executeAll(line));
        engine.tick();
        console.pumpBindings();
    }
    std::printf("\n");
    return 0;
}
