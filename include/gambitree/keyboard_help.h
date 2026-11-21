#ifndef GAMBITREE_KEYBOARD_HELP_H
#define GAMBITREE_KEYBOARD_HELP_H

#include <X11/Xlib.h>
#include "gambitree/notation_layout.h"

namespace gambitree {

struct KeyboardHelpEntry {
    const char* group;
    const char* keys;
    const char* description;
};

/* One content table for the overlay; bindings remain in their existing handlers. */
const KeyboardHelpEntry* keyboard_help_entries(std::size_t& count);
bool keyboard_help_requested(KeySym translated, const char* text, int length);

struct KeyboardHelpState {
    bool visible;
    int scroll;
    KeyboardHelpState() : visible(false), scroll(0) {}
    /* True consumes the event. An existing confirmation always has priority. */
    bool key(KeySym translated, bool question, bool confirmation,
             int line_height, int viewport_height, int maximum_scroll);
    void scroll_by(int distance, int maximum_scroll);
};

enum KeyboardHelpTextKind { HELP_TITLE, HELP_CLOSE, HELP_GROUP, HELP_KEY, HELP_DESCRIPTION, HELP_FOOTER };
struct KeyboardHelpText {
    KeyboardHelpTextKind kind;
    std::string text;
    int x, y, width, height;
};

struct KeyboardHelpLayout {
    int x, y, width, height;
    int body_top, body_bottom, line_height, content_height, columns;
    std::vector<KeyboardHelpText> fixed;
    /* Content y is relative to body_top, before the independent help scroll. */
    std::vector<KeyboardHelpText> content;
    int viewport_height() const;
    int max_scroll() const;
    static KeyboardHelpLayout build(int window_width, int window_height,
        const NotationFontMetrics& body, const NotationFontMetrics& keys,
        const NotationFontMetrics& title);
};

}
#endif
