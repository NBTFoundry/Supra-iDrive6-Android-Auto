// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
//
// AsoundDevice's A2I output (NBT_BROWSER_A2I): the mix written straight into one io-amp input through liba2i_client,
// opened the way the head unit's media player (io-media-cinemo's a2i_writer) opens input_stereo_0: 8 buffers, 16-bit
// stereo at 44100 Hz, cookie 0x05555555, a timeout of twice the buffers' duration. Every buffer is queued with
// silence, then Start; a write fills the buffer A2I_Client_GetBuffer hands out and queues it when full, waiting with
// A2I_Client_GetNotification while every buffer is queued. For a connection whose source plays that input (0x1d0, the
// media player's online stream source, plays input_stereo_0), unlike the browser source and its libasound shim.
#include "a2i_client.h"
#include "audio/asound_device.hpp"
#include <stdio.h>
#include <string.h>

namespace nbt {

namespace {
const uint32_t kA2iRate = 44100;
const uint32_t kA2iBuffers = 8;
const int kMaxWaits = 20;   // GetNotification waits at most the timeout: give up after this many
}

bool AsoundDevice::openA2i() {
    if (a2i_) return true;
    uint32_t frag = (uint32_t)(frag_cfg_ < 32 ? 32 : frag_cfg_);
    while (frag & (frag - 1)) frag &= frag - 1;   // A2I takes a power of two
    if (frag > 16384) frag = 16384;
    a2i_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = 0x02000000u;
    cfg.direction = A2I_DIR_OUTPUT_DOWNLINK;
    cfg.field_08 = 1;
    cfg.mode = kA2iBuffers;               // the media player's buffer count
    cfg.buffer_size = frag;
    cfg.channels = kChannels;
    cfg.bytes_per_sample = 2;
    cfg.sample_rate = kA2iRate;
    cfg.endpoint_name = a2i_ep_;
    cfg.channel_type = 1;                 // stereo
    cfg.sample_bits = A2I_BITS_16;
    cfg.field_34 = A2I_COOKIE_LEGACY_PLAYBACK;
    cfg.timeout_ms = (uint32_t)(2000ull * frag * kA2iBuffers / (kChannels * 2u * kA2iRate));
    a2i_client_handle_t h = NULL;
    const int rc = A2I_Client_Open(&h, &cfg);
    if (rc != 0 || !h) {
        printf("[Browser] A2I open %s failed: %d\n", a2i_ep_, rc);
        return false;
    }
    a2i_ = h;
    a2i_buf_ = NULL;
    a2i_size_ = a2i_fill_ = 0;
    uint32_t primed = 0;
    while (primed < kA2iBuffers && nextA2iBuffer(false)) {
        memset(a2i_buf_, 0, a2i_size_);
        a2i_fill_ = a2i_size_;
        if (!flushA2iBuffer()) break;
        ++primed;
    }
    const int started = A2I_Client_Start(a2i_);
    rate_ = kA2iRate;
    frag_bytes_ = frag;
    printf("[Browser] A2I %s open: %u Hz, %u-byte buffers x%u, %u primed, start=%d\n", a2i_ep_, kA2iRate, frag,
           kA2iBuffers, primed, started);
    return true;
}

bool AsoundDevice::nextA2iBuffer(bool wait) {
    for (int waits = 0; waits <= kMaxWaits; ++waits) {
        void* p = NULL;
        size_t n = 0;
        const int rc = A2I_Client_GetBuffer(a2i_, &p, &n);
        if (rc == A2I_OK && p && n > 0) {
            a2i_buf_ = (uint8_t*)p;
            a2i_size_ = (uint32_t)n;
            a2i_fill_ = 0;
            return true;
        }
        if (rc != A2I_ERR_NO_BUFFER) {
            if (++write_errors_ <= 5) printf("[Browser] A2I get buffer failed: %d\n", rc);
            return false;
        }
        if (!wait) return false;
        A2I_Client_GetNotification(a2i_);
    }
    if (++write_errors_ <= 5) printf("[Browser] A2I: no buffer came free\n");
    return false;
}

bool AsoundDevice::flushA2iBuffer() {
    const int rc = A2I_Client_Out_Write(a2i_, a2i_buf_, a2i_fill_, 0);
    a2i_buf_ = NULL;
    a2i_size_ = a2i_fill_ = 0;
    if (rc != 0) {
        if (++write_errors_ <= 5) printf("[Browser] A2I write failed: %d\n", rc);
        return false;
    }
    return true;
}

bool AsoundDevice::writeA2i(const int16_t* buf, size_t frames) {
    if (!a2i_) return false;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(buf);
    size_t left = frames * kChannels * 2;
    while (left > 0) {
        if (!a2i_buf_ && !nextA2iBuffer(true)) return false;
        size_t n = a2i_size_ - a2i_fill_;
        if (n > left) n = left;
        memcpy(a2i_buf_ + a2i_fill_, src, n);
        a2i_fill_ += (uint32_t)n;
        src += n;
        left -= n;
        if (a2i_fill_ == a2i_size_ && !flushA2iBuffer()) return false;
    }
    return true;
}

void AsoundDevice::closeA2i() {
    if (!a2i_) return;
    a2i_buf_ = NULL;
    a2i_size_ = a2i_fill_ = 0;
    A2I_Client_Stop(a2i_);
    a2i_client_handle_t h = a2i_;
    A2I_Client_Close(&h);
    a2i_ = NULL;
    printf("[Browser] A2I %s closed\n", a2i_ep_);
}

} // namespace nbt
