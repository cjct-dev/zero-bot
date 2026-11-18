#include "ui/key_guide.h"

namespace keyguide {

const std::vector<Section> &sections() {
    // Keys are named as the README names them. Each line has to fit one row of a 320-pixel
    // screen: the --shots run measures them with the real fonts and fails when one does not.
    static const std::vector<Section> guide = {
        {"On the chat screen",
         {{"TAB", "talk; TAB again to send"},
          {"ENTER", "type; ENTER again to send"},
          {"SPACE", "settings (or TAB twice)"},
          {"del del", "start a new conversation"},
          {"ESC", "stop what is happening"},
          {"hold ESC", "leave Zero Bot"},
          {"F  X", "scroll up, down"},
          {"fn+F  fn+X", "scroll while typing"},
          {"H", "this list of keys"}}},
        {"Everywhere",
         {{"fn+S  fn+D", "volume down, up"},
          {"fn+A", "mute or unmute"},
          {"fn+U  fn+I", "screen dimmer, brighter"},
          {"fn+Q", "pause or resume music"},
          {"fn+W  fn+E", "previous, next track"},
          {"any key", "silence a timer's alarm"}}},
        {"In settings",
         {{"F  X", "move up, down"},
          {"Z  C", "change a value"},
          {"ENTER", "choose"},
          {"ESC", "go back"},
          {"SPACE  TAB", "close settings"}}},
    };
    return guide;
}

bool is_guide_key(const KeyEvent &ev) {
    return ev.state == 1 && ev.key == Key::Char && (ev.text == "h" || ev.text == "H" || ev.text == "?");
}

bool is_settings_key(const KeyEvent &ev) { return ev.state == 1 && ev.key == Key::Char && ev.text == " "; }

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
