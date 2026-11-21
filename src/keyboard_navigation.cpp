#include "gambitree/keyboard_navigation.h"

#include <X11/keysym.h>
#include <algorithm>

namespace gambitree {

bool pgn_navigation_key(KeySym key, unsigned int state)
{
    const KeyboardNavigationCommand command = keyboard_navigation_command(key, state);
    return key == XK_Left || key == XK_Right || key == XK_Up || key == XK_Down ||
           key == XK_Home || key == XK_End || command == KEYBOARD_PREVIOUS_GAME ||
           command == KEYBOARD_NEXT_GAME || command == KEYBOARD_PREVIOUS_FILE ||
           command == KEYBOARD_NEXT_FILE;
}

KeyboardNavigationCommand keyboard_navigation_command(KeySym key, unsigned int state)
{
    bool control = (state & ControlMask) != 0;
    bool shift = (state & ShiftMask) != 0;
    if (shift && key == XK_Page_Up) return KEYBOARD_SCROLL_PAGE_UP;
    if (shift && key == XK_Page_Down) return KEYBOARD_SCROLL_PAGE_DOWN;
    if (key == XK_Page_Up) return control ? KEYBOARD_PREVIOUS_FILE : KEYBOARD_PREVIOUS_GAME;
    if (key == XK_Page_Down) return control ? KEYBOARD_NEXT_FILE : KEYBOARD_NEXT_GAME;
    return KEYBOARD_NONE;
}

NavigationListShortcut navigation_list_shortcut(KeySym key,
                                                 unsigned int state)
{
    if (!(state & ControlMask) || (key != XK_g && key != XK_G))
        return NAVIGATION_LIST_SHORTCUT_NONE;
    return (state & ShiftMask) ? NAVIGATION_LIST_SHORTCUT_FILES
                               : NAVIGATION_LIST_SHORTCUT_GAMES;
}

int notation_page_scroll_offset(int current, int viewport_height,
                                int line_height, int maximum_scroll,
                                bool down)
{
    if (maximum_scroll < 0) maximum_scroll = 0;
    if (current < 0) current = 0;
    if (current > maximum_scroll) current = maximum_scroll;
    if (line_height < 1) line_height = 1;
    int page = viewport_height - 2 * line_height;
    if (page < line_height) page = line_height;
    if (down) {
        if (page > maximum_scroll - current) return maximum_scroll;
        return current + page;
    }
    return current < page ? 0 : current - page;
}

}
