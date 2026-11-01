// xyo-cpp: SList のプロセス全体設定（リスト長上限）。
#include "slist.hpp"

// 0 = 無制限（既定）。`--list-limit N` / `XYO_LIST_LIMIT=N` で設定する。
// JIT 生成コードもこのシンボルを直接ロードする（src/jit/codegen.cpp）。
extern "C" uint32_t xyo_list_limit = 0;
