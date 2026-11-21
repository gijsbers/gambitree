#ifndef GAMBITREE_ENGINE_DISPLAY_H
#define GAMBITREE_ENGINE_DISPLAY_H

#include <string>

#include "gambitree/uci_engine.h"
#include "gambitree/notation_layout.h"

namespace gambitree {

/* One measured row; each item corresponds to the same-index UCI PV move. */
struct EnginePvLayout {
    std::string text;
    std::vector<NotationItem> moves;
    int hit_test(int x, int y) const;
};
EnginePvLayout layout_engine_pv(const UciAnalysis& analysis, bool pending, int width,
                               const NotationFontMetrics& metrics, bool figurines);

enum EngineDisplayState { ENGINE_DISPLAY_OFF, ENGINE_DISPLAY_UNAVAILABLE, ENGINE_DISPLAY_STARTING,
                          ENGINE_DISPLAY_READY, ENGINE_DISPLAY_ANALYZING, ENGINE_DISPLAY_PENDING,
                          ENGINE_DISPLAY_TERMINAL };

struct EngineDisplayGeometry {
    int depth_label_x;
    int depth_right_x;
    int score_x;
    int seldepth_x;
    int pv_x;
    int status_x;
    EngineDisplayGeometry();
};

class EngineDisplay {
public:
    EngineDisplay();
    bool update(const UciEngine& engine, bool enabled);
    bool update_snapshot(unsigned long generation, UciState engine_state, const std::string& engine_name,
                         const UciAnalysis& analysis, bool enabled);
    void clear();
    EngineDisplayState state() const;
    const std::string& name() const;
    const UciAnalysis* score_analysis() const;
    const UciAnalysis* pv_analysis() const;
    const UciAnalysis* detail_analysis() const;
    bool score_is_current() const;
    bool pv_is_current() const;
    std::string status() const;
    static EngineDisplayGeometry geometry(int left);
    bool select_preview(const UciAnalysis& shown, std::size_t index,
                        const Position& origin, const MoveNode* node,
                        unsigned long generation);
    void end_preview();
    void sync_position(const Position& origin, const MoveNode* node);
    int preview_index() const;
    const Position* preview_position() const;
    const Move* preview_move() const;
private:
    bool enabled_;
    bool have_current_;
    bool have_pending_;
    std::string name_;
    unsigned long observed_generation_;
    UciAnalysis current_analysis_;
    UciAnalysis pending_analysis_;
    EngineDisplayState state_;
    int preview_index_;
    UciAnalysis preview_analysis_;
    std::string preview_name_;
    Position preview_origin_, preview_position_;
    const MoveNode* preview_node_;
    Move preview_move_;
};

class EngineUiThrottle {
public:
    EngineUiThrottle();
    static unsigned long now_milliseconds();
    bool engine_update_due(unsigned long now);
    void engine_presented(unsigned long now);
    void user_presented(unsigned long now);
    bool pending() const;
private:
    unsigned long last_engine_present_;
    bool pending_;
};

}

#endif
