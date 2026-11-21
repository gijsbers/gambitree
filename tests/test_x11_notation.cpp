#include "gambitree/x11_notation_renderer.h"
#include "gambitree/board_renderer.h"
#include "gambitree/navigation.h"
#include "gambitree/navigation_list.h"
#include "gambitree/wheel_event_batch.h"
#include "gambitree/session_state.h"
#include "test_paths.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <X11/keysym.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

using namespace gambitree;
namespace {
int failures = 0;
std::string test_scratch_directory;
void check(bool ok, const char* label) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << label << "\n"; }
}

unsigned char channel_from_pixel(unsigned long value, unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long component = mask;
    while (component && !(component & 1UL)) { ++shift; component >>= 1; }
    if (!component) return 0;
    return static_cast<unsigned char>((((value & mask) >> shift) * 255UL +
                                       component / 2) / component);
}

bool save_ppm(const char* path, Display* display, Visual* visual,
              Drawable drawable, int width, int height)
{
    XImage* image = XGetImage(display, drawable, 0, 0,
                              static_cast<unsigned int>(width),
                              static_cast<unsigned int>(height),
                              AllPlanes, ZPixmap);
    if (!image) return false;
    FILE* output = std::fopen(path, "wb");
    if (!output) { XDestroyImage(image); return false; }
    std::fprintf(output, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const unsigned long pixel = XGetPixel(image, x, y);
        const unsigned char rgb[3] = {
            channel_from_pixel(pixel, visual->red_mask),
            channel_from_pixel(pixel, visual->green_mask),
            channel_from_pixel(pixel, visual->blue_mask)
        };
        std::fwrite(rgb, 1, sizeof(rgb), output);
    }
    XDestroyImage(image);
    return std::fclose(output) == 0;
}

pid_t start_session_viewer(Display* display, const std::string& directory,
                            const char* pieces = 0, const char* engine = 0,
                            const char* pgn = 0, Window* mapped_window = 0,
                            const char* title = 0, const char* second_pgn = 0,
                            const char* board = 0, const char* locale = 0,
                            const char* geometry = 0, const char* role = 0,
                            bool fullscreen = false, bool iconic = false)
{
    int output[2];
    if (pipe(output) != 0) return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        close(output[0]);
        dup2(output[1], STDOUT_FILENO); close(output[1]);
        const int log = open((directory + "/viewer.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (log >= 0) { dup2(log, STDERR_FILENO); close(log); }
        close(ConnectionNumber(display));
        setenv("XDG_STATE_HOME", directory.c_str(), 1);
        setenv("GAMBITREE_DIAGNOSTICS", "1", 1);
        if (locale) setenv("LC_ALL", locale, 1);
        std::vector<std::string> arguments;
        arguments.push_back("gambitree"); arguments.push_back("--engine");
        arguments.push_back(engine ? engine : "/no/gambitree-test-engine");
        arguments.push_back("--print-window-id");
        if (pieces) { arguments.push_back("--pieces"); arguments.push_back(pieces); }
        if (pgn) arguments.push_back(pgn);
        if (second_pgn) arguments.push_back(second_pgn);
        if (title) { arguments.push_back("--title"); arguments.push_back(title); }
        if (board) { arguments.push_back("--board"); arguments.push_back(board); }
        if (geometry) { arguments.push_back("--geometry"); arguments.push_back(geometry); }
        if (role) { arguments.push_back("--role"); arguments.push_back(role); }
        if (fullscreen) arguments.push_back("--fullscreen");
        if (iconic) arguments.push_back("--iconic");
        std::vector<char*> argv;
        for (std::size_t i = 0; i < arguments.size(); ++i)
            argv.push_back(const_cast<char*>(arguments[i].c_str()));
        argv.push_back(0);
        execv("./gambitree", &argv[0]);
        _exit(127);
    }
    close(output[1]);
    std::string greeting;
    for (int i = 0; pid > 0 && i < 100; ++i) {
        struct pollfd descriptor = {output[0], POLLIN, 0};
        if (poll(&descriptor, 1, 50) > 0) {
            char bytes[128]; const ssize_t count = read(output[0], bytes, sizeof(bytes));
            if (count <= 0) break;
            greeting.append(bytes, static_cast<std::size_t>(count));
            if (greeting.find('\n') != std::string::npos) break;
        }
    }
    close(output[0]);
    check(greeting.find("WINDOWID=") == 0, "actual session viewer reached its mapped window");
    if (mapped_window && greeting.find("WINDOWID=") == 0)
        *mapped_window = static_cast<Window>(std::strtoul(greeting.c_str() + 9, 0, 0));
    return pid;
}

bool stop_session_viewer(pid_t pid)
{
    if (pid <= 0) return false;
    kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 250; ++i) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        usleep(20000);
    }
    kill(pid, SIGKILL); // Cleanup only this failed test child.
    waitpid(pid, &status, 0);
    return false;
}

void test_session_window(Display* display)
{
    const int previous_failures = failures;
    const std::string directory = test_paths::make_directory("session", test_scratch_directory);
    if (directory.empty()) { check(false, "create isolated X11 session fixture"); return; }
    const std::string base(directory), path = base + "/gambitree/state";
    SessionState initial = default_session_state(), loaded;
    initial.width = 800; initial.height = 600;
    check(save_session_state(path, initial), "seed X11 preferences without touching user settings");
    pid_t viewer = start_session_viewer(display, base, "cburnett");
    bool saved = false;
    for (int i = 0; viewer > 0 && i < 300 && !saved; ++i) {
        saved = load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                loaded.piece_set_id == "cburnett";
        if (!saved) usleep(20000); // No new X events: exercise the real idle deadline.
    }
    const bool idle_exit = stop_session_viewer(viewer);
    check(saved && idle_exit,
          "actual idle event loop saves a preference without further X11 events");
    viewer = start_session_viewer(display, base, "meridian-shaded");
    check(stop_session_viewer(viewer) &&
          load_session_state(path, loaded) == SESSION_STATE_LOADED && loaded.piece_set_id == "meridian-shaded",
          "SIGTERM flushes a pending explicit preference through normal shutdown");
    viewer = start_session_viewer(display, base);
    loaded.board_theme_id = "wood"; loaded.engine_points = 20;
    check(save_session_state(path, loaded), "another process publishes newer preferences");
    // A WM may still adjust startup geometry; even then unrelated fields must
    // survive. The CPU test separately requires zero writes for a clean saver.
    check(stop_session_viewer(viewer) && load_session_state(path, loaded) == SESSION_STATE_LOADED &&
          loaded.board_theme_id == "wood" && loaded.engine_points == 20,
          "a clean live process does not overwrite newer preferences when terminated");
    if (failures != previous_failures) {
        std::cerr << "Session X11 diagnostics retained in " << base << '\n';
    } else {
        unlink(path.c_str()); unlink((path + ".lock").c_str());
        rmdir((base + "/gambitree").c_str()); unlink((base + "/viewer.log").c_str());
        rmdir(base.c_str());
    }
}

bool matches_image_region(XImage* image, int x, int y, XImage* reference)
{
    if (!image || !reference) return false;
    for (int row = 0; row < reference->height; ++row)
        for (int column = 0; column < reference->width; ++column)
            if (XGetPixel(image, x + column, y + row) != XGetPixel(reference, column, row)) return false;
    return true;
}

void test_analysis_preview_rendering(Display* display, int screen)
{
    PgnDocument document;
    check(PgnParser::parse("1. d4 d5 *", document), "analysis preview rendering PGN parses");
    if (document.games.empty()) return;
    PgnGame& game = *document.games[0];
    MoveNavigator navigator(game);
    check(navigator.forward(), "select a PGN move as the analysis origin");
    const MoveNode* pgn_selection = navigator.current();
    X11NotationRenderer notation(display, screen);
    BoardRenderer board(display, screen);
    std::string error;
    check(board.load_piece_set("cburnett", error), "preview uses the existing piece renderer");
    board.set_board_theme("wood-veneer");
    UciAnalysis analysis;
    analysis.generation = 11; analysis.valid = true;
    analysis.side_to_move = navigator.position().side_to_move();
    UciEngine::parse_info("info depth 22 seldepth 54 score cp 35 pv e7e5 g1f3 b8c6 c2c4", analysis.info);
    analysis.san_pv = UciEngine::pv_to_san(navigator.position(), analysis.info.pv);
    EngineDisplay engine;
    engine.update_snapshot(11, UCI_ANALYZING, "Stockfish", analysis, true);
    const int sizes[] = {8, 12, 24};
    const int widths[] = {900, 1300, 1500};
    const int height = 850;
    for (int s = 0; s < 3; ++s) for (int figurines = 0; figurines < 2; ++figurines) {
        const int width = widths[s];
        notation.set_text_sizes(sizes[s], sizes[s]);
        notation.set_figurines(figurines != 0);
        notation.set_board_share(4200);
        NotationLayout layout;
        layout.set_figurines(figurines != 0);
        layout.build(game, notation.content_width(width), notation.metrics());
        Pixmap surface = XCreatePixmap(display, RootWindow(display, screen), width, height,
                                       DefaultDepth(display, screen));
        engine.end_preview();
        notation.draw(surface, width, height, game, layout, navigator.current(), 0, engine);
        const SidebarGeometry sidebar = notation.sidebar_geometry(width);
        const SidebarBoxes boxes = notation.boxes(width, height, game);
        const std::vector<NotationSelectionRect> selection = layout.selection_rects(pgn_selection);
        check(!selection.empty(), "PGN origin has a visible selection rectangle");
        if (selection.empty()) { XFreePixmap(display, surface); continue; }
        const NotationSelectionRect& rect = selection[0];
        const int pgn_x = sidebar.content_left + rect.x, pgn_y = boxes.notation_top + rect.y;
        XImage* pgn_reference = XGetImage(display, surface, pgn_x, pgn_y, rect.width, rect.height,
                                         AllPlanes, ZPixmap);
        const int top = boxes.engine_top + 2 * notation.engine_metrics().line_height();
        const EnginePvLayout pv = layout_engine_pv(analysis, false, sidebar.content_width,
                                                  notation.engine_metrics(), figurines != 0);
        check(pv.moves.size() == 4 && notation.engine_pv_snapshot() &&
              notation.engine_pv_hit_test(sidebar.content_left, top - 1) == -1,
              "actual Xft engine row exposes only visible move hitboxes");
        for (std::size_t i = 0; i < pv.moves.size(); ++i) {
            const NotationItem& item = pv.moves[i];
            const int hit = notation.engine_pv_hit_test(sidebar.content_left + item.x + item.width / 2,
                                                       top + item.height / 2);
            check(hit == static_cast<int>(i), "actual engine click regions follow font size and figurines");
        }
        if (pv.moves.size() > 2 && notation.engine_pv_snapshot()) {
            check(engine.select_preview(*notation.engine_pv_snapshot(), 2, navigator.position(), navigator.current(), 11),
                  "painted snapshot selects the corresponding underlying UCI move");
            LastMoveHighlight last;
            last.valid = true; last.from = engine.preview_move()->from; last.to = engine.preview_move()->to;
            for (int flipped = 0; flipped < 2; ++flipped) {
                board.draw(surface, width, height, *engine.preview_position(), flipped != 0, last,
                           notation.sidebar_preferred_width(width));
                notation.draw(surface, width, height, game, layout, navigator.current(), 0, engine);
                XImage* pixels = XGetImage(display, surface, 0, 0, width, height, AllPlanes, ZPixmap);
                const UiPalette palette;
                if (pixels) {
                    const NotationItem& selected = pv.moves[2];
                    const unsigned long background = XGetPixel(pixels, sidebar.content_left + selected.x + selected.ink_left,
                                                               top + 2);
                    // Pixmap readbacks may have zero XImage masks; use their rendering visual.
                    const Visual* visual = DefaultVisual(display, screen);
                    check(channel_from_pixel(background, visual->red_mask) == palette.selected_move.red &&
                          channel_from_pixel(background, visual->green_mask) == palette.selected_move.green &&
                          channel_from_pixel(background, visual->blue_mask) == palette.selected_move.blue,
                          "clicked analysis move uses precisely the existing PGN selection background");
                    const unsigned long other = XGetPixel(pixels, sidebar.content_left + pv.moves[0].x, top + 2);
                    check(other != background, "only the clicked PV move is highlighted");
                    check(matches_image_region(pixels, pgn_x, pgn_y, pgn_reference),
                          "PGN origin highlight stays pixel-identical alongside the analysis highlight");
                    check(XGetPixel(pixels, pgn_x, pgn_y + 2) == background,
                          "PGN and analysis selections use the same highlight background");
                    XDestroyImage(pixels);
                } else check(false, "read selected analysis row pixels");
                check(engine.preview_index() == 2 && navigator.current() == pgn_selection &&
                      notation.engine_pv_hit_test(sidebar.content_left + pv.moves[2].x + 1, top + 3) == 2,
                      "rotation and resizing preserve PGN selection, preview and click mapping");
                if (s == 1 && figurines && !flipped)
                    check(save_ppm((test_scratch_directory + "/analysis-preview.ppm").c_str(), display,
                                   DefaultVisual(display, screen), surface, width, height),
                          "capture selected analysis move with its preview board");
            }
            engine.end_preview();
            notation.draw(surface, width, height, game, layout, navigator.current(), 0, engine);
            XImage* restored = XGetImage(display, surface, 0, 0, width, height, AllPlanes, ZPixmap);
            check(matches_image_region(restored, pgn_x, pgn_y, pgn_reference),
                  "ending analysis preview preserves the complete PGN origin highlight");
            if (restored && pgn_reference) {
                check(XGetPixel(restored, sidebar.content_left + pv.moves[2].x + pv.moves[2].ink_left, top + 2) !=
                      XGetPixel(pgn_reference, 0, 2), "ending preview removes only the analysis highlight");
            }
            if (restored) XDestroyImage(restored);
        }
        if (pgn_reference) XDestroyImage(pgn_reference);
        engine.update_snapshot(11, UCI_ANALYZING, "Stockfish", analysis, false);
        notation.draw(surface, width, height, game, layout, navigator.current(), 0, engine);
        check(!notation.engine_pv_snapshot() && notation.engine_pv_hit_test(sidebar.content_left, top + 1) == -1,
              "disabling analysis removes cached click targets");
        engine.update_snapshot(11, UCI_ANALYZING, "Stockfish", analysis, true);
        XFreePixmap(display, surface);
    }
}

