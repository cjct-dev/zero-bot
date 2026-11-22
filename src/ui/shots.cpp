// --shots DIR: every screen drawn to a PNG, with the checks that need the real screen code (a key on
// a dark screen only wakes it, labels fit, the status bar redraws only what changed, ...). Exits 1
// when a check fails. tests/sim/screens/shots_compare.py runs it and compares the pictures with the stored ones.
#include "ui/app_impl.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "agent/agent.h"
#include "core/activity.h"
#include "core/radio.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/timers.h"
#include "core/ui_queue.h"
#include "core/util.h"
#include "lvgl.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/sys.h"
#include "smarthome/home.h"
#include "smarthome/internal.h"   // testing::set_network_source: a slow first look at the network
#include "ui/key_guide.h"
#include "ui/models.h"
#include "ui/settings_pages.h"
#include "ui/theme.h"

namespace ui {

namespace {

// The stand-in test network as a slow first look at the real one would find it (the Smart home check
// in shots_smart_home): the same network, after a second and a half.
discovery::Route slow_route() { return {"test", "127.0.0.1"}; }
std::string slow_router_mac(const std::string &) { return "02:00:00:00:00:00"; }
discovery::Network slow_look() {
    usleep(1500 * 1000);
    discovery::Network net;
    net.iface = "test";
    net.ssid = "Test network";
    net.address = net.gateway = "127.0.0.1";
    net.broadcast = "127.255.255.255";
    net.gateway_mac = "02:00:00:00:00:00";
    net.usable = true;
    return net;
}

// The same through another interface, for the check of the Your data page, which comes first: the
// look it leaves in home::network_now's memory does not answer the Smart home page's check (another
// route is another network, looked at afresh).
discovery::Route slow_route_data() { return {"test-data", "127.0.0.1"}; }
discovery::Network slow_look_data() {
    discovery::Network net = slow_look();
    net.iface = "test-data";
    return net;
}

// And through a third, for the check of a device's page, which comes after the Smart home page's.
discovery::Route slow_route_device() { return {"test-device", "127.0.0.1"}; }
discovery::Network slow_look_device() {
    discovery::Network net = slow_look();
    net.iface = "test-device";
    return net;
}

// Delivers what slow work posts to the UI queue, which --shots drains itself, until `shown` holds
// (up to 4 s); whether it does.
bool arrives(const std::function<bool()> &shown) {
    for (int i = 0; i < 200 && !shown(); ++i) {
        uiq::drain();
        usleep(20 * 1000);
    }
    return shown();
}

// Waits (up to 3 s) until an answer from slow work waits in the UI queue, undelivered: a check can
// then move on in the menu first, and deliver it after with uiq::drain().
bool answer_waiting() {
    for (int i = 0; i < 150 && uiq::pending() == 0; ++i) usleep(20 * 1000);
    return uiq::pending() > 0;
}

// A stand-in AI service on 127.0.0.1, at a port the system picks, for as long as it lives: every request is
// answered with a list of one chat model, and noted as the path it asked for and the key it carried
// ("/a/v1/models key-a").
class StandInService {
public:
    StandInService() {
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(at);
        if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr *>(&at), len) != 0 || listen(fd_, 4) != 0 ||
            getsockname(fd_, reinterpret_cast<sockaddr *>(&at), &len) != 0)
            return;
        port_ = ntohs(at.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~StandInService() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
    }
    std::string address(const std::string &path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    // The requests heard, once there are `n` (waiting up to 3 s).
    std::vector<std::string> heard(size_t n) {
        for (int i = 0; i < 150 && heard_count() < n; ++i) usleep(20 * 1000);
        std::lock_guard<std::mutex> lk(mtx_);
        return heard_;
    }

private:
    // What follows `start` in `text`, up to `end`; "" without `start`.
    static std::string between(const std::string &text, const std::string &start, const std::string &end) {
        const size_t at = text.find(start);
        if (at == std::string::npos) return "";
        const size_t from = at + start.size();
        return text.substr(from, text.find(end, from) - from);
    }
    size_t heard_count() {
        std::lock_guard<std::mutex> lk(mtx_);
        return heard_.size();
    }
    void serve() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            const int peer = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (peer < 0) continue;
            answer(peer);
            close(peer);
        }
    }
    void answer(int peer) {
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            pollfd p{peer, POLLIN, 0};
            const ssize_t n = poll(&p, 1, 2000) > 0 ? read(peer, buf, sizeof(buf)) : -1;
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            heard_.push_back(between(req, " ", " ") + " " + between(req, "\r\nAuthorization: Bearer ", "\r\n"));
        }
        const std::string body = R"({"data": [{"id": "chatty", "mode": "chat"}]})";
        const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\n\r\n" + body;
        send(peer, reply.data(), reply.size(), MSG_NOSIGNAL);
    }

    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mtx_;
    std::vector<std::string> heard_;
};

// Presses ENTER on the row under the cursor while the network is found only after a slow first look
// (`slow`): opening a page never waits for the network on the UI thread. True when the key came back
// at once with the page `title` up and still without what `shown` looks for, which then came with
// the answer (up to 4 s).
bool opens_at_once(Menu &menu, const home::testing::NetworkSource &slow, const std::string &title,
                   const std::function<bool()> &shown) {
    unsetenv("ZB_TEST_NETWORK");
    home::testing::set_network_source(&slow);
    KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
    const unsigned long t0 = sys::now_ms();
    menu.key(enter);
    const unsigned long took = sys::now_ms() - t0;
    const bool looking = menu.title() == title && !shown();
    const bool listed = arrives(shown);
    home::testing::set_network_source(nullptr);
    setenv("ZB_TEST_NETWORK", "1", 1);
    if (took > 500 || !looking || !listed)
        fprintf(stderr, "opening %s waited for the network on the UI thread (%lu ms; looking %d, listed %d)\n", title.c_str(),
                took, looking, listed);
    return took <= 500 && looking && listed;
}

// Decoded images are kept for redraws (LV_CACHE_DEF_SIZE), and one the cache cannot hold is not drawn
// at all: a PNG the size of the screen, far larger than a photo's thumbnail, is still drawn.
bool image_decodes() {
    const std::string path = util::cache_dir() + "/shots-screen.png";
    const bool saved = display::save_png(path);   // opaque everywhere
    lv_obj_t *img = lv_image_create(lv_screen_active());
    lv_image_set_src(img, ("A:" + path).c_str());
    lv_draw_buf_t *snap = saved ? lv_snapshot_take(img, LV_COLOR_FORMAT_ARGB8888) : nullptr;
    bool drawn = snap && snap->header.w == display::kWidth && snap->header.h == display::kHeight;
    for (uint32_t y = 0; drawn && y < snap->header.h; ++y)
        for (uint32_t x = 0; drawn && x < snap->header.w; ++x)
            drawn = snap->data[y * snap->header.stride + x * 4 + 3] != 0;
    if (snap) lv_draw_buf_destroy(snap);
    lv_obj_delete(img);
    unlink(path.c_str());
    if (!drawn) fprintf(stderr, "an image the size of the screen was not drawn (saved %d)\n", saved);
    return drawn;
}

