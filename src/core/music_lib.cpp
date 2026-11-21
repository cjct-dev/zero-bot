#include "core/music_lib.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <sys/stat.h>

#include "core/util.h"

namespace {

const char *kId3v1Genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age",
    "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
    "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock",
    "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
    "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
    "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House",
    "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror", "Indie", "BritPop", "Afro-Punk",
    "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop",
};
const int kGenreCount = sizeof(kId3v1Genres) / sizeof(kId3v1Genres[0]);

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes an ID3 text frame body (first byte is the encoding).
std::string decode_text(const std::string &raw) {
    if (raw.empty()) return "";
    const unsigned char enc = static_cast<unsigned char>(raw[0]);
    const std::string body = raw.substr(1);
    std::string out;
    if (enc == 0) {
        for (unsigned char c : body) { if (!c) break; append_utf8(out, c); }
    } else if (enc == 3) {
        out = body.substr(0, body.find('\0'));
    } else {  // UTF-16 with BOM (1) or big-endian without (2)
        bool big = enc == 2;
        size_t i = 0;
        if (body.size() >= 2) {
            const unsigned char b0 = static_cast<unsigned char>(body[0]), b1 = static_cast<unsigned char>(body[1]);
            if (b0 == 0xFF && b1 == 0xFE) { big = false; i = 2; }
            else if (b0 == 0xFE && b1 == 0xFF) { big = true; i = 2; }
        }
        for (; i + 1 < body.size(); i += 2) {
            const unsigned char a = static_cast<unsigned char>(body[i]), b = static_cast<unsigned char>(body[i + 1]);
            uint32_t cu = big ? (a << 8 | b) : (b << 8 | a);
            if (!cu) break;
            if (cu >= 0xD800 && cu <= 0xDBFF && i + 3 < body.size()) {
                const unsigned char c = static_cast<unsigned char>(body[i + 2]), d = static_cast<unsigned char>(body[i + 3]);
                const uint32_t lo = big ? (c << 8 | d) : (d << 8 | c);
                cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            append_utf8(out, cu);
        }
    }
    return util::trim(util::sanitize_utf8(out));
}

std::string clean_genre(std::string g) {
    g = util::trim(g);
    // "(17)", "(17)Rock" and bare "17" all refer to the ID3v1 genre table.
    if (!g.empty() && g[0] == '(') {
        const size_t close = g.find(')');
        if (close != std::string::npos) {
            const std::string rest = util::trim(g.substr(close + 1));
            if (!rest.empty()) return rest;
            g = g.substr(1, close - 1);
        }
    }
    if (!g.empty() && std::all_of(g.begin(), g.end(), ::isdigit)) {
        const int idx = atoi(g.c_str());
        if (idx >= 0 && idx < kGenreCount) return kId3v1Genres[idx];
        return "";
    }
    return g;
}

uint32_t be32(const unsigned char *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint32_t synchsafe(const unsigned char *p) { return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3]; }

void read_id3v2(FILE *fp, Track &t) {
    unsigned char hdr[10];
    if (fread(hdr, 1, 10, fp) != 10 || memcmp(hdr, "ID3", 3) != 0) return;
    const int ver = hdr[3];
    const long tag_end = 10 + static_cast<long>(synchsafe(hdr + 6));
    if (hdr[5] & 0x40) {  // extended header
        unsigned char ext[4];
        if (fread(ext, 1, 4, fp) != 4) return;
        const uint32_t ext_size = ver == 4 ? synchsafe(ext) : be32(ext) + 4;
        fseek(fp, static_cast<long>(ext_size) - 4, SEEK_CUR);
    }
    while (ftell(fp) + (ver == 2 ? 6 : 10) <= tag_end) {
        unsigned char fh[10];
        char id[5] = {0};
        uint32_t size;
        if (ver == 2) {
            if (fread(fh, 1, 6, fp) != 6 || !fh[0]) break;
            memcpy(id, fh, 3);
            size = (fh[3] << 16) | (fh[4] << 8) | fh[5];
        } else {
            if (fread(fh, 1, 10, fp) != 10 || !fh[0]) break;
            memcpy(id, fh, 4);
            size = ver == 4 ? synchsafe(fh + 4) : be32(fh + 4);
        }
        if (size == 0 || ftell(fp) + static_cast<long>(size) > tag_end) break;
        const std::string fid = id;
        std::string *dest = nullptr;
        std::string year;
        if (fid == "TIT2" || fid == "TT2") dest = &t.title;
        else if (fid == "TPE1" || fid == "TP1") dest = &t.artist;
        else if (fid == "TALB" || fid == "TAL") dest = &t.album;
        else if (fid == "TCON" || fid == "TCO") dest = &t.genre;
        else if (fid == "TYER" || fid == "TYE" || fid == "TDRC") dest = &year;
        if (dest && size < 2048) {
            std::string raw(size, '\0');
            if (fread(&raw[0], 1, size, fp) != size) break;
            *dest = decode_text(raw);
            if (dest == &year) t.year = atoi(year.c_str());
        } else {
            fseek(fp, static_cast<long>(size), SEEK_CUR);  // skips pictures and anything else
        }
    }
}

void read_id3v1(FILE *fp, Track &t) {
    char tag[128];
    if (fseek(fp, -128, SEEK_END) != 0 || fread(tag, 1, 128, fp) != 128 || memcmp(tag, "TAG", 3) != 0) return;
    auto field = [&](int off, int len) {
        std::string s(tag + off, static_cast<size_t>(len));
        s = s.substr(0, s.find('\0'));
        std::string out;
        for (unsigned char c : s) append_utf8(out, c);
        return util::trim(out);
    };
    if (t.title.empty()) t.title = field(3, 30);
    if (t.artist.empty()) t.artist = field(33, 30);
    if (t.album.empty()) t.album = field(63, 30);
    if (!t.year) t.year = atoi(field(93, 4).c_str());
    const unsigned char g = static_cast<unsigned char>(tag[127]);
    if (t.genre.empty() && g < kGenreCount) t.genre = kId3v1Genres[g];
}

std::string title_from_filename(const std::string &path) {
    std::string name = path.substr(path.rfind('/') + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // Drop a leading track number such as "03 - " or "03. ".
    size_t i = 0;
    while (i < name.size() && isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i > 0 && i < name.size()) {
        size_t j = i;
        while (j < name.size() && (name[j] == ' ' || name[j] == '-' || name[j] == '.' || name[j] == '_')) ++j;
        if (j > i) name = name.substr(j);
    }
    return util::sanitize_utf8(name);
}

bool is_audio(const std::string &name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = util::lower(name.substr(dot + 1));
    return ext == "mp3" || ext == "flac" || ext == "ogg" || ext == "m4a" || ext == "wav" || ext == "opus" || ext == "aac";
}

void walk(const std::string &dir, int depth, std::vector<std::string> &out) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (dirent *de = readdir(d)) {
        if (de->d_name[0] == '.') continue;
        const std::string path = dir + "/" + de->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) walk(path, depth + 1, out); }
        else if (S_ISREG(st.st_mode) && is_audio(de->d_name)) out.push_back(path);
    }
    closedir(d);
}

