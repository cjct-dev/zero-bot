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

// Decodes an MP3 file to interleaved 16-bit PCM at the file's own rate, with libmpg123 loaded at
// run time (about 300 KB, against the 50 MB an ffplay process holds on a device with 350 MB).
class Mp3File {
public:
    Mp3File() = default;
    Mp3File(const Mp3File &) = delete;
    Mp3File &operator=(const Mp3File &) = delete;
    ~Mp3File();
    // false: no libmpg123, the file cannot be read, or no MP3 frame in its first megabyte.
    bool open(const std::string &path);
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    // Up to `max` bytes of samples; 0 at the end of the file (or where its format changes), -1 on an error.
    long read(unsigned char *out, size_t max);

private:
    bool feed();   // hands the decoder the next part of the file; false at its end
    void *mh_ = nullptr;
    int fd_ = -1;
    int rate_ = 0, channels_ = 0;
    bool input_done_ = false;
    std::vector<unsigned char> pending_;   // decoded before the format was known, still to hand out
};

}  // namespace codec