std::string last_preview_frame(const std::string& base)
{
    std::ifstream input((base + "/viewer.log").c_str());
    std::string line, last;
    while (std::getline(input, line))
        if (line.find("diagnostic: present #") != std::string::npos) last = line;
    return last;
}

bool wait_preview_frame(const std::string& base, const std::string& marker,
                        const std::string& previous = "")
{
    for (int i = 0; i < 250; ++i) {
        const std::string frame = last_preview_frame(base);
        if (frame != previous && frame.find(marker) != std::string::npos) return true;
        usleep(20000);
    }
    return false;
}

int preview_search_count(const std::string& base)
{
    std::ifstream commands((base + "/commands").c_str());
    std::string command;
    int count = 0;
    while (std::getline(commands, command)) if (command == "go infinite") ++count;
    return count;
}

void send_preview_event(Display* display, Window window, bool click, int x, int y,
                        KeySym key = 0, unsigned int state = 0)
{
    XEvent event;
    std::memset(&event, 0, sizeof(event));
    if (click) {
        event.xbutton.type = ButtonPress; event.xbutton.display = display;
        event.xbutton.window = window; event.xbutton.root = DefaultRootWindow(display);
        event.xbutton.button = Button1; event.xbutton.same_screen = True;
        event.xbutton.x = x; event.xbutton.y = y;
    } else {
        event.xkey.type = KeyPress; event.xkey.display = display;
        event.xkey.window = window; event.xkey.root = DefaultRootWindow(display);
        event.xkey.keycode = XKeysymToKeycode(display, key);
        event.xkey.state = state; event.xkey.same_screen = True;
    }
    XSendEvent(display, window, False, click ? ButtonPressMask : KeyPressMask, &event);
    XFlush(display);
}

bool window_title_matches(Display* display, Window window,
                          const std::string& expected, bool modern)
{
    XTextProperty property = {0, 0, 0, 0};
    const Atom name = modern ? XInternAtom(display, "_NET_WM_NAME", False) : XA_WM_NAME;
    if (!XGetTextProperty(display, window, &property, name) || !property.value)
        return false;
    bool matches = false;
    if (modern) {
        matches = property.encoding == XInternAtom(display, "UTF8_STRING", False) &&
                  property.format == 8 &&
                  std::string(reinterpret_cast<char*>(property.value), property.nitems) == expected;
    } else {
        char** values = 0;
        int count = 0;
        const int conversion = expected.empty() ? Success :
            Xutf8TextPropertyToTextList(display, &property, &values, &count);
        matches = (property.encoding == XA_STRING ||
                   property.encoding == XInternAtom(display, "COMPOUND_TEXT", False) ||
                   property.encoding == XInternAtom(display, "UTF8_STRING", False)) &&
                  property.format == 8 &&
                  (expected.empty() ? property.nitems == 0 :
                   conversion == Success &&
                   count == 1 && values && values[0] == expected);
        if (!matches) {
            char* encoding = XGetAtomName(display, property.encoding);
            std::cerr << "WM_NAME expected='" << expected << "' type="
                      << (encoding ? encoding : "unknown") << " format=" << property.format
                      << " bytes=" << property.nitems << " conversion=" << conversion
                      << " count=" << count << " decoded='"
                      << (count && values ? values[0] : "") << "'\n";
            if (encoding) XFree(encoding);
        }
        if (values) XFreeStringList(values);
    }
    XFree(property.value);
    return matches;
}

void check_window_role(Display* display, Window window, const char* expected)
{
    Atom type = None;
    int format = 0;
    unsigned long length = 0, remaining = 0;
    unsigned char* value = 0;
    const int status = XGetWindowProperty(display, window,
        XInternAtom(display, "WM_WINDOW_ROLE", False), 0, 8192, False,
        AnyPropertyType, &type, &format, &length, &remaining, &value);
    check(status == Success && (expected ?
        type == XA_STRING && format == 8 && remaining == 0 && value &&
        std::string(reinterpret_cast<char*>(value), length) == expected : type == None),
        "WM_WINDOW_ROLE is absent by default or precisely the requested STRING/8 value");
    if (value) XFree(value);
}

void check_window_title(Display* display, Window window, const std::string& expected,
                        const char* role)
{
    for (int i = 0; i < 100 && !window_title_matches(display, window, expected, true); ++i)
        usleep(10000);
    check(window_title_matches(display, window, expected, true),
          "_NET_WM_NAME is UTF8_STRING and contains precisely the expected title");
    check(window_title_matches(display, window, expected, false),
          "WM_NAME preserves the exact title with the declared text encoding");
    check_window_role(display, window, role);
}