Track read_track(const std::string &path) {
    Track t;
    t.path = path;
    if (FILE *fp = fopen(path.c_str(), "rb")) {
        read_id3v2(fp, t);
        read_id3v1(fp, t);
        fclose(fp);
    }
    t.genre = clean_genre(t.genre);
    if (t.title.empty()) t.title = title_from_filename(path);
    return t;
}

std::string index_path() { return util::cache_dir() + "/music_index.json"; }

}  // namespace

MusicLibrary &MusicLibrary::get() {
    static MusicLibrary m;
    return m;
}

void MusicLibrary::refresh(const std::string &dir, bool force) {
    std::vector<std::string> files;
    walk(dir, 0, files);
    std::sort(files.begin(), files.end());

    // The cache is valid when it was built from exactly this set of files.
    std::vector<Track> tracks;
    const json cached = util::load_json(index_path(), json::object());
    bool reuse = !force && util::jget(cached, "dir", std::string()) == dir && cached.contains("tracks") &&
                 cached["tracks"].is_array() && cached["tracks"].size() == files.size();
    if (reuse) {
        size_t i = 0;
        for (auto &j : cached["tracks"]) {
            Track t;
            t.path = util::jget(j, "path", std::string());
            if (t.path != files[i++]) { reuse = false; break; }
            t.title = util::jget(j, "title", std::string());
            t.artist = util::jget(j, "artist", std::string());
            t.album = util::jget(j, "album", std::string());
            t.genre = util::jget(j, "genre", std::string());
            t.year = util::jget(j, "year", 0);
            tracks.push_back(std::move(t));
        }
    }
    if (!reuse) {
        tracks.clear();
        json arr = json::array();
        for (auto &f : files) {
            Track t = read_track(f);
            arr.push_back({{"path", t.path}, {"title", t.title}, {"artist", t.artist}, {"album", t.album},
                           {"genre", t.genre}, {"year", t.year}});
            tracks.push_back(std::move(t));
        }
        util::save_json(index_path(), {{"dir", dir}, {"tracks", arr}});
    }
    std::lock_guard<std::mutex> lk(mtx_);
    tracks_ = std::move(tracks);
}

size_t MusicLibrary::size() {
    std::lock_guard<std::mutex> lk(mtx_);
    return tracks_.size();
}

std::vector<Track> MusicLibrary::all() {
    std::lock_guard<std::mutex> lk(mtx_);
    return tracks_;
}

static bool field_matches(const std::string &value, const std::string &want) {
    return want.empty() || util::contains_ci(value, want);
}

std::vector<Track> MusicLibrary::find(const std::string &query, const std::string &artist, const std::string &genre,
                                      const std::string &title, const std::string &album) {
    std::vector<std::string> words;
    for (auto &w : util::split(util::lower(query), ' '))
        if (!w.empty()) words.push_back(w);
    std::vector<Track> out;
    for (auto &t : all()) {
        if (!field_matches(t.artist, artist) || !field_matches(t.genre, genre) || !field_matches(t.title, title) ||
            !field_matches(t.album, album))
            continue;
        const std::string hay = util::lower(t.title + " " + t.artist + " " + t.album + " " + t.genre);
        bool ok = true;
        for (auto &w : words)
            if (hay.find(w) == std::string::npos) { ok = false; break; }
        if (ok) out.push_back(t);
    }
    return out;
}

static std::vector<std::string> ranked(const std::map<std::string, int> &counts) {
    std::vector<std::pair<std::string, int>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
    std::vector<std::string> out;
    for (auto &p : v) out.push_back(p.first);
    return out;
}

std::vector<std::string> MusicLibrary::genres() {
    std::map<std::string, int> counts;
    for (auto &t : all())
        if (!t.genre.empty()) ++counts[t.genre];
    return ranked(counts);
}

std::vector<std::string> MusicLibrary::artists() {
    std::map<std::string, int> counts;
    for (auto &t : all())
        if (!t.artist.empty()) ++counts[t.artist];
    return ranked(counts);
}

Track MusicLibrary::lookup(const std::string &path) {
    for (auto &t : all())
        if (t.path == path) return t;
    Track t;
    t.path = path;
    t.title = title_from_filename(path);
    return t;
}
