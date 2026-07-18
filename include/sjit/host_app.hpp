#ifndef SJIT_HOST_APP_HPP
#define SJIT_HOST_APP_HPP

namespace sjit {

struct ProjectRunOptions {
    int max_frames = 0;
    double fps = 60.0;
    bool turbo_mode = false;
};

int runHostDemo();
int runProjectFile(const char *path, ProjectRunOptions options = {});
int runProjectWindow(const char *path, ProjectRunOptions options = {});

}

#endif
