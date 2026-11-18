// DIAGNOSTICS, TEMPORARY: see activity.h for what this is, and where it is used, before changing it.
#include "core/activity.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

#include "core/settings.h"

#ifndef ZB_VERSION
#define ZB_VERSION "unknown"
#endif

namespace activity {
namespace {

constexpr off_t kMaxBytes = 256 * 1024;
constexpr size_t kTextBytes = 4000;     // of a message's words (a reply is far shorter)
constexpr size_t kArgsBytes = 2000;     // of a tool call's arguments, when they are not JSON
std::mutex g_mtx;
std::mutex g_save_mtx;                  // the conversations folder

// One line: control characters (a result's line breaks among them) become spaces.
std::string flat(const std::string &s, size_t max_bytes) {
    std::string out = util::truncate_utf8(s, max_bytes, " …");
    for (char &c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    return out;
}

void write_line(const std::string &line) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const std::string file = path();
    util::make_dirs(util::cache_dir());
    struct stat st{};
    if (stat(file.c_str(), &st) == 0 && st.st_size > kMaxBytes) rename(file.c_str(), (file + ".1").c_str());
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    fchmod(fd, 0600);
    const std::string text = util::format_time(time(nullptr), "%Y-%m-%d %H:%M:%S ") + line + "\n";
    if (write(fd, text.data(), text.size()) < 0) { /* a log that cannot be written is not worth stopping for */ }
    close(fd);
}

// ---------------------------------------------------------------- saved conversations
std::string role_of(const json &m) { return m.is_object() ? util::jget(m, "role", std::string()) : std::string(); }

// A message's words. Content that is not text (a list of parts) is kept as its JSON, without images.
std::string text_of(const json &m) {
    const auto it = m.find("content");
    if (it == m.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    json parts = *it;
    if (parts.is_array())
        for (json &p : parts)
            if (p.is_object() && p.contains("image_url")) p["image_url"] = "(an image)";
    return parts.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string shorten(const std::string &s, size_t max_bytes) {
    return util::truncate_utf8(util::sanitize_utf8(s), max_bytes, " …");
}

// "[Wed 18 Nov 2026, 11:14] what time..." (the stamp the agent puts on each user message) gives
// "2026-11-18 11:14"; anything else gives "".
std::string stamp_time(const std::string &content) {
    if (content.empty() || content[0] != '[') return "";
    const size_t close = content.find(']');
    if (close == std::string::npos || close > 40) return "";
    int day = 0, year = 0, hour = 0, minute = 0;
    char month[8] = {0};
    if (sscanf(content.substr(1, close - 1).c_str(), "%*s %d %3s %d, %d:%d", &day, month, &year, &hour, &minute) != 5) return "";
    static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i)
        if (std::string(month) == kMonths[i]) {
            char out[32];
            snprintf(out, sizeof(out), "%04d-%02d-%02d %02d:%02d", year, i + 1, day, hour, minute);
            return out;
        }
    return "";
}

// Every text in `j` with each secret replaced by "[removed]". One pass over the original text (the
// longest secret first where several start at one place): what has been put in is never searched
// again, so a secret found inside "[removed]" itself (a key "removed") can neither loop for ever
// nor be blanked twice.
void blank_out(json &j, const std::vector<std::string> &secrets) {
    if (j.is_string()) {
        const std::string &s = j.get_ref<const std::string &>();
        if (std::none_of(secrets.begin(), secrets.end(),
                         [&](const std::string &x) { return !x.empty() && s.find(x) != std::string::npos; }))
            return;
        std::string out;
        for (size_t i = 0; i < s.size();) {
            size_t hit = 0;
            for (const std::string &secret : secrets)
                if (secret.size() > hit && s.compare(i, secret.size(), secret) == 0) hit = secret.size();
            if (hit) { out += "[removed]"; i += hit; }
            else out += s[i++];
        }
        j = out;
    } else if (j.is_structured()) {
        for (json &v : j) blank_out(v, secrets);
    }
}

struct SavedFile { std::string name; off_t bytes; };

// A name save_conversation gives a file: 2026-11-18-121314.json, or 2026-11-18-121314_02.json for
// another save in the same second. Pruning and deleting touch nothing else in the folder.
bool is_save_name(const std::string &name) {
    static const char kShape[] = "dddd-dd-dd-dddddd";
    const size_t n = sizeof(kShape) - 1;
    if (name.size() < n + 5 || name.compare(name.size() - 5, 5, ".json") != 0) return false;
    for (size_t i = 0; i < n; ++i)
        if (kShape[i] == 'd' ? !isdigit(static_cast<unsigned char>(name[i])) : name[i] != kShape[i]) return false;
    const std::string rest = name.substr(n, name.size() - 5 - n);
    if (rest.empty()) return true;
    if (rest.size() < 3 || rest[0] != '_') return false;
    for (size_t i = 1; i < rest.size(); ++i)
        if (!isdigit(static_cast<unsigned char>(rest[i]))) return false;
    return true;
}

// The saves in the folder, oldest first (their names are the times they were made).
std::vector<SavedFile> saved_files(const std::string &dir) {
    std::vector<SavedFile> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    while (const dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (!is_save_name(name)) continue;
        struct stat st{};
        if (lstat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back({name, st.st_size});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const SavedFile &a, const SavedFile &b) { return a.name < b.name; });
    return out;
}

}  // namespace

std::string path() { return util::cache_dir() + "/activity.log"; }

void request() { write_line("--- request"); }

void tool(const std::string &name, const std::string &args, const std::string &result, long ms) {
    write_line(flat(name, 40) + " " + flat(args, 400) + " -> " + flat(result, 600) + " (" + std::to_string(ms) + " ms)");
}

std::string conversations_dir() { return util::data_dir() + "/conversations"; }

json compact(const json &message) {
    if (role_of(message) != "tool") return message;
    const std::string content = text_of(message);
    if (content.size() <= kResultBytes) return message;
    json out = message;
    out["content"] = shorten(content, kResultBytes);
    out["content_bytes"] = util::jget(message, "content_bytes", content.size());
    return out;
}

json conversation_record(const json &messages, size_t dropped, time_t now, const std::vector<std::string> &secrets) {
    // Each tool result goes with its call; one whose call is not here stands alone.
    std::map<std::string, const json *> results;
    std::set<std::string> asked;
    for (const json &m : messages) {
        if (role_of(m) == "tool") results[util::jget(m, "tool_call_id", std::string())] = &m;
        if (m.is_object() && m.contains("tool_calls") && m["tool_calls"].is_array())
            for (const json &c : m["tool_calls"]) asked.insert(util::jget(c, "id", std::string()));
    }
    auto result_of = [](const json &m, json &into) {
        const std::string content = text_of(m);
        into["result"] = shorten(content, kResultBytes);
        const size_t bytes = util::jget(m, "content_bytes", content.size());
        if (bytes > kResultBytes) into["result_bytes"] = bytes;
    };

    std::vector<json> out;
    for (const json &m : messages) {
        const std::string role = role_of(m);
        if (role.empty()) continue;
        if (role == "tool") {
            if (asked.count(util::jget(m, "tool_call_id", std::string()))) continue;
            json e = {{"role", "tool"}};
            result_of(m, e);
            out.push_back(std::move(e));
            continue;
        }
        json e = {{"role", role}};
        const std::string content = text_of(m);
        if (role == "user") {
            const std::string when = stamp_time(content);
            if (!when.empty()) e["time"] = when;
        }
        e["content"] = shorten(content, kTextBytes);
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            std::vector<json> calls;
            for (const json &c : m["tool_calls"]) {
                const json fn = c.is_object() ? c.value("function", json::object()) : json::object();
                json call = {{"tool", util::jget(fn, "name", std::string())}};
                const std::string raw = util::jget(fn, "arguments", std::string());
                json args = json::parse(util::trim(raw).empty() ? "{}" : raw, nullptr, false);
                call["arguments"] = args.is_discarded() ? json(shorten(raw, kArgsBytes)) : args;
                const auto r = results.find(util::jget(c, "id", std::string()));
                if (r != results.end()) result_of(*r->second, call);
                else call["pending"] = true;   // saved while the tool was still running
                calls.push_back(std::move(call));
            }
            e["tool_calls"] = calls;
        }
        out.push_back(std::move(e));
    }

