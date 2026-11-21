#include "gambitree/x11_window.h"
#include "gambitree/window_geometry.h"
#include "gambitree/board_renderer.h"
#include "gambitree/configure_event_batch.h"
#include "gambitree/application_icon.h"
#include "gambitree/complete_frame_buffer.h"
#include "gambitree/delete_dialog.h"
#include "gambitree/asset_locator.h"
#include "gambitree/engine_display.h"
#include "gambitree/deferred_piece_resize.h"
#include "gambitree/keyboard_navigation.h"
#include "gambitree/navigation.h"
#include "gambitree/navigation_list.h"
#include "gambitree/visited_game_state.h"
#include "gambitree/wheel_event_batch.h"
#include "gambitree/notation_layout.h"
#include "gambitree/pgn.h"
#include "gambitree/pgn_library.h"
#include "gambitree/piece_set.h"
#include "gambitree/session_state.h"
#include "gambitree/uci_engine.h"
#include "gambitree/x11_notation_renderer.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>

#include <cstdlib>
#include <algorithm>
#include <clocale>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <iostream>
#include <new>
#include <poll.h>
#include <sys/select.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <signal.h>

namespace gambitree {

#ifndef GAMBITREE_DATA_DIR
#define GAMBITREE_DATA_DIR "/usr/local/share/gambitree"
#endif

namespace {

const char* const kResourceName = "gambitree";
const char* const kResourceClass = "Gambitree";
const unsigned long kPieceResizeSettleMilliseconds = 100;
uint64_t diagnostics_origin_us = 0;

volatile sig_atomic_t session_termination_requested = 0;
void request_session_termination(int) { session_termination_requested = 1; }

class SessionTerminationHandler {
public:
    SessionTerminationHandler() : installed_(false) {
        session_termination_requested = 0;
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_handler = request_session_termination;
        installed_ = sigaction(SIGTERM, &action, &previous_) == 0;
    }
    ~SessionTerminationHandler() {
        if (installed_) sigaction(SIGTERM, &previous_, 0);
    }
private:
    bool installed_;
    struct sigaction previous_;
};

// Server ConfigureNotify coordinates are parent-relative; WM synthetic ones
// are root-relative. Compare each space separately, without X11 round trips.
struct SessionGeometryChanges {
    int x[2], y[2], width, height;
    bool known[2], pending;
    SessionGeometryChanges() : width(0), height(0), pending(false) {
        known[0] = known[1] = false;
        x[0] = x[1] = y[0] = y[1] = 0;
    }
    void include(const XConfigureEvent& event) {
        const int kind = event.send_event ? 1 : 0;
        pending = pending || !known[kind] || x[kind] != event.x || y[kind] != event.y ||
                  width != event.width || height != event.height;
        known[kind] = true;
        x[kind] = event.x; y[kind] = event.y;
        width = event.width; height = event.height;
    }
};

uint64_t monotonic_milliseconds()
{
    static uint64_t last = 0;
    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) == 0)
        last = static_cast<uint64_t>(current.tv_sec) * 1000 +
               static_cast<uint64_t>(current.tv_nsec / 1000000L);
    return last; // Never fall back to a wall clock for a debounce deadline.
}

uint64_t monotonic_microseconds()
{
    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0)
        return EngineUiThrottle::now_milliseconds() * 1000;
    return static_cast<uint64_t>(current.tv_sec) * 1000000 +
           static_cast<uint64_t>(current.tv_nsec / 1000L);
}

void append_search_input(std::string& text, const char* bytes, int length)
{
    int index = 0;
    while (index < length) {
        const unsigned char first = static_cast<unsigned char>(bytes[index]);
        if (first < 0x80U) {
            if (first >= 0x20U && first != 0x7fU)
                text += static_cast<char>(first);
            ++index;
        } else {
            int count = 0;
            if ((first & 0xe0U) == 0xc0U) count = 2;
            else if ((first & 0xf0U) == 0xe0U) count = 3;
            else if ((first & 0xf8U) == 0xf0U) count = 4;
            bool valid = count && index + count <= length;
            for (int part = 1; valid && part < count; ++part)
                valid = (static_cast<unsigned char>(bytes[index + part]) &
                         0xc0U) == 0x80U;
            if (valid) {
                text.append(bytes + index, static_cast<std::size_t>(count));
                index += count;
            } else {
                /* XLookupString may return a single Latin-1 byte on older
                   input methods. Store it as UTF-8 so it matches PGN text. */
                text += static_cast<char>(0xc0U | (first >> 6));
                text += static_cast<char>(0x80U | (first & 0x3fU));
                ++index;
            }
        }
    }
}

void schedule_piece_resize(DeferredPieceResize& deferred_resize,
                           BoardRenderer& renderer, int width, int height,
                           int sidebar_preferred_width)
{
    BoardGeometry geometry = BoardLayout::calculate(
        width, height, sidebar_preferred_width);
    unsigned long old_generation = deferred_resize.generation();
    bool pending = deferred_resize.configure(
        geometry.square_size, renderer.high_quality_cache_square_size(),
        monotonic_milliseconds());
    if (deferred_resize.generation() != old_generation)
        renderer.cancel_high_quality_rebuild();
    renderer.set_interactive_resize(pending);
}

void begin_immediate_piece_rebuild(DeferredPieceResize& deferred_resize,
                                   BoardRenderer& renderer, int width,
                                   int height, int sidebar_preferred_width)
{
    BoardGeometry geometry = BoardLayout::calculate(
        width, height, sidebar_preferred_width);
    uint64_t now = monotonic_milliseconds();
    bool pending = deferred_resize.configure(
        geometry.square_size, renderer.high_quality_cache_square_size(), now);
    if (pending && deferred_resize.begin_build(now, true)) {
        if (!renderer.begin_high_quality_rebuild(
                deferred_resize.target_square_size(),
                deferred_resize.generation()))
            deferred_resize.cancel();
    }
    renderer.set_interactive_resize(deferred_resize.pending());
}

enum ResizeDeadlineResult {
    RESIZE_DEADLINE_NONE,
    RESIZE_DEADLINE_UPDATED,
    RESIZE_DEADLINE_BUILD_STARTED
};

ResizeDeadlineResult service_piece_resize_deadline(
    Display* display, Window window, DeferredPieceResize& deferred_resize,
    BoardRenderer& renderer, int& width, int& height,
    const X11NotationRenderer& notation, SessionGeometryChanges& geometry_changes,
    unsigned long& configure_events_seen,
    unsigned long& configure_events_coalesced)
{
    uint64_t now = monotonic_milliseconds();
    if (!deferred_resize.due(now))
        return RESIZE_DEADLINE_NONE;
    {
        ConfigureEventBatch batch;
        XEvent queued_event;
        while (XCheckTypedWindowEvent(display, window, ConfigureNotify,
                                      &queued_event)) {
            geometry_changes.include(queued_event.xconfigure);
            batch.include(queued_event.xconfigure.width,
                          queued_event.xconfigure.height);
        }
        if (batch.event_count() != 0) {
            width = batch.width();
            height = batch.height();
            configure_events_seen +=
                static_cast<unsigned long>(batch.event_count());
            configure_events_coalesced +=
                static_cast<unsigned long>(batch.event_count());
            schedule_piece_resize(deferred_resize, renderer, width, height,
                                  notation.sidebar_preferred_width(width));
            return RESIZE_DEADLINE_UPDATED;
        }
    }
    if (deferred_resize.begin_build(now)) {
        if (!renderer.begin_high_quality_rebuild(
                deferred_resize.target_square_size(),
                deferred_resize.generation())) {
            deferred_resize.cancel();
            renderer.cancel_high_quality_rebuild();
            return RESIZE_DEADLINE_NONE;
        }
        return RESIZE_DEADLINE_BUILD_STARTED;
    }
    return RESIZE_DEADLINE_NONE;
}

bool read_frame_extents(Display* display, Window window, Atom atom,
                        WindowFrameExtents& extents)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* property = 0;
    bool valid = false;
    if (XGetWindowProperty(display, window, atom, 0, 4, False, XA_CARDINAL,
                           &actual_type, &actual_format, &item_count,
                           &bytes_after, &property) == Success &&
        actual_type == XA_CARDINAL && actual_format == 32 &&
        item_count == 4 && bytes_after == 0 && property) {
        unsigned long* values = reinterpret_cast<unsigned long*>(property);
        const unsigned long maximum_extent = 100000UL;
        if (values[0] <= maximum_extent && values[1] <= maximum_extent &&
            values[2] <= maximum_extent && values[3] <= maximum_extent) {
            extents.left = static_cast<int>(values[0]);
            extents.right = static_cast<int>(values[1]);
            extents.top = static_cast<int>(values[2]);
            extents.bottom = static_cast<int>(values[3]);
            valid = true;
        }
    }
    if (property)
        XFree(property);
    return valid;
}

bool request_frame_extents(Display* display, Window window,
                           Atom request_atom, Atom extents_atom,
                           WindowFrameExtents& extents)
{
    XEvent event;
    struct pollfd descriptor;
    int attempt;
    if (read_frame_extents(display, window, extents_atom, extents))
        return true;
    std::memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = request_atom;
    event.xclient.format = 32;
    event.xclient.data.l[0] = static_cast<long>(window);
    if (!XSendEvent(display, RootWindow(display, DefaultScreen(display)),
                    False, SubstructureNotifyMask | SubstructureRedirectMask,
                    &event))
        return false;
    XFlush(display);
    descriptor.fd = ConnectionNumber(display);
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    for (attempt = 0; attempt < 20; ++attempt) {
        int result = poll(&descriptor, 1, 25);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            return false;
        if (read_frame_extents(display, window, extents_atom, extents))
            return true;
    }
    return false;
}

bool find_outer_root_position(Display* display, Window window, Window root,
                              int& outer_x, int& outer_y)
{
    Window current = window;
    for (;;) {
        Window query_root = None;
        Window parent = None;
        Window* children = 0;
        unsigned int child_count = 0;
        if (!XQueryTree(display, current, &query_root, &parent,
                        &children, &child_count))
            return false;
        if (children)
            XFree(children);
        if (query_root != root || parent == None)
            return false;
        if (parent == root) {
            Window child;
            return XTranslateCoordinates(display, current, root, 0, 0,
                                         &outer_x, &outer_y, &child) != False;
        }
        current = parent;
    }
}

bool get_outer_root_position(Display* display, Window window, Window root,
                             Atom extents_atom, int client_x, int client_y,
                             int& outer_x, int& outer_y,
                             WindowFrameExtents& extents, bool& have_extents,
                             bool& used_tree)
{
    have_extents = read_frame_extents(display, window, extents_atom, extents);
    used_tree = false;
    if (have_extents)
        return client_to_outer_position(client_x, client_y, extents,
                                        outer_x, outer_y);
    used_tree = find_outer_root_position(display, window, root,
                                         outer_x, outer_y);
    return used_tree;
}

void collect_session_preferences(SessionState& state, int width, int height,
                                  bool analysis_enabled,
                                  const X11NotationRenderer& notation,
                                  const std::string& pieces,
                                  const BoardRenderer& renderer)
{
    state.width = width; state.height = height;
    state.analysis_enabled = analysis_enabled;
    state.board_share = notation.board_share();
    state.figurines = notation.figurines();
    state.notation_points = notation.notation_points();
    state.engine_points = notation.engine_points();
    state.piece_set_id = pieces;
    state.board_theme_id = renderer.board_theme_id();
}

void capture_session_geometry(Display* display, Window window, Atom extents_atom,
                                SessionState& state,
                                SessionGeometryChanges* hints = 0)
{
    if (!window) return; // Retain the last reliable geometry after DestroyNotify.
    XWindowAttributes attributes;
    if (hints && XGetWindowAttributes(display, window, &attributes)) {
        state.width = attributes.width; state.height = attributes.height;
        hints->width = attributes.width; hints->height = attributes.height;
        hints->x[0] = attributes.x; hints->y[0] = attributes.y;
        hints->known[0] = true;
    }
    int client_x, client_y, outer_x, outer_y;
    Window child;
    bool have_extents, used_tree;
    WindowFrameExtents extents;
    Window root = RootWindow(display, DefaultScreen(display));
    if (XTranslateCoordinates(display, window, root, 0, 0,
                              &client_x, &client_y, &child)) {
        if (hints) {
            hints->x[1] = client_x; hints->y[1] = client_y;
            hints->known[1] = true;
        }
        if (get_outer_root_position(display, window, root, extents_atom,
                                   client_x, client_y, outer_x, outer_y,
                                   extents, have_extents, used_tree)) {
            state.x = outer_x; state.y = outer_y;
            state.has_position = true;
            state.position_is_outer = true;
        }
    }
}

bool transient_window_geometry(Display* display, Window window)
{
    const Atom net_state = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom full = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    const Atom wm_state = XInternAtom(display, "WM_STATE", False);
    const Atom properties[] = {net_state, wm_state};
    bool transient = false;
    for (int i = 0; i < 2; ++i) {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* bytes = 0;
        if (XGetWindowProperty(display, window, properties[i], 0, 1024, False,
                i == 0 ? XA_ATOM : wm_state, &type, &format, &count,
                &remaining, &bytes) == Success && bytes && format == 32 &&
                type == (i == 0 ? XA_ATOM : wm_state)) {
            const unsigned long* values = reinterpret_cast<unsigned long*>(bytes);
            for (unsigned long j = 0; j < (i == 0 ? count : std::min(count, 1UL)); ++j)
                if (values[j] == (i == 0 ? full : static_cast<Atom>(IconicState)))
                    transient = true;
        }
        if (bytes) XFree(bytes);
    }
    return transient;
}

