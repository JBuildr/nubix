// Nubix — voice chat end-to-end loopback test (local libdatachannel "server" peer).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// voice chat end-to-end over a local libdatachannel "server" peer: mic RTP upstream,
// game vs chat SSRC demux downstream, chat renegotiation (apply + rollback).
// Needs working local UDP (host ICE candidates); run with: xc-tests voice_loopback
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/protocol.hpp"
#include "stream/webrtc.hpp"
#include "test.hpp"

using namespace xc;

namespace {

bool waitFor(const std::function<bool()>& cond, int timeoutMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

uint32_t ssrcOf(const std::vector<uint8_t>& p) {
    return p.size() >= 12 ? (uint32_t(p[8]) << 24) | (uint32_t(p[9]) << 16) | (uint32_t(p[10]) << 8) | p[11] : 0;
}

// Plays the console: answers our offers, sends audio RTP, records what arrives on its audio track.
struct FakeServer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::mutex mu;
    std::shared_ptr<rtc::Track> audio;
    std::vector<std::vector<uint8_t>> rx;  // RTP received on the audio track
    std::vector<std::shared_ptr<rtc::DataChannel>> channels;
    std::atomic<bool> gathered{false};

    FakeServer() {
        rtc::Configuration cfg;  // host candidates only, auto-negotiation on (answers offers itself)
        pc = std::make_shared<rtc::PeerConnection>(cfg);
        pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState st) {
            if (st == rtc::PeerConnection::GatheringState::Complete) gathered = true;
        });
        pc->onTrack([this](std::shared_ptr<rtc::Track> t) {
            if (t->mid() != "audio") return;
            t->onMessage(
                [this](rtc::binary b) {
                    const auto* d = reinterpret_cast<const uint8_t*>(b.data());
                    if (b.size() < 12 || ((d[1] & 0x7F) >= 64 && (d[1] & 0x7F) <= 95)) return;  // RTP only
                    std::lock_guard<std::mutex> lk(mu);
                    rx.emplace_back(d, d + b.size());
                },
                [](rtc::string) {});
            std::lock_guard<std::mutex> lk(mu);
            audio = t;
        });
        pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            std::lock_guard<std::mutex> lk(mu);
            channels.push_back(dc);
        });
    }
    ~FakeServer() {
        try {
            pc->resetCallbacks();
            pc->close();
        } catch (...) {
        }
    }
    std::string answer(const std::string& offerSdp) {
        pc->setRemoteDescription(rtc::Description(offerSdp, rtc::Description::Type::Offer));
        waitFor([this] {
            return gathered.load() || pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete;
        }, 3000);
        auto d = pc->localDescription();
        return d ? std::string(*d) : std::string();
    }
    std::shared_ptr<rtc::DataChannel> channel(const std::string& label) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& c : channels)
            if (c->label() == label) return c;
        return nullptr;
    }
    std::shared_ptr<rtc::Track> track() {
        std::lock_guard<std::mutex> lk(mu);
        return audio;
    }
    size_t received() {
        std::lock_guard<std::mutex> lk(mu);
        return rx.size();
    }
};

}  // namespace

