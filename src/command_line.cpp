#include "gambitree/command_line.h"
#include "gambitree/theme.h"

#include <cstring>

namespace gambitree {

CommandLineOptions::CommandLineOptions()
    : paths(), engine_path("stockfish"), window_title(), window_title_explicit(false),
      window_role(), piece_theme("classic"),
      piece_theme_explicit(false), board_theme(), geometry(), warning(), error(),
      print_window_id(false), fullscreen(false), iconic(false) {}

CommandLineAction parse_command_line(int argc, char* const argv[], CommandLineOptions& options)
{
    int index;
    options = CommandLineOptions();
    for (index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--help") == 0) return COMMAND_LINE_HELP;
        if (std::strcmp(argv[index], "--version") == 0) return COMMAND_LINE_VERSION;
        if (std::strcmp(argv[index], "--print-window-id") == 0) { options.print_window_id = true; continue; }
        if (std::strcmp(argv[index], "--fullscreen") == 0) { options.fullscreen = true; continue; }
        if (std::strcmp(argv[index], "--iconic") == 0) { options.iconic = true; continue; }
        if (std::strcmp(argv[index], "--title") == 0) {
            options.window_title_explicit = true;
            options.window_title.clear();
            if (index + 1 < argc && std::strncmp(argv[index + 1], "--", 2) != 0)
                options.window_title = argv[++index];
            continue;
        }
        if (std::strcmp(argv[index], "--role") == 0) {
            if (++index >= argc || argv[index][0] == '\0') {
                options.error = "--role requires a non-empty name";
                return COMMAND_LINE_ERROR;
            }
            options.window_role = argv[index];
            for (std::size_t i = 0; i < options.window_role.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(options.window_role[i]);
                if (c < 32 || c > 126) {
                    options.error = "--role accepts only printable ASCII characters";
                    return COMMAND_LINE_ERROR;
                }
            }
            continue;
        }
        if (std::strcmp(argv[index], "--geometry") == 0) {
            if (index + 1 >= argc || std::strncmp(argv[index + 1], "--", 2) == 0) {
                options.warning = "--geometry has no value; keeping the previous or saved geometry";
                continue;
            }
            WindowGeometry parsed;
            if (!parse_window_geometry(argv[++index], parsed)) {
                options.warning = "invalid --geometry ignored; keeping the previous or saved geometry";
            } else {
                options.geometry = parsed;
                options.warning = parsed.adjusted ? "--geometry adjusted to safe limits" : "";
            }
            continue;
        }
        if (std::strcmp(argv[index], "--engine") == 0) {
            if (++index >= argc) { options.error = "--engine requires a path"; return COMMAND_LINE_ERROR; }
            options.engine_path = argv[index];
            continue;
        }
        if (std::strcmp(argv[index], "--board") == 0) {
            if (++index >= argc || argv[index][0] == '\0') {
                options.error = "--board requires a name";
                return COMMAND_LINE_ERROR;
            }
            options.board_theme = argv[index];
            if (!valid_board_theme_id(options.board_theme)) {
                options.error = "--board accepts only classic, wood or wood-veneer";
                return COMMAND_LINE_ERROR;
            }
            continue;
        }
        if (std::strcmp(argv[index], "--pieces") == 0) {
            std::size_t character;
            if (++index >= argc) { options.error = "--pieces requires a name"; return COMMAND_LINE_ERROR; }
            options.piece_theme = argv[index];
            options.piece_theme_explicit = true;
            if (options.piece_theme.empty()) { options.error = "--pieces requires a name"; return COMMAND_LINE_ERROR; }
            for (character = 0; character < options.piece_theme.size(); ++character) {
                char c = options.piece_theme[character];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_')) {
                    options.error = "--pieces accepts only letters, digits, '-' and '_'";
                    return COMMAND_LINE_ERROR;
                }
            }
            continue;
        }
        if (std::strncmp(argv[index], "--", 2) == 0) {
            options.error = std::string("unrecognized option '") + argv[index] + "'";
            return COMMAND_LINE_ERROR;
        }
        options.paths.push_back(argv[index]);
    }
    return COMMAND_LINE_RUN;
}

bool should_print_window_id(bool option_requested, bool diagnostics_enabled)
{ return option_requested || diagnostics_enabled; }

}
