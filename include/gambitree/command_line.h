#ifndef GAMBITREE_COMMAND_LINE_H
#define GAMBITREE_COMMAND_LINE_H

#include "gambitree/window_geometry.h"

#include <string>
#include <vector>

namespace gambitree {

enum CommandLineAction { COMMAND_LINE_RUN, COMMAND_LINE_HELP, COMMAND_LINE_VERSION, COMMAND_LINE_ERROR };

struct CommandLineOptions {
    std::vector<std::string> paths;
    std::string engine_path;
    std::string window_title;
    bool window_title_explicit;
    std::string window_role;
    std::string piece_theme;
    bool piece_theme_explicit;
    std::string board_theme;
    WindowGeometry geometry;
    std::string warning;
    std::string error;
    bool print_window_id;
    bool fullscreen;
    bool iconic;
    CommandLineOptions();
};

CommandLineAction parse_command_line(int argc, char* const argv[], CommandLineOptions& options);
bool should_print_window_id(bool option_requested, bool diagnostics_enabled);

}

#endif
