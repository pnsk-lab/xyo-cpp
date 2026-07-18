#ifndef SJIT_PROJECT_LOADER_HPP
#define SJIT_PROJECT_LOADER_HPP

#include <string>
#include <vector>
#include <memory>

#include "sjit/abi.hpp"

namespace sjit {

class JitEngine;

struct CostumeRenderInfo {
    std::string name;
    std::string asset_id;
    std::string data_format;
    // Original costume bytes.  Keeping these with the project lets the SDL
    // host create a real texture instead of reducing the costume to a box.
    std::string source_data;
    double width = 0.0;
    double height = 0.0;
    double rotation_center_x = 0.0;
    double rotation_center_y = 0.0;
    int fill_r = 247;
    int fill_g = 154;
    int fill_b = 43;
    int fill_a = 255;
    int stroke_r = 90;
    int stroke_g = 55;
    int stroke_b = 32;
    int stroke_a = 255;
};

struct TargetRenderInfo {
    int target_id = 0;
    std::vector<CostumeRenderInfo> costumes;
};

struct ProjectProgram {
    std::vector<SCompiledScript *> scripts;
    std::unique_ptr<JitEngine> jit;
    std::vector<TargetRenderInfo> render_targets;

    ProjectProgram() = default;
    ProjectProgram(const ProjectProgram &) = delete;
    ProjectProgram &operator=(const ProjectProgram &) = delete;
    ProjectProgram(ProjectProgram &&other) noexcept;
    ProjectProgram &operator=(ProjectProgram &&other) noexcept;
    ~ProjectProgram();
};

struct ProjectLoadResult {
    bool ok;
    std::string message;
    ProjectProgram program;
};

ProjectLoadResult loadProjectSkeleton(const std::string &path);
ProjectLoadResult loadProjectIntoRuntime(SRuntime *runtime, const std::string &path);
std::vector<std::string> emitProjectLl(const std::string &project_path, const std::string &output_path);

}

#endif
