#include "ui/key_guide.h"

namespace keyguide {

const std::vector<Section> &sections(bool in_setup_guide) {
    // Keys are named as the README names them. Each line has to fit one row of a 320-pixel
    // screen: the --shots run measures them with the real fonts and fails when one does not.
    static const std::vector<Section> guide = {
        {"On the chat screen",
         {{"TAB", "talk; TAB again to send"},
          {"ENTER", "type; ENTER again to send"},
          {"SPACE", "settings (or TAB twice)"},
          {"SPACE SPACE", "save the conversation (tests)"},   // DIAGNOSTICS (temporary)
          {"del del", "start a new conversation"},
          {"ESC", "stop what is happening"},
          {"hold ESC", "leave Zero Bot"},
          {"F  X", "scroll up, down"},
          {"fn+F  fn+X", "scroll while typing"},
          {"W  E", "previous, next track or station"},
          {"A", "mute or unmute"},
          {"S  D", "volume down, up"},
          {"H", "this list of keys"}}},
        {"Everywhere",
         {{"fn+S  fn+D", "volume down, up"},
          {"fn+A", "mute or unmute"},
          {"fn+U  fn+I", "screen dimmer, brighter"},
          {"fn+Q", "pause or resume music, radio"},
          {"fn+W  fn+E", "previous, next track or station"},
          {"any key", "silence a timer's alarm"},
          {"any key", "turn the screen back on"}}},
        {"In settings",
         {{"F  X", "move up, down"},
          {"Z  C", "change a value"},
          {"ENTER", "choose"},
          {"ESC  del", "go back"},
          {"SPACE  TAB", "close settings"}}},
    };
    static const std::vector<Section> setup = [] {
        std::vector<Section> s = guide;
        s.back().rows.back() = {"TAB", "close the setup guide"};
        return s;
    }();
    return in_setup_guide ? setup : guide;
}

bool is_guide_key(const KeyEvent &ev) {
    return ev.state == 1 && ev.key == Key::Char && (ev.text == "h" || ev.text == "H" || ev.text == "?");
}

bool is_settings_key(const KeyEvent &ev) { return ev.state == 1 && ev.key == Key::Char && ev.text == " "; }

// DIAGNOSTICS (temporary): see key_guide.h.
bool DoubleTap::press(unsigned long now) {
    if (pending_ && now - at_ < window_) {
        pending_ = false;
        return true;
    }
    pending_ = true;
    at_ = now;
    return false;
}

bool DoubleTap::single_due(unsigned long now) {
    if (!pending_ || now - at_ < window_) return false;
    pending_ = false;
    return true;
}

ChatKey chat_key(const KeyEvent &ev) {
    if (ev.state == 0) return ChatKey::None;
    const bool press = ev.state == 1;   // not a repeat of a held key
    switch (ev.key) {
        // Two taps of the delete key (not a held key) start a new conversation.
        case Key::Backspace:
        case Key::Delete: return press ? ChatKey::DeleteTap : ChatKey::None;
        case Key::Enter: return press ? ChatKey::Type : ChatKey::None;
        case Key::Up: return ChatKey::ScrollUp;
        case Key::Down: return ChatKey::ScrollDown;
        case Key::PgUp: return ChatKey::PageUp;
        case Key::PgDn: return ChatKey::PageDown;
        // Typing starts only with ENTER, so plain letters are free for reading: F and X scroll like
        // fn+F and fn+X, H shows the keys, and any other letter just says how to start typing.
        case Key::Char:
            if (ev.text == "f" || ev.text == "F") return ChatKey::ScrollUp;
            if (ev.text == "x" || ev.text == "X") return ChatKey::ScrollDown;
            // W, E, A, S and D do what they do with fn (the media and volume keys), so music needs no
            // fn either. Volume repeats while held, as fn+S and fn+D do; the others act once a press.
            if (ev.text == "w" || ev.text == "W") return press ? ChatKey::PreviousTrack : ChatKey::None;
            if (ev.text == "e" || ev.text == "E") return press ? ChatKey::NextTrack : ChatKey::None;
            if (ev.text == "a" || ev.text == "A") return press ? ChatKey::Mute : ChatKey::None;
            if (ev.text == "s" || ev.text == "S") return ChatKey::VolumeDown;
            if (ev.text == "d" || ev.text == "D") return ChatKey::VolumeUp;
            if (is_settings_key(ev)) return ChatKey::Settings;
            if (is_guide_key(ev)) return ChatKey::Guide;
            return press ? ChatKey::TypeHint : ChatKey::None;
        default: return ChatKey::None;
    }
}

std::string idle_hint(bool configured) {
    return configured ? "TAB talk   ·   ENTER type   ·   SPACE settings   ·   H keys"
                      : "SPACE settings   ·   H keys   ·   hold ESC to exit";
}

}  // namespace keyguide
