#include "gambitree/uci_engine.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gambitree {

namespace {

bool diagnostics_enabled() { return std::getenv("GAMBITREE_DIAGNOSTICS") != 0; }

bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool parse_integer(const std::string& text, int& value)
{
    char* end = 0;
    long parsed;
    errno = 0;
    parsed = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || !end || *end != '\0' || errno == ERANGE || parsed < -2147483647L - 1L || parsed > 2147483647L) return false;
    value = static_cast<int>(parsed);
    return true;
}

std::string join_tokens(const std::vector<std::string>& tokens)
{
    std::ostringstream result;
    std::size_t index;
    for (index = 0; index < tokens.size(); ++index) {
        if (index) result << ' ';
        result << tokens[index];
    }
    return result.str();
}

}

UciInfo::UciInfo() : depth(0), depth_valid(false), seldepth(0), seldepth_valid(false),
                     score_type(UCI_SCORE_NONE), score(0), score_valid(false), lowerbound(false),
                     upperbound(false), pv(), pv_valid(false) {}
UciAnalysis::UciAnalysis() : generation(0), valid(false), terminal(false), checkmate(false), finished(false),
                             side_to_move(WHITE), info(), san_pv() {}

UciEngine::UciEngine(const std::string& executable)
    : executable_(executable), name_("Stockfish"), input_fd_(-1), output_fd_(-1), child_pid_(-1),
      state_(UCI_UNAVAILABLE), input_buffer_(), output_buffer_(), pending_fen_(), pending_side_(WHITE),
      active_side_(WHITE), active_position_(), generation_(0), active_generation_(0),
      chess960_option_known_(false), chess960_option_value_(false), begin_search_after_output_drain_(false), analysis_() {}

UciEngine::~UciEngine() { shutdown(); }

bool UciEngine::start(std::string& error)
{
    int to_child[2];
    int from_child[2];
    pid_t pid;
    if (child_pid_ > 0) return true;
    if (pipe(to_child) != 0) { error = "cannot create UCI pipes"; return false; }
    if (pipe(from_child) != 0) { close(to_child[0]); close(to_child[1]); error = "cannot create UCI pipes"; return false; }
    pid = fork();
    if (pid < 0) { close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]); error = "cannot fork UCI engine"; return false; }
    if (pid == 0) {
        char* const arguments[] = { const_cast<char*>(executable_.c_str()), 0 };
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    input_fd_ = to_child[1]; output_fd_ = from_child[0]; child_pid_ = static_cast<int>(pid);
    std::signal(SIGPIPE, SIG_IGN);
    if (!set_nonblocking(input_fd_) || !set_nonblocking(output_fd_)) { error = "cannot configure nonblocking UCI pipes"; shutdown(); return false; }
    state_ = UCI_STARTING;
    if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI engine started, PID " << child_pid_ << "\n";
    if (!queue("uci\n")) { error = "cannot send UCI handshake"; mark_unavailable(); return false; }
    return true;
}

bool UciEngine::queue(const std::string& command)
{
    if (input_fd_ < 0) return false;
    input_buffer_ += command;
    return flush_writes();
}

bool UciEngine::flush_writes()
{
    while (!input_buffer_.empty()) {
        ssize_t written = write(input_fd_, input_buffer_.data(), input_buffer_.size());
        if (written > 0) input_buffer_.erase(0, static_cast<std::size_t>(written));
        else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        else { mark_unavailable(); return false; }
    }
    return true;
}

bool UciEngine::read_output()
{
    char buffer[4096];
    bool changed = false;
    for (;;) {
        ssize_t count = read(output_fd_, buffer, sizeof(buffer));
        if (count > 0) { output_buffer_.append(buffer, static_cast<std::size_t>(count)); changed = true; }
        else if (count == 0) { mark_unavailable(); return true; }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { mark_unavailable(); return true; }
    }
    for (;;) {
        std::string::size_type newline = output_buffer_.find('\n');
        std::string line;
        if (newline == std::string::npos) break;
        line = output_buffer_.substr(0, newline);
        output_buffer_.erase(0, newline + 1);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        handle_line(line);
    }
    /* Finish draining the current pipe batch before starting the replacement
       generation.  Any already-buffered output after the stopped search's
       bestmove still belongs to that old search and must not update the new one. */
    if (begin_search_after_output_drain_) {
        begin_search_after_output_drain_ = false;
        begin_search();
    }
    return changed;
}

bool UciEngine::child_exited()
{
    int status;
    pid_t result;
    if (child_pid_ <= 0) return false;
    result = waitpid(static_cast<pid_t>(child_pid_), &status, WNOHANG);
    if (result == static_cast<pid_t>(child_pid_)) {
        if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI engine exited unexpectedly\n";
        child_pid_ = -1; mark_unavailable(); return true;
    }
    return false;
}