void service_session_save(Display* display, Window window, Atom extents_atom,
                           SessionState& state, SessionStateSaver& saver,
                           const std::string& path, bool& error_reported,
                           bool diagnostics, bool immediately = false)
{
    const uint64_t now = monotonic_milliseconds();
    if (!saver.pending() || (!immediately && !saver.due(now))) return;
    if (!immediately && window) {
        XEvent configure;
        if (XCheckTypedWindowEvent(display, window, DestroyNotify, &configure) ||
            XCheckTypedWindowEvent(display, window, ConfigureNotify, &configure)) {
            XPutBackEvent(display, &configure);
            return; // Coalesce queued geometry before starting any file I/O.
        }
    }
    if (!immediately)
        capture_session_geometry(display, window, extents_atom, state);
    // Keep transient fullscreen/iconic geometry out of preference merges,
    // while still allowing changes to board, pieces and text sizes to save.
    if (window && transient_window_geometry(display, window))
        saver.retain_saved_geometry(state);
    SessionSaveResult result = saver.save(state, now, immediately);
    // Only at shutdown: let a simultaneous writer finish, with a firm bound.
    for (int attempt = 0; immediately && result == SESSION_SAVE_BUSY && attempt < 25; ++attempt) {
        poll(0, 0, 10);
        result = saver.save(state, monotonic_milliseconds(), true);
    }
    if ((result == SESSION_SAVE_FAILED || (immediately && result == SESSION_SAVE_BUSY)) &&
        !error_reported) {
        std::cerr << "gambitree: cannot save session state '" << path
                  << "': " << std::strerror(errno) << (immediately
                      ? "; pending changes could not be saved\n" : "; retrying later\n");
        error_reported = true;
    }
    if (!saver.pending()) error_reported = false;
    if (diagnostics && result == SESSION_SAVE_SAVED)
        std::cerr << "gambitree: diagnostic: session state write saved\n";
}

void report_window_geometry(Display* display, Window window, Window root,
                            Atom extents_atom, int requested_x,
                            int requested_y, bool diagnostics)
{
    XWindowAttributes attributes;
    Window child;
    int client_x;
    int client_y;
    int outer_x;
    int outer_y;
    WindowFrameExtents extents;
    bool have_extents;
    bool used_tree;
    if (!diagnostics || !XGetWindowAttributes(display, window, &attributes) ||
        !XTranslateCoordinates(display, window, root, 0, 0,
                                &client_x, &client_y, &child))
        return;
    if (!get_outer_root_position(display, window, root, extents_atom,
                                 client_x, client_y, outer_x, outer_y,
                                 extents, have_extents, used_tree))
        return;
    std::cerr << "gambitree: diagnostic: window geometry requested-outer="
              << requested_x << "," << requested_y << " client-root="
              << client_x << "," << client_y;
    if (have_extents)
        std::cerr << " frame-extents=" << extents.left << ","
                  << extents.right << "," << extents.top << ","
                  << extents.bottom;
    std::cerr << " outer-root=" << outer_x << "," << outer_y
              << (used_tree ? " (parent-tree)" : " (frame-extents)")
              << " size=" << attributes.width << "x" << attributes.height
              << "\n";
}

void update_highlight(const MoveNode* node, LastMoveHighlight& highlight)
{
    highlight.valid = node != 0 && node->has_move;
    if (highlight.valid) { highlight.from = node->move.from; highlight.to = node->move.to; }
}

std::string basename(const std::string& path)
{
    std::string::size_type slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct DeleteDialogState {
    Window window;
    Window closing_window;
    int width;
    int height;
    bool error;
    DeleteDialogInputState input;
    int hovered_button;
    std::vector<std::string> lines;
    DeleteDialogState()
        : window(0), closing_window(0), width(0), height(0), error(false),
          input(false), hovered_button(-1)
    {
    }
};

void position_delete_dialog(Display* display, Window parent,
                            X11NotationRenderer& notation,
                            DeleteDialogState& dialog, int parent_width,
                            int parent_height)
{
    int available_width = parent_width - 16;
    int available_height = parent_height - 16;
    int x = (parent_width - dialog.width) / 2;
    int y = (parent_height - dialog.height) / 2;
    int new_width;
    int new_height;
    if (available_width < 180)
        available_width = 180;
    new_width = available_width < 680 ? available_width : 680;
    new_height = notation.delete_dialog_height(new_width, dialog.error,
                                                dialog.lines);
    if (available_height >= 120 && new_height > available_height)
        new_height = available_height;
    if (new_width != dialog.width || new_height != dialog.height) {
        dialog.width = new_width;
        dialog.height = new_height;
        XResizeWindow(display, dialog.window,
                      static_cast<unsigned int>(dialog.width),
                      static_cast<unsigned int>(dialog.height));
    }
    x = (parent_width - dialog.width) / 2;
    y = (parent_height - dialog.height) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    XMoveWindow(display, dialog.window, x, y);
    (void)parent;
}

void draw_delete_dialog(Display* display, X11NotationRenderer& notation,
                        const DeleteDialogState& dialog)
{
    if (!dialog.window)
        return;
    notation.draw_delete_dialog(dialog.window, dialog.width, dialog.height,
                                dialog.error, dialog.lines,
                                dialog.input.focused_button,
                                dialog.hovered_button);
    XFlush(display);
}

void close_delete_dialog(Display* display, Window parent,
                         DeleteDialogState& dialog)
{
    if (dialog.window) {
        dialog.closing_window = dialog.window;
        XDestroyWindow(display, dialog.window);
    }
    dialog.window = 0;
    XSetInputFocus(display, parent, RevertToParent, CurrentTime);
}

void open_delete_dialog(Display* display, Window parent,
                        X11NotationRenderer& notation, int parent_width,
                        int parent_height, DeleteDialogState& dialog,
                        bool is_error,
                        const std::vector<std::string>& lines)
{
    if (dialog.window)
        close_delete_dialog(display, parent, dialog);
    dialog.error = is_error;
    dialog.input = DeleteDialogInputState(is_error);
    dialog.hovered_button = -1;
    dialog.lines = lines;
    if (parent_width >= 296)
        dialog.width = parent_width - 16 < 680 ? parent_width - 16 : 680;
    else
        dialog.width = 260;
    dialog.height = notation.delete_dialog_height(dialog.width, is_error,
                                                   dialog.lines);
    if (parent_height >= 170 && dialog.height > parent_height - 16)
        dialog.height = parent_height - 16;
    if (dialog.height < 154)
        dialog.height = 154;
    dialog.window = XCreateSimpleWindow(
        display, parent, 0, 0, static_cast<unsigned int>(dialog.width),
        static_cast<unsigned int>(dialog.height), 1,
        BlackPixel(display, DefaultScreen(display)),
        WhitePixel(display, DefaultScreen(display)));
    if (!dialog.window) {
        dialog.error = true;
        dialog.lines.clear();
        dialog.lines.push_back("The graphical confirmation window could not be opened.");
        return;
    }
    XSelectInput(display, dialog.window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 PointerMotionMask | StructureNotifyMask);
    position_delete_dialog(display, parent, notation, dialog, parent_width,
                           parent_height);
    draw_delete_dialog(display, notation, dialog);
    XMapRaised(display, dialog.window);
    XSetInputFocus(display, dialog.window, RevertToParent, CurrentTime);
}

std::string game_label(const PgnLibrary* library)
{
    std::ostringstream text;
    if (!library) return "Demo game";
    if (library->game_count() == 0) return "No indexed game";
    text << "Game " << library->game_index() + 1 << "/" << library->game_count();
    if (!library->indexing_complete()) text << "+";
    return text.str();
}

std::vector<std::string> game_navigation_items(PgnLibrary* library,
                                                std::string& error,
                                                bool figurines,
                                                unsigned int figurine_mask,
                                                std::size_t first = 0)
{
    std::vector<std::string> items;
    if (!library || !library->current_file()) return items;
    const std::size_t count = library->game_count();
    if (first > count) first = count;
    items.reserve(count - first);
    for (std::size_t index = first; index < count; ++index) {
        std::vector<PgnTag> tags;
        std::string tag_error;
        if (!library->game_tags(index, tags, tag_error)) {
            std::ostringstream fallback;
            fallback << index + 1 << ". Game " << index + 1;
            items.push_back(fallback.str());
            if (error.empty()) error = tag_error;
        } else {
            items.push_back(navigation_game_label(
                index + 1, tags, figurines, figurine_mask));
        }
    }
    return items;
}

std::size_t requested_file_index(const PgnLibrary* library)
{
    if (!library || !library->current_file())
        return static_cast<std::size_t>(-1);
    const std::vector<std::string>& paths = library->requested_paths();
    for (std::size_t i = 0; i < paths.size(); ++i)
        if (paths[i] == library->current_file()->path) return i;
    return static_cast<std::size_t>(-1);
}

std::string indexing_status(const PgnLibrary* library)
{
    std::ostringstream text;
    if (!library || library->indexing_complete()) return "";
    text << "Indexing " << library->indexed_file_count() << " / " << library->file_count()
         << " files - " << library->indexed_game_count() << " games found";
    return text.str();
}

bool load_selected_game(PgnLibrary* library, PgnGame& placeholder, PgnGame*& game,
                        MoveNavigator*& navigator, int& scroll, std::string& status)
{
    std::string error;
    delete navigator; navigator = new MoveNavigator(placeholder); game = &placeholder; scroll = 0;
    if (!library->materialize_current(error)) {
        status = "Loading game failed: " + error;
        std::cerr << "gambitree: " << library->current_file()->path << ": game "
                  << library->game_index() + 1 << ": " << error << "\n";
        return false;
    }
    game = library->current_game();
    delete navigator; navigator = new MoveNavigator(*game); status.clear(); return true;
}

void set_title(Display* display, Window window, const PgnLibrary* library,
               const std::string* window_title)
{
    std::ostringstream title;
    title << (window_title ? *window_title : "Gambitree");
    if (!window_title && library && library->current_file()) {
        title << " - " << basename(library->current_file()->path);
        if (library->game_count() != 0)
            title << " - " << library->game_index() + 1 << "/" << library->game_count();
    }
    const std::string text = title.str();
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_NAME", False),
                    utf8, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(text.c_str()),
                    static_cast<int>(text.size()));
    // Keep the existing default WM_NAME; explicit UTF-8 titles use ICCCM
    // STRING or COMPOUND_TEXT for older window managers when convertible.
    // Otherwise retain UTF-8 with its correct type, rather than label it STRING.
    XTextProperty legacy = {0, 0, 0, 0};
    char* value = const_cast<char*>(text.c_str());
    if (window_title &&
        Xutf8TextListToTextProperty(display, &value, 1, XStdICCTextStyle,
                                   &legacy) == Success)
        XSetWMName(display, window, &legacy);
    else if (window_title)
        XChangeProperty(display, window, XA_WM_NAME, utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(text.c_str()),
                        static_cast<int>(text.size()));
    else
        XStoreName(display, window, text.c_str());
    if (legacy.value) XFree(legacy.value);
}

void set_net_wm_icon(Display* display, Window window)
{
    std::vector<unsigned long> property;
    std::string error;
    Atom icon_atom;
    bool first = true;
    std::size_t offset = 0;
    const std::size_t chunk_words = 16384U;
    if (!load_application_icon(GAMBITREE_DATA_DIR, AssetLocator::executable_path(), property, error)) {
        if (std::getenv("GAMBITREE_DIAGNOSTICS"))
            std::cerr << "gambitree: diagnostic: window icon unavailable: " << error << "\n";
        return;
    }
    icon_atom = XInternAtom(display, "_NET_WM_ICON", False);
    /* For format 32, Xlib consumes native long elements and narrows each to
       one protocol CARDINAL. Chunking keeps each request below core limits. */
    while (offset < property.size()) {
        std::size_t remaining = property.size() - offset;
        int count = static_cast<int>(remaining < chunk_words ? remaining : chunk_words);
        XChangeProperty(display, window, icon_atom, XA_CARDINAL, 32,
                        first ? PropModeReplace : PropModeAppend,
                        reinterpret_cast<const unsigned char*>(&property[offset]), count);
        first = false;
        offset += static_cast<std::size_t>(count);
    }
}

void set_cardinal_property(Display* display, Window window, Atom property, unsigned long value)
{
    XChangeProperty(display, window, property, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&value), 1);
}

void set_client_machine(Display* display, Window window)
{
    char hostname[256];
    char* values[1];
    XTextProperty property;
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) return;
    hostname[sizeof(hostname) - 1] = '\0';
    values[0] = hostname;
    if (XStringListToTextProperty(values, 1, &property)) {
        XSetWMClientMachine(display, window, &property);
        XFree(property.value);
    }
}

void sync_stockfish_pid(Display* display, Window window, Atom property, const UciEngine& engine)
{
    if (engine.child_pid() > 0)
        set_cardinal_property(display, window, property, static_cast<unsigned long>(engine.child_pid()));
    else
        XDeleteProperty(display, window, property);
}

void ensure_selection_visible(const PgnGame& game, X11NotationRenderer& notation, NotationLayout& layout,
                              const MoveNavigator& navigator, int width, int height, int& scroll)
{
    layout.set_figurines(notation.figurines());
    layout.build(game, notation.content_width(width), notation.metrics());
    const int viewport = notation.notation_viewport_height(width, height, game);
    if (navigator.at_root()) scroll = 0;
    else scroll = layout.ensure_visible(navigator.current(), scroll, viewport);
}

void remember_current_game_navigation(VisitedGameStates& states,
                                      const PgnLibrary* library,
                                      const PgnGame* game,
                                      const MoveNavigator& navigator)
{
    const PgnFile* file;
    if (!library || !game || library->current_game() != game)
        return;
    file = library->current_file();
    if (file)
        states.save(file->session_identity, library->game_index(), *game,
                    navigator);
}