// A made-up home for the screenshots, saved on the stand-in test network: none of its addresses
// is ever contacted, and nothing is scanned.
void sample_home() {
    setenv("ZB_TEST_NETWORK", "1", 1);
    const discovery::Network net = home::network_now(true);
    auto plug = [](const char *id, const char *name, const char *room, bool on, bool ask) {
        return json{{"id", id}, {"name", name}, {"room", room}, {"type", "plug"}, {"caps", home::kPower}, {"ask_first", ask},
                    {"ref", {{"child", std::string(id) == "main" ? "" : id}}}, {"state", {{"on", on}}}};
    };
    const json plug_info = {{"scheme", "KLAP"}, {"device_type", "SMART.TAPOPLUG"}};
    json devices = json::array();
    devices.push_back({{"id", "tapo:sample-1"}, {"kind", "tapo"}, {"name", "Bed heater"}, {"model", "P110"}, {"last_ip", "127.0.0.2"},
                       {"port", 80}, {"info", plug_info}, {"things", {plug("main", "Bed heater", "Bedroom", false, true)}}});
    devices.push_back({{"id", "tapo:sample-2"}, {"kind", "tapo"}, {"name", "Office strip"}, {"model", "P304M"},
                       {"last_ip", "127.0.0.3"}, {"port", 80}, {"info", plug_info},
                       {"things", {plug("s1", "Desk lamp", "Office", true, false), plug("s2", "Monitor", "Office", false, false),
                                   plug("s3", "Fan", "", false, false), plug("s4", "Kettle", "", false, true)}}});
    json other_owner = plug_info;
    other_owner["owner"] = "00112233445566778899AABBCCDDEEFF";
    devices.push_back({{"id", "tapo:sample-3"}, {"kind", "tapo"}, {"name", "Tapo P110"}, {"model", "P110"}, {"last_ip", "127.0.0.4"},
                       {"port", 80}, {"info", other_owner}, {"needs_setup", true}, {"things", json::array()}});
    // A strip whose last answer listed no sockets: still on the page, under "To set up".
    json pump = plug("s1", "Pond pump", "Garden", false, false);
    pump["gone"] = 1;
    devices.push_back({{"id", "tapo:sample-4"}, {"kind", "tapo"}, {"name", "Garden strip"}, {"model", "P300"}, {"last_ip", "127.0.0.7"},
                       {"port", 80}, {"info", plug_info}, {"things", json::array({pump})}});
    devices.push_back({{"id", "hue:sample"}, {"kind", "hue_bridge"}, {"name", "Hue Bridge"}, {"model", "BSB002"}, {"last_ip", "127.0.0.5"}});
    devices.push_back({{"id", "cast:sample"}, {"kind", "cast"}, {"name", "Living room TV"}, {"model", "Chromecast"}, {"last_ip", "127.0.0.6"}});
    const json root = {{"version", 2},
                       {"secrets", {{"tapo", {{"email", "sam@example.com"}, {"password_sha1", std::string(40, '0')}}}}},
                       {"networks", {{net.id(), {{"label", net.label()}, {"gateway_mac", net.gateway_mac}, {"allowed_at", 1},
                                                 {"last_scan", 1}, {"devices", devices}}}}}};
    util::save_json(util::data_dir() + "/smarthome.json", root);
}

// The labels of the rows on the page on top, read by moving the cursor down to its last row.
std::vector<std::string> page_rows(Menu &menu) {
    KeyEvent key; key.key = Key::Down; key.state = 1;
    std::vector<std::string> rows{menu.selected()};
    for (int i = 0; i < 80; ++i) {
        menu.key(key);
        if (menu.selected() == rows.back()) break;
        rows.push_back(menu.selected());
    }
    return rows;
}

// "Start at power-on" was removed (ZB-085): its row must not come back, on the Device page or in the guide.
bool without_power_on(Menu &menu, const char *page, std::initializer_list<const char *> expected) {
    const std::vector<std::string> rows = page_rows(menu);
    bool ok = std::find(rows.begin(), rows.end(), "Start at power-on") == rows.end();
    for (const char *row : expected) ok = ok && std::find(rows.begin(), rows.end(), row) != rows.end();
    if (!ok) fprintf(stderr, "the %s page lists \"Start at power-on\", or its rows could not be read\n", page);
    return ok;
}

// The screenshots change settings in memory only.
void set(const std::function<void(Settings &)> &change) { Settings::update(change, false); }

// Moves the cursor of the page on top to the row with this label, one Up or Down press at a time as
// a user would (the screens come out as they did from counted presses), wherever the row is: a row
// added to a page shifts no later screen. False, with nothing pressed, when the page has no such row.
bool move_to(Menu &menu, const std::string &label) {
    const int at = menu.row_of(label);
    if (at < 0) return false;
    KeyEvent key;
    key.key = at < menu.cursor() ? Key::Up : Key::Down;
    key.state = 1;
    for (int i = 0; i < 200 && menu.cursor() != at; ++i) menu.key(key);
    return menu.cursor() == at;
}

}  // namespace

// What the groups of screens share: where the pictures go and how many there are so far, whether
// a check has failed, and the sample data the pages show.
struct Shots {
    App &app;
    std::string dir;
    int n = 0;
    bool failed = false;
    // Timers, at fixed times so that the images come out the same on every run.
    time_t t0 = 1800000000;
    int food = 0;   // the timer "Food out of the oven" (shots_sample_data), which the Reminders page sees end

    void snap(const char *name) {
        for (int i = 0; i < 6; ++i) { app.chat.tick(); app.menu.tick(); }
        lv_refr_now(nullptr);
        char path[512];
        snprintf(path, sizeof(path), "%s/%02d-%s.png", dir.c_str(), ++n, name);
        if (!display::save_png(path)) fprintf(stderr, "could not write %s\n", path);
        else printf("%s\n", path);
    }

    // move_to, and a failed run when the page on top has no such row.
    void menu_to(const std::string &label) {
        if (move_to(app.menu, label)) return;
        fprintf(stderr, "the page \"%s\" has no row \"%s\" to move to\n", app.menu.title().c_str(), label.c_str());
        failed = true;
    }

    // menu_to, then ENTER.
    void press(const std::string &label) {
        menu_to(label);
        KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
        app.menu.key(enter);
    }
};