bool UciEngine::poll()
{
    UciState before = state_;
    unsigned long before_generation = analysis_.generation;
    int before_depth = analysis_.info.depth;
    int before_score = analysis_.info.score;
    std::string before_pv = analysis_.san_pv;
    if (state_ == UCI_UNAVAILABLE) return false;
    flush_writes();
    if (output_fd_ >= 0) read_output();
    child_exited();
    return before != state_ || before_generation != analysis_.generation || before_depth != analysis_.info.depth ||
           before_score != analysis_.info.score || before_pv != analysis_.san_pv;
}

void UciEngine::handle_line(const std::string& line)
{
    if (line.compare(0, 8, "id name ") == 0) name_ = line.substr(8);
    if (line == "uciok" && state_ == UCI_STARTING) { state_ = protocol_state_after(state_, line); if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI handshake accepted\n"; queue("isready\n"); return; }
    if (line == "readyok" && state_ == UCI_WAITING_READY) { state_ = protocol_state_after(state_, line); if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI engine ready\n"; begin_search(); return; }
    if (line.compare(0, 5, "info ") == 0 && accepts_info(state_, active_generation_, generation_)) {
        UciInfo info;
        if (parse_info(line, info)) {
            int before_depth = analysis_.info.depth;
            int before_score = analysis_.info.score;
            std::string before_pv = analysis_.san_pv;
            if (analysis_.generation != active_generation_) {
                analysis_ = UciAnalysis(); analysis_.generation = active_generation_; analysis_.side_to_move = active_side_;
                analysis_.terminal = active_position_.legal_moves().empty();
                analysis_.checkmate = analysis_.terminal && active_position_.in_check(active_side_);
                analysis_.valid = analysis_.terminal;
            }
            merge_info(analysis_.info, info);
            if (info.pv_valid) {
                /* Convert the merged principal PV, not a discarded bound-only input PV. */
                analysis_.san_pv = pv_to_san(active_position_, analysis_.info.pv);
            }
            analysis_.valid = analysis_.info.score_valid || analysis_.info.pv_valid;
            if (diagnostics_enabled() && (info.depth_valid || info.score_valid || info.pv_valid)) {
                std::cerr << "gambitree: diagnostic: UCI info generation=" << active_generation_
                          << " raw='" << line << "'";
                if (info.depth_valid) std::cerr << " depth=" << info.depth;
                if (info.seldepth_valid) std::cerr << " seldepth=" << info.seldepth;
                if (info.score_valid) std::cerr << " score=" << info.score;
                if (info.pv_valid) std::cerr << " pv_tokens=" << info.pv.size() << " pv='" << join_tokens(info.pv) << "'";
                std::cerr << " snapshot_changed=" << (before_depth != analysis_.info.depth || before_score != analysis_.info.score || before_pv != analysis_.san_pv ? "yes" : "no")
                          << " visible_depth=" << (analysis_.info.depth_valid ? analysis_.info.depth : 0)
                          << " visible_score=" << (analysis_.info.score_valid ? analysis_.info.score : 0)
                          << " visible_pv_tokens=" << analysis_.info.pv.size()
                          << " visible_pv='" << analysis_.san_pv << "'\n";
            }
        }
        return;
    }
    if (line.compare(0, 5, "info ") == 0) {
        if (diagnostics_enabled())
            std::cerr << "gambitree: diagnostic: ignored UCI info state=" << static_cast<int>(state_)
                      << " active_generation=" << active_generation_ << " current_generation=" << generation_
                      << " raw='" << line << "'\n";
        return;
    }
    if (line.compare(0, 9, "bestmove ") == 0) {
        if (diagnostics_enabled())
            std::cerr << "gambitree: diagnostic: UCI bestmove state=" << static_cast<int>(state_)
                      << " active_generation=" << active_generation_ << " current_generation=" << generation_
                      << " raw='" << line << "'\n";
        if (state_ == UCI_STOPPING) { state_ = UCI_READY; begin_search_after_output_drain_ = true; }
        else if (protocol_state_after(state_, line, active_generation_, generation_) == UCI_READY) {
            analysis_.generation = active_generation_;
            analysis_.side_to_move = active_side_;
            analysis_.finished = true;
            state_ = protocol_state_after(state_, line);
        }
    }
}

void UciEngine::begin_search()
{
    if (state_ != UCI_READY || pending_fen_.empty()) return;
    {
        bool desired_chess960 = active_position_.variant() == CHESS960;
        /* UCI engines start in standard mode by default.  Once a Chess960
           game has been analyzed, explicitly restore false before standard
           analysis so a shared engine instance cannot retain stale rules. */
        if (!chess960_option_known_ && !desired_chess960) {
            chess960_option_known_ = true;
            chess960_option_value_ = false;
        }
        if (!chess960_option_known_ || chess960_option_value_ != desired_chess960) {
            if (!queue(std::string("setoption name UCI_Chess960 value ") +
                       (desired_chess960 ? "true\n" : "false\n"))) {
                mark_unavailable();
                return;
            }
            chess960_option_known_ = true;
            chess960_option_value_ = desired_chess960;
            if (diagnostics_enabled())
                std::cerr << "gambitree: diagnostic: UCI_Chess960 set to "
                          << (desired_chess960 ? "true" : "false") << "\n";
            state_ = UCI_WAITING_READY;
            if (!queue("isready\n")) mark_unavailable();
            return;
        }
    }
    active_generation_ = generation_; active_side_ = pending_side_;
    analysis_ = UciAnalysis();
    analysis_.generation = active_generation_;
    analysis_.side_to_move = active_side_;
    analysis_.terminal = active_position_.legal_moves().empty();
    analysis_.checkmate = analysis_.terminal && active_position_.in_check(active_side_);
    analysis_.valid = analysis_.terminal;
    queue("position fen " + pending_fen_ + "\n");
    queue("go infinite\n");
    state_ = UCI_ANALYZING;
    if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI search generation " << active_generation_
                                        << " fen='" << pending_fen_ << "' position_command='position fen "
                                        << pending_fen_ << "'\n";
}

void UciEngine::request_analysis(const Position& position)
{
    ++generation_;
    pending_fen_ = position.to_fen(); pending_side_ = position.side_to_move(); active_position_ = position;
    analysis_ = UciAnalysis();
    analysis_.generation = generation_;
    analysis_.side_to_move = pending_side_;
    analysis_.terminal = position.legal_moves().empty();
    analysis_.checkmate = analysis_.terminal && position.in_check(pending_side_);
    analysis_.valid = analysis_.terminal;
    if (state_ == UCI_READY) begin_search();
    else if (state_ == UCI_ANALYZING) { queue("stop\n"); state_ = UCI_STOPPING; if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI stop\n"; }
}

void UciEngine::stop()
{
    pending_fen_.clear(); ++generation_; analysis_ = UciAnalysis();
    if (state_ == UCI_ANALYZING) { queue("stop\n"); state_ = UCI_STOPPING; if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI stop\n"; }
}

void UciEngine::mark_unavailable()
{
    if (input_fd_ >= 0) { close(input_fd_); input_fd_ = -1; }
    if (output_fd_ >= 0) { close(output_fd_); output_fd_ = -1; }
    state_ = UCI_UNAVAILABLE;
}

void UciEngine::shutdown()
{
    int attempts;
    if (child_pid_ <= 0) { mark_unavailable(); return; }
    if (diagnostics_enabled()) std::cerr << "gambitree: diagnostic: UCI shutdown\n";
    if (input_fd_ >= 0) { queue("stop\nquit\n"); }
    for (attempts = 0; attempts < 25; ++attempts) {
        int status;
        if (waitpid(static_cast<pid_t>(child_pid_), &status, WNOHANG) == static_cast<pid_t>(child_pid_)) { child_pid_ = -1; mark_unavailable(); return; }
        usleep(10000);
    }
    kill(static_cast<pid_t>(child_pid_), SIGTERM);
    waitpid(static_cast<pid_t>(child_pid_), 0, 0);
    child_pid_ = -1; mark_unavailable();
}

UciState UciEngine::state() const { return state_; }
const std::string& UciEngine::name() const { return name_; }
const UciAnalysis& UciEngine::analysis() const { return analysis_; }
unsigned long UciEngine::generation() const { return generation_; }
int UciEngine::output_fd() const { return output_fd_; }
int UciEngine::child_pid() const { return child_pid_; }
bool UciEngine::analysis_is_current(const UciAnalysis& analysis, unsigned long generation)
{ return analysis.generation == generation && (analysis.valid || analysis.terminal); }
bool UciEngine::accepts_info(UciState state, unsigned long active_generation, unsigned long current_generation)
{ return state == UCI_ANALYZING && active_generation == current_generation; }
UciState UciEngine::protocol_state_after(UciState state, const std::string& line)
{
    if (state == UCI_STARTING && line == "uciok") return UCI_WAITING_READY;
    if (state == UCI_WAITING_READY && line == "readyok") return UCI_READY;
    if ((state == UCI_ANALYZING || state == UCI_STOPPING) && line.compare(0, 9, "bestmove ") == 0) return UCI_READY;
    return state;
}
UciState UciEngine::protocol_state_after(UciState state, const std::string& line,
                                         unsigned long active_generation, unsigned long current_generation)
{
    if (state == UCI_STOPPING && line.compare(0, 9, "bestmove ") == 0) return UCI_READY;
    if (state == UCI_ANALYZING && active_generation == current_generation &&
        line.compare(0, 9, "bestmove ") == 0) return UCI_READY;
    return state;
}

bool UciEngine::parse_info(const std::string& line, UciInfo& info)
{
    std::istringstream stream(line);
    std::string token;
    if (!(stream >> token) || token != "info") return false;
    while (stream >> token) {
        std::string value;
        int parsed;
        if (token == "depth") { if (stream >> value && parse_integer(value, parsed)) { info.depth = parsed; info.depth_valid = true; } }
        else if (token == "seldepth") { if (stream >> value && parse_integer(value, parsed)) { info.seldepth = parsed; info.seldepth_valid = true; } }
        else if (token == "score") {
            std::string kind;
            if (!(stream >> kind) || !(stream >> value) || !parse_integer(value, parsed)) continue;
            if (kind == "cp") info.score_type = UCI_SCORE_CENTIPAWNS;
            else if (kind == "mate") info.score_type = UCI_SCORE_MATE;
            else continue;
            info.score = parsed; info.score_valid = true;
        } else if (token == "lowerbound") info.lowerbound = true;
        else if (token == "upperbound") info.upperbound = true;
        else if (token == "pv") { while (stream >> value) info.pv.push_back(value); info.pv_valid = !info.pv.empty(); break; }
    }
    return true;
}

void UciEngine::merge_info(UciInfo& target, const UciInfo& incoming)
{
    bool incoming_bound = incoming.lowerbound || incoming.upperbound;
    bool target_bound = target.lowerbound || target.upperbound;
    if (incoming.depth_valid) { target.depth = incoming.depth; target.depth_valid = true; }
    if (incoming.seldepth_valid) { target.seldepth = incoming.seldepth; target.seldepth_valid = true; }
    if (incoming.score_valid && (!incoming_bound || !target.score_valid || target_bound)) {
        target.score_type = incoming.score_type;
        target.score = incoming.score;
        target.score_valid = true;
        target.lowerbound = incoming.lowerbound;
        target.upperbound = incoming.upperbound;
    }
    /* Bound scores are aspiration-window progress, not a replacement exact PV. */
    if (incoming.pv_valid && (!incoming_bound || !target.pv_valid || target_bound)) {
        target.pv = incoming.pv;
        target.pv_valid = true;
    }
}

bool UciEngine::pv_move(const Position& position, const std::string& token, Move& move)
{
    if (token.size() != 4 && token.size() != 5) return false;
    const int from = Position::square_from_string(token.substr(0, 2));
    const int to = Position::square_from_string(token.substr(2, 2));
    if (from < 0 || to < 0) return false;
    PieceType promotion = NO_PIECE;
    if (token.size() == 5) {
        if (token[4] == 'q') promotion = QUEEN;
        else if (token[4] == 'r') promotion = ROOK;
        else if (token[4] == 'b') promotion = BISHOP;
        else if (token[4] == 'n') promotion = KNIGHT;
        else return false;
    }
    return position.move_from_uci(from, to, promotion, move);
}

std::string UciEngine::pv_to_san(const Position& initial, const std::vector<std::string>& pv)
{
    Position position = initial;
    std::ostringstream output;
    std::size_t index;
    bool wrote = false;
    for (index = 0; index < pv.size(); ++index) {
        const std::string& token = pv[index];
        Move chosen;
        if (token.size() < 4) { if (wrote) output << ' '; output << token; wrote = true; continue; }
        if (!pv_move(position, token, chosen)) { for (; index < pv.size(); ++index) { if (wrote) output << ' '; output << pv[index]; wrote = true; } break; }
        if (wrote) output << ' ';
        output << position.san(chosen);
        wrote = true;
        {
            Undo undo;
            if (!position.make_move(chosen, undo)) {
                for (++index; index < pv.size(); ++index) { output << ' ' << pv[index]; }
                break;
            }
        }
    }
    return output.str();
}

std::string UciEngine::format_score(const UciAnalysis& analysis)
{
    std::ostringstream output;
    int score = analysis.info.score;
    if (analysis.side_to_move == BLACK) score = -score;
    if (analysis.info.score_type == UCI_SCORE_MATE) { output << (score >= 0 ? "M" : "-M") << (score >= 0 ? score : -score); return output.str(); }
    if (analysis.info.score_type != UCI_SCORE_CENTIPAWNS) return "";
    output << (score >= 0 ? "+" : "-") << (score >= 0 ? score : -score) / 100 << ".";
    int fraction = (score >= 0 ? score : -score) % 100;
    if (fraction < 10) output << '0';
    output << fraction;
    return output.str();
}

}
