#ifndef GAMBITREE_KEYBOARD_NAVIGATION_H
#define GAMBITREE_KEYBOARD_NAVIGATION_H

#include <X11/Xlib.h>

namespace gambitree {

enum KeyboardNavigationCommand { KEYBOARD_NONE, KEYBOARD_PREVIOUS_GAME, KEYBOARD_NEXT_GAME,
                                 KEYBOARD_PREVIOUS_FILE, KEYBOARD_NEXT_FILE,
                                 KEYBOARD_SCROLL_PAGE_UP, KEYBOARD_SCROLL_PAGE_DOWN };

KeyboardNavigationCommand keyboard_navigation_command(KeySym key, unsigned int state);
bool pgn_navigation_key(KeySym key, unsigned int state);
enum NavigationListShortcut { NAVIGATION_LIST_SHORTCUT_NONE,
                              NAVIGATION_LIST_SHORTCUT_GAMES,
                              NAVIGATION_LIST_SHORTCUT_FILES };
NavigationListShortcut navigation_list_shortcut(KeySym key,
                                                unsigned int state);
int notation_page_scroll_offset(int current, int viewport_height,
                                int line_height, int maximum_scroll,
                                bool down);

}

#endif
