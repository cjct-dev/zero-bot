#include "platform/keys.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if ZB_DESKTOP
#include <SDL2/SDL.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace input {

namespace {
std::mutex g_mtx;
std::deque<KeyEvent> g_queue;
std::atomic<bool> g_dump{false};
std::atomic<void (*)()> g_on_key{nullptr};   // start()'s on_key

void push(const KeyEvent &ev) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_queue.push_back(ev);
    }
    if (void (*on_key)() = g_on_key) on_key();
}

#if !ZB_DESKTOP
// The CardputerZero keypad driver reports the blue "sym" layer as dedicated key
// codes (see /usr/share/keymaps/tca8418_keypad_m5stack_keymap.map on the device).
struct SymMap { int code; const char *text; };
const SymMap kSymLayer[] = {
    {26, "!"}, {27, "@"}, {39, "#"}, {40, "$"}, {41, "%"}, {43, "^"}, {51, "&"}, {52, "*"},
    {53, "("}, {94, ")"}, {55, "~"}, {69, "`"}, {70, "_"}, {71, "-"}, {72, "+"}, {73, "="},
    {74, "["}, {75, "]"}, {76, "{"}, {77, "}"}, {79, ";"}, {80, ":"}, {81, "'"}, {82, "\""},
    {83, "<"}, {85, ">"}, {86, "\\"}, {89, "|"}, {90, ","}, {91, "."}, {92, "/"}, {93, "?"},
};

char letter_for(int code) {
    static const struct { int code; char ch; } map[] = {
        {KEY_Q, 'q'}, {KEY_W, 'w'}, {KEY_E, 'e'}, {KEY_R, 'r'}, {KEY_T, 't'}, {KEY_Y, 'y'}, {KEY_U, 'u'},
        {KEY_I, 'i'}, {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_A, 'a'}, {KEY_S, 's'}, {KEY_D, 'd'}, {KEY_F, 'f'},
        {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_J, 'j'}, {KEY_K, 'k'}, {KEY_L, 'l'}, {KEY_Z, 'z'}, {KEY_X, 'x'},
        {KEY_C, 'c'}, {KEY_V, 'v'}, {KEY_B, 'b'}, {KEY_N, 'n'}, {KEY_M, 'm'},
    };
    for (auto &m : map)
        if (m.code == code) return m.ch;
    return 0;
}

char digit_for(int code) {
    if (code >= KEY_1 && code <= KEY_9) return static_cast<char>('1' + (code - KEY_1));
    if (code == KEY_0) return '0';
    return 0;
}

int open_keyboard() {
    const char *env = getenv("APPLAUNCH_LINUX_KEYBOARD_DEVICE");
    const char *candidates[] = {env, "/dev/input/by-path/platform-3f804000.i2c-event"};
    for (const char *path : candidates) {
        if (!path || !*path) continue;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    // Fall back to scanning for the keypad by name.
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;
    int found = -1;
    while (dirent *de = readdir(dir)) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + de->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        if (strstr(name, "tca8418")) { found = fd; break; }
        close(fd);
    }
    closedir(dir);
    return found;
}

struct Decoder {
    bool shift = false, fn = false, caps = false;

