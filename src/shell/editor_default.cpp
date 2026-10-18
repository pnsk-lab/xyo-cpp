// xyo-cpp: ビルトインエディタ（src/shell/editor.cpp）がビルドに含まれない
// 間の既定実装。editor.cpp をビルドへ戻すと強い定義が優先される。
#include "editor.hpp"

#include <cstdio>

namespace xyo {

__attribute__((weak)) int editorMain(const char* path) {
    std::fprintf(stderr, "editor: %s (このビルドでは利用できません)\n", path);
    return 1;
}

} // namespace xyo