namespace {

// Only chat models are chosen without the user (ZB-088; ui/models.h): from a service that labels its
// models, the one labelled "chat", even with an unlabelled one listed first; from one that labels none,
// nothing (while settings list every model, for the user to choose); from one with no chat model, no
// language model (settings list the unlabelled ones). A model already chosen stays. Checked, not pictured.
bool models_chosen_by_rule() {
    const models::List labelled = {{"embed", ""}, {"chatty", "chat"}, {"ears", "audio_transcription"}, {"voice", "audio_speech"}};
    const models::List unlabelled = {{"one", ""}, {"two", ""}};
    const models::List no_chat = {{"embed", ""}, {"ears", "audio_transcription"}};
    // Only the speech models labelled: the chat model, unlabelled, is not taken without the user,
    // but settings list it for them to choose (else the list for the language model was empty).
    const models::List speech_only = {{"talker", ""}, {"ears", "audio_transcription"}, {"voice", "audio_speech"}};
    Settings picked, none, no_llm, kept, partial;
    kept.llm_model = "mine";
    models::choose(picked, labelled);
    models::choose(none, unlabelled);
    models::choose(no_llm, no_chat);
    models::choose(kept, labelled);
    models::choose(partial, speech_only);
    const bool automatic = picked.llm_model == "chatty" && picked.stt_model == "ears" && picked.tts_model == "voice" &&
                           none.llm_model.empty() && none.stt_model.empty() && none.tts_model.empty() &&
                           no_llm.llm_model.empty() && no_llm.stt_model == "ears" && kept.llm_model == "mine" &&
                           partial.llm_model.empty() && partial.tts_model == "voice";
    const bool by_hand = models::offered(labelled, "chat") == std::vector<std::string>{"chatty"} &&
                         models::offered(unlabelled, "chat") == std::vector<std::string>{"one", "two"} &&
                         models::offered(unlabelled, "audio_speech") == std::vector<std::string>{"one", "two"} &&
                         models::offered(no_chat, "chat") == std::vector<std::string>{"embed"} &&
                         models::offered(speech_only, "chat") == std::vector<std::string>{"talker"};
    if (!automatic || !by_hand)
        fprintf(stderr, "a model was chosen from the service's list by the wrong rule (automatic %d, settings list %d)\n",
                automatic, by_hand);
    return automatic && by_hand;
}

// A setup guide opened afresh and walked to its AI service page, past the internet step (with a
// connection or without).
void setup_at_service(Shots &shots, std::function<void()> on_finish) {
    open_setup(shots.app.menu, std::move(on_finish));
    shots.press("Start setup");
    shots.press(shots.app.menu.row_of("Continue") >= 0 ? "Continue" : "Continue without internet");
}

// Answers to the setup guide's slow requests that come back after the user has left the page that
// asked: each is held in the UI queue until the user has moved on, then delivered, and the check fails
// unless it was delivered and changed nothing on screen. Nothing listens at the service's address
// here, so each answer is a quick failure. Checked, not pictured.
// - "Test and continue", answered after Skip and Finish (refactor review B5): it reopened the
//   finished guide on its models page, whose Continue then used a self-reference already let go (a
//   crash), or showed "That did not work" over the chat.
// - The list of models asked for on the models page, answered after Continue (fix round 2, C8): it
//   opened the picker, or "Could not load models", over the page about the user.
bool late_setup_answers_ignored(Shots &shots) {
    Menu &menu = shots.app.menu;
    const Settings was = Settings::get();
    set([](Settings &s) { s.api_base = "http://127.0.0.1:9/v1"; s.api_key = "unused"; });
    uiq::drain();   // nothing else waits to be delivered

    bool ended = false;
    setup_at_service(shots, [&ended] { ended = true; });
    shots.press("Test and continue");
    const bool tested = answer_waiting();
    for (const char *row : {"Skip this step", "Continue", "Continue", "Finish"}) shots.press(row);
    const bool finished = ended && !menu.open();
    const size_t test_answers = uiq::drain();
    const bool test_ok = tested && test_answers > 0 && finished && !menu.open();
    if (!test_ok)
        fprintf(stderr, "a late answer to the setup guide's service test was not held and delivered after the guide finished, or "
                        "reopened it (held %d, delivered %zu, finished %d, reopened on \"%s\")\n",
                tested, test_answers, finished, menu.title().c_str());

    setup_at_service(shots, nullptr);
    shots.press("Skip this step");
    shots.press("Language model");
    const bool listed = answer_waiting();
    shots.press("Continue");
    const std::string next = menu.title();
    const size_t list_answers = uiq::drain();
    const bool models_ok = listed && list_answers > 0 && next == "Setup 4 of 5: about you" && menu.title() == next;
    if (!models_ok)
        fprintf(stderr, "a list of models asked for on the setup guide's models page, answered on the next page, was not held and "
                        "delivered, or opened over it (held %d, delivered %zu, \"%s\" became \"%s\")\n",
                listed, list_answers, next.c_str(), menu.title().c_str());

    menu.close();
    for (int i = 0; i < Menu::kToastTicks; ++i) menu.tick();   // the toasts go, for the screens to come
    set([&was](Settings &s) { s = was; });
    return test_ok && models_ok;
}

// Answers to the setup guide's slow requests that come back to the page that asked, but no longer belong
// there (fix round 3, D11), held and delivered as above:
// - "Test and continue", answered after Skip and then ESC back to the same step: the page that asked was
//   on top again, and its answer moved the user on a second time (here: "That did not work" over it).
// - "Test and continue", and the list of models asked for on the models page, answered after the service's
//   address had changed: the old service's answer was taken as the new one's.
bool returned_setup_answers_ignored(Shots &shots) {
    Menu &menu = shots.app.menu;
    const Settings was = Settings::get();
    set([](Settings &s) { s.api_base = "http://127.0.0.1:9/v1"; s.api_key = "unused"; });
    uiq::drain();
    const char *service_page = "Setup 2 of 5: AI service";
    // Delivers what waits and says whether anything did, with the page `title` still on top as it was.
    auto delivered_on = [&menu](const std::string &title) {
        const size_t depth = menu.depth();
        return uiq::drain() > 0 && menu.title() == title && menu.depth() == depth;
    };

    setup_at_service(shots, nullptr);
    shots.press("Test and continue");
    bool asked = answer_waiting();
    shots.press("Skip this step");
    KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
    menu.key(esc);
    const bool back_ok = asked && menu.title() == service_page && delivered_on(service_page);

    shots.press("Test and continue");
    asked = answer_waiting();
    set([](Settings &s) { s.api_base = "http://127.0.0.1:9/other/v1"; });
    const bool test_ok = asked && delivered_on(service_page);

    shots.press("Skip this step");
    shots.press("Language model");
    asked = answer_waiting();
    set([](Settings &s) { s.api_base = "http://127.0.0.1:9/v1"; });
    const bool list_ok = asked && delivered_on("Setup 3 of 5: models");
    if (!back_ok || !test_ok || !list_ok)
        fprintf(stderr, "a late answer to the setup guide acted on a page it no longer belonged to (\"Test and continue\" after the "
                        "step was left and gone back to %d, after the address changed %d; the list of models after it changed %d; "
                        "on \"%s\")\n", back_ok, test_ok, list_ok, menu.title().c_str());

    menu.close();
    for (int i = 0; i < Menu::kToastTicks; ++i) menu.tick();
    set([&was](Settings &s) { s = was; });
    return back_ok && test_ok && list_ok;
}

// A bubble once kept the height of one line while its text wrapped onto a second, so the
// last word vanished ("...cybersecurity" for "...cybersecurity news").
bool bubbles_fit(ChatView &chat) {
    bool ok = true;
    for (const char *said : {"What is the latest cybersecurity news", "What's the weather like in Lisbon tomorrow?", "Hi"}) {
        if (!theme::label_fits(chat.add_user(said, false))) {
            fprintf(stderr, "a chat bubble is too short for its text: %s\n", said);
            ok = false;
        }
    }
    chat.clear();
    return ok;
}

// A photo's thumbnail goes with its chat row, and one left by an earlier run goes at
// the next start; an image the row does not own stays.
bool thumbnails_go_with_rows(ChatView &chat) {
    const std::string own = util::cache_dir() + "/" + kThumbPrefix + "shots.png", other = util::cache_dir() + "/shots-image.png";
    util::write_file_atomic(own, "x");
    util::write_file_atomic(other, "x");
    chat.add_image(own, true);
    chat.add_image(other);
    const bool shown = access(own.c_str(), F_OK) == 0;
    chat.clear();
    const bool gone = access(own.c_str(), F_OK) != 0, stayed = access(other.c_str(), F_OK) == 0;
    unlink(other.c_str());
    util::write_file_atomic(own, "x");
    remove_old_thumbnails();
    const bool cleared = access(own.c_str(), F_OK) != 0;
    if (!shown || !gone || !stayed || !cleared)
        fprintf(stderr, "a thumbnail was not deleted with its chat row or at the next start (kept while shown %d, "
                        "gone with the row %d, other image kept %d, left over removed %d)\n", shown, gone, stayed, cleared);
    return shown && gone && stayed && cleared;
}

// While a request is worked on, the footer and status bar are set on every tick with the
// same words and colours: nothing is redrawn until something changes (the dots, every
// fourth tick, are tick()'s).
bool busy_footer_still(ChatView &chat) {
    static int invalidated = 0;
    const lv_event_cb_t count = [](lv_event_t *) { ++invalidated; };
    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, count, LV_EVENT_REFR_REQUEST, nullptr);   // sent for anything newly to redraw
    invalidated = 0;
    for (int i = 0; i < 3; ++i) {
        chat.footer_busy("Calculating", theme::kThink);
        chat.set_status(Settings::get().agent_name, theme::kThink);
    }
    const int same = invalidated;
    chat.footer_busy("Calculating", theme::kAccent);
    const int changed = invalidated - same;
    chat.footer_busy("Calculating", theme::kThink);
    lv_display_remove_event_cb_with_user_data(disp, count, nullptr);
    lv_refr_now(nullptr);
    if (same != 0 || changed == 0)
        fprintf(stderr, "the busy footer redrew with nothing changed (%d requests), or not for a new colour (%d)\n", same, changed);
    return same == 0 && changed != 0;
}

// A station plays for hours, and a scrolling name would redraw the status bar many times
// a second for all that time: one too long for its room is cut short instead. A track's
// title still scrolls.
bool station_name_still(ChatView &chat) {
    chat.set_now_playing("Radio Harbour Classic and Jazz", true);
    lv_refr_now(nullptr);
    const bool still = !chat.now_playing_moves();
    chat.set_now_playing("Radio Harbour Classic and Jazz");
    lv_refr_now(nullptr);
    const bool moves = chat.now_playing_moves();
    if (!still || !moves) fprintf(stderr, "a station's long name scrolled in the status bar, or a track's long title did not\n");
    return still && moves;
}

// Every line of the key guide, and the hints on the bottom row, fit their room in the real fonts.
bool key_words_fit() {
    auto width = [](const std::string &text, const lv_font_t *font) {
        lv_point_t size;
        lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return static_cast<int>(size.x);
    };
    bool ok = true;
    for (const keyguide::Section &sec : keyguide::sections())
        for (const keyguide::Row &row : sec.rows)
            if (width(row.keys, theme::bold()) > Menu::kPairKeyW - 4 || width(row.action, theme::body()) > Menu::pair_text_width()) {
                fprintf(stderr, "the key guide's line for %s does not fit the screen (keys %d of %d px, words %d of %d px)\n",
                        row.keys.c_str(), width(row.keys, theme::bold()), Menu::kPairKeyW - 4,
                        width(row.action, theme::body()), Menu::pair_text_width());
                ok = false;
            }
    for (const bool configured : {true, false})
        if (width(keyguide::idle_hint(configured), theme::small()) > theme::kScreenW - 12) {
            fprintf(stderr, "the hints on the bottom row do not fit: %s\n", keyguide::idle_hint(configured).c_str());
            ok = false;
        }
    // DIAGNOSTICS (temporary): what SPACE twice says, on the same row.
    activity::SaveResult nothing, saved, broken;
    nothing.nothing = true;
    saved.path = "x";
    broken.error = "x";
    for (const activity::SaveResult &r : {nothing, saved, broken})
        if (width(activity::saved_toast(r), theme::small()) > theme::kScreenW - 12) {
            fprintf(stderr, "the words after SPACE twice do not fit the bottom row: %s\n", activity::saved_toast(r).c_str());
            ok = false;
        }
    return ok;
}

