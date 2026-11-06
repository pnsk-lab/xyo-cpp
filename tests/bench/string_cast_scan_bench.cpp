// Benchmark Scratch cast predicates with flat and cons strings.
#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "core/gc.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <utility>
using namespace xyo;
using Clock=std::chrono::steady_clock;
static volatile uint64_t sink;
template<class Func>
double bench(Func fn) {
    constexpr int n=5000;
    uint64_t acc=0;
    const auto start=Clock::now();
    for(int i=0;i<n;++i) acc+=fn();
    const auto end=Clock::now();
    sink=acc;
    return std::chrono::duration<double,std::nano>(end-start).count()/n;
}
int main() {
    Gc::DeferCollect defer;
    SStr* digits=SStr::create(std::u16string(4096,u'7'));
    SStr* spaces=SStr::create(std::u16string(4096,u' '));
    SStr* letters=SStr::create(std::u16string(4096,u'漢'));
    SStr* cons=sstrCons(SStr::create(std::u16string(2048,u'7')),
                        SStr::create(std::u16string(2048,u'7')));
    (void)cons->view();
    for(auto sample : {std::pair{"digits",digits},
                       std::pair{"spaces",spaces},
                       std::pair{"letters",letters},
                       std::pair{"cons",cons}}) {
        const Value value=Value::string(sample.second);
        std::printf("%s isInt=%.2fns isWhiteSpace=%.2fns notZero=%.2fns\n",
            sample.first,
            bench([&]{return cast::isInt(value);}),
            bench([&]{return cast::isWhiteSpace(value);}),
            bench([&]{return cast::isNotActuallyZero(value);}));
    }
}
