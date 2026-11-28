/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "BTAudioSessionQtiShim"

#include <aidl/lineage/compat/bluetooth/audio/v4/AudioConfiguration.h>
#include <dlfcn.h>
#include <log/log.h>

#include <cstdint>

namespace v4 = aidl::lineage::compat::bluetooth::audio::v4;

// The QTI A2DP software module (android.hardware.bluetooth.audio_sw.so) is
// redirected here from BluetoothAudioSession::GetAudioConfig(). It is built
// against the V4 interface, which the lineage compat v4 snapshot mirrors.
extern const v4::AudioConfiguration getAudioConfigQti(void* session) asm(
        "_ZN4aidl7android8hardware9bluetooth5audio21BluetoothAudioSession14GetAudioConfQtEv");

namespace {

constexpr const char* kSessionLib = "libbluetooth_audio_session_aidl_qti.so";
constexpr const char* kGetAudioConfig =
        "_ZN4aidl7android8hardware9bluetooth5audio21BluetoothAudioSession14GetAudioConfigEv";

// AudioFlinger mixes in multiples of 16 frames.
constexpr int64_t kFrameCountAlignment = 16;
// A2DP codec intervals are 20ms and up; LE audio and HFP use shorter ones.
constexpr int32_t kMinA2dpDataIntervalUs = 20000;

using GetAudioConfigFn = v4::AudioConfiguration (*)(void*);

GetAudioConfigFn getQtiGetAudioConfig() {
    static GetAudioConfigFn fn = [] {
        void* handle = dlopen(kSessionLib, RTLD_NOW | RTLD_NOLOAD);
        if (handle == nullptr) handle = dlopen(kSessionLib, RTLD_NOW);
        if (handle == nullptr) {
            ALOGE("Failed to open %s: %s", kSessionLib, dlerror());
            return static_cast<GetAudioConfigFn>(nullptr);
        }
        auto sym = reinterpret_cast<GetAudioConfigFn>(dlsym(handle, kGetAudioConfig));
        if (sym == nullptr) ALOGE("Failed to find GetAudioConfig: %s", dlerror());
        return sym;
    }();
    return fn;
}

// The QTI StreamBluetooth caps every transfer at dataIntervalUs worth of
// frames (e.g. 23000us at 44.1kHz = 1014 frames), while AudioFlinger writes
// 1024 frames. Each write then splits into a 1014-frame burst plus a 10-frame
// remainder, starving the Bluetooth stack's AAC encoder, which only reads
// whole 1024-frame blocks. Round the interval up to an aligned frame count.
void alignPcmDataInterval(v4::AudioConfiguration& config) {
    if (config.getTag() != v4::AudioConfiguration::Tag::pcmConfig) return;
    auto& pcm = config.get<v4::AudioConfiguration::Tag::pcmConfig>();
    if (pcm.sampleRateHz <= 0 || pcm.dataIntervalUs < kMinA2dpDataIntervalUs) return;

    const int64_t rate = pcm.sampleRateHz;
    int64_t frames = static_cast<int64_t>(pcm.dataIntervalUs) * rate / 1000000;
    frames = (frames + kFrameCountAlignment - 1) / kFrameCountAlignment * kFrameCountAlignment;
    const int64_t intervalUs = (frames * 1000000 + rate - 1) / rate;
    if (intervalUs != pcm.dataIntervalUs) {
        ALOGI("Aligning PCM data interval from %dus to %lldus (%lld frames at %dHz)",
              pcm.dataIntervalUs, static_cast<long long>(intervalUs),
              static_cast<long long>(frames), pcm.sampleRateHz);
        pcm.dataIntervalUs = static_cast<int32_t>(intervalUs);
    }
}

}  // namespace

const v4::AudioConfiguration getAudioConfigQti(void* session) {
    GetAudioConfigFn getAudioConfig = getQtiGetAudioConfig();
    if (getAudioConfig == nullptr) return v4::AudioConfiguration{};
    auto config = getAudioConfig(session);
    alignPcmDataInterval(config);
    return config;
}
