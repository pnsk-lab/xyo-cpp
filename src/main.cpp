#include "sjit/compiler.hpp"
#include "sjit/host_app.hpp"
#include "sjit/jit.hpp"
#include "sjit/project_loader.hpp"

#include <cmath>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void printProjectUsage(const char *program) {
    std::cerr << "usage: " << program << " <project.sb3> [--frames N] [--fps N] [--turbo]"
              << " [--compatibility scratch|turbowarp] [--list-limit N] [--stage-scale N]\n";
    std::cerr << "       " << program << " --window <project.sb3> [--frames N] [--fps N] [--turbo]"
              << " [--compatibility scratch|turbowarp] [--list-limit N] [--stage-scale N]\n";
    std::cerr << "       " << program << " --emit-ll <output.ll>\n";
    std::cerr << "       " << program << " --emit-ll <project.sb3> <output.ll>\n";
    std::cerr << "       " << program << " --emit-runtime-ll <output.ll>\n";
}

bool parsePositiveDouble(const std::string &text, double *out) {
    try {
        size_t parsed = 0;
        const double value = std::stod(text, &parsed);
        if (parsed != text.size() || !std::isfinite(value) || value <= 0.0) {
            return false;
        }
        *out = value;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool parsePositiveInt(const std::string &text, int *out) {
    try {
        size_t parsed = 0;
        const int value = std::stoi(text, &parsed);
        if (parsed != text.size() || value <= 0) {
            return false;
        }
        *out = value;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool parseCompatibilityMode(const std::string &text, int *out) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const unsigned char character : text) {
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    if (normalized == "scratch" || normalized == "scratch-compatible") {
        *out = SJIT_COMPATIBILITY_MODE_SCRATCH;
        return true;
    }
    if (normalized == "turbowarp" || normalized == "turbo-warp" || normalized == "tw") {
        *out = SJIT_COMPATIBILITY_MODE_TURBOWARP;
        return true;
    }
    return false;
}

bool parseRunOptions(int argc, char **argv, int start, sjit::ProjectRunOptions *options) {
    for (int i = start; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--turbo") {
            options->turbo_mode = true;
        } else if (arg == "--no-turbo") {
            options->turbo_mode = false;
        } else if (arg == "--turbowarp-compat") {
            options->compatibility_mode = SJIT_COMPATIBILITY_MODE_TURBOWARP;
        } else if (arg == "--scratch-compat") {
            options->compatibility_mode = SJIT_COMPATIBILITY_MODE_SCRATCH;
        } else if (arg == "--compatibility" || arg == "--compat") {
            if (i + 1 >= argc ||
                !parseCompatibilityMode(argv[++i], &options->compatibility_mode)) {
                std::cerr << "--compatibility requires scratch or turbowarp\n";
                return false;
            }
        } else if (arg.rfind("--compatibility=", 0) == 0 ||
                   arg.rfind("--compat=", 0) == 0) {
            const size_t separator = arg.find('=');
            if (!parseCompatibilityMode(arg.substr(separator + 1), &options->compatibility_mode)) {
                std::cerr << "--compatibility requires scratch or turbowarp\n";
                return false;
            }
        } else if (arg == "--list-limit" || arg == "--max-list-items") {
            if (i + 1 >= argc || !parsePositiveInt(argv[++i], &options->list_item_limit)) {
                std::cerr << "--list-limit requires a positive integer\n";
                return false;
            }
        } else if (arg.rfind("--list-limit=", 0) == 0 ||
                   arg.rfind("--max-list-items=", 0) == 0) {
            const size_t separator = arg.find('=');
            if (!parsePositiveInt(arg.substr(separator + 1), &options->list_item_limit)) {
                std::cerr << "--list-limit requires a positive integer\n";
                return false;
            }
        } else if (arg == "--stage-scale" || arg == "--scale" || arg == "--window-scale") {
            if (i + 1 >= argc || !parsePositiveDouble(argv[++i], &options->stage_scale) ||
                options->stage_scale < 0.25 || options->stage_scale > 8.0) {
                std::cerr << "--stage-scale requires a number from 0.25 to 8\n";
                return false;
            }
        } else if (arg.rfind("--stage-scale=", 0) == 0 ||
                   arg.rfind("--scale=", 0) == 0 ||
                   arg.rfind("--window-scale=", 0) == 0) {
            const size_t separator = arg.find('=');
            if (!parsePositiveDouble(arg.substr(separator + 1), &options->stage_scale) ||
                options->stage_scale < 0.25 || options->stage_scale > 8.0) {
                std::cerr << "--stage-scale requires a number from 0.25 to 8\n";
                return false;
            }
        } else if (arg == "--frames") {
            if (i + 1 >= argc || !parsePositiveInt(argv[++i], &options->max_frames)) {
                std::cerr << "--frames requires a positive integer\n";
                return false;
            }
        } else if (arg == "--fps") {
            if (i + 1 >= argc || !parsePositiveDouble(argv[++i], &options->fps)) {
                std::cerr << "--fps requires a positive number\n";
                return false;
            }
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::cout << "xyo-cpp Scratch LLVM VM/JIT MVP\n";
    std::cout << "host boundary: startup, LLJIT, input, rendering\n";
    std::cout << "runtime boundary: values, LLVM-backed scheduler, events, sprite state, draw queue\n";

    const auto compile = sjit::compileProjectSkeleton();
    const auto load = sjit::loadProjectSkeleton("");
    std::cout << compile.message << "\n";
    std::cout << load.message << "\n";

    if (argc > 1) {
        const std::string command = argv[1];
        if (command == "--help" || command == "-h") {
            printProjectUsage(argv[0]);
            return 0;
        }
        if (command == "--emit-ll") {
            if (argc < 3) {
                printProjectUsage(argv[0]);
                return 2;
            }
            try {
                if (argc >= 4) {
                    const auto paths = sjit::emitProjectLl(argv[2], argv[3]);
                    for (const std::string &path : paths) {
                        std::cout << "wrote LLVM IR: " << path << "\n";
                    }
                } else {
                    sjit::emitSmokeLl(argv[2]);
                    std::cout << "wrote LLVM IR: " << argv[2] << "\n";
                }
                return 0;
            } catch (const std::exception &error) {
                std::cerr << "failed to emit LLVM IR: " << error.what() << "\n";
                return 1;
            }
        }
        if (command == "--emit-runtime-ll") {
            if (argc < 3) {
                printProjectUsage(argv[0]);
                return 2;
            }
            try {
                sjit::emitRuntimeLl(argv[2]);
                std::cout << "wrote runtime LLVM IR: " << argv[2] << "\n";
                return 0;
            } catch (const std::exception &error) {
                std::cerr << "failed to emit runtime LLVM IR: " << error.what() << "\n";
                return 1;
            }
        }
        if (command == "--window" || command == "--gui") {
            if (argc < 3) {
                printProjectUsage(argv[0]);
                return 2;
            }
            sjit::ProjectRunOptions options;
            if (!parseRunOptions(argc, argv, 3, &options)) {
                printProjectUsage(argv[0]);
                return 2;
            }
            return sjit::runProjectWindow(argv[2], options);
        }
        sjit::ProjectRunOptions options;
        if (!parseRunOptions(argc, argv, 2, &options)) {
            printProjectUsage(argv[0]);
            return 2;
        }
        return sjit::runProjectFile(argv[1], options);
    }

    return sjit::runHostDemo();
}
