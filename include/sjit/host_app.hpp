#ifndef SJIT_HOST_APP_HPP
#define SJIT_HOST_APP_HPP

#include "sjit/abi.hpp"

namespace sjit {

struct ProjectRunOptions {
    int max_frames = 0;
    double fps = 60.0;
    bool turbo_mode = false;
    int compatibility_mode = SJIT_COMPATIBILITY_MODE_TURBOWARP;
    /* Zero selects the compatibility preset. */
    int list_item_limit = 0;
    /* Initial visible stage scale. The window remains freely resizable. */
    double stage_scale = 2.0;
};

int runHostDemo();
int runProjectFile(const char *path, ProjectRunOptions options = {});
int runProjectWindow(const char *path, ProjectRunOptions options = {});

}

#endif
