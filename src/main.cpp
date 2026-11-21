#include <cstdlib>
#include <iostream>
#include <new>

#include "gambitree/command_line.h"
#include "gambitree/version.h"
#include "gambitree/x11_window.h"
#include "gambitree/pgn_library.h"

namespace {

void print_help(std::ostream& output)
{
    output
        << "Usage: " << GAMBITREE_NAME << " [OPTION]... [PGN_FILE]...\n"
        << "\n"
        << "View chess games stored in PGN files.\n"
        << "\n"
        << "Options:\n"
        << "  --board BOARD     select a board (classic, wood or wood-veneer)\n"
        << "  --engine PATH     use PATH as UCI engine (default: stockfish)\n"
        << "  --fullscreen      request fullscreen at startup\n"
        << "  --geometry GEO    set the window size and/or position\n"
        << "  --help            display this help and exit\n"
        << "  --iconic          request minimized startup\n"
        << "  --pieces NAME     select pieces (classic, meridian-shaded or cburnett)\n"
        << "  --print-window-id print WINDOWID=<X11-window-id> after mapping\n"
        << "  --role NAME       set WM_WINDOW_ROLE for window-manager rules\n"
        << "  --title TITLE     set the window title\n"
        << "  --version         display version information and exit\n"
        ;
}

void print_version(std::ostream& output)
{
    output << GAMBITREE_NAME << " " << GAMBITREE_VERSION << "\n";
}

}

int main(int argc, char* argv[])
{
    int index;
    std::string error;
    std::vector<std::string> load_errors;
    gambitree::CommandLineOptions options;
    gambitree::CommandLineAction action;
    gambitree::PgnLibrary library;

    action = gambitree::parse_command_line(argc, argv, options);
    if (!options.warning.empty())
        std::cerr << GAMBITREE_NAME << ": warning: " << options.warning << "\n";
    if (action == gambitree::COMMAND_LINE_HELP) {
            print_help(std::cout);
            return 0;
    }
    if (action == gambitree::COMMAND_LINE_VERSION) {
            print_version(std::cout);
            return 0;
    }
    if (action == gambitree::COMMAND_LINE_ERROR) {
        std::cerr << GAMBITREE_NAME << ": " << options.error << "\n"
                  << "Try '" << GAMBITREE_NAME << " --help' for more information.\n";
        return 2;
    }

    if (!options.paths.empty() && !library.load(options.paths, load_errors)) {
        for (index = 0; index < static_cast<int>(load_errors.size()); ++index)
            std::cerr << GAMBITREE_NAME << ": " << load_errors[index] << "\n";
        return 1;
    }
    for (index = 0; index < static_cast<int>(load_errors.size()); ++index)
        std::cerr << GAMBITREE_NAME << ": " << load_errors[index] << "\n";
    try {
        bool diagnostics = std::getenv("GAMBITREE_DIAGNOSTICS") != 0;
        if (!gambitree::run_x11_window(options.paths.empty() ? 0 : &library,
                                       options.engine_path, options.piece_theme,
                                       options.piece_theme_explicit,
                                       options.board_theme,
                                       options.geometry,
                                       gambitree::should_print_window_id(options.print_window_id, diagnostics),
                                       options.window_title_explicit ? &options.window_title : 0,
                                       options.window_role,
                                       options.fullscreen, options.iconic,
                                       argc, argv, error)) {
            std::cerr << GAMBITREE_NAME << ": " << error << "\n";
            return 1;
        }
    } catch (const std::bad_alloc&) {
        std::cerr << GAMBITREE_NAME << ": insufficient memory\n";
        return 1;
    }
    return 0;
}