// A title that fits its whole room stays still when a countdown takes part of it: it
// is cut short, not scrolled (a moving title redraws the status bar many times a
// second for as long as the timer runs). Too long for its whole room, a title scrolls
// again once the countdown has gone.
bool title_still_beside_countdown(ChatView &chat, time_t t0) {
    chat.set_timer("");
    chat.set_now_playing("Round Midnight");
    lv_refr_now(nullptr);
    const bool fits = !chat.now_playing_moves();
    chat.set_timer(timers::badge({t0 + 300}, t0 + 1));
    lv_refr_now(nullptr);
    const bool still = !chat.now_playing_moves();
    chat.set_timer("");
    chat.set_now_playing("Dancing Queen · ABBA");
    lv_refr_now(nullptr);
    const bool moves = chat.now_playing_moves();
    if (!fits || !still || !moves) fprintf(stderr, "the now-playing text scrolled beside a countdown, or not when it needs to\n");
    return fits && still && moves;
}

}  // namespace

// ------------------------------------------------------------ screenshots
int App::render_shots(const std::string &dir) {
    util::make_dirs(dir);
    Shots shots{*this, dir};
    shots_chat(shots);
    if (!image_decodes()) shots.failed = true;
    shots_dark_chat(shots);
    shots_keys(shots);
    shots_volume(shots);
    shots_breathing(shots);
    shots_timers(shots);
    shots_sample_data(shots);
    shots_setup(shots);
    shots_settings(shots);
    shots_dark_settings(shots);
    shots.failed = diagnostics_shots(shots) || shots.failed;   // DIAGNOSTICS (temporary)
    shots_idle(shots);
    timers::fix_clock(0);
    return shots.failed ? 1 : 0;
}

void App::shots_chat(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool &failed = shots.failed;
    // The status bar shows the name it is given from the first call on, an empty one included.
    chat.set_status("", theme::kFaint);
    failed = !chat.status_name().empty();
    if (failed) fprintf(stderr, "the status bar kept a name it was not given\n");
    set([](Settings &s) { s.agent_name = "Zero"; });
    chat.set_status("Zero", theme::kFaint);
    chat.set_indicators(true, false, 68, false, "18:42");
    chat.footer_hints(keyguide::idle_hint(true));
    // Checked, not pictured.
    if (!bubbles_fit(chat)) failed = true;
    if (!thumbnails_go_with_rows(chat)) failed = true;
    chat.add_note("Press TAB to talk, press it again when you have finished. Or press ENTER and type.", theme::kDim);
    snap("welcome");
    {
        // Before an AI service is set up, the note and the bottom row say how to reach settings.
        const Settings was = Settings::get();
        set([](Settings &s) { s.api_base.clear(); s.api_key.clear(); });
        chat.clear();
        welcome();
        update_footer();
        snap("not-set-up");
        set([&was](Settings &s) { s.api_base = was.api_base; s.api_key = was.api_key; });
        chat.footer_hints(keyguide::idle_hint(true));   // as a set-up device shows it, for the frames to come
    }

    chat.clear();
    chat.add_user("What's the weather like in Lisbon tomorrow?", false);
    chat.add_tool("Checking the weather");
    chat.begin_answer();
    chat.append_answer("Tomorrow in Lisbon looks wet: rain most of the day, 18 to 21 degrees, with a 70 percent chance of showers. Take an umbrella.");
    chat.end_answer("", false);
    snap("answer");

    chat.add_user("How much is 85 euros in pounds, split three ways?", false);
    chat.add_tool("Converting currency");
    chat.add_tool("Calculating");
    chat.footer_busy("Calculating", theme::kThink);
    chat.set_status(Settings::get().agent_name, theme::kThink);
    snap("working");
    if (!busy_footer_still(chat)) failed = true;   // checked, not pictured

    chat.begin_answer();
    chat.append_answer("85 euros is about 70.93 pounds, so that's 23.64 each.");
    chat.end_answer("", false);
    chat.footer_busy("Speaking   ·   TAB to interrupt", theme::kAccent);
    chat.set_status(Settings::get().agent_name, theme::kAccent);
    chat.set_now_playing("Dancing Queen · ABBA");
    snap("speaking");
    chat.set_now_playing("Harbour Classic FM", true);   // a radio station shows its name, as the player gives it
    snap("radio-playing");
    if (!station_name_still(chat)) failed = true;
    chat.set_now_playing("Dancing Queen · ABBA");

    for (int i = 0; i < 40; ++i) {
        const float lvl = 0.15f + 0.75f * static_cast<float>((i * 37 % 17)) / 17.0f * (i % 9 < 6 ? 1.f : 0.25f);
        chat.footer_listening(lvl, 3.4, true);
    }
    chat.set_status(Settings::get().agent_name, theme::kListen);
    snap("listening");

    chat.footer_hints("");
    chat.typing_begin("remind me at 7 to call the hot");
    chat.set_status(Settings::get().agent_name, theme::kFaint);
    snap("typing");
}

void App::shots_dark_chat(Shots &shots) {
    bool &failed = shots.failed;
    {
        // Unused for the minutes set under Device, the screen dims, then goes off: drawn black,
        // then nothing more drawn however much changes. A key wakes it to what it shows now and
        // does nothing else, whatever is on screen: not ENTER (it would send the text being
        // typed), not a letter (typed), not SPACE or a key in settings or the key guide, nor
        // their repeats or releases. The next press works as usual.
        const int minutes = Settings::get().screen_off_min;
        set([](Settings &s) { s.screen_off_min = 5; });
        chat.set_now_playing("");
        auto go_dark = [&] {
            const unsigned long t = sys::now_ms();
            wake_screen(t);
            tick_screen(t + screen::kDimAfterMs - 1, false);
            const bool lit = idle.light() == screen::Light::Lit;
            tick_screen(t + screen::kDimAfterMs, false);
            const bool dim = idle.light() == screen::Light::Dimmed && !display::screen_is_off();
            tick_screen(t + 5 * 60000UL - 1, false);
            const bool still = !display::screen_is_off();
            tick_screen(t + 5 * 60000UL, false);
            return lit && dim && still && display::screen_is_off() && display::frame_black();
        };
        auto press = [&](Key k, const char *text, std::initializer_list<int> states) {
            KeyEvent ev; ev.key = k; ev.text = text;
            for (int st : states) { ev.state = st; handle_key(ev); }
        };
        const std::string typed = chat.typing_text();
        bool dark = go_dark();                                     // checked on the frame itself: no image of it
        // While it is off: a new time, a countdown, a title that scrolls. None of it is drawn.
        const unsigned long drawn = display::refreshes();
        chat.set_indicators(true, false, 67, false, "18:43");
        chat.set_timer(timers::badge({1800000300}, 1800000001));
        chat.set_now_playing("Harbour Classic FM · Morning concert with a long title");
        for (int i = 0; i < 20; ++i) {
            chat.tick();
            menu.tick();
            lv_timer_handler();
            usleep(20000);
        }
        const bool nothing_drawn = display::refreshes() == drawn && display::frame_black();
        press(Key::Enter, "", {1, 0});
        const bool woke = !display::screen_is_off() && display::frame_current() && chat.typing() && chat.typing_text() == typed;
        chat.set_indicators(true, false, 68, false, "18:42");
        chat.set_timer("");
        chat.set_now_playing("");
        dark = go_dark() && dark;
        press(Key::Char, "w", {1, 2, 2, 0});
        bool swallowed = !display::screen_is_off() && chat.typing_text() == typed;
        press(Key::Char, "w", {1, 0});
        const bool next_typed = chat.typing_text() == typed + "w";
        press(Key::Backspace, "", {1, 0});
        // fn or shift tapped on its own (the reader sends it as Key::None): it wakes the screen and
        // types nothing, and on a lit screen does nothing at all.
        dark = go_dark() && dark;
        press(Key::None, "", {1});
        bool modifier_woke = !display::screen_is_off() && display::frame_current() && chat.typing_text() == typed;
        press(Key::None, "", {1});
        modifier_woke = modifier_woke && chat.typing() && chat.typing_text() == typed && !menu.open();
        chat.typing_end();
        dark = go_dark() && dark;
        press(Key::Char, " ", {1, 0});
        swallowed = swallowed && !display::screen_is_off() && !menu.open();
        press(Key::Char, " ", {1, 0});                              // settings, on "Assistant",
        settle_keys(sys::now_ms() + kDoubleTapMs);              // once SPACE SPACE is ruled out
        dark = go_dark() && dark;
        press(Key::Down, "", {1, 2, 0});
        swallowed = swallowed && menu.open() && menu.selected() == "Assistant";
        press(Key::Char, " ", {1, 0});                              // closes them
        press(Key::Char, "h", {1, 0});                              // the key guide
        dark = go_dark() && dark;
        press(Key::Down, "", {1, 0});
        dark = go_dark() && dark;
        press(Key::Esc, "", {1, 0});
        swallowed = swallowed && menu.open() && menu.title() == kKeyGuideTitle && menu.first_shown() == 0;
        press(Key::Esc, "", {1, 0});
        if (!dark || !nothing_drawn || !woke || !swallowed || !next_typed || !modifier_woke || menu.open() ||
            display::screen_is_off()) {
            fprintf(stderr, "the screen did not go off, drew while off, or a waking key did more than wake it "
                            "(dark %d, nothing drawn %d, woke %d, swallowed %d, next typed %d, a modifier woke %d)\n",
                    dark, nothing_drawn, woke, swallowed, next_typed, modifier_woke);
            failed = true;
        }
        set([minutes](Settings &s) { s.screen_off_min = minutes; });
        chat.set_now_playing("Dancing Queen · ABBA");          // as it was, for the frames to come
    }
    chat.typing_end();
}

