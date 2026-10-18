// xyo-cpp: 単一ヘッダライブラリの実装 TU。
// stb_image (PNG), stb_truetype (フォント), nanosvg (SVG) をここで実体化する。

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../third_party/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb_truetype.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "../../third_party/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "../../third_party/nanosvgrast.h"