void test_window_title(Display* display)
{
    const char* titles[] = {0, 0, "Carlsen - Anand 2013",
                           "Schaakpartij \342\200\223 Z\303\274rich", "",
                           "Schaakpartij \342\200\223 Z\303\274rich"};
    for (std::size_t mode = 0; mode < sizeof(titles) / sizeof(titles[0]); ++mode) {
        const char* role = mode == 0 ? 0 : (mode == 1 ? "analysis" : "Study window");
        const std::string base = test_paths::make_directory("title", test_scratch_directory);
        if (base.empty()) { check(false, "create isolated window-title fixture"); return; }
        const std::string first = base + "/first.pgn", second = base + "/second.pgn";
        { std::ofstream out(first.c_str());
          out << "[Event \"One\"]\n\n1. e4 e5 *\n\n[Event \"Two\"]\n\n1. d4 d5 *\n"; }
        { std::ofstream out(second.c_str()); out << "[Event \"Three\"]\n\n1. c4 e5 *\n"; }
        SessionState initial = default_session_state();
        initial.width = 800; initial.height = 600; initial.analysis_enabled = false;
        const std::string state_path = base + "/gambitree/state";
        check(save_session_state(state_path, initial), "seed isolated title preferences");
        Window window = 0;
        const pid_t viewer = start_session_viewer(display, base, 0, 0,
            mode ? first.c_str() : 0, &window, titles[mode], mode ? second.c_str() : 0,
            0, mode == 5 ? "C" : 0, 0, role);
        if (window) {
            check(wait_preview_frame(base, "indexing=no"), "title fixture finishes initial rendering");
            const std::string expected = titles[mode] ? titles[mode] :
                (mode ? "Gambitree - first.pgn" : "Gambitree");
            check_window_title(display, window, expected, role);
            if (mode) {
                const std::string previous = last_preview_frame(base);
                send_preview_event(display, window, false, 0, 0, XK_Page_Down);
                check(wait_preview_frame(base, "reason=keyboard", previous), "title test switches game");
                check_window_title(display, window, titles[mode] ? expected : "Gambitree - first.pgn - 2/2", role);
                const std::string game_frame = last_preview_frame(base);
                send_preview_event(display, window, false, 0, 0, XK_Page_Down, ControlMask);
                check(wait_preview_frame(base, "reason=keyboard", game_frame), "title test switches PGN file");
                check_window_title(display, window, titles[mode] ? expected : "Gambitree - second.pgn - 1/1", role);
            }
            if (titles[mode]) {
                const KeySym keys[] = {XK_Right, XK_f, XK_b, XK_p};
                for (std::size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
                    const std::string previous = last_preview_frame(base);
                    send_preview_event(display, window, false, 0, 0, keys[i]);
                    check(wait_preview_frame(base, "reason=keyboard", previous), "title test processes UI activity");
                    check_window_title(display, window, expected, role);
                }
                const std::string previous = last_preview_frame(base);
                XResizeWindow(display, window, 920, 680); XFlush(display);
                check(wait_preview_frame(base, "reason=resize", previous), "title test processes resizing");
                check_window_title(display, window, expected, role);
                XClassHint identity = {0, 0};
                check(XGetClassHint(display, window, &identity) && identity.res_name && identity.res_class &&
                      std::string(identity.res_name) == "gambitree" && std::string(identity.res_class) == "Gambitree",
                      "a custom title leaves WM_CLASS unchanged");
                if (identity.res_name) XFree(identity.res_name);
                if (identity.res_class) XFree(identity.res_class);
            }
        }
        check(stop_session_viewer(viewer), "window-title viewer closes normally");
        std::ifstream saved(state_path.c_str());
        const std::string contents((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
        check(contents.find("title") == std::string::npos && contents.find("role") == std::string::npos,
              "window title and role are never persisted in session preferences");
    }
}

void test_board_option_window(Display* display)
{
    const char* saved[] = {"wood", "wood", "classic", "classic"};
    const char* chosen[] = {"classic", 0, "wood-veneer", "wood"};
    const char* expected[] = {"classic", "wood-veneer", "classic", "wood"};
    const char* labels[] = {
        "--board classic overrides and saves the stored Wood preference",
        "without --board, B cycles from the saved board theme",
        "B cycles from the explicit --board wood-veneer theme",
        "--board wood is saved after idle without further X11 events"
    };
    for (std::size_t i = 0; i < sizeof(chosen) / sizeof(chosen[0]); ++i) {
        const std::string base = test_paths::make_directory("board-option", test_scratch_directory);
        if (base.empty()) { check(false, "create isolated board-option fixture"); return; }
        const std::string path = base + "/gambitree/state";
        SessionState initial = default_session_state(), loaded;
        initial.width = 800; initial.height = 600; initial.analysis_enabled = false;
        initial.board_theme_id = saved[i];
        check(save_session_state(path, initial), "seed isolated board preferences");
        Window window = 0;
        const pid_t viewer = start_session_viewer(display, base, 0, 0, 0,
                                                 &window, 0, 0, chosen[i]);
        if (window) {
            check(wait_preview_frame(base, "indexing=no"), "board-option viewer presents its initial board");
            if (i == 1 || i == 2) {
                const std::string previous = last_preview_frame(base);
                send_preview_event(display, window, false, 0, 0, XK_b);
                check(wait_preview_frame(base, "reason=keyboard", previous), "board option preserves the B shortcut");
            }
            if (i == 3) {
                bool saved_after_idle = false;
                for (int attempt = 0; attempt < 300 && !saved_after_idle; ++attempt) {
                    saved_after_idle = load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                                       loaded.board_theme_id == expected[i];
                    if (!saved_after_idle) usleep(20000);
                }
                check(saved_after_idle, labels[i]);
            }
        }
        check(stop_session_viewer(viewer), "board-option viewer closes normally");
        check(load_session_state(path, loaded) == SESSION_STATE_LOADED &&
              loaded.board_theme_id == expected[i], labels[i]);
    }
}

void test_geometry_window(Display* display)
{
    struct Case { const char* text; int width, height, x, y, gravity; };
    const Case cases[] = {
        {"1000x700+40+60", 1000, 700, 40, 60, NorthWestGravity},
        {"900x650", 900, 650, -1, -1, 0},
        {"+50+70", 800, 600, 50, 70, NorthWestGravity},
        {"800x600-0-0", 800, 600, -2, -2, SouthEastGravity},
        {"1x1", 480, 320, -1, -1, 0},
        {"bad", 800, 600, -1, -1, 0},
        {"", 800, 600, -1, -1, 0},
        {"999999999999x999999999999+999999+999999", 8192, 8192, 32767, 32767, NorthWestGravity},
        {"x500+25", 800, 500, 25, 80, NorthWestGravity}
    };
    const int screen_width = DisplayWidth(display, DefaultScreen(display));
    const int screen_height = DisplayHeight(display, DefaultScreen(display));
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const std::string base = test_paths::make_directory("geometry", test_scratch_directory);
        if (base.empty()) { check(false, "create isolated geometry fixture"); return; }
        const std::string path = base + "/gambitree/state";
        SessionState initial = default_session_state(), loaded = initial;
        initial.width = 800; initial.height = 600; initial.analysis_enabled = false;
        if (i == 8) {
            initial.has_position = true; initial.position_is_outer = true;
            initial.x = 20; initial.y = 80;
        }
        check(save_session_state(path, initial), "seed geometry session defaults");
        Window window = 0;
        const pid_t viewer = start_session_viewer(display, base, 0, 0, 0,
            &window, 0, 0, 0, 0, cases[i].text);
        int wanted_width = std::min(cases[i].width, screen_width);
        int wanted_height = std::min(cases[i].height, screen_height);
        if (window) {
            check(wait_preview_frame(base, "indexing=no"), "geometry viewer starts and renders");
            XWindowAttributes attributes;
            std::memset(&attributes, 0, sizeof(attributes));
            XGetWindowAttributes(display, window, &attributes);
            if (attributes.width != wanted_width || attributes.height != wanted_height)
                std::cerr << "Geometry '" << cases[i].text << "': requested="
                          << wanted_width << 'x' << wanted_height << " actual="
                          << attributes.width << 'x' << attributes.height
                          << " log=" << base << "/viewer.log\n";
            check(attributes.width == wanted_width && attributes.height == wanted_height,
                  "explicit, partial, adjusted and invalid geometries produce the expected client size");
            XSizeHints hints; long supplied = 0;
            std::memset(&hints, 0, sizeof(hints));
            check(XGetWMNormalHints(display, window, &hints, &supplied), "geometry publishes normal WM hints");
            if (cases[i].gravity) {
                const int max_x = std::max(0, screen_width - wanted_width - 2);
                const int max_y = std::max(0, screen_height - wanted_height - 2);
                const int x = cases[i].x == -2 ? max_x : std::min(cases[i].x, max_x);
                const int y = cases[i].y == -2 ? max_y : std::min(cases[i].y, max_y);
                check((hints.flags & (USPosition | PWinGravity)) == (USPosition | PWinGravity) &&
                      hints.win_gravity == cases[i].gravity && hints.x == x && hints.y == y,
                      "WM hints preserve requested offsets, missing axes and negative-zero gravity");
            }
            if (i == 1) {
                bool saved_after_idle = false;
                for (int attempt = 0; attempt < 300 && !saved_after_idle; ++attempt) {
                    saved_after_idle = load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                        loaded.width == wanted_width && loaded.height == wanted_height;
                    if (!saved_after_idle) usleep(20000);
                }
                check(saved_after_idle, "explicit geometry is saved after idle without a resize event");
                if (!saved_after_idle)
                    std::cerr << "Geometry '" << cases[i].text << "': idle save expected="
                              << wanted_width << 'x' << wanted_height << " stored="
                              << loaded.width << 'x' << loaded.height
                              << " log=" << base << "/viewer.log\n";
            }
            if (i == 0) {
                wanted_width = std::min(640, screen_width);
                wanted_height = std::min(480, screen_height);
                const std::string previous = last_preview_frame(base);
                XResizeWindow(display, window, wanted_width, wanted_height); XFlush(display);
                check(wait_preview_frame(base, "reason=resize", previous), "--geometry leaves normal resizing interactive");
            }
        }
        check(stop_session_viewer(viewer), "geometry viewer closes normally");
        const bool stored_size = load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                                 loaded.width == wanted_width && loaded.height == wanted_height;
        if (!stored_size)
            std::cerr << "Geometry '" << cases[i].text << "': shutdown expected="
                      << wanted_width << 'x' << wanted_height << " stored="
                      << loaded.width << 'x' << loaded.height << " log=" << base << "/viewer.log\n";
        check(stored_size,
              "normal shutdown remembers actual window dimensions after explicit geometry or resizing");
    }
}

void test_window_modes(Display* display)
{
    const Atom net_state = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom full_state = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    const Atom wm_state = XInternAtom(display, "WM_STATE", False);
    for (int mode = 0; mode < 4; ++mode) {
        const bool fullscreen = (mode & 1) != 0, iconic = (mode & 2) != 0;
        const std::string base = test_paths::make_directory("window-modes", test_scratch_directory);
        if (base.empty()) { check(false, "create isolated window-mode fixture"); return; }
        const std::string path = base + "/gambitree/state";
        SessionState initial = default_session_state(), loaded = initial;
        initial.width = 800; initial.height = 600; initial.analysis_enabled = false;
        check(save_session_state(path, initial), "seed normal geometry before a mode request");
        Window window = 0;
        const pid_t viewer = start_session_viewer(display, base, 0, 0, 0,
            &window, "Window modes", 0, 0, 0, 0, 0, fullscreen, iconic);
        if (window) {
            check_window_title(display, window, "Window modes", 0);
            XWindowAttributes attributes;
            if (XGetWindowAttributes(display, window, &attributes) && attributes.map_state == IsViewable)
                check(wait_preview_frame(base, "indexing=no"), "mode fixture presents its initial window");
            Atom type = None; int format = 0;
            unsigned long count = 0, remaining = 0; unsigned char* bytes = 0;
            const int status = XGetWindowProperty(display, window, net_state, 0, 64, False,
                XA_ATOM, &type, &format, &count, &remaining, &bytes);
            bool requested_fullscreen = false;
            if (type == XA_ATOM && format == 32 && bytes)
                for (unsigned long i = 0; i < count; ++i)
                    if (reinterpret_cast<unsigned long*>(bytes)[i] == full_state) requested_fullscreen = true;
            if (bytes) XFree(bytes);
            check(status == Success && requested_fullscreen == fullscreen,
                  "--fullscreen publishes the EWMH startup state only when requested");
            XWMHints* hints = XGetWMHints(display, window);
            const bool requested_iconic = hints && (hints->flags & StateHint) && hints->initial_state == IconicState;
            check(requested_iconic == iconic, "--iconic publishes the ICCCM IconicState startup hint");
            if (hints) XFree(hints);

            // Simulate WM-owned state on this test window; no WM is needed in Xvfb.
            if (iconic) {
                const unsigned long state[] = {IconicState, None};
                XChangeProperty(display, window, wm_state, wm_state, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(state), 2);
            }
            XResizeWindow(display, window, 1100, 750); XMoveWindow(display, window, 100, 100);
            const std::string previous = last_preview_frame(base);
            send_preview_event(display, window, false, 0, 0, XK_b);
            check(wait_preview_frame(base, "reason=keyboard", previous), "preferences remain interactive in startup modes");
            bool saved = false;
            for (int i = 0; i < 300 && !saved; ++i) {
                saved = load_session_state(path, loaded) == SESSION_STATE_LOADED && loaded.board_theme_id == "wood";
                if (!saved) usleep(20000);
            }
            check(saved, "appearance preferences still autosave during fullscreen or iconic state");
            check(loaded.width == (mode ? 800 : 1100) && loaded.height == (mode ? 600 : 750) &&
                  (!mode || loaded.has_position == initial.has_position),
                  "transient dimensions and positions are excluded while normal resizing is saved");
            if (mode == 3) {
                XDeleteProperty(display, window, net_state);
                const unsigned long normal[] = {NormalState, None};
                XChangeProperty(display, window, wm_state, wm_state, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(normal), 2);
                XResizeWindow(display, window, 640, 480); XMoveWindow(display, window, 50, 60);
                send_preview_event(display, window, false, 0, 0, XK_b);
                bool restored = false;
                for (int i = 0; i < 300 && !restored; ++i) {
                    restored = load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                        loaded.board_theme_id == "wood-veneer" && loaded.width == 640 && loaded.height == 480;
                    if (!restored) usleep(20000);
                }
                check(restored, "normal geometry saving resumes after leaving both transient states");
                if (!restored)
                    std::cerr << "Mode " << mode << ": restored size expected=640x480 stored="
                              << loaded.width << 'x' << loaded.height
                              << " log=" << base << "/viewer.log\n";
            }
        }
        check(stop_session_viewer(viewer), "mode fixture exits through normal shutdown");
        const int expected_width = mode == 0 ? 1100 : mode == 3 ? 640 : 800;
        const int expected_height = mode == 0 ? 750 : mode == 3 ? 480 : 600;
        const bool stored_size = load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                                 loaded.width == expected_width && loaded.height == expected_height;
        if (!stored_size)
            std::cerr << "Mode " << mode << ": shutdown expected="
                      << expected_width << 'x' << expected_height << " stored="
                      << loaded.width << 'x' << loaded.height << " log=" << base << "/viewer.log\n";
        check(stored_size,
              "shutdown does not persist fullscreen or iconic dimensions");
        std::ifstream input(path.c_str());
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        check(text.find("fullscreen") == std::string::npos && text.find("iconic") == std::string::npos,
              "window modes are never serialized as session preferences");
    }
}

void test_analysis_preview_window(Display* display, int screen)
{
    const int previous_failures = failures;
    const std::string directory = test_paths::make_directory("preview", test_scratch_directory);
    if (directory.empty()) { check(false, "create isolated live analysis fixture"); return; }
    const std::string base(directory), script = base + "/engine", pgn = base + "/game.pgn";
    { std::ofstream output(pgn.c_str()); output << "1. d4 d5 *\n"; }
    {
        std::ofstream output(script.c_str());
        output << "#!/bin/sh\nwhile IFS= read -r command; do\n"
               << "printf '%s\\n' \"$command\" >> '" << base << "/commands'\n"
               << "case \"$command\" in\n"
               << "uci) echo 'id name Stockfish test'; echo uciok;;\n"
               << "isready) echo readyok;;\n"
               << "go*) echo 'info depth 12 score cp 25 pv e2e4 e7e5 g1f3 b8c6';\n"
               << "if [ ! -e '" << base << "/update' ]; then\n"
               << "(while [ ! -e '" << base << "/update' ]; do sleep 0.02; done;\n"
               << "echo 'info depth 30 seldepth 54 score cp 81 pv d2d4 d7d5') &\nfi;;\n"
               << "stop) echo 'bestmove e2e4';;\nquit) exit 0;;\nesac\ndone\n";
    }
    chmod(script.c_str(), 0700);
    SessionState initial = default_session_state();
    initial.width = 1300; initial.height = 850; initial.board_share = 4200;
    initial.notation_points = 12; initial.engine_points = 12; initial.figurines = false;
    initial.analysis_enabled = true;
    check(save_session_state(base + "/gambitree/state", initial), "seed isolated analysis UI preferences");
    Window window = 0;
    const pid_t viewer = start_session_viewer(display, base, 0, script.c_str(), pgn.c_str(), &window);
    check(window && wait_preview_frame(base, "pending=no"), "live viewer presents a current engine PV");
    if (window) {
        XWindowAttributes attributes;
        XGetWindowAttributes(display, window, &attributes);
        X11NotationRenderer notation(display, screen);
        notation.set_text_sizes(12, 12); notation.set_board_share(4200); notation.set_figurines(false);
        PgnDocument document;
        PgnParser::parse("1. d4 d5 *", document);
        const SidebarGeometry sidebar = notation.sidebar_geometry(attributes.width);
        const SidebarBoxes boxes = notation.boxes(attributes.width, attributes.height, *document.games[0]);
        UciAnalysis analysis; analysis.san_pv = "e4 e5 Nf3 Nc6";
        const EnginePvLayout row = layout_engine_pv(analysis, false, sidebar.content_width, notation.engine_metrics(), false);
        const int y = boxes.engine_top + 2 * notation.engine_metrics().line_height() + 4;
        if (row.moves.size() > 2) {
            const int original_searches = preview_search_count(base);
            const int x = sidebar.content_left + row.moves[2].x + 2;
            const std::string before = last_preview_frame(base);
            send_preview_event(display, window, true, x, y);
            check(wait_preview_frame(base, "preview=2", before) && last_preview_frame(base).find("selected=root") != std::string::npos &&
                  last_preview_frame(base).find("board_fen='rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2'") != std::string::npos,
                  "actual mouse event previews Nf3 without moving the PGN cursor");
            { std::ofstream update((base + "/update").c_str()); update << "update\n"; }
            // The next user redraw must still show the frozen snapshot after new engine info arrives.
            bool received = false;
            for (int i = 0; i < 250 && !received; ++i) {
                std::ifstream input((base + "/viewer.log").c_str());
                std::string line;
                while (std::getline(input, line))
                    if (line.find("raw='info depth 30 seldepth 54") != std::string::npos) received = true;
                if (!received) usleep(20000);
            }
            check(received, "engine continues producing updates during preview");
            const std::string frozen = last_preview_frame(base);
            send_preview_event(display, window, false, 0, 0, XK_f);
            check(wait_preview_frame(base, "preview=2", frozen), "rotation keeps the live preview frozen");
            send_preview_event(display, window, true, sidebar.content_left + row.moves[1].x + 2, y);
            check(wait_preview_frame(base, "preview=1"), "second click uses frozen e5 rather than newly reported d5");
            check(original_searches > 0 && preview_search_count(base) == original_searches,
                  "analysis clicks and rotation never start a new engine search");
            const std::string clicked = last_preview_frame(base);
            send_preview_event(display, window, false, 0, 0, XK_Left);
            check(wait_preview_frame(base, "preview=-1", clicked) && last_preview_frame(base).find("selected=root") != std::string::npos,
                  "unsuccessful real PGN back navigation dismisses preview at the root");
            const std::string ended = last_preview_frame(base);
            send_preview_event(display, window, true, sidebar.content_left + row.moves[1].x + 2, y);
            check(wait_preview_frame(base, "preview=1", ended) && last_preview_frame(base).find("board_fen='rnbqkbnr/ppp1pppp/8/3p4/3P4/8/PPP1PPPP/RNBQKBNR w KQkq d6 0 2'") != std::string::npos,
                  "ending preview exposes the latest PV and subsequent clicks reconstruct that line");
            NotationLayout pgn_layout;
            pgn_layout.build(*document.games[0], sidebar.content_width, notation.metrics());
            for (std::size_t i = 0; i < pgn_layout.items().size(); ++i) {
                const NotationItem& item = pgn_layout.items()[i];
                if (item.kind != NOTATION_MOVE || !item.node) continue;
                const std::string previous = last_preview_frame(base);
                send_preview_event(display, window, true, sidebar.content_left + item.x + item.width / 2,
                                   boxes.notation_top + item.y + item.height / 2);
                check(wait_preview_frame(base, "preview=-1", previous) &&
                      last_preview_frame(base).find("selected=d4 ") != std::string::npos,
                      "clicking a real PGN move dismisses preview and selects the original PGN node");
                break;
            }
            const KeySym keys[] = {XK_Left, XK_Right, XK_Up, XK_Down, XK_Home, XK_End,
                                   XK_Page_Up, XK_Page_Down, XK_Page_Up, XK_Page_Down};
            for (std::size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
                const std::string before_home = last_preview_frame(base);
                send_preview_event(display, window, false, 0, 0, XK_Home);
                check(wait_preview_frame(base, "selected=root", before_home), "reset live PGN cursor to its original root");
                check(wait_preview_frame(base, "pending=no"), "new root analysis arrives after real PGN navigation");
                const std::string before_click = last_preview_frame(base);
                send_preview_event(display, window, true, x, y);
                check(wait_preview_frame(base, "preview=2", before_click), "enter preview before each PGN navigation action");
                const std::string before_key = last_preview_frame(base);
                send_preview_event(display, window, false, 0, 0, keys[i], i >= 8 ? ControlMask : 0);
                check(wait_preview_frame(base, "preview=-1", before_key),
                      "every actual PGN navigation action dismisses preview, including boundary failures");
            }
            const std::string reset = last_preview_frame(base);
            send_preview_event(display, window, false, 0, 0, XK_Home);
            check(wait_preview_frame(base, "selected=root", reset) && wait_preview_frame(base, "pending=no"),
                  "root restored before analysis toggle test");
            const std::string ready = last_preview_frame(base);
            send_preview_event(display, window, true, x, y);
            check(wait_preview_frame(base, "preview=2", ready), "select preview before toggling analysis");
            const std::string selected = last_preview_frame(base);
            send_preview_event(display, window, false, 0, 0, XK_a);
            check(wait_preview_frame(base, "preview=-1", selected) && last_preview_frame(base).find("analysis=0") != std::string::npos,
                  "analysis toggle dismisses actual preview and restores the PGN board");
        } else check(false, "live test engine PV fits the panel");
    }
    { std::ofstream update((base + "/update").c_str()); update << "finish\n"; }
    check(stop_session_viewer(viewer), "live analysis test viewer exits normally");
    if (failures != previous_failures) std::cerr << "Preview X11 diagnostics retained in " << base << '\n';
    else {
        unlink((base + "/gambitree/state").c_str()); unlink((base + "/gambitree/state.lock").c_str());
        rmdir((base + "/gambitree").c_str());
        unlink(script.c_str()); unlink(pgn.c_str()); unlink((base + "/commands").c_str());
        unlink((base + "/update").c_str()); unlink((base + "/viewer.log").c_str()); rmdir(base.c_str());
    }
}

void test_veneer_board(Display* display, int screen)
{
    const std::string output_storage = test_scratch_directory + "/wood-veneer";
    const char* output = output_storage.c_str();
    mkdir(output, 0700);
    const char* diagnostics = std::getenv("GAMBITREE_DIAGNOSTICS");
    const bool had_diagnostics = diagnostics != 0;
    const std::string saved_diagnostics = diagnostics ? diagnostics : "";
    setenv("GAMBITREE_DIAGNOSTICS", "1", 1);
    BoardRenderer renderer(display, screen);
    if (had_diagnostics) setenv("GAMBITREE_DIAGNOSTICS", saved_diagnostics.c_str(), 1);
    else unsetenv("GAMBITREE_DIAGNOSTICS");
    BoardVeneer reference;
    std::string error;
    check(reference.load(std::vector<std::string>(1, "assets/boards/wood-veneer-light.png"),
                         std::vector<std::string>(1, "assets/boards/wood-veneer-dark.png"), error),
          "load reference veneer sheets for X11 pixel checks");
    check(renderer.set_board_theme("wood-veneer") &&
          renderer.load_piece_set("cburnett", error),
          "select veneer independently of Cburnett pieces");
    Position empty, positions[3];
    positions[0] = Position::standard();
    check(Position::from_fen(
        "r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P4/2PBPN2/PP1N1PPP/R1BQ1RK1 w - - 0 9", positions[1], &error) &&
          Position::from_fen("8/5pk1/6p1/3K4/3P4/8/8/8 w - - 0 40", positions[2], &error),
          "prepare middle and sparse positions without changing chess logic");
    const int sizes[] = {320, 600, 1000};
    for (int i = 0; i < 3; ++i) {
        const int width = sizes[i] + 400, height = sizes[i] + 60;
        const BoardGeometry geometry = BoardLayout::calculate(width, height, 300);
        Pixmap surface = XCreatePixmap(display, RootWindow(display, screen), width, height,
                                       DefaultDepth(display, screen));
        unsigned long builds = renderer.board_background_rebuild_count();
        for (int flip = 0; flip < 2; ++flip) {
            renderer.draw(surface, width, height, empty, flip != 0, LastMoveHighlight(), 300);
            check(renderer.board_background_rebuild_count() == ++builds &&
                  renderer.board_background_square_size() == geometry.square_size,
                  "size/orientation changes rebuild the shared veneer Pixmap once");
            PngImage expected;
            check(reference.render(geometry.square_size, flip != 0, expected),
                  "prepare matching CPU reference for real X11 upload");
            XImage* pixels = XGetImage(display, surface, geometry.x, geometry.y,
                geometry.board_size, geometry.board_size, AllPlanes, ZPixmap);
            bool matches = pixels && expected.valid();
            for (int square = 0; matches && square < 64; ++square) {
                int x, y;
                BoardLayout::square_rect(geometry, square, flip != 0, x, y);
                x += geometry.square_size / 3;
                y += geometry.square_size / 4;
                check(BoardLayout::square_at(geometry, x, y, flip != 0) == square,
                      "veneer preserves every mouse hitbox under both orientations");
                const int px = x - geometry.x, py = y - geometry.y;
                const unsigned long value = XGetPixel(pixels, px, py);
                const unsigned char* rgb = &expected.rgba[py * expected.stride + px * 4];
                Visual* visual = DefaultVisual(display, screen);
                matches = std::abs(static_cast<int>(channel_from_pixel(value, visual->red_mask)) - rgb[0]) <= 4 &&
                          std::abs(static_cast<int>(channel_from_pixel(value, visual->green_mask)) - rgb[1]) <= 4 &&
                          std::abs(static_cast<int>(channel_from_pixel(value, visual->blue_mask)) - rgb[2]) <= 4;
            }
            check(matches, "X11 uses each logical square's correct filtered crop after rotation");
            if (pixels) XDestroyImage(pixels);
            renderer.draw(surface, width, height, empty, flip != 0, LastMoveHighlight(), 300);
            check(renderer.board_background_rebuild_count() == builds,
                  "unchanged redraw reuses the veneer background Pixmap");
        }
        renderer.draw(surface, width, height, empty, false, LastMoveHighlight(), 300);
        builds = renderer.board_background_rebuild_count();
        LastMoveHighlight mark;
        mark.from = 12; mark.to = 28; mark.valid = true;
        renderer.draw(surface, width, height, empty, false, mark, 300);
        int x, y;
        BoardLayout::square_rect(geometry, mark.to, false, x, y);
        XImage* overlay = XGetImage(display, surface, x + 3, y + 3, 1, 1, AllPlanes, ZPixmap);
        check(overlay && channel_from_pixel(XGetPixel(overlay, 0, 0),
                  DefaultVisual(display, screen)->red_mask) > 220 &&
              renderer.board_background_rebuild_count() == builds,
              "last-move overlay is visible above veneer without rebuilding the background");
        if (overlay) XDestroyImage(overlay);
        // Old Wood captures already exist in the Wood/Cburnett tests. Exercise
        // the transition once per size, then keep only the new-theme captures.
        renderer.set_board_theme("wood");
        renderer.draw(surface, width, height, positions[0], false, LastMoveHighlight(), 300);
        const unsigned long piece_builds = renderer.scaled_piece_build_count();
        check(renderer.set_board_theme("wood-veneer"), "switch Wood to Wood Veneer");
        const unsigned long background = renderer.board_background_rebuild_count() + 1;
        for (int position = 0; position < 3; ++position) {
            const std::string fen = positions[position].to_fen();
            renderer.draw(surface, width, height, positions[position], false,
                          position == 1 ? mark : LastMoveHighlight(), 300);
            check(renderer.scaled_piece_build_count() == piece_builds &&
                  renderer.board_background_rebuild_count() == background &&
                  positions[position].to_fen() == fen,
                  "theme/position changes reuse pieces and rebuild the veneer background only once");
            std::ostringstream filename;
            filename << output << "/wood-veneer-" << sizes[i] << '-' << position << ".ppm";
            check(save_ppm(filename.str().c_str(), display, DefaultVisual(display, screen),
                           surface, width, height), "save real X11 veneer capture");
        }
        check(renderer.set_board_theme("classic") && renderer.board_background_square_size() == 0,
              "returning to Classic invalidates the shared texture cache");
        builds = renderer.board_background_rebuild_count();
        renderer.draw(surface, width, height, empty, false, LastMoveHighlight(), 300);
        check(renderer.board_background_rebuild_count() == builds,
              "Classic remains the existing uncached flat rendering path");
        renderer.set_board_theme("wood-veneer");
        XFreePixmap(display, surface);
    }
}

void test_wood_board(Display* display, int screen)
{
    const int targets[] = {320, 600, 1000};
    const std::string output_storage = test_scratch_directory + "/wood-board";
    const char* output_dir = output_storage.c_str();
    mkdir(output_dir, 0700);
    BoardRenderer renderer(display, screen);
    Position empty;
    Position initial = Position::standard();
    const std::string initial_fen = initial.to_fen();
    std::string error;
    check(renderer.board_theme_id() == "classic" &&
          !renderer.set_board_theme("unknown") &&
          renderer.board_theme_id() == "classic",
          "board theme starts classic and rejects unknown names safely");
    check(renderer.set_board_theme("wood") &&
          renderer.board_theme_id() == "wood",
          "wood board theme can be selected independently of piece design");

    for (std::size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        const int width = targets[i] + 400;
        const int height = targets[i] + 60;
        const BoardGeometry geometry = BoardLayout::calculate(width, height, 300);
        check(geometry.board_size == targets[i],
              "wood background is exercised at 320, 600 and 1000 pixel board sizes");
        Pixmap surface = XCreatePixmap(display, RootWindow(display, screen),
            static_cast<unsigned int>(width), static_cast<unsigned int>(height),
            static_cast<unsigned int>(DefaultDepth(display, screen)));
        renderer.draw(surface, width, height, empty, false,
                      LastMoveHighlight(), 300);
        if (i == 0)
            check(renderer.wood_texture_assets_loaded(),
                  "wood board loads its bundled generated light and dark texture atlases");
        const unsigned long first_build =
            renderer.board_background_rebuild_count();
        check(first_build > 0 &&
              renderer.board_background_square_size() == geometry.square_size,
              "wood background cache records the current square size");
        if (i == 0) {
            int a1x, a1y, c1x, c1y;
            BoardLayout::square_rect(geometry, 0, false, a1x, a1y);
            BoardLayout::square_rect(geometry, 2, false, c1x, c1y);
            XImage* pixels = XGetImage(display, surface, 0, 0,
                static_cast<unsigned int>(width),
                static_cast<unsigned int>(height), AllPlanes, ZPixmap);
            const unsigned long a1 = XGetPixel(pixels,
                a1x + geometry.square_size / 2,
                a1y + geometry.square_size / 2);
            const unsigned long c1 = XGetPixel(pixels,
                c1x + geometry.square_size / 2,
                c1y + geometry.square_size / 2);
            check(a1 != c1,
                  "same-colour wood squares have different stable tints");
            {
                bool has_internal_grain = false;
                int sample_x;
                int sample_y;
                for (sample_y = 1; sample_y < 8; ++sample_y) {
                    for (sample_x = 1; sample_x < 8; ++sample_x) {
                        if (XGetPixel(pixels,
                                a1x + geometry.square_size * sample_x / 8,
                                a1y + geometry.square_size * sample_y / 8) != a1) {
                            has_internal_grain = true;
                            break;
                        }
                    }
                    if (has_internal_grain) break;
                }
                check(has_internal_grain,
                      "wood square has measurable stable internal grain");
            }
            XDestroyImage(pixels);
            renderer.draw(surface, width, height, empty, false,
                          LastMoveHighlight(), 300);
            check(renderer.board_background_rebuild_count() == first_build,
                  "unchanged redraw reuses the cached wood background");
            check(save_ppm((std::string(output_dir) + "/wood-320-empty.ppm").c_str(),
                           display, DefaultVisual(display, screen), surface,
                           width, height),
                  "save 320 pixel wood board screenshot without pieces");
            renderer.draw(surface, width, height, empty, true,
                          LastMoveHighlight(), 300);
            BoardGeometry flipped_geometry = geometry;
            BoardLayout::square_rect(flipped_geometry, 0, true, a1x, a1y);
            pixels = XGetImage(display, surface, a1x + geometry.square_size / 2,
                a1y + geometry.square_size / 2, 1, 1, AllPlanes, ZPixmap);
            const unsigned long flipped_a1 = XGetPixel(pixels, 0, 0);
            XDestroyImage(pixels);
            check(flipped_a1 == a1 &&
                  renderer.board_background_rebuild_count() == first_build + 1,
                  "rotation rebuilds cache but preserves each logical square texture");
            renderer.draw(surface, width, height, empty, false,
                          LastMoveHighlight(), 300);
        }
        const unsigned long before_resize = renderer.board_background_rebuild_count();
        if (i > 0)
            check(before_resize > (i == 1 ? 3UL : 4UL),
                  "resizing to another board size invalidates the background cache");
        if (i == 1)
            check(save_ppm((std::string(output_dir) + "/wood-600-empty.ppm").c_str(),
                           display, DefaultVisual(display, screen), surface,
                           width, height),
                  "save 600 pixel wood board screenshot without pieces");
        if (i == 2)
            check(save_ppm((std::string(output_dir) + "/wood-1000-empty.ppm").c_str(),
                           display, DefaultVisual(display, screen), surface,
                           width, height),
                  "save 1000 pixel wood board screenshot without pieces");

        const unsigned long before_pieces = renderer.board_background_rebuild_count();
        if (i == 0) {
            check(renderer.load_piece_set(default_piece_set().id, error),
                  "load the existing piece set independently over the wood board");
        }
        renderer.draw(surface, width, height, initial, false,
                      LastMoveHighlight(), 300);
        check(renderer.last_piece_instances_drawn() == 32,
              "standard position draws all 32 pieces over the wood board");
        if (i == 0) {
            const BoardGeometry board = BoardLayout::calculate(
                width, height, 300);
            int pawn_x, pawn_y;
            BoardLayout::square_rect(board, 8, false, pawn_x, pawn_y);
            XImage* piece_image = XGetImage(display, surface,
                pawn_x, pawn_y,
                static_cast<unsigned int>(board.square_size),
                static_cast<unsigned int>(board.square_size),
                AllPlanes, ZPixmap);
            if (piece_image) {
                const unsigned long background = XGetPixel(piece_image, 1, 1);
                unsigned long brightest = 0;
                int px, py;
                for (py = 0; py < board.square_size; ++py) {
                    for (px = 0; px < board.square_size; ++px) {
                        const unsigned long value = XGetPixel(
                            piece_image, px, py);
                        const unsigned long rgb_sum =
                            channel_from_pixel(value,
                                DefaultVisual(display, screen)->red_mask) +
                            channel_from_pixel(value,
                                DefaultVisual(display, screen)->green_mask) +
                            channel_from_pixel(value,
                                DefaultVisual(display, screen)->blue_mask);
                        if (rgb_sum > brightest) brightest = rgb_sum;
                    }
                }
                const unsigned long background_sum =
                    channel_from_pixel(background,
                        DefaultVisual(display, screen)->red_mask) +
                    channel_from_pixel(background,
                        DefaultVisual(display, screen)->green_mask) +
                    channel_from_pixel(background,
                        DefaultVisual(display, screen)->blue_mask);
                check(brightest > background_sum + 30,
                      "classic white pawn remains distinguishable from the light wood square");
                XDestroyImage(piece_image);
            } else {
                check(false, "read classic white pawn contrast sample");
            }
        }
        check(renderer.board_background_rebuild_count() == before_pieces,
              "adding chess pieces does not invalidate the wood texture cache");
        if (i == 0)
            check(save_ppm((std::string(output_dir) + "/wood-320-pieces.ppm").c_str(),
                           display, DefaultVisual(display, screen), surface,
                           width, height),
                  "save 320 pixel wood board screenshot with pieces");
        if (i == 1)
            check(save_ppm((std::string(output_dir) + "/wood-600-pieces.ppm").c_str(),
                           display, DefaultVisual(display, screen), surface,
                           width, height),
                  "save 600 pixel wood board screenshot with pieces");
        if (i == 2)
            check(save_ppm((std::string(output_dir) + "/wood-1000-pieces.ppm").c_str(),
                           display, DefaultVisual(display, screen), surface,
                           width, height),
                  "save 1000 pixel wood board screenshot with pieces");
        XFreePixmap(display, surface);
    }
    {
        const BoardGeometry geometry = BoardLayout::calculate(720, 380, 300);
        check(initial.to_fen() == initial_fen &&
              BoardLayout::square_at(geometry, geometry.x + 2,
                                     geometry.y + 2, false) == 56,
              "board theming preserves chess position and logical square hitboxes");
    }
    check(renderer.set_board_theme("classic") &&
          renderer.board_background_square_size() == 0,
          "switching back to classic invalidates only the wood background cache");
    {
        const int width = targets[0] + 400, height = targets[0] + 60;
        Pixmap surface = XCreatePixmap(display, RootWindow(display, screen),
            static_cast<unsigned int>(width), static_cast<unsigned int>(height),
            static_cast<unsigned int>(DefaultDepth(display, screen)));
        renderer.draw(surface, width, height, initial, false,
                      LastMoveHighlight(), 300);
        check(renderer.board_background_rebuild_count() == 5,
              "classic board rendering does not allocate or rebuild a wooden cache");
        renderer.set_board_theme("wood");
        renderer.draw(surface, width, height, initial, false,
                      LastMoveHighlight(), 300);
        check(renderer.board_background_rebuild_count() == 6,
              "returning to wood recreates the invalidated texture cache once");
        XFreePixmap(display, surface);
    }
    {
        const int width = 1000, height = 660;
        Pixmap surface = XCreatePixmap(display, RootWindow(display, screen),
            static_cast<unsigned int>(width), static_cast<unsigned int>(height),
            static_cast<unsigned int>(DefaultDepth(display, screen)));
        check(renderer.load_piece_set("meridian-shaded", error),
              "load Meridian Shaded for a matched board-theme screenshot");
        renderer.draw(surface, width, height, initial, false,
                      LastMoveHighlight(), 300);
        check(renderer.last_piece_instances_drawn() == 32,
              "Meridian standard position draws all 32 pieces");
        check(save_ppm((std::string(output_dir) +
                        "/wood-meridian-600-pieces.ppm").c_str(),
                       display, DefaultVisual(display, screen), surface,
                       width, height),
              "save a 600 pixel wood board with Meridian Shaded pieces");
        XFreePixmap(display, surface);
    }
}

void test_cburnett_rendering(Display* display, int screen)
{
    const std::string output_storage = test_scratch_directory + "/cburnett/screenshots";
    const char* output = output_storage.c_str();
    mkdir((test_scratch_directory + "/cburnett").c_str(), 0700);
    mkdir(output, 0700);
    const char* old_diagnostics = std::getenv("GAMBITREE_DIAGNOSTICS");
    const bool had_diagnostics = old_diagnostics != 0;
    const std::string saved_diagnostics = old_diagnostics ? old_diagnostics : "";
    setenv("GAMBITREE_DIAGNOSTICS", "1", 1);
    BoardRenderer renderer(display, screen);
    if (had_diagnostics) setenv("GAMBITREE_DIAGNOSTICS", saved_diagnostics.c_str(), 1);
    else unsetenv("GAMBITREE_DIAGNOSTICS");
    Position initial = Position::standard();
    const std::string fen = initial.to_fen();
    std::string error;
    const int sizes[] = { 320, 600, 1000 };
    const char* themes[] = { "classic", "wood" };
    check(renderer.load_piece_set("gambitree", error), "load unchanged Classic for comparison");
    Pixmap baseline = XCreatePixmap(display, RootWindow(display, screen), 1000, 660,
                                   DefaultDepth(display, screen));
    renderer.draw(baseline, 1000, 660, initial, false, LastMoveHighlight(), 300);
    check(save_ppm((std::string(output) + "/gambitree-classic-600.ppm").c_str(),
                   display, DefaultVisual(display, screen), baseline, 1000, 660),
          "capture existing pieces on classic board");
    XFreePixmap(display, baseline);
    check(renderer.load_piece_set("cburnett", error) && renderer.piece_set_id() == "cburnett" &&
          renderer.high_quality_cache_square_size() == 0,
          "Cburnett selection invalidates only the old piece raster cache");
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const int width = sizes[i] + 400, height = sizes[i] + 60;
        const BoardGeometry geometry = BoardLayout::calculate(width, height, 300);
        Pixmap surface = XCreatePixmap(display, RootWindow(display, screen), width, height,
                                       DefaultDepth(display, screen));
        for (std::size_t t = 0; t < 2; ++t) {
            const unsigned long before_theme = renderer.scaled_piece_build_count();
            check(renderer.set_board_theme(themes[t]), "select Cburnett board background");
            renderer.draw(surface, width, height, initial, false, LastMoveHighlight(), 300);
            check(renderer.last_piece_instances_drawn() == 32 &&
                  renderer.high_quality_cache_square_size() == geometry.square_size,
                  "Cburnett draws every initial piece at each board size");
            if (t) check(renderer.scaled_piece_build_count() == before_theme,
                         "switching classic to wood reuses all Cburnett rasters");
            std::ostringstream name;
            name << output << "/cburnett-" << themes[t] << '-' << sizes[i] << ".ppm";
            check(save_ppm(name.str().c_str(), display, DefaultVisual(display, screen),
                           surface, width, height), "capture Cburnett board comparison");
            const unsigned long built = renderer.scaled_piece_build_count();
            const unsigned long background = renderer.board_background_rebuild_count();
            renderer.draw(surface, width, height, initial, false, LastMoveHighlight(), 300);
            check(renderer.scaled_piece_build_count() == built &&
                  renderer.board_background_rebuild_count() == background,
                  "ordinary Cburnett redraw rebuilds neither pieces nor background");
            renderer.draw(surface, width, height, initial, true, LastMoveHighlight(), 300);
            check(renderer.scaled_piece_build_count() == built && initial.to_fen() == fen,
                  "board rotation reuses Cburnett rasters and preserves chess position");
            for (int square = 0; square < 64; ++square) {
                int x, y;
                BoardLayout::square_rect(geometry, square, true, x, y);
                check(BoardLayout::square_at(geometry, x + geometry.square_size / 2,
                          y + geometry.square_size / 2, true) == square,
                      "Cburnett rotation preserves every mouse square hitbox");
            }
            if (i == 1 && t == 1)
                check(save_ppm((std::string(output) + "/cburnett-wood-flipped-600.ppm").c_str(),
                               display, DefaultVisual(display, screen), surface, width, height),
                      "capture rotated Cburnett on wood");
        }
        XFreePixmap(display, surface);
    }
    // Two positions put all twelve different pieces on both square colours.
    const char* galleries[] = {
        "8/kqrbnp2/8/8/8/8/KQRBNP2/8 w - - 0 1",
        "8/1kqrbnp1/8/8/8/8/1KQRBNP1/8 w - - 0 1"
    };
    Pixmap surface = XCreatePixmap(display, RootWindow(display, screen), 1000, 660,
                                   DefaultDepth(display, screen));
    for (std::size_t t = 0; t < 2; ++t) {
        renderer.set_board_theme(themes[t]);
        for (std::size_t g = 0; g < 2; ++g) {
            Position gallery;
            check(Position::from_fen(galleries[g], gallery, &error), "load twelve-piece comparison position");
            renderer.draw(surface, 1000, 660, gallery, false, LastMoveHighlight(), 300);
            check(renderer.last_piece_instances_drawn() == 12, "render every Cburnett type on each square colour");
            std::ostringstream name;
            name << output << "/cburnett-gallery-" << themes[t] << '-' << g << ".ppm";
            check(save_ppm(name.str().c_str(), display, DefaultVisual(display, screen),
                           surface, 1000, 660), "capture all twelve Cburnett pieces on both backgrounds");
        }
    }
    const int old_square = renderer.high_quality_cache_square_size();
    const BoardGeometry resized = BoardLayout::calculate(1120, 780, 300);
    XFreePixmap(display, surface);
    surface = XCreatePixmap(display, RootWindow(display, screen), 1120, 780,
                            DefaultDepth(display, screen));
    renderer.set_interactive_resize(true);
    renderer.draw(surface, 1120, 780, initial, false, LastMoveHighlight(), 300);
    check(renderer.temporary_piece_build_count() == 12 &&
          renderer.high_quality_cache_square_size() == old_square,
          "interactive Cburnett resize uses twelve temporary rasters and retains the HQ cache");
    unsigned long temporary = renderer.temporary_piece_build_count();
    renderer.draw(surface, 1120, 780, initial, false, LastMoveHighlight(), 300);
    check(renderer.temporary_piece_build_count() == temporary,
          "unchanged interactive redraw reuses Cburnett temporary rasters");
    check(renderer.begin_high_quality_rebuild(resized.square_size, 42), "start deferred Cburnett rebuild");
    HighQualityPieceWorkResult result = HQ_PIECE_WORK_MORE;
    int piece = EMPTY;
    double elapsed = 0;
    for (int steps = 0; !renderer.high_quality_rebuild_ready(42) && steps < 10000; ++steps) {
        result = renderer.build_next_high_quality_piece(42, piece, elapsed);
        if (result == HQ_PIECE_WORK_FAILED) break;
    }
    check(result != HQ_PIECE_WORK_FAILED && renderer.commit_high_quality_rebuild(42) &&
          renderer.high_quality_cache_square_size() == resized.square_size,
          "deferred resize commits a complete Cburnett raster cache");
    renderer.set_interactive_resize(false);
    renderer.draw(surface, 1120, 780, initial, false, LastMoveHighlight(), 300);
    const unsigned long background = renderer.board_background_rebuild_count();
    check(renderer.load_piece_set("meridian-shaded", error) &&
          renderer.board_theme_id() == "wood" && renderer.load_piece_set("cburnett", error),
          "switch between existing Meridian and Cburnett independently of wood");
    renderer.draw(surface, 1120, 780, initial, false, LastMoveHighlight(), 300);
    check(renderer.board_background_rebuild_count() == background &&
          renderer.last_piece_instances_drawn() == 32 && initial.to_fen() == fen,
          "piece-set changes preserve wood cache, positions and rendered occupancy");
    XFreePixmap(display, surface);
}

