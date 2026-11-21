#ifndef GAMBITREE_X11_WINDOW_H
#define GAMBITREE_X11_WINDOW_H

#include <string>

namespace gambitree {

class PgnLibrary;
struct WindowGeometry;

/* Opens and owns the native X11 shell until the user closes it. */
/* A null window_title keeps the default; a non-null title may be empty. */
bool run_x11_window(PgnLibrary* library, const std::string& engine_path,
                    const std::string& piece_theme, bool piece_theme_explicit,
                    const std::string& board_theme,
                    const WindowGeometry& geometry,
                    bool print_window_id, const std::string* window_title,
                    const std::string& window_role,
                    bool fullscreen, bool iconic,
                    int argc, char* argv[], std::string& error);

}

#endif