    // One save stays under kMaxFileBytes as written (dump(1)): the oldest messages go first, back to
    // a user's message. Each entry's size counts the two spaces of indentation its lines get in the file.
    std::vector<size_t> sizes;
    size_t total = 0;
    for (const json &e : out) {
        const std::string text = e.dump(1, ' ', false, json::error_handler_t::replace);
        total += sizes.emplace_back(text.size() + 2 * static_cast<size_t>(std::count(text.begin(), text.end(), '\n') + 1) + 2);
    }
    size_t first = 0;
    auto drop_turn = [&] {
        do { total -= sizes[first]; ++first; ++dropped; } while (first < out.size() && role_of(out[first]) != "user");
    };
    while (first < out.size() && total > kMaxFileBytes - 1024) drop_turn();
    std::vector<std::string> real;
    for (const std::string &s : secrets)
        if (util::trim(s).size() >= 6) real.push_back(util::trim(s));
    for (;;) {
        json record = {{"format", 1},
                       {"saved_at", util::format_time(now, "%Y-%m-%d %H:%M:%S")},
                       {"earlier_messages_not_kept", dropped},
                       {"messages", std::vector<json>(out.begin() + static_cast<long>(first), out.end())}};
        blank_out(record, real);
        // The reckoning above is close; should it fall short, one more turn goes.
        if (first >= out.size() || record.dump(1, ' ', false, json::error_handler_t::replace).size() <= kMaxFileBytes - 512)
            return record;
        drop_turn();
    }
}

SaveResult save_conversation(const json &messages, size_t dropped) {
    SaveResult r;
    bool said = false;
    for (const json &m : messages) said = said || role_of(m) == "user";
    if (!said) { r.nothing = true; return r; }
    const Settings &s = Settings::get();
    const time_t now = time(nullptr);
    json record = conversation_record(messages, dropped, now, {s.api_key, s.search_key});
    record["app_version"] = ZB_VERSION;
    record["fast_mode"] = s.fast_mode;
    record["time_zone"] = s.timezone.empty() ? util::system_timezone() : s.timezone;

    std::lock_guard<std::mutex> lk(g_save_mtx);
    const std::string dir = conversations_dir();
    util::make_dirs(dir);
    chmod(dir.c_str(), 0700);
    // Named by the time. Another save in the same second gets _02, _03... after the newest of that
    // second, so that the names sort in the order the saves were made, even after pruning.
    const std::string stem = util::format_time(now, "%Y-%m-%d-%H%M%S");
    int next = 1;
    for (const SavedFile &f : saved_files(dir)) {
        if (f.name.compare(0, stem.size(), stem) != 0) continue;
        if (f.name == stem + ".json") next = std::max(next, 2);
        else if (f.name.size() > stem.size() + 1 && f.name[stem.size()] == '_') next = std::max(next, atoi(f.name.c_str() + stem.size() + 1) + 1);
    }
    char suffix[16] = "";
    if (next > 1) snprintf(suffix, sizeof(suffix), "_%02d", next);
    const std::string file = dir + "/" + stem + suffix + ".json";
    if (!util::write_file_atomic(file, record.dump(1, ' ', false, json::error_handler_t::replace), 0600)) {
        r.error = "the file could not be written";
        return r;
    }
    r.path = file;
    // The oldest go first, never the one just written.
    const std::vector<SavedFile> files = saved_files(dir);
    size_t count = files.size(), bytes = 0;
    for (const SavedFile &f : files) bytes += static_cast<size_t>(f.bytes);
    for (size_t i = 0; i < files.size() && (count > kMaxSaved || bytes > kMaxSavedBytes); ++i) {
        if (dir + "/" + files[i].name == file) continue;
        if (unlink((dir + "/" + files[i].name).c_str()) == 0) {
            bytes -= static_cast<size_t>(files[i].bytes);
            --count;
        }
    }
    return r;
}

std::string saved_toast(const SaveResult &r) {
    if (r.nothing) return "Nothing to save yet";
    if (!r.error.empty()) return "Could not save the conversation";
    return "Conversation saved";
}

size_t saved_count() {
    std::lock_guard<std::mutex> lk(g_save_mtx);
    return saved_files(conversations_dir()).size();
}

size_t delete_saved() {
    std::lock_guard<std::mutex> lk(g_save_mtx);
    const std::string dir = conversations_dir();
    size_t n = 0;
    for (const SavedFile &f : saved_files(dir)) n += unlink((dir + "/" + f.name).c_str()) == 0;
    // Half-written files from a save that was cut short, too (write_file_atomic's NAME.tmp).
    if (DIR *d = opendir(dir.c_str())) {
        std::vector<std::string> rest;
        while (const dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0 && is_save_name(name.substr(0, name.size() - 4)))
                rest.push_back(name);
        }
        closedir(d);
        for (const std::string &name : rest) unlink((dir + "/" + name).c_str());
    }
    rmdir(dir.c_str());
    return n;
}

}  // namespace activity