void App::shots_keys(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool &failed = shots.failed;
    chat.add_alert("Check the oven");
    chat.add_user("こんにちは、元気ですか？", false);
    chat.footer_hints(keyguide::idle_hint(true));
    snap("alert");
    // The double tap goes through the real key handler, so this also checks the shortcut.
    KeyEvent del; del.key = Key::Backspace; del.state = 1;
    handle_key(del);
    snap("delete-once");                   // one tap alone changes nothing
    handle_key(del);
    snap("new-conversation");
    // A letter key does not start typing (ENTER does); it says how to.
    KeyEvent letter; letter.key = Key::Char; letter.text = "k"; letter.state = 1;
    handle_key(letter);
    update_footer();
    snap("letter-hint");
    if (chat.typing()) {
        fprintf(stderr, "a letter key started typing; only ENTER should\n");
        failed = true;
    }
    level_until = 0;
    {
        // SPACE opens settings and closes them again. H shows every key, as a page that
        // scrolls; ESC (pressed and let go) goes back to the chat.
        KeyEvent space; space.key = Key::Char; space.text = " "; space.state = 1;
        handle_key(space);
        const bool waited = !menu.open();                 // a second SPACE may yet come (DIAGNOSTICS)
        settle_keys(sys::now_ms() + kDoubleTapMs);    // none did: settings open
        const bool opened = waited && menu.open() && menu.depth() == 1 && menu.title() == "Settings" &&
                            menu.selected() == "Assistant";
        handle_key(space);
        const bool closed = !menu.open();
        KeyEvent h; h.key = Key::Char; h.text = "h"; h.state = 1;
        handle_key(h);
        const bool guide = menu.open() && menu.title() == kKeyGuideTitle && menu.first_shown() == 0;
        snap("keys");
        KeyEvent down; down.key = Key::Down; down.state = 1;
        for (int i = 0; i < 8; ++i) handle_key(down);
        snap("keys-more");
        for (int i = 0; i < 30; ++i) handle_key(down);   // stops at the end
        snap("keys-end");
        const bool scrolled = menu.first_shown() > 8;
        handle_key(h);                                   // already showing: nothing more
        const bool once = menu.depth() == 1;
        KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
        handle_key(esc);
        esc.state = 0;
        handle_key(esc);
        const bool back = !menu.open() && !chat.typing();
        if (!opened || !closed || !guide || !scrolled || !once || !back) {
            fprintf(stderr, "SPACE or H did not open and close settings or the key guide as they should\n");
            failed = true;
        }
        if (!key_words_fit()) failed = true;
    }
}

void App::shots_volume(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    KeyEvent vol; vol.key = Key::VolumeUp; vol.state = 1;
    handle_key(vol);
    update_footer();
    snap("volume");
    vol.key = Key::Mute;
    handle_key(vol);
    update_footer();
    snap("muted");
    handle_key(vol);                       // unmute again
    level_until = 0;
}

void App::shots_breathing(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    for (const auto &shot : {std::make_pair("box", 2500L), std::make_pair("box", 6000L), std::make_pair("sigh", 9000L)}) {
        breath.start(shot.first, 5);
        breath.show_at(shot.second);
        snap(shot.second == 2500L ? "breathing-in" : shot.second == 6000L ? "breathing-hold" : "breathing-out");
    }
    breath.stop();
}

void App::shots_timers(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool &failed = shots.failed;
    const time_t t0 = shots.t0;
    {
        chat.clear();
        chat.add_user("The food needs to come out of the oven in 5 mins", false);
        chat.add_tool("Timers");
        chat.begin_answer();
        chat.append_answer("Your 5 minute timer for the food is running.");
        chat.end_answer("", false);
        chat.footer_hints(keyguide::idle_hint(true));
        chat.set_status(Settings::get().agent_name, theme::kFaint);
        chat.set_now_playing("So What");                       // short enough not to scroll
        chat.set_timer(timers::badge({t0 + 300}, t0 + 1));
        snap("timer-one");
        if (!title_still_beside_countdown(chat, t0)) failed = true;
        chat.add_user("I want to study for an hour, can you set a timer", false);
        chat.add_tool("Timers");
        chat.begin_answer();
        chat.append_answer("Your study timer is set for an hour. The food has 4 minutes 41 seconds left.");
        chat.end_answer("", false);
        chat.set_now_playing("");
        chat.set_timer(timers::badge({t0 + 300, t0 + 19 + 3600}, t0 + 19));
        snap("timer-two");
        chat.add_alert("Time's up: Food out of the oven");
        chat.set_timer(timers::badge({t0 + 19 + 3600}, t0 + 300));
        banner.show("Time's up", "Food out of the oven");
        snap("timer-alarm");
        banner.hide();
        chat.set_timer("");
    }
    {
        // The same through the app's own path: a timer that has just ended rings (silently,
        // as when the user has muted the device), and a key press silences it and goes no further.
        // The screen was off: the alarm turns it on at once.
        muted = true;
        const int minutes = Settings::get().screen_off_min;
        set([](Settings &s) { s.screen_off_min = 5; });
        const unsigned long t = sys::now_ms();
        wake_screen(t);
        tick_screen(t + 5 * 60000UL, false);
        const bool was_off = display::screen_is_off();
        set([minutes](Settings &s) { s.screen_off_min = minutes; });
        Store::get().add_timer("Eggs", 1, time(nullptr) - 5);
        update_timers(sys::now_ms());
        // The first frame drawn before the light comes on has the banner and the countdown.
        const bool rang = was_off && !display::screen_is_off() && display::frame_current() && alarm.ringing() &&
                          banner.visible() && audio::Music::get().holding(audio::Music::kAlarm);
        KeyEvent key; key.key = Key::Char; key.text = "a"; key.state = 1;
        handle_key(key);
        snap("timer-silenced");
        if (!rang || alarm.ringing() || banner.visible() || chat.typing() || audio::Music::get().holding(audio::Music::kAlarm)) {
            fprintf(stderr, "the timer alarm did not ring and stop as it should\n");
            failed = true;
        }
        muted = false;
    }
}

// The calendar, memories, reminders, timers and smart home the settings pages show.
void App::shots_sample_data(Shots &shots) {
    const time_t t0 = shots.t0;
    Store &store = Store::get();
    CalEvent e;
    auto day_at = [](int days_ahead, const char *clock) {
        time_t t = 0;
        util::parse_local(util::format_time(time(nullptr) + days_ahead * 86400, "%Y-%m-%d ") + clock, t);
        return t;
    };
    e.title = "Dinner with Marta"; e.start = day_at(3, "19:30"); e.remind_min = 30;
    store.add_event(e);
    e.title = "Flight to Lisbon"; e.start = day_at(5, "07:15"); e.notes = "Seat 14A"; e.remind_min = 120;
    const int flight = store.add_event(e);
    e = CalEvent();
    e.title = "Mum's birthday"; e.all_day = true; e.repeat = "yearly"; e.remind_min = 0;
    e.start = util::make_local(1960, util::local_fields(day_at(9, "00:00")).month, util::local_fields(day_at(9, "00:00")).day, 0, 0);
    const int birthday = store.add_event(e);
    store.add_memory("Allergic to shellfish");
    store.add_memory("Prefers window seats");
    store.add_reminder("Call the hotel", t0 + 5400);   // fixed, like the timers: the Reminders page shows it
    shots.food = store.add_timer("Food out of the oven", 300, t0);
    store.add_timer("Study", 3600, t0 + 19);
    timers::fix_clock(t0 + 60);                    // what the Reminders page counts down from
    sample_home();
    (void)flight; (void)birthday;
}

