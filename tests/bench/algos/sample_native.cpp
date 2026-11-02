// Optional Linux x86_64 self sampler; compile -O2 -std=c++20 -shared -fPIC.
// No stack unwinding. Process-wide SIGPROF delivery can bias main-thread samples.
// Set LD_PRELOAD on the final executable, AFTER taskset/env wrappers: exec resets
// handlers but can retain an armed timer. Not intended for programs that exec.
// Histogram writes are confined to main's non-reentrant handler; dumping blocks
// that handler after disabling the timer. Other threads only touch lock-free other.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#if !defined(__linux__) || !defined(__x86_64__)
#error Linux x86_64 required
#endif
namespace {
constexpr unsigned N=1u<<18, probes=32;
struct Entry { std::atomic<uintptr_t> pc{0}; std::atomic<uint64_t> count{0}; };
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
Entry entries[N];
std::atomic<uint64_t> other{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
std::atomic<uint64_t> total{0}, dropped{0};
pid_t main_tid;
bool enabled;
const char *prefix;
void sample(int, siginfo_t*, void *context) {
    int saved=errno;
    if (syscall(SYS_gettid)!=main_tid) other.fetch_add(1,std::memory_order_relaxed);
    else {
        ++total;
        uintptr_t pc=static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP];
        unsigned slot=((pc>>4)*11400714819323198485ull)&(N-1);
        bool found=false;
        for(unsigned i=0;i<probes;++i) {
            Entry &e=entries[(slot+i)&(N-1)];
            if(!e.pc || e.pc==pc) { e.pc=pc; ++e.count; found=true; break; }
        }
        if(!found) ++dropped;
    }
    errno=saved;
}
__attribute__((constructor)) void start() {
    prefix=getenv("XYO_NATIVE_SAMPLE_OUT");
    if(!prefix || !*prefix) return;
    main_tid=syscall(SYS_gettid);
    struct sigaction previous{}, action{};
    if(sigaction(SIGPROF,nullptr,&previous) || previous.sa_handler!=SIG_DFL) {
        fprintf(stderr,"sampler: SIGPROF already owned; disabled\n"); return;
    }
    itimerval old{};
    if(getitimer(ITIMER_PROF,&old) || old.it_value.tv_sec || old.it_value.tv_usec) {
        fprintf(stderr,"sampler: timer already owned; disabled\n"); return;
    }
    action.sa_sigaction=sample; action.sa_flags=SA_SIGINFO|SA_RESTART;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGPROF,&action,nullptr)) { perror("sampler sigaction"); return; }
    itimerval timer{}; timer.it_interval.tv_usec=timer.it_value.tv_usec=1000;
    if(setitimer(ITIMER_PROF,&timer,nullptr)) { perror("sampler timer"); return; }
    enabled=true;
}
__attribute__((destructor)) void stop() {
    if(!enabled) return;
    itimerval off{}; setitimer(ITIMER_PROF,&off,nullptr);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask,SIGPROF);
    sigprocmask(SIG_BLOCK,&mask,nullptr); // only main touches histogram
    char path[4096]; snprintf(path,sizeof(path),"%s.samples",prefix);
    FILE *out=fopen(path,"w");
    if(!out) { perror("sampler output"); return; }
    fprintf(out,"# main=%llu other=%llu dropped=%llu interval_us=1000\n",
        (unsigned long long)total.load(),(unsigned long long)other.load(),(unsigned long long)dropped.load());
    for(const auto &e:entries) if(e.pc) fprintf(out,"%lx %llu\n",(unsigned long)e.pc.load(),(unsigned long long)e.count.load());
    fclose(out);
    snprintf(path,sizeof(path),"%s.maps",prefix);
    FILE *maps=fopen("/proc/self/maps","r"); out=fopen(path,"w");
    if(!maps || !out) { perror("sampler maps"); if(maps) fclose(maps); if(out) fclose(out); return; }
    char buffer[8192]; size_t n;
    while((n=fread(buffer,1,sizeof(buffer),maps))) fwrite(buffer,1,n,out);
    fclose(maps); fclose(out);
}
}
