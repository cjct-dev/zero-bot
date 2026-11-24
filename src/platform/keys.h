// Keyboard abstraction shared by the device (evdev) and the simulator (SDL).
#pragma once
#include <string>

enum class Key {
    None, Char, Enter, Backspace, Delete, Tab, Esc, Up, Down, Left, Right, Home, End, PgUp, PgDn,
    // The fn layer's control keys: fn+A/S/D, fn+U/I and fn+Q/W/E on the CardputerZero.
    Mute, VolumeDown, VolumeUp, BrightnessDown, BrightnessUp, MediaPlay, MediaPrevious, MediaNext,
};

struct KeyEvent {
    Key key = Key::None;
    std::string text;   // UTF-8 for Key::Char
    int state = 1;      // 0 = released, 1 = pressed, 2 = auto-repeat
};

// A modifier (shift, ctrl, fn, caps lock) pressed and let go with no other key in between is a tap
// of its own, which the reader passes on as a Key::None press: it wakes a dark screen (and keeps a
// lit one lit), and does nothing else. A chord (fn+S, shift+A) is not a tap.
class LoneModifier {
public:
    // For every key code, pressed (`down`) or let go: true when this is the release that ends a tap.
    bool step(bool modifier, bool down) {
        if (!modifier) { armed_ = false; return false; }
        if (down) { armed_ = true; return false; }
        const bool tap = armed_;
        armed_ = false;
        return tap;
    }

private:
    bool armed_ = false;
};

namespace input {
// Begin reading the hardware keyboard (device) or SDL (simulator). `on_key` is called after each key
// is queued, on the thread that read it: the main loop's wake (uiq::wake), so a key is not left
// waiting for its sleep to end.
void start(void (*on_key)());
bool poll(KeyEvent &out);     // non-blocking; call from the UI thread
void set_dump(bool on);       // log raw key codes to stderr (for mapping work)
// A secret (a key, a password, a code) is being typed: nothing is logged until it ends, whatever set_dump says.
void set_secret(bool on);
bool dumping();               // a key read now would be logged: set_dump on, and no secret being typed
}  // namespace input
