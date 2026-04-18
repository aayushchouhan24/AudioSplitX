#include "AudioEngine.h"
#include "Common.h"
#include "DeviceEnumerator.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop { false };

BOOL WINAPI consoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

struct CliOptions {
    bool list = false;
    bool preferExclusive = true;
    bool consoleMeter = true;
    double bufferMs = 10.0;
    double ringMs = 750.0;
    int runSeconds = 0;
    std::wstring sourceMatch;
    std::wstring sourceId;
    std::vector<std::wstring> outputMatches;
    std::vector<std::wstring> outputIds;
    std::string csvPath;
};

void printUsage()
{
    std::cout
        << "AudioSplitX MVP - virtual-cable loopback to multiple WASAPI render endpoints\n\n"
        << "Usage:\n"
        << "  audiosplitx --list\n"
        << "  audiosplitx --source \"CABLE Input\" --out \"Speakers\" --out \"Headphones\"\n\n"
        << "Options:\n"
        << "  --list                  List active playback endpoints.\n"
        << "  --source <name>         Source render endpoint to loopback-capture, usually a virtual cable input.\n"
        << "  --source-id <id>        Exact source endpoint id from --list.\n"
        << "  --out <name>            Output endpoint friendly-name substring. Repeat for 2+ devices.\n"
        << "  --out-id <id>           Exact output endpoint id from --list. Repeat for 2+ devices.\n"
        << "  --shared                Do not attempt exclusive mode on output endpoints.\n"
        << "  --buffer-ms <ms>        WASAPI endpoint buffer target. Default: 10.\n"
        << "  --ring-ms <ms>          Internal per-edge ring capacity. Default: 750.\n"
        << "  --duration <seconds>    Stop automatically after N seconds. Default: run until Ctrl+C.\n"
        << "  --debug-csv <path>      Write latency/fill/ppm telemetry CSV.\n"
        << "  --quiet                 Disable console telemetry meter.\n";
}

std::wstring nextWideArg(int& i, int argc, char** argv, const char* option)
{
    if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + option);
    }
    ++i;
    return asx::widen(argv[i]);
}

std::string nextArg(int& i, int argc, char** argv, const char* option)
{
    if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + option);
    }
    ++i;
    return argv[i];
}

CliOptions parseArgs(int argc, char** argv)
{
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            options.list = true;
        } else if (arg == "--source") {
            options.sourceMatch = nextWideArg(i, argc, argv, "--source");
        } else if (arg == "--source-id") {
            options.sourceId = nextWideArg(i, argc, argv, "--source-id");
        } else if (arg == "--out") {
            options.outputMatches.push_back(nextWideArg(i, argc, argv, "--out"));
        } else if (arg == "--out-id") {
            options.outputIds.push_back(nextWideArg(i, argc, argv, "--out-id"));
        } else if (arg == "--shared") {
            options.preferExclusive = false;
        } else if (arg == "--buffer-ms") {
            options.bufferMs = std::stod(nextArg(i, argc, argv, "--buffer-ms"));
        } else if (arg == "--ring-ms") {
            options.ringMs = std::stod(nextArg(i, argc, argv, "--ring-ms"));
        } else if (arg == "--duration") {
            options.runSeconds = std::stoi(nextArg(i, argc, argv, "--duration"));
        } else if (arg == "--debug-csv") {
            options.csvPath = nextArg(i, argc, argv, "--debug-csv");
        } else if (arg == "--quiet") {
            options.consoleMeter = false;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return options;
}

void listDevices(const asx::DeviceEnumerator& enumerator)
{
    const auto endpoints = enumerator.listRenderEndpoints();
    std::cout << "Active playback endpoints:\n";
    for (size_t i = 0; i < endpoints.size(); ++i) {
        const auto& endpoint = endpoints[i];
        std::cout << "\n[" << i << "] " << asx::narrow(endpoint.name);
        if (endpoint.isDefault) {
            std::cout << " (default)";
        }
        std::cout << "\n    id: " << asx::narrow(endpoint.id) << "\n";

        try {
            const auto format = enumerator.getMixFormat(endpoint.id);
            std::cout << "    mix: " << asx::describeFormat(format) << "\n";
        } catch (const std::exception& ex) {
            std::cout << "    mix: unavailable (" << ex.what() << ")\n";
        }
    }
}

void listDeviceChoices(const asx::DeviceEnumerator& enumerator, const std::vector<asx::AudioEndpoint>& endpoints)
{
    for (size_t i = 0; i < endpoints.size(); ++i) {
        const auto& endpoint = endpoints[i];
        std::cout << "[" << i << "] " << asx::narrow(endpoint.name);
        if (endpoint.isDefault) {
            std::cout << " (default)";
        }

        try {
            const auto format = enumerator.getMixFormat(endpoint.id);
            std::cout << " - " << asx::describeFormat(format);
        } catch (...) {
            std::cout << " - format unavailable";
        }
        std::cout << "\n";
    }
}

size_t promptEndpointIndex(const char* prompt, size_t maxExclusive)
{
    for (;;) {
        std::cout << prompt;
        std::string line;
        if (!std::getline(std::cin, line)) {
            throw std::runtime_error("Input closed");
        }

        try {
            const size_t value = static_cast<size_t>(std::stoull(line));
            if (value < maxExclusive) {
                return value;
            }
        } catch (...) {
        }

        std::cout << "Invalid index. Try again.\n";
    }
}

