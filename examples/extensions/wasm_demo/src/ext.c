typedef unsigned int u32;

typedef struct {
    u32 tag;
    double num;
    u32 str_ptr;
    u32 str_len;
} XyoArg;

__attribute__((import_module("env"), import_name("xyo_arg_count")))
int xyo_arg_count(void);
__attribute__((import_module("env"), import_name("xyo_arg_num")))
double xyo_arg_num(int index);

static unsigned char heap[8192];
static u32 heap_top = 0;

__attribute__((export_name("xyo_ext_abi")))
int xyo_ext_abi(void) { return 1; }

__attribute__((export_name("xyo_ext_alloc")))
u32 xyo_ext_alloc(u32 n) {
    u32 p = (heap_top + 7u) & ~7u;
    if (p > sizeof(heap) || n > sizeof(heap) - p) return 0;
    heap_top = p + n;
    return (u32)heap + p;
}

static const char kHello[] = "hello from wasm";

__attribute__((export_name("xyo_ext_call")))
int xyo_ext_call(u32 op, u32 args, u32 nargs, u32 retp) {
    const XyoArg* a = (const XyoArg*)args;
    XyoArg* r = (XyoArg*)retp;
    r->str_ptr = 0;
    r->str_len = 0;
    r->num = 0.0;
    if (op == 0 && nargs >= 1) {
        r->tag = 1;
        r->num = a[0].num * 2.0;
        return 0;
    }
    if (op == 1 && nargs >= 2) {
        r->tag = 1;
        r->num = a[0].num + a[1].num;
        return 0;
    }
    if (op == 2) {
        r->tag = 2;
        r->str_ptr = (u32)kHello;
        r->str_len = sizeof(kHello) - 1;
        return 0;
    }
    if (op == 3 && nargs >= 1) {
        r->tag = 2;
        r->str_ptr = a[0].str_ptr;
        r->str_len = a[0].str_len;
        return 0;
    }
    if (op == 4) {
        r->tag = 1;
        r->num = xyo_arg_num(0) + (double)xyo_arg_count();
        return 0;
    }
    r->tag = 0;
    return 0;
}