void test_wheel_event_batch(Display* display)
{
    XEvent down, down_release, up, up_release, key, other;
    std::memset(&down,0,sizeof(down));std::memset(&down_release,0,sizeof(down_release));
    std::memset(&up,0,sizeof(up));std::memset(&up_release,0,sizeof(up_release));
    std::memset(&key,0,sizeof(key));std::memset(&other,0,sizeof(other));
    down.type=ButtonPress;down.xbutton.window=0x101;down.xbutton.button=Button5;
    down_release.type=ButtonRelease;down_release.xbutton.window=0x101;down_release.xbutton.button=Button5;
    up.type=ButtonPress;up.xbutton.window=0x101;up.xbutton.button=Button4;
    up_release.type=ButtonRelease;up_release.xbutton.window=0x101;up_release.xbutton.button=Button4;
    key.type=KeyPress;key.xkey.window=0x101;
    other.type=ButtonPress;other.xbutton.window=0x202;other.xbutton.button=Button5;
    XPutBackEvent(display,&other);XPutBackEvent(display,&key);
    XPutBackEvent(display,&up_release);XPutBackEvent(display,&up);
    XPutBackEvent(display,&down_release);XPutBackEvent(display,&down);
    XEvent first;XNextEvent(display,&first);
    unsigned long count=0;
    int steps=collect_contiguous_wheel_events(display,0x101,first,count);
    check(steps==0 && count==2,
          "wheel press/release pairs coalesce without changing the net position");
    XEvent next;XNextEvent(display,&next);
    check(next.type==KeyPress && next.xkey.window==0x101,
          "wheel coalescing stops at and preserves the next keyboard event");
    XNextEvent(display,&next);
    check(next.type==ButtonPress && next.xbutton.window==0x202 &&
          collect_contiguous_wheel_events(display,0x101,next,count)==0 && count==0,
          "wheel events for another window are never merged into this scroll context");
}
int advance(Display* d, XftFont* f, const std::string& text) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(d, f, reinterpret_cast<const FcChar8*>(text.data()),
                      static_cast<int>(text.size()), &ext);
    return ext.xOff;
}
void test_keyboard_help_xft(Display* display, int screen)
{
    XKeyEvent event;
    std::memset(&event,0,sizeof(event));
    event.display=display;event.type=KeyPress;
    event.keycode=XKeysymToKeycode(display,XK_question);
    bool found=false;
    const unsigned int masks[]={0,ShiftMask,Mod5Mask,ShiftMask|Mod5Mask};
    for(std::size_t i=0;i<sizeof(masks)/sizeof(masks[0]);++i) {
        event.state=masks[i];char bytes[16];KeySym symbol=NoSymbol;
        int length=XLookupString(&event,bytes,sizeof(bytes),&symbol,0);
        if(keyboard_help_requested(symbol,bytes,length))found=true;
    }
    check(found,"the real X11 keyboard map translates question, including Shift, to a help request");
    X11NotationRenderer renderer(display,screen);
    {
        std::vector<std::string> rows;
        NavigationListState chooser;
        rows.push_back("1. White – Black · Event sample");
        rows.push_back("2. A very long player and event description that must remain clipped inside the chooser overlay");
        chooser.open(NAVIGATION_LIST_GAMES, rows, 0);
        chooser.set_query("a deliberately long filter query");
        const int sizes[][2] = {{320, 240}, {640, 480}, {1600, 900}};
        renderer.set_navigation_list(&chooser, "Choose a game", 0);
        for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            const int width = sizes[i][0], height = sizes[i][1];
            const NavigationListLayout geometry =
                renderer.navigation_list_layout(width, height, chooser);
            chooser.set_visible_rows(
                renderer.navigation_list_visible_rows(width, height));
            Pixmap surface = XCreatePixmap(
                display, RootWindow(display, screen), width, height,
                static_cast<unsigned int>(DefaultDepth(display, screen)));
            renderer.draw_navigation_list_overlay(surface, width, height);
            const int row_y = geometry.list_top + 1;
            check(renderer.navigation_list_visible_rows(width, height) > 0 &&
                  renderer.navigation_list_row_at(width, height, width / 2,
                                                   row_y) == 0,
                  "Xft chooser row hitbox maps its first rendered row at small and large sizes");
            check(renderer.navigation_list_row_at(width, height, 2, row_y) == -1,
                  "Xft chooser ignores clicks outside its row area");
            check(geometry.content_left == geometry.box_x +
                      std::min(24, std::max(8, geometry.box_width / 16)) &&
                  geometry.description_x + geometry.description_width ==
                      geometry.content_right,
                  "Xft chooser title, filter and rows share a clipped content margin");
            check(geometry.filter_query_x <= geometry.filter_query_x +
                      geometry.filter_query_width &&
                  geometry.filter_query_x + geometry.filter_query_width <=
                      geometry.filter_counter_x &&
                  geometry.filter_counter_x <= geometry.content_right,
                  "Xft chooser filter query and right-aligned count have disjoint measured areas");
            check(geometry.filter_rule_y >= geometry.filter_text_bottom + 5 &&
                  geometry.list_top - geometry.filter_rule_y >= 8 &&
                  geometry.filter_baseline > geometry.filter_top,
                  "Xft filter separator clears glyph descent and leaves space before the first row");
            check(geometry.marker_x < geometry.number_x &&
                  geometry.number_x + geometry.number_width <=
                      geometry.description_x && geometry.row_height > 0 &&
                  geometry.visible_rows > 0,
                  "Xft chooser marker, game number and description use stable columns and centered rows");
            XFreePixmap(display, surface);
        }
        std::vector<std::string> files;
        files.push_back("/a/long/path/to/a/chess/game/that/should/not/escape/the/overlay.pgn");
        files.push_back("/another/game.pgn");
        NavigationListState file_chooser;
        file_chooser.open(NAVIGATION_LIST_FILES, files, 0);
        for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            const NavigationListLayout geometry = renderer.navigation_list_layout(
                sizes[i][0], sizes[i][1], file_chooser);
            check(geometry.description_x + geometry.description_width ==
                      geometry.content_right && geometry.description_width >= 0,
                  "Xft file chooser paths end at the shared right clipping edge");
            const NavigationListLayout game_geometry =
                renderer.navigation_list_layout(sizes[i][0], sizes[i][1], chooser);
            check(geometry.filter_baseline == game_geometry.filter_baseline &&
                  geometry.filter_rule_y == game_geometry.filter_rule_y &&
                  geometry.list_top == game_geometry.list_top,
                  "file and game chooser keep filter text, separator and rows vertically aligned");
        }
        renderer.set_navigation_list(0, std::string(), 0);
    }
    const int sizes[][2]={{320,240},{640,480},{900,700},{1600,1000},{1920,1200}};
    for(std::size_t n=0;n<sizeof(sizes)/sizeof(sizes[0]);++n) {
        int width=sizes[n][0],height=sizes[n][1];
        KeyboardHelpLayout help=renderer.keyboard_help_layout(width,height);
        check(help.viewport_height()>=help.line_height && help.x+help.width<=width && help.y+help.height<=height,
              "actual Xft help fonts fit small and large clients");
        if(help.columns==2) {
            int left_description=-1,right_description=-1;
            for(std::size_t i=0;i<help.content.size();++i)
                if(help.content[i].kind==HELP_DESCRIPTION) {
                    int& aligned=help.content[i].x<help.x+help.width/2
                        ? left_description:right_description;
                    if(aligned<0)aligned=help.content[i].x;
                    check(aligned==help.content[i].x,
                          "real Xft help rows use a fixed description column in each half");
                }
        }
        for(std::size_t i=0;i<help.content.size();++i) {
            const KeyboardHelpText& line=help.content[i];
            bool reachable=false;
            for(int scroll=0;scroll<=help.max_scroll();scroll+=help.line_height)
                if(line.y>=scroll && line.y+line.height<=scroll+help.viewport_height())reachable=true;
            if(line.y>=help.max_scroll() && line.y+line.height<=
               help.max_scroll()+help.viewport_height())reachable=true;
            if(!reachable || line.x+line.width>help.x+help.width)
                std::cerr<<"help line out of range client="<<width<<"x"<<height
                         <<" kind="<<line.kind<<" x="<<line.x<<" width="<<line.width
                         <<" y="<<line.y<<" line-height="<<line.height
                         <<" body="<<help.body_top<<".."<<help.body_bottom
                         <<" content="<<help.content_height<<" scroll="<<help.max_scroll()
                         <<" text='"<<line.text<<"'\n";
            check(reachable && line.x+line.width<=help.x+help.width,
                  "actual measured help lines wrap and can all be reached by scrolling");
        }
        Pixmap pixmap=XCreatePixmap(display,RootWindow(display,screen),width+20,height+20,DefaultDepth(display,screen));
        GC gc=XCreateGC(display,pixmap,0,0);
        const unsigned long sentinel=WhitePixel(display,screen);
        XSetForeground(display,gc,sentinel);XFillRectangle(display,pixmap,gc,0,0,width+20,height+20);
        int scroll=0;renderer.draw_keyboard_help(pixmap,width,height,scroll);
        XImage* pixels=XGetImage(display,pixmap,0,0,width+20,height+20,AllPlanes,ZPixmap);
        bool contained=pixels!=0;
        for(int y=0;pixels && y<height+20;++y)for(int x=0;x<width+20;++x)
            if((x>=width || y>=height) && XGetPixel(pixels,x,y)!=sentinel)contained=false;
        check(contained,"real help rendering never paints outside the client rectangle");
        if(pixels)XDestroyImage(pixels);
        scroll=100000;renderer.draw_keyboard_help(pixmap,width,height,scroll);
        check(scroll==help.max_scroll(),"resized help scroll is clamped to its measured content");
        renderer.set_text_sizes(24,8);
        KeyboardHelpLayout changed=renderer.keyboard_help_layout(width,height);
        check(help.content_height==changed.content_height && help.line_height==changed.line_height &&
              help.columns==changed.columns && help.content.size()==changed.content.size(),
              "help readability is independent of notation and engine text preferences");
        renderer.set_text_sizes(12,12);
        XFreeGC(display,gc);XFreePixmap(display,pixmap);
    }
}

