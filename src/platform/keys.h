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
    bool ctrl = false;
};

namespace input {
void start();                 // begin reading the hardware keyboard (device) or SDL (simulator)
bool poll(KeyEvent &out);     // non-blocking; call from the UI thread
void inject(const KeyEvent &ev);
void set_dump(bool on);       // log raw key codes to stderr (for mapping work)
}  // namespace input
