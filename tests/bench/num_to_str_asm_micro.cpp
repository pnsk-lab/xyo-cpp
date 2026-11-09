#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
extern "C" size_t xyo_i64_to_ascii_asm(int64_t, char*);
static volatile uint64_t sink = 0;

static void bench(const char* group, const std::array<int64_t,256>& nums,
                  bool asm_enabled) {
    char buffer[32]; uint64_t checksum = 0;
    auto begin=std::chrono::steady_clock::now();
    constexpr int repeats = 3000;
    for (int round=0;round<repeats;round++) {
        for (int64_t value : nums) {
            size_t n;
            if (asm_enabled) {
                n=xyo_i64_to_ascii_asm(value,buffer);
            } else {
                auto r=std::to_chars(buffer,buffer+32,value);
                n=size_t(r.ptr-buffer);
            }
            checksum += n + uint8_t(buffer[0]) +
                uint8_t(buffer[n-1]);
        }
    }
    sink=checksum;
    auto ns = std::chrono::duration<double,std::nano>(
            std::chrono::steady_clock::now()-begin).count()/
            double(repeats*nums.size());
    std::printf("%s %s %.3f ns/op sum=%016llx\n",
                group,asm_enabled?"asm":"chars",ns,
                (unsigned long long)sink);
}
int main() {
    for (int k=0;k<3;k++){
        for (const auto& name : {"digit","small","negative-small","medium","large","negative","random"}) {
            std::array<int64_t,256> nums{};
            uint64_t seed = 0x6a09e667f3bcc909ull;
            for (int i=0;i<256;i++){
                seed^=seed>>12; seed^=seed<<25; seed^=seed>>27;
                auto rand = seed*0x2545F4914F6CDD1Dull;
                int64_t v = 0;
                if (std::strcmp(name,"digit")==0) v=i%10;
                else if (std::strcmp(name,"small")==0) v=i%100;
                else if (std::strcmp(name,"negative-small")==0) v=-int64_t(i%90+10);
                else if (std::strcmp(name,"medium")==0) v=int64_t(rand%100000000);
                else if (std::strcmp(name,"large")==0) v=int64_t(rand%9007199254740993ull);
                else if (std::strcmp(name,"negative")==0) v=-int64_t(rand%9007199254740993ull);
                else v=static_cast<int64_t>(rand);
                nums[i]=v;
            }
            if (k % 2) {
                bench(name,nums,true); bench(name,nums,false);
            } else {
                bench(name,nums,false); bench(name,nums,true);
            }
        }
    }
}