void test_search_ranges_xft(Display* display, int screen)
{
    const std::string source =
        "[Source \"ChessBase\"]\n[Mega2PGNRecordId \"1\"]\n"
        "[Mega2PGNEcoIndex \"1\"]\n\n"
        "1. Nf3 {A long endgame comment with Kramnik and a second line.} "
        "Nf6 (1... d5 {nested note}) 2. Ng1 *";
    PgnDocument document;
    check(PgnParser::parse(source, document) && document.games.size() == 1,
          "Xft search range fixture parses with a nested variation");
    if (document.games.empty()) return;
    X11NotationRenderer renderer(display, screen);
    XftFont* probe_text = XftFontOpenName(display, screen, "Roboto-12");
    XftFont* probe_symbols = open_annotation_font(display, screen, probe_text);
    const FcChar32 annotation_glyphs[] = {
        0x25a1,0x2299,0x21bb,0x2192,0x221e,0x2295,
        0x2206,0x2207,0x2194,0x2264,0x2248,0x2265
    };
    bool all_glyphs = probe_symbols != 0;
    for (std::size_t i = 0; all_glyphs &&
         i < sizeof(annotation_glyphs)/sizeof(annotation_glyphs[0]); ++i)
        all_glyphs = XftCharExists(display, probe_symbols, annotation_glyphs[i]) != 0;
    check(all_glyphs,
          "the same-size DejaVu fallback contains every glyph used by compact NAG displays");
    if (probe_text && probe_symbols) {
        XftNotationText probe(display, probe_text, 0, probe_symbols);
        check(probe.text_width("↔W") > 0 && probe.text_width("=∞") > 0 &&
              probe.text_width("⊕++W") > 0,
              "fallback symbol runs have measured widths for counterplay, compensation and time pressure");
    }
    if (probe_symbols) XftFontClose(display, probe_symbols);
    if (probe_text) XftFontClose(display, probe_text);
    NotationLayout layout;
    NotationSearch search;
    for (int mode = 0; mode < 2; ++mode) {
        renderer.set_figurines(mode != 0);
        layout.set_figurines(mode != 0);
        layout.build(*document.games[0], 360, renderer.metrics());
        search.set_query("Nf3", layout, 0, 180);
        std::vector<NotationSearchHighlight> hits =
            search.highlights(layout, 0, layout.content_height());
        bool exact = false;
        for (std::size_t i = 0; i < hits.size(); ++i) {
            if (!hits[i].current) continue;
            for (std::size_t j = 0; j < layout.items().size(); ++j) {
                const NotationItem& item = layout.items()[j];
                if (item.kind != NOTATION_MOVE || !item.node ||
                    item.text.find(mode ? "♘f3" : "Nf3") == std::string::npos)
                    continue;
                const std::size_t start = mode ? 0 : 0;
                const std::size_t length = mode ? 5 : 3;
                NotationTextRangeRect expected;
                if (layout.text_range_rect(j, start, length, expected) &&
                    expected.x == hits[i].rect.x &&
                    expected.y == hits[i].rect.y &&
                    expected.width == hits[i].rect.width)
                    exact = true;
            }
        }
        check(search.result_count() >= 1 && exact,
              "search highlights use the same live Xft advances as figurine and letter layout");
    }
}

