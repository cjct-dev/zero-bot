// Audio compression for slow links. The codec libraries are loaded at run time, so the app
// still works (sending and receiving uncompressed audio) on a system that lacks them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace codec {

// Encodes mono 16-bit PCM as MP3. Returns false if the encoder (libmp3lame) is unavailable.
bool mp3_encode(const std::vector<int16_t> &pcm, int sample_rate, int kbps, std::string &mp3);

// Decodes an Ogg Opus file held in memory to mono 16-bit PCM at 48 kHz. Returns false if the
// decoder (libopusfile) is unavailable or the data is not valid Opus.
bool opus_decode(const std::string &ogg, std::vector<int16_t> &pcm);
constexpr int kOpusRate = 48000;

bool can_encode_mp3();
bool can_decode_opus();

}  // namespace codec
