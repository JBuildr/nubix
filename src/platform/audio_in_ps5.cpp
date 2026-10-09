// Nubix — PS5 microphone capture via libSceAudioIn (headset / DualSense mic).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// libSceAudioIn is not among the payload's DT_NEEDED modules and the SDK ships no stub for it,
// so it is resolved at run time: dlopen("libSceAudioIn.sprx") first, then
// sceKernelLoadStartModule + sceKernelDlsym. The PS4-era ABI is used:
//   int32_t sceAudioInOpen(userId, type, index, len, freq, param)   (HqOpen: same shape)
//   int32_t sceAudioInInput(handle, dst)   blocks for one grain
//   int32_t sceAudioInGetSilentState(handle)
// Format/type/rate combinations differ between firmware and device, so open() walks a small
// table and logs every attempt; the first one accepted wins.
#ifdef XC_PS5

#include "platform/audio_in.hpp"

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "core/log.hpp"

extern "C" {
int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addr);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetForegroundUser(int32_t* userId);
int sceUserServiceGetInitialUser(int32_t* userId);
}

namespace xc {
namespace platform {
namespace {

using OpenFn = int32_t (*)(int32_t userId, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
using InputFn = int32_t (*)(int32_t handle, void* dst);
using CloseFn = int32_t (*)(int32_t handle);
using SilentFn = int32_t (*)(int32_t handle);
using InitFn = int32_t (*)(void);

constexpr const char* kModuleName = "libSceAudioIn.sprx";
constexpr const char* kModulePath = "/system/common/lib/libSceAudioIn.sprx";
constexpr int32_t kUserSystem = 0xFF;  // never open for SYSTEM
constexpr uint32_t kParamS16Mono = 0;

struct AudioInApi {
    OpenFn open = nullptr;
    OpenFn hqOpen = nullptr;
    InputFn input = nullptr;
    CloseFn close = nullptr;
    SilentFn silent = nullptr;
    InitFn init = nullptr;
    bool ok() const { return (open || hqOpen) && input && close; }
};

std::once_flag g_loadOnce;
AudioInApi g_api;

void* symDl(void* h, const char* name) { return h ? dlsym(h, name) : nullptr; }

void* symKernel(int h, const char* name) {
    void* p = nullptr;
    if (h < 0 || sceKernelDlsym(h, name, &p) != 0) return nullptr;
    return p;
}

template <typename Lookup>
void resolveAll(AudioInApi& api, Lookup&& sym) {
    api.open = reinterpret_cast<OpenFn>(sym("sceAudioInOpen"));
    api.hqOpen = reinterpret_cast<OpenFn>(sym("sceAudioInHqOpen"));
    api.input = reinterpret_cast<InputFn>(sym("sceAudioInInput"));
    api.close = reinterpret_cast<CloseFn>(sym("sceAudioInClose"));
    api.silent = reinterpret_cast<SilentFn>(sym("sceAudioInGetSilentState"));
    api.init = reinterpret_cast<InitFn>(sym("sceAudioInInit"));
}

// Resolve the module once per process. Every outcome is logged.
void loadApi() {
    AudioInApi api;
    if (void* h = dlopen(kModuleName, RTLD_LAZY)) {
        resolveAll(api, [h](const char* n) { return symDl(h, n); });
        if (api.ok()) {
            XC_LOGI("voice: ps5 libSceAudioIn via %s (%s)", "dlopen", "ok");
        } else {
            XC_LOGW("voice: ps5 libSceAudioIn via %s (%s)", "dlopen", "FAILED symbols");
            api = AudioInApi{};
        }
    } else {
        const char* e = dlerror();
        XC_LOGW("voice: ps5 libSceAudioIn via %s (%s: %s)", "dlopen", "FAILED", e ? e : "?");
    }
    if (!api.ok()) {
        int res = 0;
        const int h = sceKernelLoadStartModule(kModulePath, 0, nullptr, 0, nullptr, &res);
        if (h >= 0) {
            resolveAll(api, [h](const char* n) { return symKernel(h, n); });
            if (api.ok()) {
                XC_LOGI("voice: ps5 libSceAudioIn via %s (%s)", "sceKernelLoadStartModule", "ok");
            } else {
                XC_LOGW("voice: ps5 libSceAudioIn via %s (%s)", "sceKernelLoadStartModule", "FAILED symbols");
                api = AudioInApi{};
            }
        } else {
            char what[32];
            std::snprintf(what, sizeof(what), "FAILED 0x%08x", static_cast<unsigned>(h));
            XC_LOGW("voice: ps5 libSceAudioIn via %s (%s)", "sceKernelLoadStartModule", what);
        }
    }
    if (api.ok() && api.init) {
        const int32_t rc = api.init();
        XC_LOGD("voice: ps5 sceAudioInInit -> 0x%08x", static_cast<unsigned>(rc));
    }
    g_api = api;
}

bool userId(int32_t& uid) {
    static std::once_flag initOnce;
    std::call_once(initOnce, [] { sceUserServiceInitialize(nullptr); });  // already-initialised is fine
    int32_t u = -1;
    if (sceUserServiceGetForegroundUser(&u) == 0 && u >= 0 && u != kUserSystem) {
        XC_LOGI("voice: ps5 user id 0x%x (%s)", static_cast<unsigned>(u), "foreground");
        uid = u;
        return true;
    }
    u = -1;
    if (sceUserServiceGetInitialUser(&u) == 0 && u >= 0 && u != kUserSystem) {
        XC_LOGI("voice: ps5 user id 0x%x (%s)", static_cast<unsigned>(u), "initial");
        uid = u;
        return true;
    }
    XC_LOGW("voice: ps5 no foreground or initial user");
    return false;
}

struct OpenAttempt {
    uint32_t type, len, freq;
};
// type 0 = VOICE_CHAT, 1 = GENERAL; all S16 mono.
constexpr OpenAttempt kAttempts[] = {
    {0, 256, 16000}, {1, 256, 48000}, {1, 480, 48000}, {0, 256, 48000}, {1, 256, 16000},
};

class Ps5AudioIn final : public AudioIn {
public:
    ~Ps5AudioIn() override { close(); }

    bool open(int& rate, int& grain) override {
        close();
        std::call_once(g_loadOnce, loadApi);
        if (!g_api.ok()) return false;
        int32_t uid = -1;
        if (!userId(uid)) return false;

        const struct {
            const char* name;
            OpenFn fn;
        } variants[] = {{"Open", g_api.open}, {"HqOpen", g_api.hqOpen}};
        for (const auto& v : variants) {
            if (!v.fn) continue;
            // HqOpen's argument layout is unconfirmed (assumed equal to Open); the capture buffer
            // has headroom for a larger grain, so a mismatch cannot overrun it.
            if (v.fn == g_api.hqOpen) XC_LOGI("voice: ps5 every Open attempt failed, trying HqOpen (experimental)");
            for (const OpenAttempt& a : kAttempts) {
                const int32_t h = v.fn(uid, a.type, 0, a.len, a.freq, kParamS16Mono);
                XC_LOGI("voice: ps5 %s(user=0x%x type=%u len=%u freq=%u) -> 0x%08x", v.name,
                        static_cast<unsigned>(uid), a.type, a.len, a.freq, static_cast<unsigned>(h));
                if (h >= 0) {
                    handle_ = h;
                    grain_ = static_cast<int>(a.len);
                    rate = static_cast<int>(a.freq);
                    grain = grain_;
                    firstInput_ = true;
                    lastErr_ = 0;
                    lastErrLog_ = {};
                    lastSilent_ = 0xFFFFFFFFu;
                    XC_LOGI("voice: ps5 mic open: %d Hz grain %d handle 0x%08x", rate, grain,
                            static_cast<unsigned>(h));
                    return true;
                }
            }
        }
        return false;
    }

    int read(int16_t* buf, int /*timeoutMs*/) override {
        if (handle_ < 0) return -1;
        const int32_t rc = g_api.input(handle_, buf);
        if (firstInput_) {
            firstInput_ = false;
            XC_LOGI("voice: ps5 first sceAudioInInput -> %d", static_cast<int>(rc));
        }
        if (rc >= 0) return grain_;
        lastErr_ = rc;
        const auto now = std::chrono::steady_clock::now();
        if (lastErrLog_ == std::chrono::steady_clock::time_point{} || now - lastErrLog_ >= std::chrono::seconds(5)) {
            lastErrLog_ = now;
            XC_LOGW("voice: ps5 sceAudioInInput -> 0x%08x", static_cast<unsigned>(rc));
        }
        return -1;
    }

    uint32_t silentState() override {
        if (handle_ < 0 || !g_api.silent) return 0;
        const int32_t rc = g_api.silent(handle_);
        // Negative = error code, not a bit set: treat as "no device" so zeros flow.
        const uint32_t s = rc < 0 ? kAudioInSilentNoDevice : static_cast<uint32_t>(rc);
        if (s != lastSilent_) {
            XC_LOGI("voice: ps5 silent state 0x%x -> 0x%x",
                    lastSilent_ == 0xFFFFFFFFu ? 0u : lastSilent_, s);
            lastSilent_ = s;
        }
        return s;
    }

    void close() override {
        if (handle_ >= 0) {
            const int32_t rc = g_api.close(handle_);
            XC_LOGI("voice: ps5 mic closed (handle 0x%08x -> 0x%08x)", static_cast<unsigned>(handle_),
                    static_cast<unsigned>(rc));
            handle_ = -1;
        }
    }

    const char* backend() const override { return "ps5-audioin"; }
    int32_t lastError() const override { return lastErr_; }

private:
    int32_t handle_ = -1;
    int grain_ = 0;
    bool firstInput_ = true;
    int32_t lastErr_ = 0;
    std::chrono::steady_clock::time_point lastErrLog_{};
    uint32_t lastSilent_ = 0xFFFFFFFFu;
};

}  // namespace

std::unique_ptr<AudioIn> createAudioIn() { return std::make_unique<Ps5AudioIn>(); }

}  // namespace platform
}  // namespace xc

#endif  // XC_PS5
