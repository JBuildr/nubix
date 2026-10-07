// Nubix — dependency link check (not shipped). Touches every third-party library the app
// uses so that a full static link of every dependency is exercised independently of the app.
// Build: -DXC_BUILD_LINKCHECK=ON  -> target xc-linkcheck. Running it prints a local SDP offer.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <SDL.h>
#include <SDL_ttf.h>
#include <curl/curl.h>
#include <opus.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

int main(int, char**) {
    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Configuration cfg;
    cfg.disableAutoNegotiation = true;
    auto pc = std::make_shared<rtc::PeerConnection>(cfg);
    rtc::Description::Video v("video", rtc::Description::Direction::RecvOnly);
    v.addH264Codec(102, std::string("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"));
    auto vt = pc->addTrack(v);
    vt->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    rtc::Description::Audio a("audio", rtc::Description::Direction::RecvOnly);
    a.addOpusCodec(111);
    auto at = pc->addTrack(a);
    rtc::DataChannelInit init;
    init.protocol = "messageV1";
    auto dc = pc->createDataChannel("message", init);
    std::promise<void> done;
    std::atomic<bool> fired{false};
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete && !fired.exchange(true)) done.set_value();
    });
    pc->setLocalDescription(rtc::Description::Type::Offer);
    done.get_future().wait_for(std::chrono::seconds(5));
    if (auto ld = pc->localDescription()) std::printf("%s\n", std::string(*ld).c_str());

    unsigned char rnd[8];
    std::printf("openssl %s rand=%d\n", OPENSSL_VERSION_TEXT, RAND_bytes(rnd, sizeof(rnd)));
    std::printf("curl %s\n", curl_version());
    std::printf("opus %s\n", opus_get_version_string());
    std::printf("avcodec h264 %s\n", avcodec_find_decoder(AV_CODEC_ID_H264) ? "yes" : "no");
    AVFrame* f = av_frame_alloc();
    av_frame_free(&f);
    SDL_version sv;
    SDL_GetVersion(&sv);
    std::printf("SDL %d.%d.%d ttf init=%d\n", sv.major, sv.minor, sv.patch, TTF_Init());
    TTF_Quit();
    pc->close();
    return 0;
}