    // Returns true and fills `ev` when the code maps to something the app understands.
    bool decode(int code, KeyEvent &ev) const {
        switch (code) {
            case KEY_ENTER: case KEY_KPENTER: ev.key = Key::Enter; return true;
            case KEY_BACKSPACE: ev.key = fn ? Key::Delete : Key::Backspace; return true;
            case KEY_DELETE: ev.key = Key::Delete; return true;
            case KEY_TAB: ev.key = Key::Tab; return true;
            case KEY_ESC: ev.key = Key::Esc; return true;
            case KEY_UP: ev.key = Key::Up; return true;
            case KEY_DOWN: ev.key = Key::Down; return true;
            case KEY_LEFT: ev.key = Key::Left; return true;
            case KEY_RIGHT: ev.key = Key::Right; return true;
            case KEY_HOME: ev.key = Key::Home; return true;
            case KEY_END: ev.key = Key::End; return true;
            case KEY_PAGEUP: ev.key = Key::PgUp; return true;
            case KEY_PAGEDOWN: ev.key = Key::PgDn; return true;
            case KEY_SPACE: ev.key = Key::Char; ev.text = " "; return true;
            // The keypad driver's fn layer sends these as their own codes.
            case KEY_MUTE: ev.key = Key::Mute; return true;
            case KEY_VOLUMEDOWN: ev.key = Key::VolumeDown; return true;
            case KEY_VOLUMEUP: ev.key = Key::VolumeUp; return true;
            case KEY_BRIGHTNESSDOWN: ev.key = Key::BrightnessDown; return true;
            case KEY_BRIGHTNESSUP: ev.key = Key::BrightnessUp; return true;
            case KEY_PLAYPAUSE: ev.key = Key::MediaPlay; return true;
            case KEY_PREVIOUSSONG: ev.key = Key::MediaPrevious; return true;
            case KEY_NEXTSONG: ev.key = Key::MediaNext; return true;
            default: break;
        }
        if (fn) {  // orange layer, in case the driver leaves it to userspace
            switch (code) {
                case KEY_F: ev.key = Key::Up; return true;
                case KEY_X: ev.key = Key::Down; return true;
                case KEY_Z: ev.key = Key::Left; return true;
                case KEY_C: ev.key = Key::Right; return true;
                case KEY_K: ev.key = Key::Home; return true;
                case KEY_N: ev.key = Key::End; return true;
                case KEY_L: ev.key = Key::PgUp; return true;
                case KEY_M: ev.key = Key::PgDn; return true;
                case KEY_A: ev.key = Key::Mute; return true;
                case KEY_S: ev.key = Key::VolumeDown; return true;
                case KEY_D: ev.key = Key::VolumeUp; return true;
                case KEY_U: ev.key = Key::BrightnessDown; return true;
                case KEY_I: ev.key = Key::BrightnessUp; return true;
                case KEY_Q: ev.key = Key::MediaPlay; return true;
                case KEY_W: ev.key = Key::MediaPrevious; return true;
                case KEY_E: ev.key = Key::MediaNext; return true;
                default: break;
            }
        }
        for (auto &s : kSymLayer)
            if (s.code == code) { ev.key = Key::Char; ev.text = s.text; return true; }
        if (char ch = letter_for(code)) {
            if (shift != caps) ch = static_cast<char>(ch - 'a' + 'A');
            ev.key = Key::Char; ev.text = std::string(1, ch); return true;
        }
        if (char ch = digit_for(code)) { ev.key = Key::Char; ev.text = std::string(1, ch); return true; }
        return false;
    }
};

bool repeats(Key k) {
    return k == Key::Backspace || k == Key::Delete || k == Key::Up || k == Key::Down ||
           k == Key::Left || k == Key::Right || k == Key::PgUp || k == Key::PgDn ||
           k == Key::VolumeDown || k == Key::VolumeUp || k == Key::BrightnessDown || k == Key::BrightnessUp;
}

