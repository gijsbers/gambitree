#include "gambitree/engine_display.h"

#include <sys/time.h>
#include <time.h>
#include <sstream>

namespace gambitree {

EnginePvLayout layout_engine_pv(const UciAnalysis& analysis, bool pending, int width,
                               const NotationFontMetrics& metrics, bool figurines)
{
    EnginePvLayout result;
    std::istringstream san(analysis.san_pv);
    std::string token;
    Color side = analysis.side_to_move;
    if (pending) result.text = "[pending]";
    while (san >> token) {
        if (!result.text.empty()) result.text += ' ';
        result.text += NotationLayout::format_san(token, figurines, metrics.figurine_mask());
        if (side == BLACK) result.text += '.';
        side = side == WHITE ? BLACK : WHITE;
    }
    result.text = NotationLayout::fit_tokens(result.text, width, metrics, true);
    std::size_t start = pending ? std::string("[pending]").size() : 0;
    while (start < result.text.size()) {
        if (result.text[start] == ' ') { ++start; continue; }
        std::size_t end = result.text.find(' ', start);
        if (end == std::string::npos) end = result.text.size();
        NotationItem item;
        item.text = result.text.substr(start, end - start);
        item.x = metrics.text_range_width(result.text, true, 0, start);
        item.width = metrics.text_range_width(result.text, true, start, end - start);
        item.height = metrics.line_height();
        metrics.text_bounds(item.text, true, item.ink_left, item.ink_right);
        result.moves.push_back(item);
        start = end;
    }
    return result;
}

int EnginePvLayout::hit_test(int x, int y) const
{
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const NotationItem& item = moves[i];
        if (y >= 0 && y < item.height && x >= item.x && x < item.x + item.width)
            return static_cast<int>(i);
    }
    return -1;
}

EngineDisplay::EngineDisplay() : enabled_(true), have_current_(false), have_pending_(false), name_("Stockfish"),
                                 observed_generation_(0), current_analysis_(), pending_analysis_(), state_(ENGINE_DISPLAY_STARTING),
                                 preview_index_(-1), preview_node_(0) {}
EngineDisplayGeometry::EngineDisplayGeometry() : depth_label_x(0), depth_right_x(0), score_x(0), seldepth_x(0), pv_x(0), status_x(0) {}

bool EngineDisplay::update(const UciEngine& engine, bool enabled)
{
    return update_snapshot(engine.generation(), engine.state(), engine.name(), engine.analysis(), enabled);
}