void test_spacing_and_selection(Display* display, int screen)
{
    const int sizes[]={8,12,16,24};
    const char* moves[]={"♘c3","♗e6","♕c2","♖d1","♔f1","♘bd2","♖1e2","♕xf7+"};
    const int base[]={2,2,1,1,0,2,1,1};
    for(int n=0;n<4;++n) {
        std::ostringstream name;name<<"Roboto-"<<sizes[n];
        XftFont* normal=XftFontOpenName(display,screen,name.str().c_str());
        XftFont* pieces=open_figurine_font(display,screen,normal);
        XftNotationText text(display,normal,pieces);
        for(int i=0;i<8;++i) {
            int delta=text.text_width(moves[i])-text.move_text_width(moves[i]);
            int expected=(base[i]*sizes[n]+6)/12;
            const std::string token=moves[i];
            XGlyphInfo piece_ink, next_ink;
            XftTextExtentsUtf8(display,pieces,reinterpret_cast<const FcChar8*>(token.data()),3,&piece_ink);
            XftTextExtentsUtf8(display,normal,reinterpret_cast<const FcChar8*>(token.data()+3),1,&next_ink);
            int safe=piece_ink.xOff-piece_ink.width+piece_ink.x-next_ink.x;
            if(expected>safe) expected=safe;
            if(expected<0) expected=0;
            check(delta==expected,"optical correction scales deterministically and respects actual glyph ink");
            if(sizes[n]==12 && i<7)
                check(delta==base[i],"the five pieces and disambiguation match the requested 12 pt corrections");
            check(delta==0 || piece_ink.width-piece_ink.x<=piece_ink.xOff-delta-next_ink.x,
                  "corrected piece and following character have no ink rectangle overlap");
            std::cout<<sizes[n]<<"pt "<<moves[i]<<" correction="<<delta<<"px\n";
        }
        check(text.move_text_width("e8=♕+")==text.text_width("e8=♕+") &&
              text.move_text_width("axb8=♘#")==text.text_width("axb8=♘#") &&
              text.move_text_width("Nbd2 Nxf7+ e4 O-O")==text.text_width("Nbd2 Nxf7+ e4 O-O"),
              "letters, promotions, castling and pawn SAN keep their original advances");
        Pixmap pixmap=XCreatePixmap(display,RootWindow(display,screen),1000,1000,DefaultDepth(display,screen));
        GC gc=XCreateGC(display,pixmap,0,0);
        XftDraw* draw=XftDrawCreate(display,pixmap,DefaultVisual(display,screen),DefaultColormap(display,screen));
        XRenderColor white={65535,65535,65535,65535};XftColor color;
        bool allocated=XftColorAllocValue(display,DefaultVisual(display,screen),DefaultColormap(display,screen),&white,&color);
        check(allocated && draw,"spacing test has a real Xft drawable");
        for(int i=0;allocated && draw && i<8;++i)
            check(text.draw(draw,&color,20,50,moves[i],true)==text.move_text_width(moves[i]),
                  "actual corrected drawing consumes exactly the measured advance");
        X11NotationRenderer renderer(display,screen);renderer.set_text_sizes(sizes[n],sizes[n]);
        UciAnalysis analysis;analysis.san_pv="Nc3 Be6 Qc2 Rd1 Kf1 Nbd2 R1e2 Qxf7+ e8=Q+";
        renderer.set_figurines(true);
        std::string pv=renderer.engine_pv_text(analysis,false,2000);
        check(renderer.engine_metrics().text_width(pv,true)==text.move_text_width(pv),
              "engine PV uses exactly the same corrected advances as notation");
        int fit_left, fit_right;
        text.text_bounds("♘c3 ♗e6.",true,fit_left,fit_right);
        check(renderer.engine_pv_text(analysis,false,fit_right)=="♘c3 ♗e6.",
              "engine PV fitting includes optical corrections");
        const char* q[]={"!","?","!!","??","!?","?!"};
        for(int style=0;style<2;++style) for(int suffix=0;suffix<2;++suffix) for(int nag=1;nag<=6;++nag) {
            std::ostringstream input;input<<"1. e4 e5 2. Nf3 Nc6 3. Bb5";
            if(suffix) input<<q[nag-1]; else input<<" $"<<nag;
            input<<" $15 {A comment ♘c3 untouched.} *";
            PgnDocument doc;check(PgnParser::parse(input.str(),doc),"real Xft quality fixture parses");
            if(doc.games.empty()) continue;
            const MoveNode* selected=doc.games[0]->root;
            for(int k=0;k<5;++k) selected=selected->children[0];
            const int widths[]={82,600};
            for(int w=0;w<2;++w) {
                NotationLayout layout;layout.set_figurines(style!=0);layout.build(*doc.games[0],widths[w],renderer.metrics());
                std::vector<NotationSelectionRect> rects=layout.selection_rects(selected);
                if(allocated && draw) {
                    XSetForeground(display,gc,BlackPixel(display,screen));XFillRectangle(display,pixmap,gc,0,0,1000,1000);
                    for(std::size_t i=0;i<layout.items().size();++i) {
                        const NotationItem& item=layout.items()[i];
                        if(item.kind==NOTATION_COMMENT)
                            check(item.width==text.text_width(item.text),"literal comment figurines receive no optical correction");
                        if(!item.selects_move() || item.node!=selected) continue;
                        check(layout.hit_test(item.x+item.ink_right-1,item.y+1,0)==selected,
                              "rightmost actual quality ink is clickable after size and style changes");
                        int ascent=normal->ascent;
                        if(text.ink_ascent()>ascent)ascent=text.ink_ascent();
                        text.draw(draw,&color,20+item.x,22+item.y+ascent,item.text,item.kind==NOTATION_MOVE);
                    }
                    XImage* pixels=XGetImage(display,pixmap,0,0,1000,1000,AllPlanes,ZPixmap);
                    bool covered=pixels!=0;
                    for(int y=0;pixels && y<1000;++y) for(int x=0;x<widths[w]+40;++x) {
                        if(XGetPixel(pixels,x,y)==BlackPixel(display,screen)) continue;
                        bool inside=false;
                        for(std::size_t r=0;r<rects.size();++r)
                            if(x>=20+rects[r].x && x<20+rects[r].x+rects[r].width &&
                               y>=22+rects[r].y && y<18+rects[r].y+rects[r].height) inside=true;
                        if(!inside) covered=false;
                    }
                    check(covered,"every rendered SAN and quality pixel is covered by the measured selection rectangles");
                    if(pixels)XDestroyImage(pixels);
                }
                for(std::size_t r=0;r<rects.size();++r)
                    check(rects[r].x+rects[r].width<=widths[w],"quality selection fits the narrow right edge without arbitrary padding");
                for(std::size_t i=0;i<layout.items().size();++i) {
                    const NotationItem& item=layout.items()[i];
                    if(item.node==selected && item.kind==NOTATION_NAG && !item.move_quality)
                        for(std::size_t r=0;r<rects.size();++r)
                            check(rects[r].y!=item.y || rects[r].x+rects[r].width<=item.x,
                                  "position verdict and preceding space remain outside the selected background");
                }
            }
        }
        if(draw)XftDrawDestroy(draw);
        if(allocated)XftColorFree(display,DefaultVisual(display,screen),DefaultColormap(display,screen),&color);
        XFreeGC(display,gc);XFreePixmap(display,pixmap);
        if(pieces)XftFontClose(display,pieces);
        if(normal)XftFontClose(display,normal);
    }
}

