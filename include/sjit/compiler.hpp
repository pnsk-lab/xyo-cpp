#ifndef SJIT_COMPILER_HPP
#define SJIT_COMPILER_HPP

#include <string>

namespace sjit {

struct CompileResult {
    bool ok;
    std::string message;
};

CompileResult compileProjectSkeleton();

}

#endif