void App::shots_setup(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool &failed = shots.failed;
    if (!models_chosen_by_rule()) failed = true;
    run_setup();
    snap("setup-welcome");
    {
        // SPACE does not close the guide (a new user presses it to choose), on its own pages
        // or on a page opened from one; it says which key does. Settings it still closes (above).
        KeyEvent space; space.key = Key::Char; space.text = " "; space.state = 1;
        handle_key(space);
        const bool kept = menu.open() && menu.title() == "Welcome to Zero Bot" && menu.in_guide();
        menu.choose("Language model", {"one", "two"}, "one", [](const std::string &) {});
        handle_key(space);
        const bool kept_above = menu.open() && menu.title() == "Language model";
        menu.pop();
        for (int i = 0; i < Menu::kToastTicks; ++i) menu.tick();   // the hint goes, for the screens to come
        if (!kept || !kept_above) {
            fprintf(stderr, "SPACE closed the setup guide (on its page %d, on a page opened from it %d)\n", kept, kept_above);
            failed = true;
        }
    }
    {
        KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
        menu.key(enter);
        snap("setup-internet");
        if (!move_to(menu, "Continue")) shots.menu_to("Continue without internet");
        menu.key(enter);                       // continue (with or without a connection)
        snap("setup-service");
        menu.problem("That did not work", "The AI service is down or unreachable (HTTP 530, code 1033). "
                                          "This is on the service's side; try again later.");
        snap("setup-problem");
        menu.pop();
        shots.menu_to("Skip this step");
        menu.key(enter);                       // skip the live test in screenshots
        set([](Settings &s) { s.llm_model = "example-chat-model"; s.stt_model = "example-speech-to-text"; s.tts_model = "example-text-to-speech"; });
        menu.refresh();
        snap("setup-models");
        shots.menu_to("Continue");
        menu.key(enter);
        set([](Settings &s) { s.user_name = "Sam"; s.home_city = "London"; s.home_country = "United Kingdom"; });
        menu.refresh();
        snap("setup-you");
        shots.menu_to("Continue");
        menu.key(enter);
        snap("setup-extras");
        if (!without_power_on(menu, "setup guide's extras", {"Speak replies", "Finish"})) failed = true;
        menu.close();
    }
    if (!late_setup_answers_ignored(shots)) failed = true;
    if (!returned_setup_answers_ignored(shots)) failed = true;
}

void App::shots_settings(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool &failed = shots.failed;
    // Made-up stations for the radio page, one entry of them without its address.
    util::make_dirs(util::config_dir());
    util::write_file_atomic(radio::path(), R"({"stations": [
        {"name": "Harbour Classic FM", "nicknames": ["Harbour"], "url": "https://stream.example.org/harbour.mp3"},
        {"name": "Valley Radio", "url": "https://stream.example.org/valley.aac",
         "low_data_url": "https://stream.example.org/valley-low.aac"},
        {"name": "Night Jazz", "url": "https://stream.example.org/jazz/live.m3u8"},
        {"name": "Coast News"}]})");
    open_menu();
    snap("settings");                              // the top page: a row for each section
    KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
    KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
    shots.menu_to("Assistant");
    menu.key(enter);
    snap("settings-assistant");
    menu.key(esc);
    shots.menu_to("Device");
    menu.key(enter);
    snap("settings-device");
    if (!without_power_on(menu, "Device", {"Time zone", "Internet via phone"})) failed = true;
    menu.key(esc);
    shots.menu_to("Your data");                    // whose first row is "Calendar"
    // Its Smart home row counts the things on the network the device is on, in the background: the
    // page is up at once, and the count comes with the answer (as pictured).
    static const home::testing::NetworkSource slow{slow_route_data, slow_router_mac, slow_look_data};
    if (!opens_at_once(menu, slow, "Your data", [this] { return menu.value_of("Smart home") == "5 devices"; })) failed = true;
    shots_data(shots);
    shots_smart_home(shots);
    shots_radio(shots);
    menu.key(esc);                                 // the top page, still on "Your data"
    shots.menu_to("AI service");
    menu.key(enter);
    snap("settings-service");
    menu.key(esc);
    shots.menu_to("About");
    menu.key(enter);
    snap("settings-about");
    menu.key(esc);
    shots.menu_to(kKeyGuideTitle);                 // from settings, it opens at the keys for settings
    menu.key(enter);
    snap("keys-settings");
    if (menu.title() != kKeyGuideTitle || menu.first_shown() == 0) {
        fprintf(stderr, "the key guide did not open at the keys for settings\n");
        failed = true;
    }
    menu.close();
}

// Your data: calendar, reminders and timers, memory. From the section's page, and back to it.
void App::shots_data(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool &failed = shots.failed;
    KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
    KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
    Store &store = Store::get();
    const time_t t0 = shots.t0;
    const int food = shots.food;
    snap("settings-data");
    if (menu.title() != "Your data" || menu.selected() != "Calendar") {
        fprintf(stderr, "the settings section did not open on its first row\n");
        failed = true;
    }
    menu.key(enter);
    snap("calendar");
    shots.menu_to("Dinner with Marta");
    menu.key(enter);
    snap("event");
    shots.menu_to("Title");
    menu.key(enter);
    snap("edit-text");
    menu.key(esc); menu.key(esc);
    shots.menu_to("Mum's birthday");
    menu.key(enter);
    snap("event-birthday");
    menu.key(esc); menu.key(esc);
    shots.menu_to("Reminders and timers");
    menu.key(enter);
    snap("reminders");
    {
        // A timer that ends while the page is open takes its row away: the cursor stays on the
        // timer it was on, and the question before cancelling one says which.
        shots.menu_to("Study");
        store.delete_timer(food);                  // as when "Food out of the oven" has just ended
        menu.tick();
        const bool kept = menu.selected() == "Study";
        menu.key(enter);
        const bool named = menu.title() == "Cancel Study?";
        menu.key(esc);
        store.add_timer("Food out of the oven", 300, t0);       // as it was, for the screens to come
        menu.tick();
        if (!kept || !named || menu.selected() != "Study") {
            fprintf(stderr, "the cursor left its timer when the rows changed, or the question did not name it\n");
            failed = true;
        }
    }
    menu.key(esc);
    shots.menu_to("Memory");
    menu.key(enter);
    snap("memory");
    menu.key(esc);
}

// Your data > Smart home, the sample home's pages. From the section's page, and back to it.
void App::shots_smart_home(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
    KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
    shots.menu_to("Smart home");
    // Opening Smart home never waits for the network on the UI thread: the first full look runs `iw`
    // and can take seconds, in which keys, redraws and the alarm stood still. Here the look takes a
    // second and a half: the page is up at once, says it is looking, and lists the home when the
    // answer comes (as pictured).
    static const home::testing::NetworkSource slow{slow_route, slow_router_mac, slow_look};
    if (!opens_at_once(menu, slow, "Smart home", [this] { return menu.row_of("Bed heater") >= 0; })) shots.failed = true;
    snap("smart-home");
    shots.menu_to("Bed heater");                   // the first thing
    menu.key(enter);
    snap("home-thing");
    menu.key(esc);
    shots.menu_to("Desk lamp");                    // one socket of a strip
    menu.key(enter);
    shots.menu_to("Part of");
    menu.key(enter);
    // What a device still needs is asked in the background (it looks at the network): pictured once known.
    if (!arrives([this] { return menu.row_of("Read it again") >= 0; })) {
        fprintf(stderr, "the device's page never learnt that it needs no setting up\n");
        shots.failed = true;
    }
    snap("home-device");
    menu.key(esc); menu.key(esc);
    shots.menu_to("Garden strip");                 // the strip that listed nothing, under "To set up"
    snap("home-to-set-up");
    shots.menu_to("Tapo P110");                    // the plug still to be set up
    // A device's page never waits for the network on the UI thread either (fix round 2, C9): what the
    // plug still needs takes the first full look at the network, here a second and a half.
    static const home::testing::NetworkSource slow_device{slow_route_device, slow_router_mac, slow_look_device};
    if (!opens_at_once(menu, slow_device, "Home device", [this] { return menu.row_of("Set up…") >= 0; })) shots.failed = true;
    snap("home-setup");
    shots.menu_to("Set up…");
    menu.key(enter);
    snap("home-pairing");
    menu.key(esc); menu.key(esc);
    shots.menu_to("Tapo account");                 // past the strip that listed nothing
    menu.key(enter);
    snap("tapo-account");
    menu.key(esc); menu.key(esc);                  // back to "Your data", on "Smart home"
}

