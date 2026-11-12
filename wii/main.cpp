// Experimental Wii Homebrew Channel launcher for the xyo-cpp interpreter.
// Stage rendering, audio and native JIT are not yet available.
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "core/sstr.hpp"

namespace {

constexpr size_t kMaxArchiveBytes = 12 * 1024 * 1024;

bool readProject(const char* path, std::vector<uint8_t>& bytes) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long length = std::ftell(file);
    if (length <= 0 || static_cast<unsigned long>(length) > kMaxArchiveBytes ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(length));
    const bool ok = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!ok) bytes.clear();
    return ok;
}

void initConsole() {
    VIDEO_Init();
    GXRModeObj* mode = VIDEO_GetPreferredMode(nullptr);
    void* framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    console_init(framebuffer, 20, 20, mode->fbWidth - 40,
                 mode->xfbHeight - 40, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(framebuffer);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    WPAD_Init();
}

void updateKeyboard(xyo::Runtime& rt, uint32_t held, uint32_t previous) {
    struct Mapping {
        uint32_t button;
        const char16_t* key;
    };
    constexpr Mapping keys[] = {
        {WPAD_BUTTON_UP, u"ArrowUp"},
        {WPAD_BUTTON_DOWN, u"ArrowDown"},
        {WPAD_BUTTON_LEFT, u"ArrowLeft"},
        {WPAD_BUTTON_RIGHT, u"ArrowRight"},
        {WPAD_BUTTON_2, u" "},
        {WPAD_BUTTON_1, u"Enter"},
    };
    for (const Mapping& m : keys) {
        if (bool(held & m.button) != bool(previous & m.button))
            rt.ioDevices.keyboard.postData(m.key, (held & m.button) != 0);
    }
}

bool homePressed() {
    WPAD_ScanPads();
    return (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) != 0;
}

[[noreturn]] void waitForExit() {
    while (SYS_MainLoop()) {
        if (homePressed()) std::exit(1);
        VIDEO_WaitVSync();
    }
    std::exit(0);
}

} // namespace

int main() {
    initConsole();
    std::puts("xyo-cpp / Nintendo Wii: interpreter prototype");
    std::puts("A: green flag / HOME: exit");
    std::puts("D-Pad: arrows / 1: Enter / 2: Space");
    std::puts("No stage graphics or sound yet.\n");

    if (!fatInitDefault()) {
        std::puts("SD/USB FAT mount failed.");
        waitForExit();
    }

    std::vector<uint8_t> data;
    const char* selected = nullptr;
    constexpr const char* paths[] = {
        "sd:/apps/xyo-cpp/project.sb3",
        "sd:/xyo/project.sb3",
        "usb:/apps/xyo-cpp/project.sb3",
    };
    for (const char* path : paths) {
        if (readProject(path, data)) {
            selected = path;
            break;
        }
    }
    if (!selected) {
        std::puts("Missing project.sb3.");
        std::puts("Copy to sd:/apps/xyo-cpp/project.sb3");
        waitForExit();
    }
    std::printf("Reading: %s (%lu bytes)\n", selected,
                static_cast<unsigned long>(data.size()));

    xyo::Runtime rt;
    const xyo::Sb3LoadResult load = xyo::loadSb3File(rt, data.data(), data.size());
    std::vector<uint8_t>().swap(data);
    if (!load.ok) {
        std::printf("SB3 load failed: %s\n", load.error.c_str());
        waitForExit();
    }

    rt.onSay = [](xyo::Target*, const char* type, const std::u16string& msg) {
        if (!msg.empty())
            std::printf("[%s] %s\n", type ? type : "say",
                        xyo::SStr::toUtf8(msg).c_str());
    };
    rt.onQuestion = [](const std::u16string* msg) {
        if (msg) std::printf("[ask] %s\n", xyo::SStr::toUtf8(*msg).c_str());
    };

    std::printf("Loaded %lu targets. Running...\n",
                static_cast<unsigned long>(rt.targets.size()));
    rt.greenFlag();
    uint32_t previous = 0;
    unsigned ticks = 0;
    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        const uint32_t held = WPAD_ButtonsHeld(0);
        const uint32_t pressed = WPAD_ButtonsDown(0);
        if (pressed & WPAD_BUTTON_HOME) break;
        if (pressed & WPAD_BUTTON_A) {
            rt.greenFlag();
            std::puts("[green flag]");
        }
        updateKeyboard(rt, held, previous);
        previous = held;

        // 60 Hz video / 2 = approximately 30 Scratch ticks/s.
        if ((ticks++ & 1u) == 0)
            rt.step();
        VIDEO_WaitVSync();
    }
    rt.stopAll();
    return 0;
}