bool restore_current_game_navigation(VisitedGameStates& states,
                                     const PgnLibrary* library,
                                     PgnGame* game,
                                     MoveNavigator& navigator,
                                     X11NotationRenderer& notation,
                                     NotationLayout& layout,
                                     int width, int height, int& scroll,
                                     bool diagnostics)
{
    const PgnFile* file;
    bool exact_path = false;
    bool restored;
    if (!library || !game)
        return false;
    file = library->current_file();
    if (!file || library->current_game() != game)
        return false;
    restored = states.restore(file->session_identity, library->game_index(), *game,
                             navigator, exact_path);
    if (restored) {
        ensure_selection_visible(*game, notation, layout, navigator,
                                 width, height, scroll);
        if (diagnostics && !exact_path)
            std::cerr << "gambitree: diagnostic: saved navigation path was "
                      << "truncated to its valid prefix\n";
    }
    return restored;
}

bool activate_navigation_list(
    NavigationListState& list, PgnLibrary* library, PgnGame& placeholder,
    PgnGame*& game, MoveNavigator*& navigator, int& scroll,
    VisitedGameStates& visited, X11NotationRenderer& notation,
    NotationLayout& layout, int width, int height, bool diagnostics,
    UciEngine& engine, bool analysis_enabled, EngineDisplay& engine_display,
    Display* display, Window window, const std::string* window_title,
    std::string& status)
{
    if (!library || !list.has_selection()) return false;
    const std::size_t selected = list.selected_source_index();
    bool changed = false;
    remember_current_game_navigation(visited, library, game, *navigator);
    if (list.kind() == NAVIGATION_LIST_GAMES)
        changed = library->select_game(selected);
    else if (list.kind() == NAVIGATION_LIST_FILES) {
        const std::vector<std::string>& paths = library->requested_paths();
        if (selected >= paths.size()) return false;
        changed = library->select_file_path(paths[selected]);
        if (!changed) {
            status = "File was not available when Gambitree started: " +
                     paths[selected];
            return false;
        }
    }
    if (!changed) return false;
    if (analysis_enabled) engine.stop();
    engine_display.clear();
    if (load_selected_game(library, placeholder, game, navigator, scroll,
                           status)) {
        restore_current_game_navigation(visited, library, game, *navigator,
                                        notation, layout, width, height,
                                        scroll, diagnostics);
        if (analysis_enabled) engine.request_analysis(navigator->position());
    }
    set_title(display, window, library, window_title);
    return true;
}

void redraw(Display* display, int screen, Window window, int width, int height, BoardRenderer& board,
            X11NotationRenderer& notation, const PgnGame& game, MoveNavigator& navigator,
            NotationLayout& layout, bool flipped, LastMoveHighlight& highlight, int& scroll,
            Pixmap& backbuffer, int& backbuffer_width, int& backbuffer_height, const PgnLibrary* library,
    const std::string& status, const UciEngine& engine, bool analysis_enabled, EngineDisplay& engine_display,
    KeyboardHelpState& keyboard_help, const char* reason)
{
    static unsigned long sequence = 0;
    static bool first_notation_frame_reported = false;
    uint64_t render_started = monotonic_microseconds();
    uint64_t frame_ready = 0;
    uint64_t board_finished = 0;
    uint64_t sidebar_started = 0;
    uint64_t sidebar_finished = 0;
    uint64_t present_started = 0;
    uint64_t presented = 0;
    uint64_t layout_started = 0;
    uint64_t layout_finished = 0;
    Pixmap previous_backbuffer = backbuffer;
    Pixmap candidate = 0;
    Pixmap target = backbuffer;
    CompleteFrameBuffer frame(static_cast<unsigned long>(backbuffer),
                              backbuffer_width, backbuffer_height);
    int notation_viewport = notation.notation_viewport_height(width, height,
                                                              game);
    engine_display.sync_position(navigator.position(), navigator.current());
    engine_display.update(engine, analysis_enabled);
    if (frame.needs_replacement(width, height)) {
        candidate = XCreatePixmap(
            display, window, static_cast<unsigned int>(width),
            static_cast<unsigned int>(height),
            static_cast<unsigned int>(DefaultDepth(display, screen)));
        if (!candidate) {
            if (std::getenv("GAMBITREE_DIAGNOSTICS"))
                std::cerr << "gambitree: diagnostic: cannot allocate "
                          << "complete " << width << "x" << height
                          << " frame; retaining previous frame\n";
            if (frame.valid()) {
                unsigned int copy_width = static_cast<unsigned int>(
                    width < frame.width() ? width : frame.width());
                unsigned int copy_height = static_cast<unsigned int>(
                    height < frame.height() ? height : frame.height());
                XCopyArea(display, static_cast<Pixmap>(frame.drawable()),
                          window, DefaultGC(display, screen), 0, 0,
                          copy_width, copy_height, 0, 0);
                XFlush(display);
            }
            return;
        }
        target = candidate;
    }
    layout.set_figurines(notation.figurines());
    layout_started = monotonic_microseconds();
    layout.build(game, notation.content_width(width), notation.metrics());
    notation.search().refresh(layout);
    layout_finished = monotonic_microseconds();
    if (std::getenv("GAMBITREE_DIAGNOSTICS") && !first_notation_frame_reported &&
        game.root && !game.root->children.empty() && !layout.items().empty()) {
        first_notation_frame_reported = true;
        std::cerr << "gambitree: diagnostic: first usable notation frame after "
                  << (layout_finished - diagnostics_origin_us) / 1000
                  << " ms, items=" << layout.items().size() << "\n";
    }
    if (scroll < 0) scroll = 0;
    if (scroll > layout.max_scroll(notation_viewport)) scroll = layout.max_scroll(notation_viewport);
    update_highlight(navigator.current(), highlight);
    if (engine_display.preview_move()) {
        highlight.from = engine_display.preview_move()->from;
        highlight.to = engine_display.preview_move()->to;
        highlight.valid = true;
    }
    board.draw(target, width, height, engine_display.preview_position() ?
               *engine_display.preview_position() : navigator.position(), flipped, highlight,
               notation.sidebar_preferred_width(width));
    board_finished = monotonic_microseconds();
    sidebar_started = board_finished;
    notation.draw(target, width, height, game, layout,
                  navigator.current(), scroll, engine_display);
    notation.draw_controls(target, width, game_label(library), library && library->can_previous_file(),
                           library && library->can_next_file(), library && library->can_previous_game(),
                           library && library->can_next_game());
    if (!status.empty()) notation.draw_status(target, width, status);
    {
        SidebarGeometry sidebar = notation.sidebar_geometry(width);
        int divider = sidebar.left - BoardLayout::sidebar_gutter() / 2;
        XSetForeground(display, DefaultGC(display, screen), BlackPixel(display, screen));
        XDrawLine(display, target, DefaultGC(display, screen), divider, 8,
                  divider, height - 8);
    }
    if (keyboard_help.visible) notation.draw_keyboard_help(target, width, height, keyboard_help.scroll);
    notation.draw_navigation_list_overlay(target, width, height);
    sidebar_finished = monotonic_microseconds();
    frame_ready = monotonic_microseconds();
    if (candidate) {
        /* The candidate is app-owned only. The window keeps a flat solid
           background so X11 cannot tile an old, smaller frame on resize. */
        frame.commit_completed(static_cast<unsigned long>(candidate), width,
                               height, true);
        backbuffer = candidate;
        backbuffer_width = width;
        backbuffer_height = height;
    }
    present_started = monotonic_microseconds();
    XCopyArea(display, target, window, DefaultGC(display, screen),
              0, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(height), 0, 0);
    XFlush(display);
    presented = monotonic_microseconds();
    if (candidate && previous_backbuffer &&
        previous_backbuffer != candidate)
        XFreePixmap(display, previous_backbuffer);
    ++sequence;
    if (std::getenv("GAMBITREE_DIAGNOSTICS") &&
        (sequence <= 120 || sequence % 50 == 0 ||
         (reason && (std::strcmp(reason, "resize") == 0 ||
                     std::strcmp(reason, "keyboard") == 0 ||
                     std::strcmp(reason, "analysis-click") == 0 ||
                     std::strcmp(reason, "help-mouse") == 0 ||
                     std::strcmp(reason, "divider-drag") == 0)))) {
        std::cerr << "gambitree: diagnostic: present #" << sequence << " t="
                  << EngineUiThrottle::now_milliseconds() << " reason=" << (reason ? reason : "unknown")
                  << " since_window_start_ms="
                  << (render_started - diagnostics_origin_us) / 1000
                  << " indexing=" << (library && !library->indexing_complete() ? "yes" : "no")
                  << " engine_generation=" << engine.generation()
                  << " pending=" << ((!engine_display.score_is_current() || !engine_display.pv_is_current()) ? "yes" : "no")
                  << " selected=" << (navigator.current()->has_move ? navigator.current()->san : "root")
                  << " selected_node=" << navigator.current()
                  << " notation_scroll=" << scroll
                  << " board_share=" << notation.board_share()
                  << " notation_width=" << notation.sidebar_geometry(width).width
                  << " help=" << (keyboard_help.visible ? 1 : 0)
                  << " help_scroll=" << keyboard_help.scroll
                  << " flipped=" << (flipped ? 1 : 0)
                  << " analysis=" << (analysis_enabled ? 1 : 0)
                  << " preview=" << engine_display.preview_index()
                  << " pieces=" << board.piece_set_id()
                  << " figurines=" << (notation.figurines() ? 1 : 0)
                  << " notation_points=" << notation.notation_points()
                  << " engine_points=" << notation.engine_points()
                  << " fen='" << navigator.position().to_fen() << "'"
                  << " board_fen='" << (engine_display.preview_position() ?
                        engine_display.preview_position()->to_fen() : navigator.position().to_fen()) << "'"
                  << " backbuffer=" << backbuffer_width << "x" << backbuffer_height
                  << " board-piece-instances-drawn="
                  << board.last_piece_instances_drawn()
                  << " full_background=yes engine_background=yes copyarea=yes";
        if (frame_ready != 0) {
            std::cerr << " frame_render_us="
                      << frame_ready - render_started
                      << " board_draw_us=" << board_finished - render_started
                      << " board_base_ms="
                      << board.last_draw_metrics().board_base_ms
                      << " piece_instances="
                      << board.last_draw_metrics().piece_instances
                      << " ximage_get_calls="
                      << board.last_draw_metrics().ximage_get_calls
                      << " piece_destination_readbacks="
                      << board.last_draw_metrics().piece_destination_readbacks
                      << " board_snapshot_ximage_get_ms="
                      << board.last_draw_metrics().ximage_get_ms
                      << " board_snapshot_bytes="
                      << board.last_draw_metrics().ximage_get_bytes
                      << " piece_alpha_composite_ms="
                      << board.last_draw_metrics().alpha_composite_ms
                      << " ximage_put_calls="
                      << board.last_draw_metrics().ximage_put_calls
                      << " board_ximage_put_ms="
                      << board.last_draw_metrics().ximage_put_ms
                      << " board_ximage_put_bytes="
                      << board.last_draw_metrics().ximage_put_bytes
                      << " sidebar_notation_us="
                      << sidebar_finished - sidebar_started
                      << " layout_build_us=" << layout_finished - layout_started
                      << " layout_items=" << layout.items().size()
                      << " board_total_ms="
                      << board.last_draw_metrics().total_ms
                      << " xcopy_pixels="
                      << static_cast<unsigned long>(width) *
                             static_cast<unsigned long>(height)
                      << " xcopy_submit_us="
                      << presented - present_started
                      << " present_queued_us=" << presented - frame_ready;
        }
        std::cerr << "\n";
    }
}

void present_backbuffer(Display* display, int screen, Window window,
                        const Pixmap backbuffer, int backbuffer_width,
                        int backbuffer_height)
{
    CompleteFrameBuffer frame(static_cast<unsigned long>(backbuffer),
                              backbuffer_width, backbuffer_height);
    unsigned long expose_drawable = frame.drawable_for_expose();
    if (!expose_drawable)
        return;
    XCopyArea(display, static_cast<Pixmap>(expose_drawable), window,
              DefaultGC(display, screen), 0, 0,
              static_cast<unsigned int>(backbuffer_width),
              static_cast<unsigned int>(backbuffer_height), 0, 0);
    XFlush(display);
}

}