XC_TEST(voice_loopback, mic_up_chat_down_and_renegotiation) {
    std::mutex mu;
    std::vector<uint32_t> gameSsrcs, chatSsrcs;
    std::vector<std::string> closed;
    WebRtc::Callbacks cb;
    cb.onAudioRtp = [&](const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        gameSsrcs.push_back(ssrcOf(std::vector<uint8_t>(d, d + n)));
    };
    cb.onChatAudioRtp = [&](const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        chatSsrcs.push_back(ssrcOf(std::vector<uint8_t>(d, d + n)));
    };
    cb.onChannelClosed = [&](const std::string& ch) {
        std::lock_guard<std::mutex> lk(mu);
        closed.push_back(ch);
    };

    WebRtc rtc;
    XC_REQUIRE(rtc.init(cb, true));
    const WebRtc::MicInfo mic = rtc.micInfo();
    XC_REQUIRE(mic.ssrc != 0);
    std::string u, p, fp;
    std::vector<std::string> cands;
    rtc.gatherLocal(u, p, fp, cands, 3000);
    XC_REQUIRE(!u.empty() && !fp.empty());

    FakeServer server;
    const std::string answer = server.answer(rtc.localDescriptionSdp());
    XC_REQUIRE(!answer.empty());
    XC_REQUIRE(rtc.setRemote(answer, {}));
    XC_CHECK_EQ(rtc.answerAudioDirection(), std::string("sendrecv"));

    XC_REQUIRE(waitFor([&] { auto t = server.track(); return t && t->isOpen(); }, 10000));
    // Our track opens with the same DTLS transport; give it a moment.
    const uint8_t opus[] = {0xF8, 0xFF, 0xFE};
    XC_REQUIRE(waitFor([&] { return rtc.sendMicOpus(opus, sizeof(opus), 960, true); }, 5000));
    for (int i = 0; i < 9; ++i) XC_CHECK(rtc.sendMicOpus(opus, sizeof(opus), 960, false));
    XC_REQUIRE(waitFor([&] { return server.received() >= 10; }, 5000));
    {
        std::lock_guard<std::mutex> lk(server.mu);
        const auto& first = server.rx.front();
        XC_CHECK_EQ(ssrcOf(first), mic.ssrc);
        XC_CHECK_EQ(int(first[1] & 0x7F), 111);
        XC_CHECK(first[1] & 0x80);                // marker on the first frame
        XC_CHECK(!(server.rx[1][1] & 0x80));      // not on the next
        const uint16_t s0 = uint16_t(first[2] << 8 | first[3]), s1 = uint16_t(server.rx[1][2] << 8 | server.rx[1][3]);
        XC_CHECK_EQ(uint16_t(s0 + 1), s1);
        const uint32_t t0 = (uint32_t(first[4]) << 24) | (uint32_t(first[5]) << 16) | (uint32_t(first[6]) << 8) | first[7];
        const uint32_t t1 = (uint32_t(server.rx[1][4]) << 24) | (uint32_t(server.rx[1][5]) << 16) |
                            (uint32_t(server.rx[1][6]) << 8) | server.rx[1][7];
        XC_CHECK_EQ(uint32_t(t1 - t0), 960u);
        XC_CHECK_EQ(first.size(), size_t(15));
    }
    XC_CHECK(rtc.micTxStats().packets >= 10);

    // Downstream: game SSRC first, then a second SSRC = chat voice.
    auto st = server.track();
    XC_REQUIRE(st);
    const uint32_t game = 0x11111111u, chat = 0x22222222u;
    uint16_t gseq = 100, cseq = 500;
    for (int i = 0; i < 5; ++i) {
        auto pkt = buildOpusRtp(gseq++, 960u * i, game, false, opus, sizeof(opus));
        st->send(reinterpret_cast<const std::byte*>(pkt.data()), pkt.size());
    }
    XC_REQUIRE(waitFor([&] { std::lock_guard<std::mutex> lk(mu); return gameSsrcs.size() >= 5; }, 5000));
    for (int i = 0; i < 5; ++i) {
        auto g = buildOpusRtp(gseq++, 960u * (5 + i), game, false, opus, sizeof(opus));
        st->send(reinterpret_cast<const std::byte*>(g.data()), g.size());
        auto c = buildOpusRtp(cseq++, 960u * i, chat, false, opus, sizeof(opus));
        st->send(reinterpret_cast<const std::byte*>(c.data()), c.size());
    }
    XC_REQUIRE(waitFor([&] { std::lock_guard<std::mutex> lk(mu); return chatSsrcs.size() >= 5 && gameSsrcs.size() >= 10; }, 5000));
    {
        std::lock_guard<std::mutex> lk(mu);
        for (uint32_t s : gameSsrcs) XC_CHECK_EQ(s, game);
        for (uint32_t s : chatSsrcs) XC_CHECK_EQ(s, chat);
    }
    XC_CHECK_EQ(rtc.audioSsrc(), game);
    XC_CHECK_EQ(rtc.chatAudioSsrc(), chat);

    // Renegotiation, rolled back first ...
    std::string ru, rp, rfp;
    XC_REQUIRE(rtc.beginRenegotiation(ru, rp, rfp));
    XC_CHECK_EQ(ru, u);
    XC_CHECK_EQ(rp, p);
    XC_CHECK_EQ(rfp, fp);
    rtc.abortRenegotiation();
    // ... then applied for real.
    XC_REQUIRE(rtc.beginRenegotiation(ru, rp, rfp));
    const std::string offer2 = rtc.localDescriptionSdp();
    XC_CHECK_EQ(proto::sdpMediaDirection(offer2, "audio"), std::string("sendrecv"));
    const std::string answer2 = server.answer(offer2);
    XC_REQUIRE(!answer2.empty());
    XC_REQUIRE(rtc.finishRenegotiation(answer2));
    XC_CHECK_EQ(rtc.answerAudioDirection(), std::string("sendrecv"));
    XC_CHECK(!rtc.finishRenegotiation(answer2));  // nothing pending any more

    // Media keeps flowing both ways after the renegotiation.
    const size_t before = server.received();
    for (int i = 0; i < 5; ++i) XC_CHECK(rtc.sendMicOpus(opus, sizeof(opus), 960, false));
    XC_CHECK(waitFor([&] { return server.received() >= before + 5; }, 5000));
    const size_t gameBefore = [&] { std::lock_guard<std::mutex> lk(mu); return gameSsrcs.size(); }();
    for (int i = 0; i < 3; ++i) {
        auto g = buildOpusRtp(gseq++, 960u * (10 + i), game, false, opus, sizeof(opus));
        st->send(reinterpret_cast<const std::byte*>(g.data()), g.size());
    }
    XC_CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(mu); return gameSsrcs.size() >= gameBefore + 3; }, 5000));

    // Game SSRC silent for > 2 s while another stream flows: the new one takes over as game.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const uint32_t game2 = 0x33333333u;
    for (int i = 0; i < 3; ++i) {
        auto g = buildOpusRtp(uint16_t(900 + i), 960u * i, game2, false, opus, sizeof(opus));
        st->send(reinterpret_cast<const std::byte*>(g.data()), g.size());
    }
    XC_CHECK(waitFor([&] {
        std::lock_guard<std::mutex> lk(mu);
        return !gameSsrcs.empty() && gameSsrcs.back() == game2;
    }, 5000));
    XC_CHECK_EQ(rtc.audioSsrc(), game2);

    // The server closing a data channel (as xCloud does with "chat") reaches onChannelClosed.
    XC_REQUIRE(waitFor([&] { return server.channel("chat") != nullptr; }, 5000));
    server.channel("chat")->close();
    XC_CHECK(waitFor([&] {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& c : closed)
            if (c == "chat") return true;
        return false;
    }, 5000));
    XC_CHECK(rtc.isChannelOpen("control"));

    rtc.close();
    XC_CHECK_EQ(rtc.micInfo().ssrc, 0u);
}
