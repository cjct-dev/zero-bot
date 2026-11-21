// Scans the music folder, reads tags and answers "play something by X" style queries.
#pragma once
#include <mutex>
#include <string>
#include <vector>

struct Track {
    std::string path, title, artist, album, genre;
    int year = 0;
};

class MusicLibrary {
public:
    static MusicLibrary &get();
    // Loads the cached index, rescanning when the folder contents changed (or force is set).
    void refresh(const std::string &dir, bool force = false);
    size_t size();
    std::vector<Track> all();
    // Any field may be empty. `query` matches loosely across every field.
    std::vector<Track> find(const std::string &query, const std::string &artist, const std::string &genre,
                            const std::string &title, const std::string &album);
    std::vector<std::string> genres();     // distinct, most common first
    std::vector<std::string> artists();
    Track lookup(const std::string &path);

private:
    std::mutex mtx_;
    std::vector<Track> tracks_;
};