bool run_x11_window(PgnLibrary* library, const std::string& engine_path,
                    const std::string& piece_theme, bool piece_theme_explicit,
                    const std::string& board_theme,
                    const WindowGeometry& geometry,
                    bool print_window_id, const std::string* window_title,
                    const std::string& window_role,
                    bool fullscreen, bool iconic,
                    int argc, char* argv[], std::string& error)
{
    // Xlib's text converters need the user's character locale. Keep numeric
    // parsing in the C locale for PGN and engine output.
    std::setlocale(LC_CTYPE, "");
    diagnostics_origin_us = monotonic_microseconds();
    Display* display = XOpenDisplay(0);
    Window window = 0;
    Atom delete_window;
    Atom net_wm_pid;
    Atom net_frame_extents;
    Atom net_request_frame_extents;
    Atom stockfish_pid;
    Atom net_wm_state;
    Atom wm_state;
    bool transient_geometry = fullscreen || iconic;
    XEvent event;
    int width = 900;
    int height = 650;
    unsigned long window_background_pixel = 0;
    bool window_background_color_allocated = false;
    int window_x = 0;
    int window_y = 0;
    bool use_saved_position = false;
    bool startup_geometry_reported = false;
    WindowFrameExtents frame_extents;
    bool have_frame_extents = false;
    bool diagnostics = std::getenv("GAMBITREE_DIAGNOSTICS") != 0;
    SessionState session_state = default_session_state();
    SessionStateLoadStatus session_status = SESSION_STATE_ABSENT;
    std::string session_path;
    std::string selected_piece_set_id;
    bool running = true;
    bool flipped = false;
    LastMoveHighlight highlight;
    PgnDocument document;
    PgnGame placeholder;
    std::ostringstream demo_text;
    PgnGame* game = 0;
    std::string status;
    int scroll = 0;
    NotationLayout layout;
    Pixmap backbuffer = 0;
    int backbuffer_width = 0;
    int backbuffer_height = 0;
    bool dragging_sidebar = false;
    int sidebar_drag_offset = 0;
    bool resize_cursor_active = false;
    Cursor resize_cursor = 0;
    unsigned long configure_events_seen = 0;
    unsigned long configure_redraws = 0;
    unsigned long configure_events_coalesced = 0;
    DeferredPieceResize deferred_piece_resize(
        kPieceResizeSettleMilliseconds);

    if (!display) {
        const char* display_name = std::getenv("DISPLAY");
        error = "cannot open X display";
        if (display_name && *display_name) error += std::string(" '") + display_name + "'";
        return false;
    }
    SessionTerminationHandler termination_handler;
    try {
        session_path = session_state_path(std::getenv("XDG_STATE_HOME"),
                                          std::getenv("HOME"));
        if (!session_path.empty())
            session_status = load_session_state(session_path, session_state);
    } catch (const std::bad_alloc&) {
        session_state = default_session_state();
        session_path.clear();
        session_status = SESSION_STATE_INVALID;
    }
    if (diagnostics) {
        std::cerr << "gambitree: diagnostic: session state path="
                  << (session_path.empty() ? "unavailable" : session_path)
                  << " status=";
        if (session_status == SESSION_STATE_ABSENT)
            std::cerr << "absent";
        else if (session_status == SESSION_STATE_LOADED)
            std::cerr << "loaded";
        else if (session_status == SESSION_STATE_LOADED_WITH_INVALID_FIELDS)
            std::cerr << "loaded; invalid fields defaulted";
        else
            std::cerr << "ignored-invalid";
        std::cerr << " width=" << session_state.width
                  << " height=" << session_state.height;
        if (session_state.has_position)
            std::cerr << " x=" << session_state.x << " y=" << session_state.y;
        std::cerr << " analysis=" << (session_state.analysis_enabled ? 1 : 0)
                  << " pieces=" << session_state.piece_set_id
                  << "\n";
    }
    SessionStateSaver session_saver(session_path, session_state);
    selected_piece_set_id = initial_piece_set_id(
        piece_theme, piece_theme_explicit, session_state.piece_set_id);
    width = session_state.width;
    height = session_state.height;
    if (geometry.mask & WidthValue) width = static_cast<int>(geometry.width);
    if (geometry.mask & HeightValue) height = static_cast<int>(geometry.height);
    if (width > DisplayWidth(display, DefaultScreen(display)))
        width = DisplayWidth(display, DefaultScreen(display));
    if (height > DisplayHeight(display, DefaultScreen(display)))
        height = DisplayHeight(display, DefaultScreen(display));
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (session_state.has_position) {
        window_x = session_state.x;
        window_y = session_state.y;
        use_saved_position = true;
    } else if (diagnostics) {
        std::cerr << "gambitree: diagnostic: saved position unavailable\n";
    }
    if (!library || library->empty()) demo_text << "[Event \"Gambitree notation demo with a deliberately long wrapped title\"]\n"
              << "[Site \"Brussels, Belgium\"]\n[Date \"2026.09.19\"]\n[Round \"1\"]\n"
              << "[White \"Ada Lovelace\"]\n[Black \"Alan Turing\"]\n[Result \"*\"]\n\n"
              << "1. Nf3 Nf6 $1 {A long demo game makes wheel scrolling visible.} "
              << "(1... d5 $5 {First sibling variation} 2. Ng1 (2. e3 $6 {Nested variation}) d4) "
              << "(1... c5 {Second sibling variation}) ";
    if (!library || library->empty()) { int move_number; for (move_number = 2; move_number <= 52; ++move_number)
        demo_text << move_number << ". " << (move_number % 2 ? "Nf3 Nf6 " : "Ng1 Ng8 "); }
    if (!library || library->empty()) demo_text << "*";
    if ((!library || library->empty()) && (!PgnParser::parse(demo_text.str(), document) || document.games.empty())) {
        error = "cannot load built-in notation demo";
        XCloseDisplay(display);
        return false;
    }
    game = library && !library->empty() ? &placeholder : document.games[0];
    status = indexing_status(library);

    {
        XColor background;
        window_background_pixel = BlackPixel(
            display, DefaultScreen(display));
        background.red = static_cast<unsigned short>(35 * 257);
        background.green = static_cast<unsigned short>(39 * 257);
        background.blue = static_cast<unsigned short>(43 * 257);
        background.flags = DoRed | DoGreen | DoBlue;
        if (XAllocColor(display,
                        DefaultColormap(display, DefaultScreen(display)),
                        &background)) {
            window_background_pixel = background.pixel;
            window_background_color_allocated = true;
        }
    }
    window = XCreateSimpleWindow(display, RootWindow(display, DefaultScreen(display)),
                                 0, 0,
                                 static_cast<unsigned int>(width),
                                 static_cast<unsigned int>(height), 1,
                                 BlackPixel(display, DefaultScreen(display)),
                                 window_background_pixel);
    if (!window) {
        error = "cannot create X11 window";
        XCloseDisplay(display);
        return false;
    }

    {
        XClassHint class_hint;
        class_hint.res_name = const_cast<char*>(kResourceName);
        class_hint.res_class = const_cast<char*>(kResourceClass);
        XSetClassHint(display, window, &class_hint);
    }
    net_wm_pid = XInternAtom(display, "_NET_WM_PID", False);
    net_frame_extents = XInternAtom(display, "_NET_FRAME_EXTENTS", False);
    net_request_frame_extents = XInternAtom(
        display, "_NET_REQUEST_FRAME_EXTENTS", False);
    stockfish_pid = XInternAtom(display, "STOCKFISH_PID", False);
    set_cardinal_property(display, window, net_wm_pid, static_cast<unsigned long>(getpid()));
    XSetCommand(display, window, argv, argc);
    if (!window_role.empty())
        XChangeProperty(display, window, XInternAtom(display, "WM_WINDOW_ROLE", False),
                        XA_STRING, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(window_role.c_str()),
                        static_cast<int>(window_role.size()));
    set_client_machine(display, window);
    set_title(display, window, library, window_title);
    set_net_wm_icon(display, window);
    XSelectInput(display, window,
                 ExposureMask | StructureNotifyMask | PropertyChangeMask |
                 KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &delete_window, 1);
    net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    wm_state = XInternAtom(display, "WM_STATE", False);
    if (fullscreen) {
        const Atom state = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(display, window, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char*>(&state), 1);
    }
    if (iconic) {
        XWMHints hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.flags = StateHint;
        hints.initial_state = IconicState;
        XSetWMHints(display, window, &hints);
    }
    if (use_saved_position &&
        (geometry.mask & (XValue | YValue)) != (XValue | YValue)) {
        have_frame_extents = request_frame_extents(
            display, window, net_request_frame_extents, net_frame_extents,
            frame_extents);
        if (!can_restore_session_position(session_state,
                                         have_frame_extents)) {
            use_saved_position = false;
        } else if (!session_state.position_is_outer) {
            int outer_x;
            int outer_y;
            if (client_to_outer_position(session_state.x, session_state.y,
                                          frame_extents, outer_x, outer_y)) {
                window_x = outer_x;
                window_y = outer_y;
                session_state.x = outer_x;
                session_state.y = outer_y;
                session_state.position_is_outer = true;
            } else {
                use_saved_position = false;
            }
        }
        if (use_saved_position) {
            int frame_width = width;
            int frame_height = height;
            int original_x = window_x;
            int original_y = window_y;
            if (have_frame_extents) {
                frame_width += frame_extents.left + frame_extents.right;
                frame_height += frame_extents.top + frame_extents.bottom;
            }
            use_saved_position = keep_window_visible(
                window_x, window_y, frame_width, frame_height,
                0, 0, DisplayWidth(display, DefaultScreen(display)),
                DisplayHeight(display, DefaultScreen(display)));
            if (use_saved_position) {
                XMoveWindow(display, window, window_x, window_y);
                if (diagnostics) {
                    std::cerr << "gambitree: diagnostic: saved outer position="
                              << session_state.x << "," << session_state.y;
                    if (have_frame_extents)
                        std::cerr << " frame-extents=" << frame_extents.left
                                  << "," << frame_extents.right << ","
                                  << frame_extents.top << ","
                                  << frame_extents.bottom;
                    if (original_x != window_x || original_y != window_y)
                        std::cerr << " adjusted-to=" << window_x << ","
                                  << window_y;
                    std::cerr << "\n";
                }
            }
        }
        if (diagnostics && !use_saved_position)
            std::cerr << "gambitree: diagnostic: saved position ignored; "
                      << (have_frame_extents
                          ? "position is off-screen\n"
                          : "frame extents unavailable\n");
    }
    {
        XSizeHints hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.flags = (geometry.mask & (WidthValue | HeightValue)) ? USSize : PSize;
        if (geometry.mask & (XValue | YValue)) {
            int x, y, resolved_width, resolved_height;
            const std::string spec = window_geometry_spec(geometry, width, height);
            XWMGeometry(display, DefaultScreen(display), spec.c_str(), 0, 1, &hints,
                        &x, &y, &resolved_width, &resolved_height, &hints.win_gravity);
            const int max_x = std::max(0, DisplayWidth(display, DefaultScreen(display)) - width - 2);
            const int max_y = std::max(0, DisplayHeight(display, DefaultScreen(display)) - height - 2);
            if (geometry.mask & XValue)
                window_x = std::max(0, std::min(x, max_x));
            if (geometry.mask & YValue)
                window_y = std::max(0, std::min(y, max_y));
            XMoveWindow(display, window, window_x, window_y);
            use_saved_position = true;
            hints.flags |= USPosition | PWinGravity;
        }
        hints.width = width;
        hints.height = height;
        if (use_saved_position) {
            if (!(hints.flags & USPosition)) hints.flags |= PPosition;
            hints.x = window_x;
            hints.y = window_y;
        }
        XSetWMNormalHints(display, window, &hints);
    }
    if (diagnostics)
        std::cerr << "gambitree: diagnostic: startup geometry=" << width << "x"
                  << height << "+" << window_x << "+" << window_y
                  << ((geometry.mask & (XValue | YValue))
                      ? " (user gravity placement)\n" : " (outer placement)\n");
    {
        BoardRenderer renderer(display, DefaultScreen(display));
        X11NotationRenderer notation(display, DefaultScreen(display));
        renderer.set_board_theme(board_theme.empty() ? session_state.board_theme_id
                                                    : board_theme);
        notation.set_board_share(session_state.board_share);
        notation.set_figurines(session_state.figurines);
        notation.set_text_sizes(session_state.notation_points, session_state.engine_points);
        UciEngine engine(engine_path);
        EngineDisplay engine_display;
        EngineUiThrottle engine_throttle;
        bool analysis_enabled = session_state.analysis_enabled;
        bool initial_analysis_requested = false;
        bool normal_shutdown = false;
        bool session_error_reported = false;
        bool session_geometry_initialized = false;
        SessionGeometryChanges session_geometry_changes;
        bool engine_error_reported = false;
        DeleteDialogState delete_dialog;
        KeyboardHelpState keyboard_help;
        NavigationListState navigation_list;
        std::string navigation_list_title;
        notation.set_navigation_list(0, std::string(), 0);
        unsigned long piece_set_status_until = 0;
        unsigned long indexing_started = EngineUiThrottle::now_milliseconds();
        unsigned long indexing_last_present = 0;
        std::size_t indexing_slices = 0;
        std::size_t indexing_redraw_requests = 0;
        std::size_t indexing_presented = 0;
        bool indexing_reported = !library || library->indexing_complete();
        unsigned long expose_events_seen = 0;
        unsigned long wheel_events_seen = 0;
        unsigned long wheel_events_coalesced = 0;
        unsigned long wheel_batches = 0;
        unsigned long hq_work_units = 0;
        unsigned long hq_row_steps = 0;
        unsigned long hq_max_queued_events = 0;
        double hq_max_work_unit_ms = 0.0;
        uint64_t hq_job_started_us = 0;
        uint64_t hq_piece_started_us = 0;
        int hq_current_piece = 0;
        unsigned long hq_piece_steps = 0;
        VisitedGameStates visited_game_states;
        MoveNavigator* navigator = new MoveNavigator(*game);
        resize_cursor = XCreateFontCursor(display, XC_sb_h_double_arrow);
        if (!renderer.load_piece_set(selected_piece_set_id, error)) {
            std::string load_error = error;
            if (selected_piece_set_id != default_piece_set().id &&
                renderer.load_piece_set(default_piece_set().id, error)) {
                std::cerr << "gambitree: cannot load piece set '"
                          << selected_piece_set_id << "' (" << load_error
                          << "); using " << default_piece_set().display_name
                          << "\n";
                selected_piece_set_id = default_piece_set().id;
                session_state.piece_set_id = selected_piece_set_id;
                status = "Piece set unavailable; using Gambitree Classic";
                piece_set_status_until =
                    EngineUiThrottle::now_milliseconds() + 3500UL;
                error.clear();
            } else {
                running = false;
            }
        } else {
            selected_piece_set_id = renderer.piece_set_id();
            session_state.piece_set_id = selected_piece_set_id;
        }
        if (error.empty()) {
            std::string engine_error;
            begin_immediate_piece_rebuild(
                deferred_piece_resize, renderer, width, height,
                notation.sidebar_preferred_width(width));
            if (deferred_piece_resize.building()) {
                hq_job_started_us = monotonic_microseconds();
                hq_work_units = 0;
                hq_row_steps = 0;
                hq_max_queued_events = 0;
                hq_max_work_unit_ms = 0.0;
                hq_current_piece = 0;
                hq_piece_steps = 0;
                if (diagnostics)
                    std::cerr << "gambitree: diagnostic: HQ build begins "
                              << "immediately generation="
                              << deferred_piece_resize.generation()
                              << " target="
                              << deferred_piece_resize.target_square_size()
                              << " set=" << renderer.piece_set_id() << "\n";
            }
            XMapWindow(display, window);
            XFlush(display);
            if (print_window_id) { std::cout << "WINDOWID=" << static_cast<unsigned long>(window) << "\n"; std::cout.flush(); }
            if (!engine.start(engine_error)) { std::cerr << "gambitree: engine unavailable: " << engine_error << "\n"; engine_error_reported = true; }
            else {
                sync_stockfish_pid(display, window, stockfish_pid, engine);
                if (initial_analysis_request_due(analysis_enabled,
                                                 game != &placeholder,
                                                 initial_analysis_requested)) {
                    engine.request_analysis(navigator->position());
                    initial_analysis_requested = true;
                }
            }
        }
        while (running) {
            if (session_termination_requested) {
                normal_shutdown = true;
                break;
            }
            collect_session_preferences(session_state, width, height,
                analysis_enabled, notation, selected_piece_set_id, renderer);
            if (transient_geometry)
                session_saver.retain_saved_geometry(session_state);
            else if (!session_geometry_initialized && !geometry.mask)
                session_saver.adopt_initial_geometry(session_state);
            session_saver.observe(session_state, monotonic_milliseconds(),
                !transient_geometry && session_geometry_initialized && session_geometry_changes.pending);
            session_geometry_changes.pending = false;
            service_session_save(display, window, net_frame_extents, session_state,
                session_saver, session_path, session_error_reported, diagnostics);
            if (piece_set_status_until != 0 &&
                EngineUiThrottle::now_milliseconds() >=
                    piece_set_status_until) {
                if (status.find("Piece set:") == 0 ||
                    status.find("Piece set unavailable") == 0)
                    status.clear();
                piece_set_status_until = 0;
                redraw(display, DefaultScreen(display), window, width, height,
                       renderer, notation, *game, *navigator, layout, flipped,
                       highlight, scroll, backbuffer, backbuffer_width,
                       backbuffer_height, library, status, engine,
                       analysis_enabled, engine_display, keyboard_help, "piece-status-expire");
            }
            if (library && !library->indexing_complete() &&
                XPending(display) == 0 && !deferred_piece_resize.pending()) {
                std::vector<std::string> index_errors;
                unsigned long now = EngineUiThrottle::now_milliseconds();
                bool loaded_game = false;
                bool complete;
                ++indexing_slices;
                library->step_indexing_for_milliseconds(8, index_errors);
                for (std::size_t error_index = 0; error_index < index_errors.size(); ++error_index)
                    std::cerr << "gambitree: " << index_errors[error_index] << "\n";
                if (game == &placeholder && library->game_count() > 0 &&
                    load_selected_game(library, placeholder, game, navigator, scroll, status)) {
                    loaded_game = true;
                    if (initial_analysis_request_due(
                            analysis_enabled, true,
                            initial_analysis_requested)) {
                        engine.request_analysis(navigator->position());
                        initial_analysis_requested = true;
                    }
                }
                complete = library->indexing_complete();
                if (complete && status.find("Indexing") == 0) {
                    status = library->indexed_game_count() == 0 ? "No valid PGN games found" : "";
                }
                if (navigation_list.visible() &&
                    navigation_list.kind() == NAVIGATION_LIST_GAMES) {
                    std::string tag_error;
                    std::vector<std::string> items = game_navigation_items(
                        library, tag_error, notation.figurines(),
                        notation.metrics().figurine_mask(),
                        navigation_list.items().size());
                    if (!items.empty())
                        navigation_list.append_items(items,
                                                     library->game_index());
                    if (complete)
                        navigation_list_title = "Choose a game";
                    notation.set_navigation_list(
                        &navigation_list, navigation_list_title,
                        library->game_index());
                }
                if (status.empty()) status = indexing_status(library);
                ++indexing_redraw_requests;
                /* Keep UCI pipes drained even while the indexer owns the idle slices. */
                if (engine.poll()) sync_stockfish_pid(display, window, stockfish_pid, engine);
                if (loaded_game || complete || indexing_last_present == 0 || now - indexing_last_present >= 333UL) {
                    indexing_last_present = now;
                    ++indexing_presented;
                    engine_throttle.user_presented(now);
                    redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "indexing");
                }
                if (complete && !indexing_reported && std::getenv("GAMBITREE_DIAGNOSTICS")) {
                    std::cerr << "gambitree: diagnostic: indexing complete in "
                              << EngineUiThrottle::now_milliseconds() - indexing_started << " ms, "
                              << library->indexed_game_count() << " games, " << library->indexed_lines()
                              << " lines, " << library->indexing_chunks() << " index chunks, "
                              << indexing_slices << " idle slices, scan CPU " << library->indexing_scan_milliseconds()
                              << " ms, redraw requests " << indexing_redraw_requests << ", presented frames "
                              << indexing_presented << "\n";
                    indexing_reported = true;
                }
                continue;
            }
            if (XPending(display) == 0) {
                fd_set readable;
                struct timeval timeout;
                int x_fd = ConnectionNumber(display);
                int engine_fd = engine.output_fd();
                int largest = x_fd > engine_fd ? x_fd : engine_fd;
                uint64_t wait_milliseconds = 100;
                if (deferred_piece_resize.waiting_for_settle()) {
                    uint64_t remaining =
                        deferred_piece_resize.remaining_milliseconds(
                            monotonic_milliseconds());
                    if (remaining < wait_milliseconds)
                        wait_milliseconds = remaining;
                } else if (deferred_piece_resize.building() ||
                           deferred_piece_resize.ready_to_commit()) {
                    wait_milliseconds = 0;
                }
                wait_milliseconds = session_saver.remaining_milliseconds(
                    monotonic_milliseconds(), wait_milliseconds);
                FD_ZERO(&readable); FD_SET(x_fd, &readable);
                if (engine_fd >= 0) FD_SET(engine_fd, &readable);
                timeout.tv_sec = static_cast<time_t>(wait_milliseconds / 1000);
                timeout.tv_usec = static_cast<suseconds_t>(
                    (wait_milliseconds % 1000) * 1000);
                if (select(largest + 1, &readable, 0, 0, &timeout) >= 0 &&
                    ((engine_fd >= 0 && FD_ISSET(engine_fd, &readable)) || engine.child_pid() > 0)) {
                    EngineDisplayState before = engine_display.state();
                    if (engine.poll()) {
                        sync_stockfish_pid(display, window, stockfish_pid, engine);
                        bool changed = engine_display.update(engine, analysis_enabled);
                        bool urgent = before != engine_display.state();
                        if (engine.state() == UCI_UNAVAILABLE && !engine_error_reported) {
                            std::cerr << "gambitree: engine unavailable: process exited or could not start\n";
                            engine_error_reported = true;
                        }
                        if (changed && (urgent || engine_throttle.engine_update_due(EngineUiThrottle::now_milliseconds()))) {
                            engine_throttle.engine_presented(EngineUiThrottle::now_milliseconds());
                            redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "engine");
                        }
                    }
                }
                if (engine_throttle.pending() && engine_throttle.engine_update_due(EngineUiThrottle::now_milliseconds())) {
                    engine_throttle.engine_presented(EngineUiThrottle::now_milliseconds());
                    redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "engine-throttle");
                }
                if (XPending(display) == 0 &&
                    deferred_piece_resize.waiting_for_settle()) {
                    ResizeDeadlineResult resize_result =
                        service_piece_resize_deadline(
                            display, window, deferred_piece_resize, renderer,
                            width, height,
                            notation, session_geometry_changes,
                            configure_events_seen,
                            configure_events_coalesced);
                    if (resize_result != RESIZE_DEADLINE_NONE) {
                        engine_throttle.user_presented(
                            EngineUiThrottle::now_milliseconds());
                        if (resize_result == RESIZE_DEADLINE_UPDATED) {
                            ++configure_redraws;
                            ensure_selection_visible(*game, notation, layout,
                                                     *navigator, width, height,
                                                     scroll);
                            redraw(display, DefaultScreen(display), window,
                                   width, height, renderer, notation, *game,
                                   *navigator, layout, flipped, highlight,
                                   scroll, backbuffer, backbuffer_width,
                                   backbuffer_height, library, status, engine,
                                   analysis_enabled, engine_display, keyboard_help,
                                   "resize-latest-configure");
                        } else {
                            hq_job_started_us = monotonic_microseconds();
                            hq_work_units = 0;
                            hq_row_steps = 0;
                            hq_max_queued_events = 0;
                            hq_max_work_unit_ms = 0.0;
                            hq_current_piece = 0;
                            hq_piece_steps = 0;
                            if (diagnostics)
                                std::cerr << "gambitree: diagnostic: HQ "
                                          << "build begins generation="
                                          << deferred_piece_resize.generation()
                                          << " target="
                                          << deferred_piece_resize.target_square_size()
                                          << " deadline="
                                          << deferred_piece_resize.deadline_milliseconds()
                                          << " set=" << renderer.piece_set_id()
                                          << "\n";
                        }
                    }
                }
                if (XPending(display) == 0 &&
                    deferred_piece_resize.ready_to_commit()) {
                    unsigned long generation =
                        deferred_piece_resize.generation();
                    int target_size =
                        deferred_piece_resize.target_square_size();
                    if (renderer.commit_high_quality_rebuild(generation) &&
                        deferred_piece_resize.commit(
                            monotonic_milliseconds())) {
                        renderer.set_interactive_resize(false);
                        if (diagnostics)
                            std::cerr << "gambitree: diagnostic: HQ build "
                                      << "committed generation=" << generation
                                      << " target=" << target_size
                                      << " unique-rasters=" << hq_work_units
                                      << " row-steps=" << hq_row_steps
                                      << " max-queued="
                                      << hq_max_queued_events
                                      << " total-ms="
                                      << (monotonic_microseconds() -
                                          hq_job_started_us) / 1000
                                      << " set=" << renderer.piece_set_id()
                                      << "\n";
                        engine_throttle.user_presented(
                            EngineUiThrottle::now_milliseconds());
                        ++configure_redraws;
                        redraw(display, DefaultScreen(display), window, width,
                               height, renderer, notation, *game, *navigator,
                               layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library,
                               status, engine, analysis_enabled, engine_display, keyboard_help,
                               "hq-piece-commit");
                    } else {
                        if (diagnostics)
                            std::cerr << "gambitree: diagnostic: HQ commit "
                                      << "rejected generation=" << generation
                                      << " target=" << target_size << "\n";
                        deferred_piece_resize.cancel();
                        renderer.cancel_high_quality_rebuild();
                    }
                }
                std::size_t pending_before_build =
                    static_cast<std::size_t>(XPending(display));
                if (pending_before_build == 0 &&
                    deferred_piece_resize.building()) {
                    int piece = 0;
                    double work_ms = 0.0;
                    uint64_t work_started = monotonic_microseconds();
                    HighQualityPieceWorkResult work_result =
                        renderer.build_next_high_quality_piece(
                            deferred_piece_resize.generation(), piece,
                            work_ms);
                    if (hq_current_piece != piece) {
                        hq_current_piece = piece;
                        hq_piece_started_us = work_started;
                        hq_piece_steps = 0;
                    }
                    ++hq_piece_steps;
                    std::size_t pending_after =
                        static_cast<std::size_t>(XPending(display));
                    ++hq_row_steps;
                    if (pending_after > hq_max_queued_events)
                        hq_max_queued_events =
                            static_cast<unsigned long>(pending_after);
                    if (work_result != HQ_PIECE_WORK_FAILED) {
                        double elapsed = static_cast<double>(
                            monotonic_microseconds() - work_started) / 1000.0;
                        if (elapsed > hq_max_work_unit_ms)
                            hq_max_work_unit_ms = elapsed;
                        deferred_piece_resize.finish_work_unit(
                            renderer.high_quality_rebuild_ready(
                                deferred_piece_resize.generation()));
                        if (work_result == HQ_PIECE_WORK_COMPLETE) {
                            ++hq_work_units;
                            if (diagnostics) {
                                std::cerr << "gambitree: diagnostic: HQ piece "
                                          << hq_work_units << "/12 id="
                                          << piece << " scale-ms=" << work_ms
                                          << " total-ms="
                                          << (monotonic_microseconds() -
                                              hq_piece_started_us) / 1000
                                          << " row-steps=" << hq_piece_steps
                                          << " job-row-steps=" << hq_row_steps
                                          << " candidate-pieces="
                                          << renderer.candidate_piece_count()
                                          << " last-step-ms=" << elapsed
                                          << " max-step-ms="
                                          << hq_max_work_unit_ms
                                          << " events-before="
                                          << pending_before_build
                                          << " events-after=" << pending_after
                                          << " max-queued="
                                          << hq_max_queued_events
                                          << " configure-coalesced="
                                          << configure_events_coalesced
                                          << " expose-seen="
                                          << expose_events_seen
                                          << " generation="
                                          << deferred_piece_resize.generation()
                                          << "\n";
                            }
                            hq_current_piece = 0;
                            hq_piece_steps = 0;
                        }
                    } else {
                        if (diagnostics)
                            std::cerr << "gambitree: diagnostic: HQ piece "
                                      << (hq_work_units + 1)
                                      << " failed; retaining complete cache\n";
                        deferred_piece_resize.cancel();
                        renderer.cancel_high_quality_rebuild();
                        /* Keep temporary rendering active: a later resize can
                           retry without forcing a synchronous rebuild. */
                    }
                }
                if (XPending(display) == 0) continue;
            }
            XNextEvent(display, &event);
            switch (event.type) {
            case Expose:
                if (delete_dialog.window &&
                    event.xexpose.window == delete_dialog.window) {
                    if (event.xexpose.count == 0)
                        draw_delete_dialog(display, notation, delete_dialog);
                    break;
                }
                ++expose_events_seen;
                if (event.xexpose.count == 0) {
                    engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                    bool current_frame_matches =
                        backbuffer && backbuffer_width == width &&
                        backbuffer_height == height && event.xexpose.x >= 0 &&
                        event.xexpose.y >= 0 &&
                        event.xexpose.x + event.xexpose.width <=
                            backbuffer_width &&
                        event.xexpose.y + event.xexpose.height <=
                            backbuffer_height;
                    if (!current_frame_matches) {
                        XWindowAttributes attributes;
                        if (XGetWindowAttributes(display, window,
                                                 &attributes)) {
                            bool geometry_changed = width != attributes.width || height != attributes.height;
                            width = attributes.width;
                            height = attributes.height;
                            if (geometry_changed)
                                ensure_selection_visible(*game, notation, layout,
                                                         *navigator, width, height, scroll);
                            schedule_piece_resize(
                                deferred_piece_resize, renderer, width,
                                height,
                                notation.sidebar_preferred_width(width));
                        }
                        ++configure_redraws;
                        redraw(display, DefaultScreen(display), window, width,
                               height, renderer, notation, *game, *navigator,
                               layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library,
                               status, engine, analysis_enabled, engine_display, keyboard_help,
                               "expose-frame-mismatch");
                    } else {
                        present_backbuffer(display, DefaultScreen(display),
                                           window, backbuffer,
                                           backbuffer_width,
                                           backbuffer_height);
                    }
                }
                break;
            case PropertyNotify:
                if (event.xproperty.window == window &&
                    (event.xproperty.atom == net_wm_state || event.xproperty.atom == wm_state))
                    transient_geometry = transient_window_geometry(display, window);
                break;
            case MapNotify:
                if (event.xmap.window == window && !session_geometry_initialized) {
                    transient_geometry = transient_window_geometry(display, window);
                    capture_session_geometry(display, window, net_frame_extents,
                                             session_state, &session_geometry_changes);
                    width = session_state.width; height = session_state.height;
                    if (transient_geometry)
                        session_saver.retain_saved_geometry(session_state);
                    else if (!geometry.mask)
                        session_saver.adopt_initial_geometry(session_state);
                    session_geometry_changes.pending = false;
                    session_geometry_initialized = true;
                }
                if (!startup_geometry_reported) {
                    report_window_geometry(
                        display, window,
                        RootWindow(display, DefaultScreen(display)),
                        net_frame_extents, window_x, window_y, diagnostics);
                    startup_geometry_reported = true;
                }
                break;
            case ConfigureNotify:
            {
                if (delete_dialog.window &&
                    event.xconfigure.window == delete_dialog.window)
                    break;
                ConfigureEventBatch batch;
                XEvent queued_event;
                unsigned long old_generation =
                    deferred_piece_resize.generation();
                session_geometry_changes.include(event.xconfigure);
                batch.include(event.xconfigure.width, event.xconfigure.height);
                while (XCheckTypedWindowEvent(display, window, ConfigureNotify,
                                              &queued_event)) {
                    session_geometry_changes.include(queued_event.xconfigure);
                    batch.include(queued_event.xconfigure.width,
                                  queued_event.xconfigure.height);
                }
                width = batch.width();
                height = batch.height();
                if (delete_dialog.window)
                    position_delete_dialog(display, window, notation,
                                           delete_dialog, width, height);
                if (delete_dialog.window)
                    draw_delete_dialog(display, notation, delete_dialog);
                configure_events_seen +=
                    static_cast<unsigned long>(batch.event_count());
                configure_events_coalesced +=
                    static_cast<unsigned long>(batch.event_count() - 1);
                ++configure_redraws;
                schedule_piece_resize(
                    deferred_piece_resize, renderer, width, height,
                    notation.sidebar_preferred_width(width));
                if (deferred_piece_resize.generation() != old_generation) {
                    hq_current_piece = 0;
                    hq_piece_steps = 0;
                }
                if (!startup_geometry_reported) {
                    report_window_geometry(
                        display, window,
                        RootWindow(display, DefaultScreen(display)),
                        net_frame_extents, window_x, window_y, diagnostics);
                    startup_geometry_reported = true;
                }
                ensure_selection_visible(*game, notation, layout, *navigator,
                                         width, height, scroll);
                engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "resize");
                break;
            }
            case KeyPress: {
                if (delete_dialog.window) {
                        KeySym symbol = XLookupKeysym(&event.xkey, 0);
                        DeleteDialogKey dialog_key = DELETE_DIALOG_KEY_OTHER;
                        DeleteDialogAction action;
                        if (symbol == XK_Escape || symbol == XK_n ||
                            symbol == XK_N)
                            dialog_key = DELETE_DIALOG_KEY_ESCAPE;
                        else if (symbol == XK_Tab || symbol == XK_Left ||
                                 symbol == XK_Right)
                            dialog_key = DELETE_DIALOG_KEY_TAB;
                        else if (symbol == XK_Return || symbol == XK_KP_Enter)
                            dialog_key = DELETE_DIALOG_KEY_ENTER;
                        else if (symbol == XK_y || symbol == XK_Y)
                            dialog_key = DELETE_DIALOG_KEY_CONFIRM;
                        action = handle_delete_dialog_key(
                            delete_dialog.input, dialog_key,
                            (event.xkey.state & ShiftMask) != 0);
                        if (dialog_key == DELETE_DIALOG_KEY_TAB)
                            draw_delete_dialog(display, notation, delete_dialog);
                        if (action == DELETE_DIALOG_CANCEL) {
                            close_delete_dialog(display, window,
                                                delete_dialog);
                        } else if (action == DELETE_DIALOG_CONFIRM) {
                            Position previous_position = navigator->position();
                            uint64_t deleted_file_identity = library &&
                                library->current_file()
                                ? library->current_file()->session_identity : 0;
                            std::size_t deleted_game_index = library
                                ? library->game_index() : 0;
                            bool committed = false;
                            bool file_removed = false;
                            bool sync_warning = false;
                            std::string delete_error;
                            engine.stop();
                            engine_display.clear();
                            remember_current_game_navigation(
                                visited_game_states, library, game,
                                *navigator);
                            if (library && library->delete_current_game(
                                    committed, file_removed, sync_warning,
                                    delete_error) && committed) {
                                if (file_removed || !library->current_file() ||
                                    library->current_file()->session_identity !=
                                        deleted_file_identity)
                                    visited_game_states.erase_file(
                                        deleted_file_identity);
                                else
                                    visited_game_states
                                        .erase_deleted_game_and_shift(
                                            deleted_file_identity,
                                            deleted_game_index);
                                delete navigator;
                                navigator = 0;
                                game = &placeholder;
                                scroll = 0;
                                if (library->game_count() > 0 &&
                                    library->current_file()) {
                                    if (!load_selected_game(
                                            library, placeholder, game,
                                            navigator, scroll, status)) {
                                        game = &placeholder;
                                        navigator = new MoveNavigator(placeholder);
                                        status = "Game removed; remaining game could not be loaded";
                                    } else {
                                        restore_current_game_navigation(
                                            visited_game_states, library,
                                            game, *navigator, notation, layout,
                                            width, height, scroll, diagnostics);
                                    }
                                } else {
                                    navigator = new MoveNavigator(placeholder);
                                    status = "No games remain";
                                }
                                set_title(display, window, library, window_title);
                                if (analysis_enabled && game != &placeholder)
                                    engine.request_analysis(navigator->position());
                                if (!delete_error.empty()) {
                                    std::vector<std::string> lines;
                                    lines.push_back(file_removed
                                        ? "The PGN file was removed."
                                        : "The game was removed from the PGN file.");
                                    lines.push_back(delete_error);
                                    open_delete_dialog(display, window, notation,
                                        width, height, delete_dialog, true, lines);
                                } else {
                                    close_delete_dialog(display, window,
                                                        delete_dialog);
                                }
                            } else {
                                if (analysis_enabled && game != &placeholder)
                                    engine.request_analysis(previous_position);
                                std::vector<std::string> lines;
                                lines.push_back(delete_error.empty()
                                    ? "The file could not be safely changed."
                                    : delete_error);
                                lines.push_back("No game was removed.");
                                open_delete_dialog(display, window, notation,
                                    width, height, delete_dialog, true, lines);
                            }
                            redraw(display, DefaultScreen(display), window,
                                   width, height, renderer, notation, *game,
                                   *navigator, layout, flipped, highlight,
                                   scroll, backbuffer, backbuffer_width,
                                   backbuffer_height, library, status, engine,
                                   analysis_enabled, engine_display, keyboard_help,
                                   "delete-game");
                        }
                    break;
                }
                // Confirmation is handled first above. Help never replaces it.
                char entered[16];
                KeySym translated = NoSymbol;
                int entered_length = XLookupString(&event.xkey, entered, sizeof(entered), &translated, 0);
                KeySym key = XLookupKeysym(&event.xkey, 0);
                const bool control_key =
                    (event.xkey.state & ControlMask) != 0;
                const bool shift_key =
                    (event.xkey.state & ShiftMask) != 0;
                if (navigation_list.visible()) {
                    bool activate = false;
                    if (key == XK_Escape) {
                        navigation_list.close();
                        notation.set_navigation_list(0, std::string(), 0);
                    } else if (key == XK_Up) {
                        navigation_list.move(-1);
                    } else if (key == XK_Down) {
                        navigation_list.move(1);
                    } else if (key == XK_Page_Up) {
                        navigation_list.page(-1);
                    } else if (key == XK_Page_Down) {
                        navigation_list.page(1);
                    } else if (key == XK_Home) {
                        navigation_list.home();
                    } else if (key == XK_End) {
                        navigation_list.end();
                    } else if (key == XK_Return || key == XK_KP_Enter) {
                        activate = true;
                    } else if (key == XK_BackSpace || key == XK_Delete) {
                        navigation_list.backspace_query();
                    } else if (entered_length > 0 &&
                               !(event.xkey.state & (ControlMask | Mod1Mask))) {
                        std::string text;
                        append_search_input(text, entered, entered_length);
                        navigation_list.append_query(text);
                    }
                    if (activate) {
                        const std::string previous_status = status;
                        if (activate_navigation_list(
                                navigation_list, library, placeholder, game,
                                navigator, scroll, visited_game_states,
                                notation, layout, width, height, diagnostics,
                                engine, analysis_enabled, engine_display,
                                display, window, window_title, status)) {
                            navigation_list.close();
                            notation.set_navigation_list(0, std::string(), 0);
                        } else if (status != previous_status) {
                            navigation_list.close();
                            notation.set_navigation_list(0, std::string(), 0);
                        }
                    }
                    navigation_list.set_visible_rows(
                        notation.navigation_list_visible_rows(width, height));
                    engine_throttle.user_presented(
                        EngineUiThrottle::now_milliseconds());
                    redraw(display, DefaultScreen(display), window, width,
                           height, renderer, notation, *game, *navigator,
                           layout, flipped, highlight, scroll, backbuffer,
                           backbuffer_width, backbuffer_height, library, status,
                           engine, analysis_enabled, engine_display,
                           keyboard_help, "navigation-list-key");
                    break;
                }
                const NavigationListShortcut list_shortcut =
                    navigation_list_shortcut(key, event.xkey.state);
                if (list_shortcut != NAVIGATION_LIST_SHORTCUT_NONE &&
                    !keyboard_help.visible && !notation.search().visible() &&
                    !delete_dialog.window && library) {
                    std::vector<std::string> items;
                    std::size_t current = 0;
                    if (list_shortcut == NAVIGATION_LIST_SHORTCUT_FILES) {
                        items = library->requested_paths();
                        current = requested_file_index(library);
                        navigation_list_title = "Choose a PGN file";
                    } else {
                        std::string tag_error;
                        items = game_navigation_items(
                            library, tag_error, notation.figurines(),
                            notation.metrics().figurine_mask());
                        current = library->game_index();
                        navigation_list_title = "Choose a game";
                        if (!library->indexing_complete())
                            navigation_list_title += " — indexing";
                    }
                    navigation_list.open(
                        list_shortcut == NAVIGATION_LIST_SHORTCUT_FILES
                            ? NAVIGATION_LIST_FILES : NAVIGATION_LIST_GAMES,
                                         items, current);
                    navigation_list.set_visible_rows(
                        notation.navigation_list_visible_rows(width, height));
                    notation.set_navigation_list(
                        &navigation_list, navigation_list_title,
                        current);
                    engine_throttle.user_presented(
                        EngineUiThrottle::now_milliseconds());
                    redraw(display, DefaultScreen(display), window, width,
                           height, renderer, notation, *game, *navigator,
                           layout, flipped, highlight, scroll, backbuffer,
                           backbuffer_width, backbuffer_height, library, status,
                           engine, analysis_enabled, engine_display,
                           keyboard_help, "navigation-list-open");
                    break;
                }
                bool question = keyboard_help_requested(translated, entered, entered_length);
                if (question || keyboard_help.visible) {
                    KeyboardHelpLayout help_layout = notation.keyboard_help_layout(width, height);
                    bool was_help_visible = keyboard_help.visible;
                    if (keyboard_help.key(translated, question, false, help_layout.line_height,
                                          help_layout.viewport_height(), help_layout.max_scroll())) {
                        if (!was_help_visible && keyboard_help.visible) {
                            dragging_sidebar = false;
                            XUndefineCursor(display, window);
                            resize_cursor_active = false;
                        }
                        engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                        redraw(display, DefaultScreen(display), window, width, height, renderer, notation,
                               *game, *navigator, layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library, status, engine,
                               analysis_enabled, engine_display, keyboard_help, "keyboard");
                        break;
                    }
                }
                if (control_key && (key == XK_f || key == XK_F)) {
                    notation.search().refresh(layout);
                    notation.search().open();
                    engine_throttle.user_presented(
                        EngineUiThrottle::now_milliseconds());
                    redraw(display, DefaultScreen(display), window, width,
                           height, renderer, notation, *game, *navigator,
                           layout, flipped, highlight, scroll, backbuffer,
                           backbuffer_width, backbuffer_height, library, status,
                           engine, analysis_enabled, engine_display,
                           keyboard_help, "search-open");
                    break;
                }
                const bool search_next_key = key == XK_Return ||
                    key == XK_KP_Enter || key == XK_F3;
                const bool search_previous_key =
                    (key == XK_Return || key == XK_KP_Enter || key == XK_F3) &&
                    shift_key;
                if (notation.search().visible()) {
                    NotationSearch& search = notation.search();
                    if (key == XK_Escape) {
                        search.close();
                    } else if (search_next_key || search_previous_key) {
                        search.refresh(layout);
                        search.move(search_previous_key ? -1 : 1);
                        scroll = search.scroll_to_current(
                            layout, scroll,
                            notation.notation_viewport_height(width, height,
                                                              *game));
                    } else if (control_key && (key == XK_a || key == XK_A)) {
                        search.set_select_all(true);
                    } else if (key == XK_BackSpace || key == XK_Delete) {
                        std::string updated = search.query();
                        if (search.select_all()) updated.clear();
                        else if (!updated.empty()) {
                            std::size_t last = updated.size() - 1;
                            while (last > 0 &&
                                   (static_cast<unsigned char>(updated[last]) &
                                    0xc0U) == 0x80U) --last;
                            updated.erase(last);
                        }
                        search.set_query(updated, layout, scroll,
                            notation.notation_viewport_height(width, height,
                                                              *game));
                        scroll = search.scroll_to_current(
                            layout, scroll,
                            notation.notation_viewport_height(width, height,
                                                              *game));
                    } else if (entered_length > 0 &&
                               !(event.xkey.state & (ControlMask | Mod1Mask))) {
                        std::string updated = search.select_all()
                            ? std::string() : search.query();
                        append_search_input(updated, entered, entered_length);
                        search.set_query(updated, layout, scroll,
                            notation.notation_viewport_height(width, height,
                                                              *game));
                        scroll = search.scroll_to_current(
                            layout, scroll,
                            notation.notation_viewport_height(width, height,
                                                              *game));
                    }
                    engine_throttle.user_presented(
                        EngineUiThrottle::now_milliseconds());
                    redraw(display, DefaultScreen(display), window, width,
                           height, renderer, notation, *game, *navigator,
                           layout, flipped, highlight, scroll, backbuffer,
                           backbuffer_width, backbuffer_height, library, status,
                           engine, analysis_enabled, engine_display,
                           keyboard_help, "search-input");
                    break;
                }
                if ((key == XK_F3) && !notation.search().query().empty()) {
                    NotationSearch& search = notation.search();
                    search.refresh(layout);
                    search.move(shift_key ? -1 : 1);
                    scroll = search.scroll_to_current(
                        layout, scroll,
                        notation.notation_viewport_height(width, height,
                                                          *game));
                    redraw(display, DefaultScreen(display), window, width,
                           height, renderer, notation, *game, *navigator,
                           layout, flipped, highlight, scroll, backbuffer,
                           backbuffer_width, backbuffer_height, library, status,
                           engine, analysis_enabled, engine_display,
                           keyboard_help, "search-next");
                    break;
                }
                bool navigated = false;
                bool keep_selection_visible = false;
                bool game_changed = false;
                if (pgn_navigation_key(key, event.xkey.state)) engine_display.end_preview();
                if (key == XK_Escape || key == XK_q || key == XK_Q) {
                    normal_shutdown = true;
                    running = false;
                }
                if (key == XK_Delete &&
                    (event.xkey.state & ControlMask) != 0) {
                    std::string reason;
                    if (!library || game == &placeholder ||
                        !library->current_game_deletion_eligible(reason)) {
                        std::vector<std::string> lines;
                        lines.push_back(reason.empty()
                            ? "No PGN game is selected." : reason);
                        open_delete_dialog(display, window, notation, width,
                                          height, delete_dialog, true, lines);
                    } else {
                        std::string verify_error;
                        const PgnFile* source = library->current_file();
                        if (!source || !pgn_delete_baseline_matches(
                                source->path, source->delete_baseline,
                                verify_error)) {
                            std::vector<std::string> lines;
                            lines.push_back(verify_error.empty()
                                ? "The source file cannot be verified."
                                : verify_error);
                            open_delete_dialog(display, window, notation,
                                              width, height, delete_dialog,
                                              true, lines);
                        } else {
                            std::vector<std::string> lines;
                            const PgnGame* selected = library->current_game();
                            std::ostringstream number;
                            number << "Game " << library->game_index() + 1
                                   << " of " << library->game_count();
                            lines.push_back("File: " + basename(source->path));
                            lines.push_back(number.str());
                            lines.push_back(format_pgn_player(
                                "White",
                                selected && selected->tag_value("White")
                                    ? *selected->tag_value("White") : "",
                                selected && selected->tag_value("WhiteElo")
                                    ? *selected->tag_value("WhiteElo") : ""));
                            lines.push_back(format_pgn_player(
                                "Black",
                                selected && selected->tag_value("Black")
                                    ? *selected->tag_value("Black") : "",
                                selected && selected->tag_value("BlackElo")
                                    ? *selected->tag_value("BlackElo") : ""));
                            if (selected && selected->tag_value("Event"))
                                lines.push_back("Event: " +
                                    *selected->tag_value("Event"));
                            if (selected && selected->tag_value("Date")) {
                                std::string date = format_pgn_game_date(
                                    *selected->tag_value("Date"));
                                if (!date.empty())
                                    lines.push_back("Date: " + date);
                            }
                            lines.push_back("This game will be permanently removed from disk.");
                            lines.push_back("This cannot be undone.");
                            open_delete_dialog(display, window, notation,
                                              width, height, delete_dialog,
                                              false, lines);
                        }
                    }
                    break;
                }
                if (key == XK_a || key == XK_A) {
                    analysis_enabled = !analysis_enabled;
                    if (analysis_enabled && game != &placeholder)
                        engine.request_analysis(navigator->position());
                    else { engine.stop(); engine_display.clear(); }
                }
                bool larger_text = key == XK_equal || key == XK_plus || key == XK_KP_Add;
                bool smaller_text = key == XK_minus || key == XK_KP_Subtract;
                if (larger_text || smaller_text) {
                    bool engine_text = (event.xkey.state & ControlMask) != 0;
                    int points = engine_text ? notation.engine_points() : notation.notation_points();
                    points += larger_text ? 1 : -1;
                    if (points < 8) points = 8;
                    if (points > 24) points = 24;
                    notation.set_text_sizes(engine_text ? notation.notation_points() : points,
                                            engine_text ? points : notation.engine_points());
                    keep_selection_visible = true;
                }
                if (key == XK_g || key == XK_G) {
                    /* This reflows presentation only. Keep the user's read
                       offset; redraw will clamp it to the new content bounds. */
                    notation.set_figurines(!notation.figurines());
                }
                if (key == XK_f || key == XK_F) {
                    flipped = !flipped;
                }
                if (key == XK_p || key == XK_P) {
                    deferred_piece_resize.cancel();
                    renderer.cancel_high_quality_rebuild();
                    renderer.set_interactive_resize(false);
                    std::string next_id = next_piece_set_id(
                        selected_piece_set_id);
                    std::string switch_error;
                    if (renderer.load_piece_set(next_id, switch_error)) {
                        selected_piece_set_id = renderer.piece_set_id();
                        session_state.piece_set_id = selected_piece_set_id;
                        const PieceSetDescriptor* selected =
                            find_piece_set(selected_piece_set_id);
                        status = std::string("Piece set: ") +
                            (selected ? selected->display_name
                                      : selected_piece_set_id);
                    } else {
                        status = std::string("Piece set unavailable: ") +
                                 next_id;
                        std::cerr << "gambitree: cannot switch to piece set '"
                                  << next_id << "': " << switch_error << "\n";
                    }
                    begin_immediate_piece_rebuild(
                        deferred_piece_resize, renderer, width, height,
                        notation.sidebar_preferred_width(width));
                    if (deferred_piece_resize.building()) {
                        hq_job_started_us = monotonic_microseconds();
                        hq_work_units = 0;
                        hq_row_steps = 0;
                        hq_max_queued_events = 0;
                        hq_max_work_unit_ms = 0.0;
                        hq_current_piece = 0;
                        hq_piece_steps = 0;
                        if (diagnostics)
                            std::cerr << "gambitree: diagnostic: HQ build "
                                      << "begins immediately generation="
                                      << deferred_piece_resize.generation()
                                      << " target="
                                      << deferred_piece_resize
                                             .target_square_size()
                                      << " set=" << renderer.piece_set_id()
                                      << "\n";
                    }
                    piece_set_status_until =
                        EngineUiThrottle::now_milliseconds() + 3000UL;
                }
                if (key == XK_b || key == XK_B) {
                    const std::string next_theme =
                        next_board_theme_id(renderer.board_theme_id());
                    if (renderer.set_board_theme(next_theme)) {
                        session_state.board_theme_id = next_theme;
                        status = std::string("Board: ") +
                            board_theme_name(next_theme);
                    }
                }
                if (key == XK_Left) navigated = navigator->back();
                if (key == XK_Right) {
                    navigated = navigator->forward();
                    keep_selection_visible = true;
                }
                if (key == XK_Home && !navigator->at_root()) { navigator->to_begin(); navigated = true; }
                if (key == XK_End) {
                    const MoveNode* before = navigator->current();
                    navigator->to_end();
                    navigated = navigator->current() != before;
                    keep_selection_visible = true;
                }
                if (key == XK_Up) navigated = navigator->leave_variation();
                if (key == XK_Down) {
                    std::vector<const MoveNode*> notation_order;
                    const std::vector<NotationItem>& items = layout.items();
                    std::size_t item_index;
                    for (item_index = 0; item_index < items.size();
                         ++item_index) {
                        const NotationItem& item = items[item_index];
                        if (item.kind == NOTATION_MOVE && item.node &&
                            (notation_order.empty() ||
                             notation_order.back() != item.node))
                            notation_order.push_back(item.node);
                    }
                    navigated = navigator->next_after_variation(
                        notation_order);
                }
                KeyboardNavigationCommand command = keyboard_navigation_command(key, event.xkey.state);
                if (command == KEYBOARD_SCROLL_PAGE_UP ||
                    command == KEYBOARD_SCROLL_PAGE_DOWN) {
                    int viewport = notation.notation_viewport_height(
                        width, height, *game);
                    int line = notation.metrics().line_height();
                    int maximum = layout.max_scroll(viewport);
                    scroll = notation_page_scroll_offset(
                        scroll, viewport, line, maximum,
                        command == KEYBOARD_SCROLL_PAGE_DOWN);
                }
                if (library) {
                    if (command == KEYBOARD_PREVIOUS_GAME ||
                        command == KEYBOARD_NEXT_GAME ||
                        command == KEYBOARD_PREVIOUS_FILE ||
                        command == KEYBOARD_NEXT_FILE)
                        remember_current_game_navigation(
                            visited_game_states, library, game, *navigator);
                    if (command == KEYBOARD_PREVIOUS_GAME) game_changed = library->previous_game();
                    if (command == KEYBOARD_NEXT_GAME) game_changed = library->next_game();
                    if (command == KEYBOARD_PREVIOUS_FILE) game_changed = library->previous_file();
                    if (command == KEYBOARD_NEXT_FILE) game_changed = library->next_file();
                }
                if (game_changed) {
                    if (analysis_enabled) engine.stop();
                    engine_display.clear();
                    if (load_selected_game(library, placeholder, game,
                                           navigator, scroll, status)) {
                        restore_current_game_navigation(
                            visited_game_states, library, game, *navigator,
                            notation, layout, width, height, scroll,
                            diagnostics);
                        if (analysis_enabled)
                            engine.request_analysis(navigator->position());
                    }
                    set_title(display, window, library, window_title);
                }
                if (navigated) ensure_selection_visible(*game, notation, layout, *navigator, width, height, scroll);
                if (keep_selection_visible && !navigated)
                    ensure_selection_visible(*game, notation, layout,
                                             *navigator, width, height, scroll);
                if (navigated)
                    remember_current_game_navigation(
                        visited_game_states, library, game, *navigator);
                if (navigated && analysis_enabled) engine.request_analysis(navigator->position());
                engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "keyboard");
                break;
            }
            case ButtonPress: {
                if (delete_dialog.window) {
                    if (event.xbutton.window == delete_dialog.window &&
                        event.xbutton.button == Button1) {
                        int button = notation.delete_dialog_button_at(
                            delete_dialog.error, delete_dialog.width,
                            delete_dialog.height, event.xbutton.x,
                            event.xbutton.y);
                        if (button == 0 || (button == 1 &&
                                            delete_dialog.error)) {
                            close_delete_dialog(display, window,
                                                delete_dialog);
                        } else if (button == 1) {
                            XEvent activate;
                            delete_dialog.input.focused_button = 1;
                            XSetInputFocus(display, delete_dialog.window,
                                           RevertToParent, CurrentTime);
                            std::memset(&activate, 0, sizeof(activate));
                            activate.xkey.type = KeyPress;
                            activate.xkey.display = display;
                            activate.xkey.window = delete_dialog.window;
                            activate.xkey.root = RootWindow(
                                display, DefaultScreen(display));
                            activate.xkey.time = CurrentTime;
                            activate.xkey.keycode = XKeysymToKeycode(
                                display, XK_Return);
                            XPutBackEvent(display, &activate);
                        }
                    }
                    break;
                }
                if (event.xbutton.window == window &&
                    (event.xbutton.button == Button4 ||
                     event.xbutton.button == Button5)) {
                    unsigned long batch_events = 0;
                    int steps = collect_contiguous_wheel_events(
                        display, window, event, batch_events);
                    ++wheel_batches;
                    wheel_events_seen += batch_events;
                    if (batch_events > 1)
                        wheel_events_coalesced += batch_events - 1;
                    if (navigation_list.visible()) {
                        long rows = static_cast<long>(steps) * 3L;
                        if (rows > 1000000L) rows = 1000000L;
                        if (rows < -1000000L) rows = -1000000L;
                        navigation_list.move(static_cast<int>(rows));
                        navigation_list.set_visible_rows(
                            notation.navigation_list_visible_rows(width,
                                                                  height));
                        engine_throttle.user_presented(
                            EngineUiThrottle::now_milliseconds());
                        redraw(display, DefaultScreen(display), window, width,
                               height, renderer, notation, *game, *navigator,
                               layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library,
                               status, engine, analysis_enabled, engine_display,
                               keyboard_help, "navigation-list-wheel");
                    } else if (keyboard_help.visible) {
                        KeyboardHelpLayout help_layout =
                            notation.keyboard_help_layout(width, height);
                        long distance = static_cast<long>(steps) * 3L *
                                        help_layout.line_height;
                        if (distance < -help_layout.max_scroll())
                            distance = -help_layout.max_scroll();
                        if (distance > help_layout.max_scroll())
                            distance = help_layout.max_scroll();
                        keyboard_help.scroll_by(static_cast<int>(distance),
                                                help_layout.max_scroll());
                        engine_throttle.user_presented(
                            EngineUiThrottle::now_milliseconds());
                        redraw(display, DefaultScreen(display), window, width,
                               height, renderer, notation, *game, *navigator,
                               layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library,
                               status, engine, analysis_enabled, engine_display,
                               keyboard_help, "help-wheel-batch");
                    } else {
                        int viewport = notation.notation_viewport_height(
                            width, height, *game);
                        long requested = static_cast<long>(scroll) +
                                         static_cast<long>(steps) * 36L;
                        int maximum = layout.max_scroll(viewport);
                        if (requested < 0) requested = 0;
                        if (requested > maximum) requested = maximum;
                        scroll = static_cast<int>(requested);
                        engine_throttle.user_presented(
                            EngineUiThrottle::now_milliseconds());
                        redraw(display, DefaultScreen(display), window, width,
                               height, renderer, notation, *game, *navigator,
                               layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library,
                               status, engine, analysis_enabled, engine_display,
                               keyboard_help, "wheel-batch");
                    }
                    if (diagnostics)
                        std::cerr << "gambitree: diagnostic: wheel_batch="
                                  << wheel_batches << " events=" << batch_events
                                  << " total_events=" << wheel_events_seen
                                  << " coalesced=" << wheel_events_coalesced
                                  << " steps=" << steps << " scroll=" << scroll
                                  << "\n";
                    break;
                }
                if (navigation_list.visible()) {
                    if (event.xbutton.button == Button1) {
                        const int row = notation.navigation_list_row_at(
                            width, height, event.xbutton.x, event.xbutton.y);
                        if (row >= 0 &&
                            navigation_list.select_match_at(
                                static_cast<std::size_t>(row))) {
                            const std::string previous_status = status;
                            if (activate_navigation_list(
                                    navigation_list, library, placeholder,
                                    game, navigator, scroll,
                                    visited_game_states, notation, layout,
                                    width, height, diagnostics, engine,
                                    analysis_enabled, engine_display, display,
                                    window, window_title, status) || status != previous_status) {
                                navigation_list.close();
                                notation.set_navigation_list(
                                    0, std::string(), 0);
                            }
                        }
                        engine_throttle.user_presented(
                            EngineUiThrottle::now_milliseconds());
                        redraw(display, DefaultScreen(display), window, width,
                               height, renderer, notation, *game, *navigator,
                               layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library,
                               status, engine, analysis_enabled, engine_display,
                               keyboard_help, "navigation-list-mouse");
                    }
                    break;
                }
                if (keyboard_help.visible) break;
                if (notation.search_bar_contains(width, height, *game,
                                                 event.xbutton.x,
                                                 event.xbutton.y))
                    break;
                SidebarGeometry sidebar = notation.sidebar_geometry(width);
                SidebarBoxes sections = notation.boxes(width, height, *game);
                int metadata_bottom = sections.notation_top;
                int viewport = notation.notation_viewport_height(
                    width, height, *game);
                bool game_changed = false;
                bool selected_move = false;
                layout.build(*game, notation.content_width(width), notation.metrics());
                if (event.xbutton.button == Button1) {
                    const int pv_move = notation.engine_pv_hit_test(event.xbutton.x, event.xbutton.y);
                    const UciAnalysis* shown = notation.engine_pv_snapshot();
                    if (pv_move >= 0 && shown && engine_display.select_preview(*shown,
                            static_cast<std::size_t>(pv_move), navigator->position(),
                            navigator->current(), engine.generation())) {
                        engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                        redraw(display, DefaultScreen(display), window, width, height, renderer, notation,
                               *game, *navigator, layout, flipped, highlight, scroll, backbuffer,
                               backbuffer_width, backbuffer_height, library, status, engine,
                               analysis_enabled, engine_display, keyboard_help, "analysis-click");
                        break;
                    }
                }
                if (event.xbutton.button == Button1 &&
                    event.xbutton.x >= sidebar.left - BoardLayout::sidebar_gutter() && event.xbutton.x < sidebar.left) {
                    dragging_sidebar = true;
                    sidebar_drag_offset = event.xbutton.x - sidebar.left;
                    schedule_piece_resize(
                        deferred_piece_resize, renderer, width, height,
                        notation.sidebar_preferred_width(width));
                    engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                    redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "divider-press");
                    break;
                }
                if (library && event.xbutton.y >= notation.controls_top() && event.xbutton.y < notation.controls_bottom() &&
                    event.xbutton.x >= sidebar.content_left && event.xbutton.x < sidebar.content_left + 4 * 31) {
                    int control = (event.xbutton.x - sidebar.content_left) / 31;
                    remember_current_game_navigation(
                        visited_game_states, library, game, *navigator);
                    if (control == 0) game_changed = library->previous_file();
                    if (control == 1) game_changed = library->next_file();
                    if (control == 2) game_changed = library->previous_game();
                    if (control == 3) game_changed = library->next_game();
                    if (game_changed) {
                        if (analysis_enabled) engine.stop();
                        engine_display.clear();
                        if (load_selected_game(library, placeholder, game,
                                               navigator, scroll, status)) {
                            restore_current_game_navigation(
                                visited_game_states, library, game,
                                *navigator, notation, layout, width, height,
                                scroll, diagnostics);
                            if (analysis_enabled)
                                engine.request_analysis(navigator->position());
                        }
                        set_title(display, window, library, window_title);
                    }
                }
                if (event.xbutton.button == 1 && event.xbutton.x >= sidebar.content_left &&
                    event.xbutton.x < sidebar.content_left + sidebar.content_width &&
                    event.xbutton.y >= metadata_bottom && event.xbutton.y < sections.notation_visible_bottom) {
                    const MoveNode* node = layout.hit_test_visible(event.xbutton.x - sidebar.content_left,
                        event.xbutton.y - metadata_bottom, scroll, viewport);
                    if (node) engine_display.end_preview();
                    if (node && navigator->select(node)) {
                        selected_move = true;
                        ensure_selection_visible(*game, notation, layout, *navigator, width, height, scroll);
                    }
                }
                if (selected_move && analysis_enabled) engine.request_analysis(navigator->position());
                if (selected_move)
                    remember_current_game_navigation(
                        visited_game_states, library, game, *navigator);
                engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "mouse");
                break;
            }
            case MotionNotify: {
                if (delete_dialog.window) {
                    if (event.xmotion.window == delete_dialog.window) {
                        int button = notation.delete_dialog_button_at(
                            delete_dialog.error, delete_dialog.width,
                            delete_dialog.height, event.xmotion.x,
                            event.xmotion.y);
                        if (button != delete_dialog.hovered_button) {
                            delete_dialog.hovered_button = button;
                            draw_delete_dialog(display, notation,
                                               delete_dialog);
                        }
                    }
                    break;
                }
                if (keyboard_help.visible || navigation_list.visible()) break;
                SidebarGeometry sidebar;
                while (XCheckTypedWindowEvent(display, window, MotionNotify, &event)) {}
                sidebar = notation.sidebar_geometry(width);
                if (dragging_sidebar) {
                    int requested_width = width - event.xmotion.x + sidebar_drag_offset;
                    if (requested_width < SidebarLayout::minimum_width())
                        requested_width = SidebarLayout::minimum_width();
                    notation.set_board_share(SidebarLayout::board_share(
                        width, SidebarLayout::clamp_width(width, requested_width)));
                    schedule_piece_resize(
                        deferred_piece_resize, renderer, width, height,
                        notation.sidebar_preferred_width(width));
                    ensure_selection_visible(*game, notation, layout, *navigator,
                                             width, height, scroll);
                    engine_throttle.user_presented(EngineUiThrottle::now_milliseconds());
                    redraw(display, DefaultScreen(display), window, width, height, renderer, notation, *game, *navigator, layout, flipped, highlight, scroll, backbuffer, backbuffer_width, backbuffer_height, library, status, engine, analysis_enabled, engine_display, keyboard_help, "divider-drag");
                } else {
                    bool hover = event.xmotion.x >= sidebar.left - BoardLayout::sidebar_gutter() && event.xmotion.x < sidebar.left;
                    if (hover != resize_cursor_active) {
                        if (hover && resize_cursor) XDefineCursor(display, window, resize_cursor);
                        else XUndefineCursor(display, window);
                        resize_cursor_active = hover;
                    }
                }
                break;
            }
            case ButtonRelease:
                if (event.xbutton.button == Button1) dragging_sidebar = false;
                break;
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == delete_window) {
                    normal_shutdown = true;
                    running = false;
                }
                break;
            case DestroyNotify:
                if (delete_dialog.closing_window &&
                    event.xdestroywindow.window ==
                        delete_dialog.closing_window) {
                    delete_dialog.closing_window = 0;
                    break;
                }
                if (delete_dialog.window &&
                    event.xdestroywindow.window == delete_dialog.window) {
                    delete_dialog.window = 0;
                    XSetInputFocus(display, window, RevertToParent,
                                   CurrentTime);
                    break;
                }
                window = 0;
                normal_shutdown = true;
                running = false;
                break;
            default:
                break;
            }
        }
        if (diagnostics)
            std::cerr << "gambitree: diagnostic: resize events="
                      << configure_events_seen << " redraws="
                      << configure_redraws << " coalesced="
                      << configure_events_coalesced
                      << " expose-events=" << expose_events_seen << "\n";
        if (diagnostics) {
            unsigned long completed =
                deferred_piece_resize.completed_targets();
            unsigned long settled = deferred_piece_resize.settled_targets();
            std::cerr << "gambitree: diagnostic: resize settle-ms="
                      << kPieceResizeSettleMilliseconds
                      << " deferred-targets="
                      << deferred_piece_resize.deferred_targets()
                      << " cancelled-targets="
                      << deferred_piece_resize.cancelled_targets()
                      << " completed-targets=" << completed
                      << " settle-average-ms="
                      << (settled
                          ? deferred_piece_resize.total_settle_milliseconds() /
                                settled
                          : 0)
                      << " settle-max-ms="
                      << deferred_piece_resize.maximum_settle_milliseconds()
                      << " temporary-draws="
                      << renderer.temporary_draw_count()
                      << " temporary-piece-builds="
                      << renderer.temporary_piece_build_count()
                      << " temporary-scaler-ms="
                      << renderer.temporary_resize_ms() << "\n";
        }
        if (diagnostics)
            std::cerr << "gambitree: diagnostic: HQ work-units="
                      << renderer.scaled_piece_build_count()
                                      << " last-job-work-units=" << hq_work_units
                      << " last-job-row-steps=" << hq_row_steps
                      << " last-job-max-queued=" << hq_max_queued_events
                      << " last-job-max-work-ms=" << hq_max_work_unit_ms
                      << " cancelled-candidates="
                      << deferred_piece_resize.cancelled_targets()
                      << " committed-candidates="
                      << deferred_piece_resize.completed_targets()
                      << " final-event-queue=" << XPending(display)
                      << "\n";
        if (diagnostics)
            std::cerr << "gambitree: diagnostic: scaled-cache rebuilds="
                      << renderer.scaled_cache_rebuild_count()
                      << " piece rasters="
                      << renderer.scaled_piece_build_count()
                      << " Lanczos horizontal-ms="
                      << renderer.resize_horizontal_ms()
                      << " vertical-ms=" << renderer.resize_vertical_ms()
                      << " total-ms=" << renderer.resize_total_ms()
                      << " temporary-ms=" << renderer.temporary_resize_ms()
                      << " alpha-bounds-ms=" << renderer.alpha_bounds_ms()
                      << " XGetImage-ms=" << renderer.ximage_get_ms()
                      << " composite-ms=" << renderer.ximage_composite_ms()
                      << " XPutImage-ms=" << renderer.ximage_put_ms()
                      << " set=" << renderer.piece_set_id() << "\n";
        if (normal_shutdown) {
            XEvent destroyed;
            if (window && XCheckTypedWindowEvent(display, window, DestroyNotify, &destroyed))
                window = 0;
            collect_session_preferences(session_state, width, height,
                analysis_enabled, notation, selected_piece_set_id, renderer);
            capture_session_geometry(display, window, net_frame_extents,
                                     session_state, &session_geometry_changes);
            if (window) transient_geometry = transient_window_geometry(display, window);
            if (transient_geometry)
                session_saver.retain_saved_geometry(session_state);
            else if (!session_geometry_initialized && !geometry.mask)
                session_saver.adopt_initial_geometry(session_state);
            session_saver.observe(session_state, monotonic_milliseconds(),
                !transient_geometry && session_geometry_initialized && session_geometry_changes.pending);
            service_session_save(display, window, net_frame_extents, session_state,
                session_saver, session_path, session_error_reported, diagnostics, true);
        }
        engine.shutdown();
        /* A window manager may have destroyed the window before we received
           DestroyNotify.  Do not issue a property request for XID zero. */
        if (window) XDeleteProperty(display, window, stockfish_pid);
        delete navigator;
        if (resize_cursor) XFreeCursor(display, resize_cursor);
    }

    if (window) XDestroyWindow(display, window);
    if (backbuffer) XFreePixmap(display, backbuffer);
    if (window_background_color_allocated) {
        XFreeColors(display,
                    DefaultColormap(display, DefaultScreen(display)),
                    &window_background_pixel, 1, 0);
    }
    XCloseDisplay(display);
    return error.empty();
}

}
