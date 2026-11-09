#include "core/jsnum.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
static volatile uint64_t checksum_sink = 0;
int main() {
    for(int trial=0;trial<5;trial++){
        for(const char* category:{"1-digit","2-digit","6-digit","15-digit",
                                  "fraction","extreme"}) {
            std::array<double,256> cases;
            uint64_t seed=0x9a72dec1179cull;
            for(size_t i=0;i<cases.size();++i) {
                seed ^= seed>>12; seed ^= seed<<25; seed ^= seed>>27;
                uint64_t raw = seed*0x2545f4914f6cdd1dull;
                if(strcmp(category,"1-digit")==0) cases[i]=double(i%10);
                else if(strcmp(category,"2-digit")==0) cases[i]=double((i%100)-1);
                else if(strcmp(category,"6-digit")==0) cases[i]=double(int64_t(raw%1000000ull));
                else if(strcmp(category,"15-digit")==0) cases[i]=double(int64_t(raw%1000000000000000ull));
                else if(strcmp(category,"fraction")==0) cases[i]=double(int64_t(raw%100000000ull))/1024.0;
                else cases[i]=std::bit_cast<double>(raw);
            }
            constexpr int loops=1500;
            uint64_t checksum=0;
            auto begin=std::chrono::steady_clock::now();
            for(int repeat=0;repeat<loops;repeat++) {
                for(double v:cases){
                    auto s=xyo::jsnum::toString(v);
                    checksum+=s.size() + uint8_t(s.front());
                }
            }
            checksum_sink=checksum;
            double nanoseconds=std::chrono::duration<double,std::nano>(
                std::chrono::steady_clock::now()-begin).count()/
                (loops*cases.size());
            printf("%s %.3f ns/op sum=%016llx\n",
                   category,nanoseconds,(unsigned long long)checksum_sink);
        }
    }
}