std::vector<size_t> promptOutputIndexes(const char* prompt, size_t maxExclusive, size_t sourceIndex)
{
    for (;;) {
        std::cout << prompt;
        std::string line;
        if (!std::getline(std::cin, line)) {
            throw std::runtime_error("Input closed");
        }

        std::vector<size_t> out;
        std::stringstream ss(line);
        std::string token;
        bool valid = true;

        while (std::getline(ss, token, ',')) {
            try {
                const size_t value = static_cast<size_t>(std::stoull(token));
                if (value >= maxExclusive || value == sourceIndex) {
                    valid = false;
                    break;
                }
                if (std::find(out.begin(), out.end(), value) == out.end()) {
                    out.push_back(value);
                }
            } catch (...) {
                valid = false;
                break;
            }
        }

        if (valid && !out.empty()) {
            return out;
        }

        std::cout << "Enter one or more output indexes separated by commas. Do not include the source index.\n";
    }
}

bool promptYesNo(const char* prompt, bool defaultValue)
{
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return defaultValue;
    }

    if (line.empty()) {
        return defaultValue;
    }

    const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(line.front())));
    if (ch == 'y') {
        return true;
    }
    if (ch == 'n') {
        return false;
    }
    return defaultValue;
}

int runInteractiveLauncher(const asx::DeviceEnumerator& enumerator)
{
    const auto endpoints = enumerator.listRenderEndpoints();
    if (endpoints.size() < 2) {
        std::cout << "Need at least two active playback endpoints: one virtual/source endpoint and one output endpoint.\n";
        std::cout << "Press Enter to exit.";
        std::string ignored;
        std::getline(std::cin, ignored);
        return 1;
    }

    std::cout << "AudioSplitX interactive launcher\n\n";
    std::cout << "Select the source endpoint first. For Spotify routing, this should usually be the virtual cable playback endpoint.\n\n";
    listDeviceChoices(enumerator, endpoints);
    std::cout << "\n";

    const size_t sourceIndex = promptEndpointIndex("Source index: ", endpoints.size());
    const auto outputIndexes = promptOutputIndexes("Output index(es), comma separated: ", endpoints.size(), sourceIndex);
    const bool preferExclusive = promptYesNo("Prefer exclusive mode? [Y/n]: ", true);

    std::vector<asx::AudioEndpoint> outputs;
    outputs.reserve(outputIndexes.size());
    for (const size_t index : outputIndexes) {
        outputs.push_back(endpoints[index]);
    }

    asx::AudioEngineConfig config;
    config.source = endpoints[sourceIndex];
    config.outputs = outputs;
    config.preferExclusive = preferExclusive;
    config.consoleMeter = true;

    std::cout << "\nStarting route. Press Enter to stop.\n";

    asx::AudioEngine engine(std::move(config));
    engine.start();

    std::string ignored;
    std::getline(std::cin, ignored);
    engine.stop();
    return 0;
}

asx::AudioEndpoint resolveSource(const CliOptions& options, const asx::DeviceEnumerator& enumerator)
{
    if (!options.sourceId.empty()) {
        return enumerator.resolveRenderEndpoint(options.sourceId);
    }
    if (!options.sourceMatch.empty()) {
        return enumerator.resolveRenderEndpoint(options.sourceMatch);
    }
    throw std::runtime_error("A source endpoint is required. Use --source \"CABLE Input\" or --source-id <id>.");
}

std::vector<asx::AudioEndpoint> resolveOutputs(const CliOptions& options, const asx::DeviceEnumerator& enumerator)
{
    std::vector<asx::AudioEndpoint> outputs;
    for (const auto& id : options.outputIds) {
        outputs.push_back(enumerator.resolveRenderEndpoint(id));
    }
    for (const auto& match : options.outputMatches) {
        outputs.push_back(enumerator.resolveRenderEndpoint(match));
    }

    if (outputs.empty()) {
        throw std::runtime_error("At least one output endpoint is required. Use --out or --out-id.");
    }

    return outputs;
}

void validateEndpoints(const asx::AudioEndpoint& source, const std::vector<asx::AudioEndpoint>& outputs)
{
    for (const auto& output : outputs) {
        if (output.id == source.id) {
            throw std::runtime_error("Source endpoint must not also be an output endpoint: " + asx::narrow(output.name));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        asx::ComApartment com;
        SetConsoleCtrlHandler(consoleHandler, TRUE);

        const CliOptions options = parseArgs(argc, argv);
        asx::DeviceEnumerator enumerator;

        if (argc == 1) {
            return runInteractiveLauncher(enumerator);
        }

        if (options.list) {
            printUsage();
            std::cout << "\n";
            listDevices(enumerator);
            return 0;
        }

        const asx::AudioEndpoint source = resolveSource(options, enumerator);
        const std::vector<asx::AudioEndpoint> outputs = resolveOutputs(options, enumerator);
        validateEndpoints(source, outputs);

        const auto sourceFormat = enumerator.getMixFormat(source.id);
        std::cout << "Source: " << asx::narrow(source.name) << "\n";
        std::cout << "Source mix: " << asx::describeFormat(sourceFormat) << "\n";
        for (const auto& output : outputs) {
            std::cout << "Output: " << asx::narrow(output.name) << "\n";
        }

        asx::AudioEngineConfig config;
        config.source = source;
        config.outputs = outputs;
        config.preferExclusive = options.preferExclusive;
        config.captureRingMs = options.ringMs;
        config.outputRingMs = options.ringMs;
        config.endpointBufferMs = options.bufferMs;
        config.consoleMeter = options.consoleMeter;
        config.debugCsvPath = options.csvPath;

        asx::AudioEngine engine(std::move(config));
        engine.start();

        const auto started = std::chrono::steady_clock::now();
        while (!g_stop.load(std::memory_order_acquire) && engine.running()) {
            if (options.runSeconds > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
                if (elapsed.count() >= options.runSeconds) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        engine.stop();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << "\n";
        return 1;
    }
}
