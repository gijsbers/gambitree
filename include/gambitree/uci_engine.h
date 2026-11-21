#ifndef GAMBITREE_UCI_ENGINE_H
#define GAMBITREE_UCI_ENGINE_H

#include <string>
#include <vector>

#include "gambitree/chess.h"

namespace gambitree {

enum UciState { UCI_UNAVAILABLE, UCI_STARTING, UCI_WAITING_READY, UCI_READY, UCI_ANALYZING, UCI_STOPPING };
enum UciScoreType { UCI_SCORE_NONE, UCI_SCORE_CENTIPAWNS, UCI_SCORE_MATE };

struct UciInfo {
    int depth;
    bool depth_valid;
    int seldepth;
    bool seldepth_valid;
    UciScoreType score_type;
    int score;
    bool score_valid;
    bool lowerbound;
    bool upperbound;
    std::vector<std::string> pv;
    bool pv_valid;
    UciInfo();
};

struct UciAnalysis {
    unsigned long generation;
    bool valid;
    bool terminal; /* Active Position has no legal moves. */
    bool checkmate;
    bool finished; /* A bestmove for this generation has been received. */
    Color side_to_move;
    UciInfo info;
    std::string san_pv;
    UciAnalysis();
};

class UciEngine {
public:
    explicit UciEngine(const std::string& executable);
    ~UciEngine();
    bool start(std::string& error);
    void shutdown();
    bool poll();
    void request_analysis(const Position& position);
    void stop();
    UciState state() const;
    const std::string& name() const;
    const UciAnalysis& analysis() const;
    unsigned long generation() const;
    int output_fd() const;
    int child_pid() const;
    static bool parse_info(const std::string& line, UciInfo& info);
    static void merge_info(UciInfo& target, const UciInfo& incoming);
    static bool analysis_is_current(const UciAnalysis& analysis, unsigned long generation);
    static bool accepts_info(UciState state, unsigned long active_generation, unsigned long current_generation);
    static UciState protocol_state_after(UciState state, const std::string& line);
    static UciState protocol_state_after(UciState state, const std::string& line,
                                         unsigned long active_generation, unsigned long current_generation);
    static std::string pv_to_san(const Position& position, const std::vector<std::string>& pv);
    static bool pv_move(const Position& position, const std::string& token, Move& move);
    static std::string format_score(const UciAnalysis& analysis);
private:
    std::string executable_;
    std::string name_;
    int input_fd_;
    int output_fd_;
    int child_pid_;
    UciState state_;
    std::string input_buffer_;
    std::string output_buffer_;
    std::string pending_fen_;
    Color pending_side_;
    Color active_side_;
    Position active_position_;
    unsigned long generation_;
    unsigned long active_generation_;
    bool chess960_option_known_;
    bool chess960_option_value_;
    bool begin_search_after_output_drain_;
    UciAnalysis analysis_;
    bool queue(const std::string& command);
    bool flush_writes();
    bool read_output();
    void handle_line(const std::string& line);
    void begin_search();
    void mark_unavailable();
    bool child_exited();
    UciEngine(const UciEngine&);
    UciEngine& operator=(const UciEngine&);
};

}

#endif
