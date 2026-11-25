#include "ui/art/pick.h"

#include <algorithm>
#include <cstdint>

#include "core/util.h"

namespace art {

namespace {

// A family of genres: its scene, every ID3v1 name that belongs to it, and the words that put any other text in it.
// In the order their words are tried.
struct Family {
    const char *scene;
    std::vector<const char *> names, words;
};

const std::vector<Family> &families() {
    static const std::vector<Family> all = {
        {"city",
         {"New Age", "Ambient", "Trip-Hop", "Space", "Meditative", "Dream", "Lo-Fi", "Easy Listening", "Acoustic"},
         {"chill", "lo fi", "lofi", "ambient", "downtempo", "sleep", "study"}},
        {"jazz",
         {"Blues", "Funk", "Jazz", "R&B", "Jazz+Funk", "Fusion", "Gospel", "Soul", "Cabaret", "Acid Jazz", "Swing",
          "Fast Fusion", "Bebop", "Big Band", "Chanson", "Slow Jam", "Rhythmic Soul"},
         {"jazz", "blues", "soul", "funk", "swing", "bebop", "r&b", "rnb", "gospel", "lounge", "motown"}},
        {"rock",
         {"Classic Rock", "Grunge", "Metal", "Alternative", "Death Metal", "Alternative Rock", "Punk", "Instrumental Rock",
          "Gothic", "Southern Rock", "Psychedelic", "Acid Punk", "Rock & Roll", "Hard Rock", "Folk-Rock", "Gothic Rock",
          "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Rock", "Primus", "Power Ballad",
          "Punk Rock", "Drum Solo", "Indie", "BritPop", "Afro-Punk", "Polsk Punk", "Heavy Metal", "Black Metal",
          "Crossover", "Christian Rock", "Thrash Metal"},
         {"rock", "metal", "punk", "grunge", "emo", "shoegaze", "indie"}},
        {"electronic",
         {"Dance", "Techno", "Industrial", "Euro-Techno", "Trance", "Acid", "House", "Game", "Noise", "Bass", "Darkwave",
          "Techno-Industrial", "Electronic", "Eurodance", "Jungle", "New Wave", "Rave", "Club", "Euro-House", "Goa",
          "Drum & Bass", "Club-House", "Hardcore", "Terror", "Synthpop"},
         {"electro", "techno", "house", "trance", "edm", "dubstep", "drum and bass", "drum & bass", "dnb", "synth",
          "rave", "idm", "breakbeat", "garage", "chiptune", "retrowave", "vaporwave", "outrun"}},
        {"classical",
         {"Classical", "Soundtrack", "Avantgarde", "Chorus", "Opera", "Chamber Music", "Sonata", "Symphony"},
         {"classical", "orchestra", "symphon", "opera", "baroque", "choral", "concerto", "sonata", "chamber",
          "piano solo", "score"}},
        {"pop",
         {"Country", "Disco", "Hip-Hop", "Oldies", "Other", "Pop", "Rap", "Reggae", "Ska", "Pranks", "Vocal",
          "Instrumental", "Sound Clip", "Instrumental Pop", "Ethnic", "Pop-Folk", "Comedy", "Cult", "Gangsta", "Top 40",
          "Christian Rap", "Pop/Funk", "Native American", "Showtunes", "Trailer", "Tribal", "Polka", "Retro", "Musical",
          "Folk", "National Folk", "Latin", "Revival", "Celtic", "Bluegrass", "Humour", "Speech", "Booty Bass",
          "Porn Groove", "Satire", "Tango", "Samba", "Folklore", "Ballad", "Freestyle", "Duet", "A Cappella",
          "Dance Hall", "Beat", "Christian Gangsta Rap", "Contemporary Christian", "Merengue", "Salsa", "Anime", "JPop"},
         {}},
    };
    return all;
}

// Lower case, "-", "_" and "." as spaces, words one space apart.
std::string normal(const std::string &text) {
    std::string out;
    for (char c : util::lower(text)) {
        if (c == '-' || c == '_' || c == '.') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

std::string scene_for_genre(const std::string &genre) {
    const std::string said = normal(genre);
    for (const Family &family : families())
        for (const char *name : family.names)
            if (said == normal(name)) return family.scene;
    for (const Family &family : families())
        for (const char *word : family.words)
            if (said.find(word) != std::string::npos) return family.scene;
    return "pop";
}

std::string scene_for_station(const std::vector<std::string> &tags) {
    static const char *const kCalm[] = {"focus", "lofi", "study", "chill", "ambient", "downtempo"};
    for (const std::string &tag : tags)
        if (std::find(std::begin(kCalm), std::end(kCalm), util::lower(tag)) != std::end(kCalm)) return "city";
    return "radio";
}

std::string Weather::follow(const std::string &scene, unsigned long now, time_t clock) {
    const bool city = scene == "city";
    if (playing_) played_ += now - last_;
    last_ = now;
    if (city && !playing_ && (!chosen_ || played_ >= kKeepMs)) {
        const uint32_t minute = static_cast<uint32_t>(clock / 60) * 2654435761u;   // scrambled: minutes in a row differ
        rain_ = (minute >> 16) % 3 == 0;
        chosen_ = true;
        played_ = 0;
    }
    playing_ = city;
    return city && rain_ ? "city_rain" : scene;
}

}  // namespace art