// Your data > Radio stations. From the section's page, and back to it.
void App::shots_radio(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool &failed = shots.failed;
    KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
    KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
    shots.menu_to("Radio stations");
    menu.key(enter);
    snap("radio-stations");
    if (menu.title() != "Radio stations") {
        fprintf(stderr, "the key presses did not reach the radio stations page\n");
        failed = true;
    }
    shots.menu_to("Read the file again");          // below how to add stations
    snap("radio-stations-help");
    {
        // ENTER on a station checks its stream before playing it, as the assistant does (a
        // playlist file is followed to its stream: the player cannot open one), and a station
        // that will not play says why. Nothing listens at this address.
        std::string kept;
        util::read_file(radio::path(), kept);
        util::write_file_atomic(radio::path(), R"({"stations": [{"name": "Closed FM", "url": "http://127.0.0.1:9/closed.pls"}]})");
        menu.refresh();
        shots.menu_to("Closed FM");
        const bool on_station = menu.selected() == "Closed FM";
        menu.key(enter);
        const bool waited = !audio::Music::get().live();   // not started before its check
        for (int i = 0; i < 250 && menu.title() != "Closed FM did not play"; ++i) {
            uiq::drain();
            usleep(20 * 1000);
        }
        const bool told = menu.title() == "Closed FM did not play";
        if (!on_station || !waited || !told || audio::Music::get().live()) {
            fprintf(stderr, "a station chosen in settings was played before its stream was checked, or did not say why it failed\n");
            failed = true;
        }
        audio::Music::get().stop();
        if (told) menu.pop();
        util::write_file_atomic(radio::path(), kept);
        menu.refresh();
        shots.menu_to("Read the file again");      // as before
    }
    menu.key(esc);                                 // back to "Your data"
}

void App::shots_dark_settings(Shots &shots) {
    bool &failed = shots.failed;
    KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
    Store &store = Store::get();
    {
        // The screen off, after the last frame: a reminder turns it on at once, and so does a
        // timer's alarm over settings, with the banner already in the frame drawn before the
        // light comes on. A key on a dark screen does nothing else in a settings text field
        // either (ENTER would save it, a letter would be typed), and a volume key changes nothing.
        const int minutes = Settings::get().screen_off_min;
        set([](Settings &s) { s.screen_off_min = 5; });
        auto go_dark = [&] {
            const unsigned long t = sys::now_ms();
            wake_screen(t);
            tick_screen(t + 5 * 60000UL, false);
            return display::screen_is_off() && display::frame_black();
        };
        auto press = [&](Key k, const char *text, std::initializer_list<int> states) {
            KeyEvent ev; ev.key = k; ev.text = text;
            for (int st : states) { ev.state = st; handle_key(ev); }
        };
        auto picture = [] {
            lv_draw_buf_t *b = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
            std::string v = b ? std::string(reinterpret_cast<const char *>(b->data), b->data_size) : std::string();
            if (b) lv_draw_buf_destroy(b);
            return v;
        };
        const bool speak = Settings::get().speak_replies;
        set([](Settings &s) { s.speak_replies = false; });       // no voice in screenshots
        bool dark = go_dark();
        store.add_reminder("Water the plants", time(nullptr) - 1);
        check_alerts();
        const bool reminder = !display::screen_is_off() && display::frame_current();
        set([speak](Settings &s) { s.speak_replies = speak; });

        open_menu();
        shots.menu_to("Your data");
        menu.key(enter);
        shots.menu_to("Calendar");
        menu.key(enter);
        shots.menu_to("Dinner with Marta");
        menu.key(enter);
        shots.menu_to("Title");
        menu.key(enter);                                           // the first event's title, in a text field
        const bool field = menu.editing();
        const std::string shown = picture();
        dark = go_dark() && dark;
        press(Key::Char, "x", {1, 0});
        bool still = menu.editing() && picture() == shown;
        dark = go_dark() && dark;
        press(Key::Enter, "", {1, 0});
        still = still && menu.editing() && picture() == shown;
        const int volume = Settings::get().volume;
        dark = go_dark() && dark;
        press(Key::VolumeUp, "", {1, 2, 0});
        still = still && Settings::get().volume == volume && !display::screen_is_off();
        menu.close();

        open_menu();
        dark = go_dark() && dark;
        muted = true;
        store.add_timer("Tea", 1, time(nullptr) - 5);
        update_timers(sys::now_ms());
        const bool alarm_first_frame = !display::screen_is_off() && alarm.ringing() && banner.visible() && display::frame_current();
        press(Key::Char, "a", {1, 0});
        bool silenced = !alarm.ringing() && menu.open();
        store.add_timer("Kettle", 1, time(nullptr) - 5);
        update_timers(sys::now_ms());
        const bool rang_again = alarm.ringing();
        press(Key::None, "", {1});                                   // fn tapped on its own: "any key" too
        silenced = silenced && rang_again && !alarm.ringing() && menu.open();
        muted = false;
        menu.close();
        set([minutes](Settings &s) { s.screen_off_min = minutes; });
        if (!dark || !reminder || !field || !still || !alarm_first_frame || !silenced) {
            fprintf(stderr, "a reminder or an alarm did not light the screen as it now is, or a key on a dark screen "
                            "did more than wake it (dark %d, reminder %d, text field %d, untouched %d, alarm frame %d, "
                            "silenced %d)\n", dark, reminder, field, still, alarm_first_frame, silenced);
            failed = true;
        }
    }
}

// DIAGNOSTICS (temporary, ZB-051): the screens of saved conversations, last so that removing them
// renumbers nothing. Returns true when something did not behave.
bool App::diagnostics_shots(Shots &shots) {
    auto snap = [&shots](const char *name) { shots.snap(name); };
    bool failed = false;
    // SPACE twice on the chat screen saves the conversation. Settings never open on the way, nor
    // after it; with nothing said yet there is nothing to save.
    chat.clear();
    chat.add_note("New conversation. What was said before is forgotten.", theme::kDim);
    KeyEvent space; space.key = Key::Char; space.text = " "; space.state = 1;
    handle_key(space);
    handle_key(space);
    settle_keys(sys::now_ms() + kDoubleTapMs);
    update_footer();
    snap("save-nothing");
    if (menu.open() || level_text != "Nothing to save yet" || activity::saved_count() != 0) {
        fprintf(stderr, "SPACE twice opened settings, or did not say there was nothing to save\n");
        failed = true;
    }
    level_until = 0;
    // SPACE and then another key at once: SPACE was a single press, so settings open and the key
    // goes to them (Down moves to the second section), as it did before SPACE SPACE existed.
    {
        KeyEvent down; down.key = Key::Down; down.state = 1;
        handle_key(space);
        handle_key(down);
        const bool quick = menu.open() && menu.depth() == 1 && menu.title() == "Settings" && menu.selected() == "Device" &&
                           !space_tap.pending();
        menu.close();
        settle_keys(sys::now_ms() + kDoubleTapMs);
        if (!quick || menu.open()) {
            fprintf(stderr, "SPACE then Down at once did not open settings on their second section\n");
            failed = true;
        }
        menu.close();
    }
    // One saved conversation (written by the real writer, into this run's own state folder), shown
    // in Settings > Your data and deleted from there.
    const json sample = json::array({
        json{{"role", "user"}, {"content", "[Wed 18 Nov 2026, 12:00] What's the weather like in Lisbon tomorrow?"}},
        json{{"role", "assistant"}, {"content", ""},
             {"tool_calls", json::array({json{{"id", "call_1"}, {"type", "function"},
                                              {"function", {{"name", "weather"}, {"arguments", "{\"location\":\"Lisbon\"}"}}}}})}},
        json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "{\"tomorrow\":\"rain, 18 to 21 C\"}"}},
        json{{"role", "assistant"}, {"content", "Tomorrow in Lisbon looks wet. Take an umbrella."}}});
    const bool wrote = !activity::save_conversation(sample, 0).path.empty();
    KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
    open_menu();
    shots.menu_to("Your data");
    menu.key(enter);
    move_to(menu, "Saved conversations");
    const bool listed = menu.selected() == "Saved conversations";
    menu.key(enter);
    snap("saved-conversations");
    const bool shown = menu.title() == "Saved conversations";
    const bool offered = menu.selected() == "Delete saved conversations";   // the only row to choose
    menu.key(enter);
    snap("saved-conversations-delete");
    const bool asked = menu.selected() == "Cancel";
    shots.menu_to("Delete all");
    menu.key(enter);
    const bool gone = activity::saved_count() == 0;
    if (!wrote || !listed || !shown || !offered || !asked || !gone) {
        fprintf(stderr, "a saved conversation was not written, listed, shown or deleted as it should be\n");
        failed = true;
    }
    menu.close();
    return failed;
}