void reader_thread() {
    int fd = open_keyboard();
    if (fd < 0) {
        fprintf(stderr, "[input] keyboard device not found\n");
        return;
    }
    Decoder dec;
    LoneModifier lone;
    int repeat_code = -1;
    KeyEvent repeat_ev;
    int timeout = -1;
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        int r = poll(&pfd, 1, timeout);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) {  // synthesize auto-repeat; the keypad driver does not send it
            if (repeat_code >= 0) { push(repeat_ev); timeout = 70; }
            continue;
        }
        input_event ie;
        if (read(fd, &ie, sizeof(ie)) != static_cast<ssize_t>(sizeof(ie))) break;
        if (ie.type != EV_KEY) continue;
        if (g_dump) fprintf(stderr, "[key] code=%d value=%d\n", ie.code, ie.value);
        const bool down = ie.value != 0;
        bool modifier = true;
        switch (ie.code) {
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: dec.shift = down; break;
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL: break;   // a modifier, with no layer of its own
            case KEY_FN: dec.fn = down; break;
            case KEY_CAPSLOCK: if (ie.value == 1) dec.caps = !dec.caps; break;
            default: modifier = false; break;
        }
        if (ie.value == 2) continue;  // we generate our own repeats
        // A modifier tapped on its own, or a key the app has no use for (alt, fn+digit): a Key::None
        // press, which wakes the screen and does nothing else.
        if (lone.step(modifier, down)) { KeyEvent tap; push(tap); }
        if (modifier) continue;
        KeyEvent ev;
        if (!dec.decode(ie.code, ev)) {
            if (down) { KeyEvent unknown; push(unknown); }
            continue;
        }
        ev.state = down ? 1 : 0;
        push(ev);
        if (down && repeats(ev.key)) {
            repeat_code = ie.code; repeat_ev = ev; repeat_ev.state = 2; timeout = 420;
        } else if (!down && ie.code == repeat_code) {
            repeat_code = -1; timeout = -1;
        } else if (down) {
            repeat_code = -1; timeout = -1;
        }
    }
    close(fd);
}
#else   // ---------------------------------------------------------------- SDL
int sdl_watch(void *, SDL_Event *e) {
    if (e->type == SDL_TEXTINPUT) {
        KeyEvent ev; ev.key = Key::Char; ev.text = e->text.text; ev.state = 1;
        if (ev.text != "\t") push(ev);
        return 0;
    }
    if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP) return 0;
    KeyEvent ev;
    ev.state = e->type == SDL_KEYUP ? 0 : (e->key.repeat ? 2 : 1);
    switch (e->key.keysym.sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: ev.key = Key::Enter; break;
        case SDLK_BACKSPACE: ev.key = Key::Backspace; break;
        case SDLK_DELETE: ev.key = Key::Delete; break;
        // Stand-ins for the device's fn layer: F9/F10/F11 volume, F7/F8 brightness, F5/F6 music.
        case SDLK_F9: ev.key = Key::Mute; break;
        case SDLK_F10: ev.key = Key::VolumeDown; break;
        case SDLK_F11: ev.key = Key::VolumeUp; break;
        case SDLK_F7: ev.key = Key::BrightnessDown; break;
        case SDLK_F8: ev.key = Key::BrightnessUp; break;
        case SDLK_F5: ev.key = Key::MediaPlay; break;
        case SDLK_F6: ev.key = Key::MediaNext; break;
        case SDLK_TAB: ev.key = Key::Tab; break;
        case SDLK_ESCAPE: ev.key = Key::Esc; break;
        case SDLK_UP: ev.key = Key::Up; break;
        case SDLK_DOWN: ev.key = Key::Down; break;
        case SDLK_LEFT: ev.key = Key::Left; break;
        case SDLK_RIGHT: ev.key = Key::Right; break;
        case SDLK_HOME: ev.key = Key::Home; break;
        case SDLK_END: ev.key = Key::End; break;
        case SDLK_PAGEUP: ev.key = Key::PgUp; break;
        case SDLK_PAGEDOWN: ev.key = Key::PgDn; break;
        default: return 0;
    }
    if (ev.key == Key::Tab && ev.state == 2) return 0;  // push-to-talk wants a clean hold
    push(ev);
    return 0;
}
#endif
}  // namespace

void start(void (*on_key)()) {
    g_on_key = on_key;
#if ZB_DESKTOP
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_AddEventWatch(sdl_watch, nullptr);
        SDL_StartTextInput();
    }
#else
    std::thread(reader_thread).detach();
#endif
}

bool poll(KeyEvent &out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_queue.empty()) return false;
    out = g_queue.front();
    g_queue.pop_front();
    return true;
}

void set_dump(bool on) { g_dump = on; }

}  // namespace input
