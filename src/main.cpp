#include "sjit/compiler.hpp"
#include "sjit/host_app.hpp"
#include "sjit/jit.hpp"
#include "sjit/project_loader.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void printProjectUsage(const char *program) {
    std::cerr << "usage: " << program << " <project.sb3> [--frames N] [--fps N] [--turbo]\n";
    std::cerr << "       " << program << " --window <project.sb3> [--frames N] [--fps N] [--turbo]\n";
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

bool parseRunOptions(int argc, char **argv, int start, sjit::ProjectRunOptions *options) {
    for (int i = start; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--turbo") {
            options->turbo_mode = true;
        } else if (arg == "--no-turbo") {
            options->turbo_mode = false;
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
