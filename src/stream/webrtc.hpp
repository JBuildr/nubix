// Nubix — libdatachannel wrapper: recvonly H.264 + Opus tracks (mids "video", "audio"),
// four reliable/ordered data channels (control/controlV1, input/1.0, message/messageV1,
// chat/chatV1, created before the offer so the data m-line gets mid "0"), raw RTP delivery
// and hand-built RTCP (PLI with sender SSRC 0, NACK, RR, REMB).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "stream/rtp.hpp"

namespace xc {

class WebRtc {
public:
    struct Callbacks {
        // Raw RTP packets (header included) of the video / audio track. Called on libdatachannel threads.
        std::function<void(const uint8_t*, size_t)> onVideoRtp, onAudioRtp;
        // Incoming RTCP sender report on a track: media SSRC + 64-bit NTP timestamp (for LSR).
        std::function<void(bool video, uint32_t ssrc, uint64_t ntp)> onSenderReport;
        // Text / binary message on data channel ch ("control", "input", "message", "chat").
        std::function<void(const std::string& ch, const std::string& text)> onText;
        std::function<void(const std::string& ch, const uint8_t*, size_t)> onBinary;
        // Data channel ch opened.
        std::function<void(const std::string& ch)> onChannelOpen;
        // PeerConnection state changes: "new", "connecting", "connected", "disconnected", "failed", "closed".
        std::function<void(const std::string& state)> onState;
    };

    WebRtc();
    ~WebRtc();
    WebRtc(const WebRtc&) = delete;
    WebRtc& operator=(const WebRtc&) = delete;

    // Create the PeerConnection, tracks and data channels. Must be called first.
    bool init(const Callbacks& cb);

    // Create the local offer, wait for ICE gathering (timeoutMs) and return the ICE credentials,
    // sha-256 fingerprint and gathered candidate lines ("candidate:..."). The real local
    // description stays inside libdatachannel; the caller sends proto::buildOffer(...) instead.
    // abort (optional): checked every 100 ms; once true the wait ends and false is returned.
    bool gatherLocal(std::string& ufrag, std::string& pwd, std::string& fingerprint,
                     std::vector<std::string>& candidates, int timeoutMs,
                     const std::atomic<bool>* abort = nullptr);

    // Full SDP of libdatachannel's own local offer (empty before gatherLocal). Diagnostics / tests.
    std::string localDescriptionSdp() const;

    // Apply the server answer SDP and the remote candidates (added with mid "0").
    bool setRemote(const std::string& answerSdp, const std::vector<std::string>& remoteCandidates);

    // Send on a data channel. False if the channel is not open.
    bool sendText(const std::string& ch, const std::string& text);
    bool sendBinary(const std::string& ch, const std::vector<uint8_t>& data);

    // True if data channel ch is open.
    bool isChannelOpen(const std::string& ch) const;

    // Send RTCP PLI (sender SSRC 0, media SSRC = video SSRC).
    void requestKeyframe();
    // Send RTCP generic NACK (RFC 4585) for the given sequence numbers.
    void sendNack(uint32_t ssrc, const std::vector<uint16_t>& seqs);
    // Send an RTCP compound RR (+ SDES) with one report block per entry.
    void sendReceiverReport(const std::vector<RtpReceiveStats>& blocks);
    // Send RTCP REMB with the given max bitrate for the video SSRC.
    void sendRemb(uint32_t bps);

    // Media SSRCs latched from the first received RTP packet (0 if none yet).
    uint32_t videoSsrc() const;
    uint32_t audioSsrc() const;

    // Close channels and the PeerConnection. Idempotent; no callbacks fire afterwards.
    // Waits for a callback that is currently running, so do not call it while holding a lock that
    // your callbacks also take, and preferably not from inside a callback (post to another thread).
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