bool EngineDisplay::update_snapshot(unsigned long generation, UciState engine_state, const std::string& engine_name,
                                    const UciAnalysis& analysis, bool enabled)
{
    const bool was_preview = preview_index_ >= 0;
    if (!enabled || generation != preview_analysis_.generation || engine_state == UCI_UNAVAILABLE)
        end_preview();
    EngineDisplayState previous_state = state_;
    bool previous_current = have_current_;
    bool previous_pending = have_pending_;
    unsigned long previous_analysis_generation = current_analysis_.generation;
    int previous_depth = current_analysis_.info.depth;
    int previous_score = current_analysis_.info.score;
    UciScoreType previous_score_type = current_analysis_.info.score_type;
    bool previous_score_valid = current_analysis_.info.score_valid;
    std::string previous_pv = current_analysis_.san_pv;
    bool previous_terminal = current_analysis_.terminal;
    bool previous_checkmate = current_analysis_.checkmate;
    bool previous_finished = current_analysis_.finished;
    enabled_ = enabled;
    name_ = engine_name;
    if (observed_generation_ != 0 && observed_generation_ != generation) {
        if (have_current_ && !current_analysis_.terminal && (!have_pending_ ||
            (current_analysis_.info.score_valid && current_analysis_.info.pv_valid))) {
            pending_analysis_ = current_analysis_;
            have_pending_ = true;
        }
        have_current_ = false;
    }
    observed_generation_ = generation;
    /* Live output keeps advancing; preview accessors expose the painted snapshot. */
    if (enabled_ && UciEngine::analysis_is_current(analysis, generation)) {
        current_analysis_ = analysis; have_current_ = true;
        if (current_analysis_.info.score_valid && current_analysis_.info.pv_valid) have_pending_ = false;
        if (current_analysis_.terminal) have_pending_ = false;
    }
    /* A pending snapshot only bridges an active search.  READY means the
       current generation has definitively ended, even when it produced no PV. */
    if (enabled_ && engine_state == UCI_READY) have_pending_ = false;
    if (!enabled_) state_ = ENGINE_DISPLAY_OFF;
    else if (engine_state == UCI_UNAVAILABLE) state_ = ENGINE_DISPLAY_UNAVAILABLE;
    else if (engine_state == UCI_STARTING || engine_state == UCI_WAITING_READY) state_ = ENGINE_DISPLAY_STARTING;
    else if (have_current_ && current_analysis_.terminal) state_ = ENGINE_DISPLAY_TERMINAL;
    else if (have_current_ && current_analysis_.info.score_valid && current_analysis_.info.pv_valid) state_ = ENGINE_DISPLAY_ANALYZING;
    else if (engine_state == UCI_READY) state_ = ENGINE_DISPLAY_READY;
    else state_ = ENGINE_DISPLAY_PENDING;
    if (was_preview) return preview_index_ < 0;
    return previous_state != state_ || previous_current != have_current_ || previous_pending != have_pending_ ||
           previous_analysis_generation != current_analysis_.generation ||
           previous_depth != current_analysis_.info.depth || previous_score != current_analysis_.info.score ||
           previous_score_type != current_analysis_.info.score_type ||
           previous_score_valid != current_analysis_.info.score_valid ||
           previous_terminal != current_analysis_.terminal || previous_checkmate != current_analysis_.checkmate ||
           previous_finished != current_analysis_.finished || previous_pv != current_analysis_.san_pv;
}

void EngineDisplay::clear()
{
    end_preview();
    have_current_ = false; have_pending_ = false; observed_generation_ = 0;
    current_analysis_ = UciAnalysis(); pending_analysis_ = UciAnalysis();
}

EngineDisplayState EngineDisplay::state() const { return preview_index_ >= 0 ? ENGINE_DISPLAY_ANALYZING : state_; }
const std::string& EngineDisplay::name() const { return preview_index_ >= 0 ? preview_name_ : name_; }
const UciAnalysis* EngineDisplay::score_analysis() const
{
    if (preview_index_ >= 0) return &preview_analysis_;
    if (state_ == ENGINE_DISPLAY_OFF || state_ == ENGINE_DISPLAY_UNAVAILABLE ||
        state_ == ENGINE_DISPLAY_STARTING) return 0;
    if (have_current_ && current_analysis_.info.score_valid) return &current_analysis_;
    if (have_pending_ && pending_analysis_.info.score_valid) return &pending_analysis_;
    return 0;
}
const UciAnalysis* EngineDisplay::pv_analysis() const
{
    if (preview_index_ >= 0) return &preview_analysis_;
    if (state_ == ENGINE_DISPLAY_OFF || state_ == ENGINE_DISPLAY_UNAVAILABLE || state_ == ENGINE_DISPLAY_STARTING) return 0;
    if (have_current_ && current_analysis_.info.pv_valid) return &current_analysis_;
    if (have_pending_ && pending_analysis_.info.pv_valid) return &pending_analysis_;
    return 0;
}
const UciAnalysis* EngineDisplay::detail_analysis() const
{ return preview_index_ >= 0 ? &preview_analysis_ : (have_current_ ? &current_analysis_ : (have_pending_ ? &pending_analysis_ : 0)); }
bool EngineDisplay::score_is_current() const { return preview_index_ >= 0 ? preview_analysis_.info.score_valid : (have_current_ && current_analysis_.info.score_valid); }
bool EngineDisplay::pv_is_current() const { return preview_index_ >= 0 || (have_current_ && current_analysis_.info.pv_valid); }
std::string EngineDisplay::status() const
{
    if (preview_index_ >= 0) return "Analyzing";
    if (state_ == ENGINE_DISPLAY_OFF) return "Analysis off (A)";
    if (state_ == ENGINE_DISPLAY_UNAVAILABLE) return "Unavailable";
    if (state_ == ENGINE_DISPLAY_STARTING) return "Starting...";
    if (state_ == ENGINE_DISPLAY_TERMINAL) return current_analysis_.checkmate ? "Checkmate" : "Stalemate";
    if (state_ == ENGINE_DISPLAY_READY) return "Ready";
    if (state_ == ENGINE_DISPLAY_PENDING) return "Analyzing...";
    return "Analyzing";
}