void test_extended_nag_layout_xft(Display* display, int screen)
{
    const int sizes[] = {8, 12, 16, 24};
    const int values[] = {1,2,3,4,5,6,7,8,10,11,13,14,15,16,17,18,19,22,23,
        30,31,32,33,36,37,40,41,44,45,50,131,132,133,136,137,138,
        140,141,142,143,144,145,146,148,149,150,220,221,222,255,4000};
    std::ostringstream pgn;
    pgn << "1.e4";
    for (std::size_t i = 0; i < sizeof(values)/sizeof(values[0]); ++i)
        pgn << " $" << values[i];
    pgn << " e5 *";
    PgnDocument document;
    check(PgnParser::parse(pgn.str(), document) && document.games.size() == 1,
          "the complete known, fallback and suppressed NAG fixture parses");
    if (document.games.empty()) return;
    X11NotationRenderer renderer(display, screen);
    MoveNode* e4 = document.games[0]->root->children.empty() ? 0 :
        document.games[0]->root->children[0];
    check(e4 && e4->nags.size() == sizeof(values)/sizeof(values[0]),
          "every NAG code remains present in the move model before Xft layout");
    for (std::size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        renderer.set_text_sizes(sizes[s], sizes[s]);
        NotationLayout layout;
        layout.build(*document.games[0], 4000, renderer.metrics());
        std::size_t measured = 0;
        bool widths_match = true, high_hidden = true;
        for (std::size_t i = 0; i < layout.items().size(); ++i) {
            const NotationItem& item = layout.items()[i];
            if (item.kind != NOTATION_NAG || item.node != e4) continue;
            ++measured;
            if (item.width != renderer.metrics().text_width(item.text) ||
                item.x + item.width > 4000) widths_match = false;
            if (item.text == "$149" || item.text == "$150" || item.text == "$220" ||
                item.text == "$221" || item.text == "$222" || item.text == "$255" ||
                item.text == "$4000") high_hidden = false;
            if (item.move_quality && item.text != "!" && item.text != "?" &&
                item.text != "!!" && item.text != "??" && item.text != "!?" &&
                item.text != "?!") widths_match = false;
        }
        std::size_t expected = 0;
        for (std::size_t i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
            const std::string text = NotationLayout::nag_text(values[i]);
            if (text.empty()) continue;
            bool in_word = false;
            for (std::size_t j = 0; j < text.size(); ++j) {
                if (text[j] == ' ') in_word = false;
                else if (!in_word) { ++expected; in_word = true; }
            }
        }
        check(measured == expected && widths_match,
              "all visible NAG widths and wrapping items use actual Xft extents at each notation size");
        check(high_hidden,
              "NAGs at and above 149 remain absent from actual Xft layout at each size");
        std::cout << sizes[s] << "pt visible NAG layout items=" << measured << "\n";
    }
}

}
int main()
{
    Display* display = XOpenDisplay(0);
    if (!display) { std::cerr << "X11 display unavailable\n"; return 77; }
    const int screen = DefaultScreen(display);
    XWindowAttributes root_attributes;
    std::memset(&root_attributes, 0, sizeof(root_attributes));
    if (!XGetWindowAttributes(display, RootWindow(display, screen), &root_attributes)) {
        std::cerr << "Cannot inspect the X11 test display\n";
        XCloseDisplay(display);
        return 77;
    }
    if (root_attributes.all_event_masks & SubstructureRedirectMask) {
        std::cerr << "X11 tests require a display without an active window manager.\n"
                  << "Run: xvfb-run -a -s \"-screen 0 1600x900x24\" make test-x11\n";
        XCloseDisplay(display);
        return 77;
    }
    test_scratch_directory = test_paths::make_directory("gambitree-x11");
    if (test_scratch_directory.empty()) {
        std::cerr << "Cannot create X11 test scratch directory\n";
        XCloseDisplay(display);
        return 1;
    }
    std::cout << "X11 captures and diagnostics: " << test_scratch_directory << "\n";
    std::cout << "X11 test display: " << DisplayString(display) << ' '
              << DisplayWidth(display, screen) << 'x' << DisplayHeight(display, screen)
              << "; no window manager\n";
    test_window_title(display);
    test_board_option_window(display);
    test_geometry_window(display);
    test_window_modes(display);
    test_session_window(display);
    test_analysis_preview_rendering(display,screen);
    test_analysis_preview_window(display,screen);
    test_spacing_and_selection(display,screen);
    test_extended_nag_layout_xft(display,screen);
    test_keyboard_help_xft(display,screen);
    test_search_ranges_xft(display,screen);
    test_wheel_event_batch(display);
    test_wood_board(display,screen);
    test_cburnett_rendering(display,screen);
    test_veneer_board(display,screen);
    XftFont* normal = XftFontOpenName(display, screen, "Roboto-12");
    XftFont* pieces = open_figurine_font(display, screen, normal);
    if (!normal) { XCloseDisplay(display); return 1; }
    {
        int weight=0;
        if (pieces) FcPatternGetInteger(pieces->pattern,FC_WEIGHT,0,&weight);
        check(pieces && weight>=FC_WEIGHT_DEMIBOLD, "a real bold figurine face is matched");
        XftNotationText text(display, normal, pieces);
        XftNotationText missing(display, normal, 0);
        XftNotationText aliased(display, pieces, pieces);
        check(aliased.move_text_width("Nbd2 King Black AV")==aliased.text_width("Nbd2 King Black AV"),
              "aliased font roles never treat ordinary letters as figurine code points");
        check((text.figurine_mask() & 31U) == 31U,
              "all five SAN figurines exist in the matched font");
        check(XftCharExists(display,normal,0x2265) &&
              XftCharExists(display,normal,0x2264),
              "Roboto contains the compact better/worse continuation glyphs");
        check(NotationLayout::format_chessbase_references("\xc2\xa7" "e4",true,
                  text.figurine_mask()) == ((text.figurine_mask() & 32U) ? "♙e4" : "Pe4"),
              "legacy pawn reference uses its supported glyph or a readable letter fallback");
        check(missing.figurine_mask() == 0 &&
              NotationLayout::format_san("Kf1", true, missing.figurine_mask()) == "Kf1",
              "missing figurine font yields a measured and drawable ASCII fallback");
        check(pieces && text.text_width("♔f1") == advance(display,pieces,"♔") + advance(display,normal,"f1") &&
              text.text_width("e8=♕+") == advance(display,normal,"e8=") +
                  advance(display,pieces,"♕") + advance(display,normal,"+"),
              "independent Xft extents equal the mixed-font run advances");
        const char* tokens[] = {"♔f1", "♕h5+", "♖1e2", "♗xb6", "♘bd2", "e8=♕+", "axb8=♘#"};
        Pixmap pixmap = XCreatePixmap(display, RootWindow(display,screen), 500, 60, DefaultDepth(display,screen));
        GC gc = XCreateGC(display,pixmap,0,0);
        XftDraw* draw = XftDrawCreate(display,pixmap,DefaultVisual(display,screen),DefaultColormap(display,screen));
        XRenderColor white = {65535,65535,65535,65535};
        XftColor color;
        bool allocated = XftColorAllocValue(display,DefaultVisual(display,screen),DefaultColormap(display,screen),&white,&color);
        check(allocated && draw, "real Xft test drawable is available");
        for (std::size_t i=0; allocated && draw && i<sizeof(tokens)/sizeof(tokens[0]); ++i) {
            XSetForeground(display,gc,BlackPixel(display,screen));
            XFillRectangle(display,pixmap,gc,0,0,500,60);
            int width = text.text_width(tokens[i]);
            check(text.draw(draw,&color,10,30,tokens[i]) == width,
                  "drawing consumes exactly the measured glyph advances");
            XImage* pixels = XGetImage(display,pixmap,0,0,500,60,AllPlanes,ZPixmap);
            int left=500,right=-1,top=60,bottom=-1;
            if (pixels) {
                for (int y=0;y<60;++y) for (int x=0;x<500;++x)
                    if (XGetPixel(pixels,x,y) != BlackPixel(display,screen)) {
                        if (x<left) left=x;
                        if (x>right) right=x;
                        if (y<top) top=y;
                        if (y>bottom) bottom=y;
                    }
                XDestroyImage(pixels);
            }
            check(left>=9 && right<10+width+2 && bottom>=top && top>=30-20 && bottom<34,
                  "real figurine ink is nonempty, bounded and aligned to the common baseline");
            std::cout << tokens[i] << " advance=" << width << " ink=" << left << ',' << top << '-' << right << ',' << bottom << "\n";
        }
        if (draw) XftDrawDestroy(draw);
        if (allocated) XftColorFree(display,DefaultVisual(display,screen),DefaultColormap(display,screen),&color);
        XFreeGC(display,gc); XFreePixmap(display,pixmap);

        X11NotationRenderer renderer(display,screen);
        const char* paths[] = {"tests/data/carlsen-pelletier-2007.pgn", "tests/data/byrne-fischer-1956.pgn", "tests/data/robert-byrne-fischer-1963.pgn"};
        const int widths[] = {192,532,1400};
        for (int f=0; f<3; ++f) {
            std::ifstream input(paths[f]); std::ostringstream pgn; pgn<<input.rdbuf(); PgnDocument doc;
            check(input.good() && PgnParser::parse(pgn.str(),doc), "real PGN opens in Xft layout regression");
            if (doc.games.empty()) continue;
            PgnGame& game=*doc.games[0];
            for (int mode=0;mode<2;++mode) for(int w=0;w<3;++w) {
                NotationLayout layout; layout.set_figurines(mode!=0); layout.build(game,widths[w],renderer.metrics());
                for (std::size_t i=0;i<layout.items().size();++i) {
                    const NotationItem& item=layout.items()[i];
                    check(item.width == renderer.metrics().text_width(item.text, item.kind==NOTATION_MOVE) && item.x+item.width<=widths[w],
                          "actual mixed-font measurement drives every layout width and wrap");
                    check(NotationLayout::normalize_display_text(item.text)==item.text,
                          "actual Xft wrapping preserves complete UTF-8 code points");
                    if (item.move_quality && item.node) {
                        check(layout.hit_test(item.x+item.ink_right-1,item.y+1,0)==item.node,
                              "quality hit testing keeps exact nodes even in the deepest real analyses");
                        std::vector<NotationSelectionRect> rectangles=layout.selection_rects(item.node);
                        bool covered=false;
                        for(std::size_t r=0;r<rectangles.size();++r)
                            if(rectangles[r].y==item.y && rectangles[r].x<=item.x+item.ink_left &&
                               rectangles[r].x+rectangles[r].width>=item.x+item.ink_right) covered=true;
                        check(covered,"deep real RAV quality ink has complete measured selection geometry");
                    }
                    if (item.kind==NOTATION_MOVE && item.node) {
                        int scroll=layout.ensure_visible(item.node,layout.max_scroll(138),138);
                        check(layout.hit_test(item.x+item.width/2,item.y+1,0)==item.node,
                              "actual Xft move hitbox selects its exact node");
                        if (item.y>=scroll && item.y+item.height<=scroll+138)
                            check(layout.hit_test_visible(item.x+item.width/2,item.y-scroll+1,scroll,138)==item.node,
                                  "scrolled Xft move hitbox keeps node identity");
                    }
                }
            }
        }
        int control_bottom=renderer.controls_bottom();
        renderer.set_board_share(2500);
        renderer.set_figurines(true);
        UciAnalysis analysis;
        analysis.generation=19; analysis.san_pv="Nf3 d5 Bxe6+ e8=Q# O-O-O";
        const std::string original=analysis.san_pv;
        const int sizes[][2]={{8,24},{12,12},{16,10},{24,8},{12,12}};
        for (std::size_t n=0;n<sizeof(sizes)/sizeof(sizes[0]);++n) {
            renderer.set_text_sizes(sizes[n][0],sizes[n][1]);
            check(renderer.notation_points()==sizes[n][0] && renderer.engine_points()==sizes[n][1] &&
                  renderer.controls_bottom()==control_bottom && renderer.board_share()==2500,
                  "independent text sizes preserve general UI and normalized pane preference");
            check(renderer.engine_pv_text(analysis,false,2000)=="♘f3 d5. ♗xe6+ e8=♕#. O-O-O" &&
                  renderer.engine_pv_text(analysis,true,2000)=="[pending] ♘f3 d5. ♗xe6+ e8=♕#. O-O-O" &&
                  analysis.san_pv==original && analysis.generation==19,
                  "engine PV figurines are a display-only transformation with existing pending semantics");
            std::string first="♘f3 d5.";
            int fit_left,fit_right;
            renderer.engine_metrics().text_bounds(first,true,fit_left,fit_right);
            check(renderer.engine_pv_text(analysis,false,fit_right)==first,
                  "PV token fitting uses its own mixed-font metrics at every analysis size");
            renderer.set_figurines(false);
            check(renderer.engine_pv_text(analysis,false,2000)=="Nf3 d5. Bxe6+ e8=Q#. O-O-O",
                  "engine letter mode keeps the original displayed SAN");
            renderer.set_figurines(true);
            for (int f=0;f<3;++f) {
                std::ifstream input(paths[f]); std::ostringstream pgn; pgn<<input.rdbuf(); PgnDocument doc;
                check(PgnParser::parse(pgn.str(),doc), "size regression PGN parses");
                if(doc.games.empty()) continue;
                PgnGame& game=*doc.games[0];
                const int panel_widths[]={332,1400};
                for(int mode=0;mode<2;++mode) for(int w=0;w<2;++w) {
                    NotationLayout layout; layout.set_figurines(mode!=0);
                    layout.build(game,panel_widths[w],renderer.metrics());
                    for(std::size_t i=0;i<layout.items().size();++i) {
                        const NotationItem& item=layout.items()[i];
                        check(item.width==renderer.metrics().text_width(item.text, item.kind==NOTATION_MOVE) &&
                              item.x+item.width<=panel_widths[w] && item.height==renderer.metrics().line_height(),
                              "resized text uses matching glyph metrics and line boxes");
                        if(item.node && item.kind==NOTATION_MOVE)
                            check(layout.hit_test(item.x+item.width/2,item.y+1,0)==item.node,
                                  "resizing keeps exact move hitboxes in main lines and deep RAVs");
                    }
                }
                renderer.set_text_sizes(8,sizes[n][1]);
                SidebarBoxes before=renderer.boxes(1920,1100,game);
                renderer.set_text_sizes(24,sizes[n][1]);
                SidebarBoxes after=renderer.boxes(1920,1100,game);
                check(before.controls_bottom==after.controls_bottom &&
                      before.engine_top==after.engine_top && before.engine_bottom==after.engine_bottom &&
                      before.metadata_bottom==after.metadata_bottom,
                      "move text resizing leaves UI, engine area and metadata geometry unchanged");
                renderer.set_text_sizes(sizes[n][0],sizes[n][1]);
            }
        }
        FcChar8* family=0;
        if (pieces) FcPatternGetString(pieces->pattern,FC_FAMILY,0,&family);
        std::cout << "figurine family=" << (family ? reinterpret_cast<char*>(family) : "missing")
                  << " mask=" << text.figurine_mask() << " line-height=" << renderer.metrics().line_height() << "\n";
    }
    if (pieces) XftFontClose(display,pieces);
    XftFontClose(display,normal); XCloseDisplay(display);
    if (failures) return 1;
    std::cout << "Xft figurine metrics, drawing, fallback and move hitboxes passed.\n";
    return 0;
}
