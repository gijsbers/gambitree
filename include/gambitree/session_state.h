#ifndef GAMBITREE_SESSION_STATE_H
#define GAMBITREE_SESSION_STATE_H

#include <string>
#include <stdint.h>

namespace gambitree {

struct SessionState {
    int width;
    int height;
    int x;
    int y;
    bool has_position;
    /* Version 2 stores the visible outer-frame origin; version 1 stored
       the client origin. */
    bool position_is_outer;
    bool analysis_enabled;
    int board_share; // Ten-thousandths of window width; zero means automatic.
    bool figurines;
    int notation_points;
    int engine_points;
    std::string piece_set_id;
    std::string board_theme_id;
};

struct WindowFrameExtents {
    int left;
    int right;
    int top;
    int bottom;
};

enum SessionStateLoadStatus {
    SESSION_STATE_ABSENT,
    SESSION_STATE_LOADED,
    SESSION_STATE_LOADED_WITH_INVALID_FIELDS,
    SESSION_STATE_INVALID
};

/* Independent appearance sizes in points; out-of-range values use 12. */
int validated_text_points(int points);
SessionState default_session_state();
std::string session_state_path(const char* xdg_state_home,
                               const char* home);
bool parse_session_state(const std::string& text, SessionState& state);
std::string serialize_session_state(const SessionState& state);
SessionStateLoadStatus load_session_state(const std::string& path,
                                          SessionState& state);
bool save_session_state(const std::string& path,
                        const SessionState& state);

enum SessionSaveResult {
    SESSION_SAVE_UNCHANGED,
    SESSION_SAVE_SAVED,
    SESSION_SAVE_BUSY,
    SESSION_SAVE_FAILED
};

/* The caller supplies monotonic milliseconds and settled geometry to save().
   Only local changes are merged into the latest on-disk state. */
class SessionStateSaver {
public:
    SessionStateSaver(const std::string& path, const SessionState& initial);
    void observe(const SessionState& state, uint64_t now,
                 bool position_hint_changed = false);
    void adopt_initial_geometry(const SessionState& state);
    void retain_saved_geometry(SessionState& state) const;
    bool pending() const;
    bool due(uint64_t now) const;
    uint64_t remaining_milliseconds(uint64_t now, uint64_t maximum) const;
    SessionSaveResult save(const SessionState& settled, uint64_t now,
                           bool immediately = false);
private:
    std::string path_;
    SessionState saved_;
    SessionState observed_;
    uint64_t deadline_;
    uint64_t retry_at_;
    bool position_pending_;
};
bool keep_window_visible(int& x, int& y, int width, int height,
                         int screen_x, int screen_y,
                         int screen_width, int screen_height);
bool client_to_outer_position(int client_x, int client_y,
                               const WindowFrameExtents& extents,
                               int& outer_x, int& outer_y);
bool outer_to_client_position(int outer_x, int outer_y,
                               const WindowFrameExtents& extents,
                               int& client_x, int& client_y);
bool can_restore_session_position(const SessionState& state,
                                  bool have_frame_extents);
bool initial_analysis_request_due(bool analysis_enabled,
                                  bool position_ready,
                                  bool request_already_sent);

}

#endif