bool EngineDisplay::select_preview(const UciAnalysis& shown, std::size_t index,
                                   const Position& origin, const MoveNode* node,
                                   unsigned long generation)
{
    if (!enabled_ || state_ == ENGINE_DISPLAY_OFF || state_ == ENGINE_DISPLAY_UNAVAILABLE ||
        generation != observed_generation_ || !pv_is_current() ||
        !UciEngine::analysis_is_current(shown, generation) || !shown.info.pv_valid ||
        shown.side_to_move != origin.side_to_move() || index >= shown.info.pv.size()) return false;
    sync_position(origin, node);
    Position position = origin;
    Move move;
    for (std::size_t i = 0; i <= index; ++i) {
        Undo undo;
        if (!UciEngine::pv_move(position, shown.info.pv[i], move) || !position.make_move(move, undo))
            return false;
    }
    preview_analysis_ = shown;
    preview_name_ = name_;
    preview_origin_ = origin;
    preview_node_ = node;
    preview_position_ = position;
    preview_move_ = move;
    preview_index_ = static_cast<int>(index);
    return true;
}

void EngineDisplay::end_preview() { preview_index_ = -1; preview_node_ = 0; }
void EngineDisplay::sync_position(const Position& origin, const MoveNode* node)
{
    if (preview_index_ >= 0 && (node != preview_node_ || !origin.equals(preview_origin_))) end_preview();
}
int EngineDisplay::preview_index() const { return preview_index_; }
const Position* EngineDisplay::preview_position() const { return preview_index_ >= 0 ? &preview_position_ : 0; }
const Move* EngineDisplay::preview_move() const { return preview_index_ >= 0 ? &preview_move_ : 0; }
EngineDisplayGeometry EngineDisplay::geometry(int left)
{
    EngineDisplayGeometry result;
    result.depth_label_x = left;
    result.depth_right_x = left + 64;
    result.score_x = left + 78;
    /* Score may carry the visible pending marker; reserve its complete measured field. */
    result.seldepth_x = left + 240;
    result.pv_x = left;
    result.status_x = left + 120;
    return result;
}

EngineUiThrottle::EngineUiThrottle() : last_engine_present_(0), pending_(false) {}
unsigned long EngineUiThrottle::now_milliseconds()
{
    struct timespec monotonic;
    struct timeval now;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0)
        return static_cast<unsigned long>(monotonic.tv_sec) * 1000UL +
               static_cast<unsigned long>(monotonic.tv_nsec / 1000000L);
    gettimeofday(&now, 0);
    return static_cast<unsigned long>(now.tv_sec) * 1000UL + static_cast<unsigned long>(now.tv_usec / 1000);
}
bool EngineUiThrottle::engine_update_due(unsigned long now)
{
    if (last_engine_present_ == 0 || now - last_engine_present_ >= 200UL) return true;
    pending_ = true;
    return false;
}
void EngineUiThrottle::engine_presented(unsigned long now) { last_engine_present_ = now; pending_ = false; }
void EngineUiThrottle::user_presented(unsigned long now) { last_engine_present_ = now; pending_ = false; }
bool EngineUiThrottle::pending() const { return pending_; }

}