namespace {

// The service's model list: one that cannot be reached is asked again after 30 s, then twice as long
// each time up to five minutes; once a list has been read, the same service is not asked again
// whatever it held (since ZB-088 one that labels none of its models gets nothing chosen, and it was
// asked every 15 s for ever), but one with another key is. A list that answers for the old key after
// the key has changed is not used (fix round 3, D11): its models were chosen for the new service.
bool model_retries_ok(App &app) {
    set([](Settings &s) {
        s.api_base = "http://127.0.0.1:9/v1";
        s.api_key = "first";
        s.llm_model.clear(); s.stt_model.clear(); s.tts_model.clear();
    });
    const llm::Service service = app.models_wanted().value_or(llm::Service{});
    std::vector<unsigned long> waits;
    for (int i = 0; i < 6; ++i) {
        app.models_answered({}, "The service did not answer.", service);
        waits.push_back(app.model_retry_ms);
    }
    const bool backs_off = waits == std::vector<unsigned long>{30000, 60000, 120000, 240000, 300000, 300000};
    app.warned_models = true;   // no note in the chat
    app.models_answered({{"one", ""}, {"two", ""}}, "", service);
    const bool read_once = app.model_retry_ms == kModelRetryMs && !app.models_wanted();
    set([](Settings &s) { s.api_key = "second"; });
    const bool new_service = app.models_wanted().has_value();
    app.models_answered({{"chatty", "chat"}}, "", service);
    const bool old_list_unused = Settings::get().llm_model.empty() && app.models_wanted().has_value();
    if (!backs_off || !read_once || !new_service || !old_list_unused)
        fprintf(stderr, "the service's model list was asked for at the wrong times, or used for another service (backs off %d, "
                        "read once %d, another service asked %d, the old one's list unused %d)\n", backs_off, read_once,
                new_service, old_list_unused);
    return backs_off && read_once && new_service && old_list_unused;
}

// A lookup of the service's models goes to the service it was started for, with that service's key, whatever is
// saved meanwhile (fix round 4, E6: the lookup read the settings again to build its request, so service A's key
// could go to service B's address, saved before B's key). models::fetch (the setup guide's "Test and continue",
// the model pages), then the choice made without the user, are each started for A at a stand-in service, and B's
// address is saved at once, under the settings' lock held for a moment: a lookup that reads the settings then
// waits for the save, and reads B's address. Neither answer is used for B.
bool lookups_keep_their_service(App &app) {
    StandInService service;
    const Settings was = Settings::get();
    const std::string a = service.address("/a/v1"), b = service.address("/b/v1");
    auto use_a = [&a](Settings &s) {
        s.api_base = a;
        s.api_key = "key-a";
        s.llm_model.clear(); s.stt_model.clear(); s.tts_model.clear();
    };
    auto save_b = [&b] { set([&b](Settings &s) { usleep(100 * 1000); s.api_base = b; }); };
    uiq::drain();

    set(use_a);
    auto used = std::make_shared<bool>(false);   // held by the answer too, however late it comes
    models::fetch([used](const models::List &, const std::string &) { *used = true; });
    save_b();
    const bool fetched = service.heard(1).size() == 1 && answer_waiting() && uiq::drain() > 0 && !*used;

    set(use_a);
    app.auto_select_models();
    save_b();
    const std::vector<std::string> heard = service.heard(2);
    const bool chose = answer_waiting() && uiq::drain() > 0 && Settings::get().llm_model.empty();
    const std::vector<std::string> only_a = {"/a/v1/models key-a", "/a/v1/models key-a"};
    if (!fetched || !chose || heard != only_a) {
        std::string where;
        for (const std::string &h : heard) where += " [" + h + "]";
        fprintf(stderr, "a lookup of the service's models went elsewhere than to the service it was started for, or its "
                        "answer was used for another (models::fetch %d, the choice without the user %d; heard%s)\n",
                fetched, chose, where.c_str());
    }
    set([&was](Settings &s) { s = was; });
    return fetched && chose && heard == only_a;
}

// Stands for the agent's worker at the moment input waiting for the phone is dropped, just before the gate
// opens (App::drop_waiting_input): notes whether the request under way had been cancelled by then. A request
// handed to the agent stands for it, as the agent's cancel clears what was handed over.
struct DropWatch {
    bool &cancelled;
    explicit DropWatch(bool &c) : cancelled(c) {}
    ~DropWatch() { cancelled = !Agent::get().has_pending(); }
};

// Input that comes while the app tries the phone (App::when_online) waits, and is handed over in the order
// it came once the phone has been tried; a request abandoned meanwhile (ESC, a new conversation) takes what
// still waits with it (fix round 3, D6: a later input went ahead of the one waiting, and the one waiting was
// handed over after a new conversation had begun), and the agent is told to cancel before what waits is
// dropped and its gate opened (fix round 4, E4: told after, a call waiting at the gate could find it open
// with nothing queued and be made in between). The phone itself is not tried: the check answers for it.
bool input_waits_for_phone(App &app) {
    std::vector<int> sent;
    auto input = [&sent](int n) { return [&sent, n] { sent.push_back(n); }; };
    // An input for a request under way, which notes as it is dropped whether the request was cancelled first.
    auto abandoned = [&sent](int n, bool &cancelled_first) {
        Agent::get().submit_text("the request under way");
        auto watch = std::make_shared<DropWatch>(cancelled_first);
        return [&sent, n, watch] { sent.push_back(n); };
    };
    app.menu.close();              // so that ESC below acts on the chat screen
    app.connecting_phone = true;   // as when_online leaves it while the phone is tried
    app.when_online(input(1));
    app.when_online(input(2));
    const bool waited = sent.empty();
    app.phone_tried();
    const bool in_order = sent == std::vector<int>{1, 2} && !app.connecting_phone;
    sent.clear();
    app.connecting_phone = true;
    bool esc_first = false, new_first = false;
    app.when_online(abandoned(3, esc_first));
    app.short_escape();            // ESC: the request is abandoned
    app.when_online(abandoned(4, new_first));   // asked after it: waits in its turn
    app.new_conversation();
    app.when_online(input(5));
    app.phone_tried();
    const bool dropped = sent == std::vector<int>{5};
    if (!waited || !in_order || !dropped || !esc_first || !new_first)
        fprintf(stderr, "input that came while the phone was tried was handed over at the wrong time (waited %d, in order %d, "
                        "dropped with the request %d; the request cancelled before it was dropped: on ESC %d, on a new "
                        "conversation %d)\n", waited, in_order, dropped, esc_first, new_first);
    return waited && in_order && dropped && esc_first && new_first;
}

// How long the main loop sleeps (sleep_ms), at made-up times well after anything the screenshots did:
// a tick after something happens, and every tick while it settles; then until the next
// once-a-second work; a tick again while a level is shown; while a timer runs, no longer than to
// the wall clock's next second.
bool loop_sleeps_ok(App &app) {
    app.menu.close();
    for (int i = 0; i < Menu::kToastTicks; ++i) { app.menu.tick(); app.chat.tick(); }
    app.level_until = app.settings_dirty_at = 0;
    app.esc_armed = false;
    app.space_tap.cancel();
    Store::get().clear_timers();
    const unsigned long t = sys::now_ms() + 60 * kSecondMs;
    app.last_tick = t;
    app.note_activity();
    const unsigned long settling = app.sleep_ms(t);
    app.last_tick = app.last_second = app.last_indicators = app.last_model_try = t + kSettleMs;
    const unsigned long quiet = app.sleep_ms(t + kSettleMs);
    app.show_level("Volume");
    const unsigned long level = app.sleep_ms(t + kSettleMs);
    app.level_until = 0;
    app.sleep_ms(t + kSettleMs);                               // quiet again from here
    app.last_second = app.last_indicators = app.last_model_try = t + 8 * kSettleMs;
    Store::get().add_timer("Tea", 300, time(nullptr));
    const unsigned long timer = app.sleep_ms(t + 2 * kSettleMs);
    Store::get().clear_timers();
    const bool ok = settling == kTickMs && quiet == kSecondMs && level == kTickMs && timer >= 1 && timer <= kSecondMs;
    if (!ok)
        fprintf(stderr, "the main loop sleeps for the wrong time (%lu ms settling, %lu quiet, %lu with a level shown, %lu "
                        "with a timer running)\n", settling, quiet, level, timer);
    return ok;
}

}  // namespace

// What the app does while nobody uses it. Checked, not pictured: last, since it changes the settings.
void App::shots_idle(Shots &shots) {
    if (!lookups_keep_their_service(*this)) shots.failed = true;
    if (!model_retries_ok(*this)) shots.failed = true;
    if (!input_waits_for_phone(*this)) shots.failed = true;
    if (!loop_sleeps_ok(*this)) shots.failed = true;
}

}  // namespace ui
