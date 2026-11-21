#include "gambitree/pgn_index.h"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace gambitree {
namespace {
const PgnOffset kProvisionalThreshold = static_cast<PgnOffset>(128) * static_cast<PgnOffset>(1024) * static_cast<PgnOffset>(1024);
const std::size_t kFastBufferSize = 1024U * 1024U;
const std::size_t kMaximumGeneralLine = 1024U * 1024U;
const std::size_t kMaximumIndexEntries = 16000000U;
const std::size_t kEntryReserveIncrement = 1024U * 1024U;
const std::size_t kMaximumFastLine = 4096U;
const PgnOffset kDiagnosticInterval = static_cast<PgnOffset>(512) * static_cast<PgnOffset>(1024) * static_cast<PgnOffset>(1024);

bool blank_line(const std::string& line) { std::size_t i; for (i = 0; i < line.size(); ++i) if (!std::isspace(static_cast<unsigned char>(line[i]))) return false; return true; }
bool line_has_non_comment_content(const std::string& line, bool initially_in_comment) {
    std::size_t i; bool brace = initially_in_comment;
    for (i = 0; i < line.size(); ++i) { if (brace) { if (line[i] == '}') brace = false; continue; } if (line[i] == '{') { brace = true; continue; } if (line[i] == ';') return false; if (!std::isspace(static_cast<unsigned char>(line[i]))) return true; }
    return false;
}
bool plausible_tag(const std::string& line, std::string* name,
                   bool allow_initial_bom = false) {
    std::size_t i = 0, first;
    if (allow_initial_bom && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xef &&
        static_cast<unsigned char>(line[1]) == 0xbb &&
        static_cast<unsigned char>(line[2]) == 0xbf)
        i = 3;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size() || line[i] != '[') return false;
    ++i;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    first = i; while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])) && line[i] != ']') ++i;
    if (first == i) return false;
    if (name) *name = line.substr(first, i - first);
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size() || line[i] != '"') return false;
    ++i;
    while (i < line.size() && line[i] != '"') { if (line[i] == '\\' && i + 1 < line.size()) ++i; ++i; }
    if (i >= line.size()) return false;
    ++i;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    return i < line.size() && line[i] == ']';
}
std::size_t resident_kib() { std::ifstream in("/proc/self/statm"); unsigned long total = 0, resident = 0; if (!(in >> total >> resident)) return 0; return static_cast<std::size_t>(resident) * static_cast<std::size_t>(getpagesize() / 1024); }
}

PgnIndexEntry::PgnIndexEntry() : begin(0), end(0) {}
PgnIndex::PgnIndex() : input_(0), file_size_(0), bytes_scanned_(0), complete_(false), failed_(false), in_brace_comment_(false), have_game_(false), saw_movetext_(false), saw_result_(false), saw_tag_like_(false), rav_depth_(0), line_number_(1), lines_scanned_(0), chunks_(0), started_(0), scan_ticks_(0), fast_mode_(false), fast_previous_blank_(true), fast_have_candidate_(false), fast_candidate_begin_(0), fast_line_begin_(0), fast_line_overflow_(false), next_diagnostic_bytes_(kDiagnosticInterval) {}
PgnIndex::~PgnIndex() { delete static_cast<std::ifstream*>(input_); }
PgnOffset PgnIndex::provisional_threshold() { return kProvisionalThreshold; }

bool PgnIndex::choose_fast_path(const std::string& path) const {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary); std::string line, name; bool previous_blank = true, candidate = false; std::size_t starts = 0, bytes = 0; const std::size_t probe = 4U * 1024U * 1024U;
    if (!input) return false;
    while (bytes < probe && std::getline(input, line)) {
        bytes += line.size() + 1;
        bool first_line = bytes == line.size() + 1;
        if (candidate) { if (plausible_tag(line, 0, first_line)) { ++starts; candidate = false; if (starts >= 2) return true; } else if (!blank_line(line)) candidate = false; }
        if (previous_blank && plausible_tag(line, &name, first_line) && name == "Event") candidate = true;
        previous_blank = blank_line(line);
    }
    return false;
}

bool PgnIndex::open(const std::string& path, std::string& error) {
    struct stat status; std::ifstream* input;
    if (stat(path.c_str(), &status) != 0) { error = "cannot determine file size"; return false; }
    if (S_ISDIR(status.st_mode)) { error = "is a directory"; return false; }
    if (status.st_size < 0 || static_cast<uint64_t>(status.st_size) > std::numeric_limits<PgnOffset>::max()) { error = "invalid file size"; return false; }
    if (static_cast<uint64_t>(status.st_size) > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) { error = "file is too large for this platform's stream offsets"; return false; }
    delete static_cast<std::ifstream*>(input_); input_ = 0; entries_.clear(); std::vector<char>().swap(fast_buffer_); std::vector<char>().swap(general_buffer_); fast_line_.clear(); complete_ = failed_ = false;
    have_game_ = saw_movetext_ = saw_result_ = saw_tag_like_ = false; in_brace_comment_ = false; rav_depth_ = 0; line_number_ = 1; lines_scanned_ = chunks_ = scan_ticks_ = 0;
    file_size_ = static_cast<PgnOffset>(status.st_size); bytes_scanned_ = 0; path_ = path; fast_mode_ = file_size_ >= kProvisionalThreshold && choose_fast_path(path);
    input = new std::ifstream(path.c_str(), std::ios::in | std::ios::binary); if (!*input) { delete input; error = "cannot open file"; return false; }
    input_ = input; started_ = std::clock(); fast_previous_blank_ = true; fast_have_candidate_ = false; fast_candidate_begin_ = 0; fast_line_begin_ = 0; fast_line_overflow_ = false; next_diagnostic_bytes_ = kDiagnosticInterval;
    if (fast_mode_) fast_buffer_.resize(kFastBufferSize);
    else general_buffer_.resize(kMaximumGeneralLine);
    return true;
}

bool PgnIndex::fallback_to_general(std::string& error) {
    std::ifstream* input;
    if (!fast_mode_) return true;
    delete static_cast<std::ifstream*>(input_); input_ = 0; entries_.clear(); std::vector<char>().swap(fast_buffer_); std::vector<char>().swap(general_buffer_); fast_line_.clear(); complete_ = failed_ = false; fast_mode_ = false; in_brace_comment_ = false; have_game_ = saw_movetext_ = saw_result_ = saw_tag_like_ = false; rav_depth_ = 0; line_number_ = 1; lines_scanned_ = chunks_ = 0; bytes_scanned_ = 0;
    input = new std::ifstream(path_.c_str(), std::ios::in | std::ios::binary); if (!*input) { delete input; error = "cannot reopen file for robust indexing"; failed_ = true; return false; }
    input_ = input; general_buffer_.resize(kMaximumGeneralLine); started_ = std::clock(); scan_ticks_ = 0; return true;
}
bool PgnIndex::line_is_tag(const std::string& line, bool allow_initial_bom) const { return plausible_tag(line, 0, allow_initial_bom); }
bool PgnIndex::line_has_top_level_result(const std::string& line) const {
    std::size_t i; bool brace = in_brace_comment_; unsigned int depth = rav_depth_; std::string token;
    for (i = 0; i <= line.size(); ++i) { char c = i < line.size() ? line[i] : ' '; bool semi = c == ';'; if (brace) { if (c == '}') brace = false; continue; } if (c == '{') { brace = true; continue; } if (semi) c = ' '; if (c == '(' || c == ')' || std::isspace(static_cast<unsigned char>(c))) { if (depth == 0 && (token == "1/2-1/2" || token == "1-0" || token == "0-1" || token == "*")) return true; token.clear(); if (c == '(') ++depth; else if (c == ')' && depth > 0) --depth; if (semi) break; } else token += c; }
    return false;
}
void PgnIndex::finish_current(PgnOffset end) { if (have_game_ && !entries_.empty() && end >= entries_.back().begin) entries_.back().end = end; }
bool PgnIndex::append_entry(PgnOffset begin, std::string& error) {
    PgnIndexEntry entry; std::size_t next_capacity;
    if (entries_.size() >= kMaximumIndexEntries) { error = "index exceeds safe limit of 16000000 games"; failed_ = true; return false; }
    entry.begin = begin;
    try {
        if (entries_.size() == entries_.capacity()) {
            next_capacity = entries_.capacity() == 0 ? kEntryReserveIncrement : entries_.capacity() + kEntryReserveIncrement;
            if (next_capacity > kMaximumIndexEntries) next_capacity = kMaximumIndexEntries;
            entries_.reserve(next_capacity);
        }
        entries_.push_back(entry);
    } catch (const std::bad_alloc&) { error = "insufficient memory while retaining compact game index"; failed_ = true; return false; }
    have_game_ = true; return true;
}
bool PgnIndex::process_fast_line(const std::string& line, bool overflow, PgnOffset begin, bool previous_blank, std::string& error) {
    std::string name; bool tag = !overflow && plausible_tag(line, &name, begin == 0);
    if (fast_have_candidate_) { if (tag) { if (!entries_.empty()) finish_current(fast_candidate_begin_); if (!append_entry(fast_candidate_begin_, error)) return false; fast_have_candidate_ = false; } else if (!blank_line(line)) fast_have_candidate_ = false; }
    if (previous_blank && tag && name == "Event") { fast_have_candidate_ = true; fast_candidate_begin_ = begin; }
    return true;
}
void PgnIndex::report_diagnostics(bool final) const {
    if (!std::getenv("GAMBITREE_DIAGNOSTICS")) return;
    std::cerr << "gambitree: diagnostic: PGN " << (fast_mode_ ? "provisional" : "general") << " index bytes=" << bytes_scanned_ << "/" << file_size_ << " games=" << entries_.size() << " entries_capacity=" << entries_.capacity() << " entry_bytes=" << entry_storage_bytes() << " tag_string_bytes=0 scanner_buffer=" << scanner_buffer_bytes() << " rss_kib=" << resident_kib() << (final ? " final" : "") << "\n";
}
bool PgnIndex::step_fast(unsigned long maximum_milliseconds, std::string& error) {
    std::ifstream* input = static_cast<std::ifstream*>(input_); std::clock_t slice = std::clock(); ++chunks_;
    while (true) {
        PgnOffset block_begin = bytes_scanned_; input->read(&fast_buffer_[0], static_cast<std::streamsize>(fast_buffer_.size())); std::streamsize got = input->gcount(); std::streamsize i;
        if (got <= 0) { if (!fast_line_.empty() && !process_fast_line(fast_line_, fast_line_overflow_, fast_line_begin_, fast_previous_blank_, error)) return false; if (fast_have_candidate_) { error = "provisional index ended with incomplete tag block"; return fallback_to_general(error); } finish_current(file_size_); complete_ = true; scan_ticks_ += std::clock() - slice; report_diagnostics(true); return true; }
        bytes_scanned_ += static_cast<PgnOffset>(got);
        for (i = 0; i < got; ++i) { char c = fast_buffer_[static_cast<std::size_t>(i)]; if (c == '\n') { if (!process_fast_line(fast_line_, fast_line_overflow_, fast_line_begin_, fast_previous_blank_, error)) return false; fast_previous_blank_ = !fast_line_overflow_ && blank_line(fast_line_); fast_line_.clear(); fast_line_overflow_ = false; ++line_number_; ++lines_scanned_; fast_line_begin_ = block_begin + static_cast<PgnOffset>(i) + 1; } else if (!fast_line_overflow_) { if (fast_line_.size() < kMaximumFastLine) fast_line_ += c; else fast_line_overflow_ = true; } }
        if (bytes_scanned_ >= next_diagnostic_bytes_) { report_diagnostics(false); next_diagnostic_bytes_ += kDiagnosticInterval; }
        if (maximum_milliseconds != 0 && static_cast<unsigned long>((std::clock() - slice) * 1000.0 / CLOCKS_PER_SEC) >= maximum_milliseconds) break;
    }
    scan_ticks_ += std::clock() - slice; return true;
}
bool PgnIndex::step(std::size_t maximum_lines, std::string& error) { return step_internal(maximum_lines, 0, error); }
bool PgnIndex::step_for_milliseconds(unsigned long maximum_milliseconds, std::string& error) { return step_internal(static_cast<std::size_t>(-1), maximum_milliseconds, error); }
bool PgnIndex::step_internal(std::size_t maximum_lines, unsigned long maximum_milliseconds, std::string& error) {
    std::ifstream* input = static_cast<std::ifstream*>(input_); std::size_t lines = 0; std::clock_t slice = std::clock(); if (complete_ || failed_) return complete_; if (fast_mode_) return step_fast(maximum_milliseconds, error); ++chunks_;
    try { while (lines < maximum_lines) {
        std::streampos start = input->tellg(); std::string line;
        input->getline(&general_buffer_[0], static_cast<std::streamsize>(general_buffer_.size()));
        if (input->fail() && !input->eof()) {
            failed_ = true;
            error = "physical line exceeds 1 MiB bounded general-index limit";
            return false;
        }
        if (!*input && input->gcount() == 0) { finish_current(file_size_); if (!have_game_ && file_size_ != 0 && !saw_tag_like_) { if (!append_entry(0, error)) return false; finish_current(file_size_); } complete_ = true; scan_ticks_ += std::clock() - slice; report_diagnostics(true); return true; }
        line.assign(&general_buffer_[0]);
        ++lines; ++lines_scanned_; ++line_number_; if (start < 0) { failed_ = true; error = "invalid stream offset"; return false; } { std::streampos after = input->tellg(); bytes_scanned_ = after < 0 ? file_size_ : static_cast<PgnOffset>(after); } if (bytes_scanned_ > file_size_) bytes_scanned_ = file_size_;
        if (line_is_tag(line, static_cast<PgnOffset>(start) == 0)) { saw_tag_like_ = true; if (!have_game_) { if (!append_entry(static_cast<PgnOffset>(start), error)) return false; } else if (saw_movetext_ || saw_result_) { finish_current(static_cast<PgnOffset>(start)); if (!append_entry(static_cast<PgnOffset>(start), error)) return false; saw_movetext_ = saw_result_ = false; } }
        else { bool result = line_has_top_level_result(line), content = line_has_non_comment_content(line, in_brace_comment_); if (!have_game_ && content && !append_entry(static_cast<PgnOffset>(start), error)) return false; if (saw_result_ && content) { finish_current(static_cast<PgnOffset>(start)); if (!append_entry(static_cast<PgnOffset>(start), error)) return false; saw_movetext_ = saw_result_ = false; } if (have_game_ && content) saw_movetext_ = true; if (result) saw_result_ = true; }
        { bool semi = false; std::size_t i; for (i = 0; i < line.size(); ++i) { if (in_brace_comment_) { if (line[i] == '}') in_brace_comment_ = false; continue; } if (semi) continue; if (line[i] == '{') in_brace_comment_ = true; else if (line[i] == ';') semi = true; else if (line[i] == '(') ++rav_depth_; else if (line[i] == ')' && rav_depth_ > 0) --rav_depth_; } }
        if (maximum_milliseconds != 0 && (lines & 255U) == 0U && static_cast<unsigned long>((std::clock() - slice) * 1000.0 / CLOCKS_PER_SEC) >= maximum_milliseconds) break;
    } } catch (const std::bad_alloc&) { failed_ = true; error = "insufficient memory while indexing"; scan_ticks_ += std::clock() - slice; return false; }
    scan_ticks_ += std::clock() - slice; return true;
}
bool PgnIndex::complete() const { return complete_; } bool PgnIndex::failed() const { return failed_; } bool PgnIndex::provisional() const { return fast_mode_; }
PgnOffset PgnIndex::bytes_scanned() const { return bytes_scanned_; } PgnOffset PgnIndex::file_size() const { return file_size_; } PgnOffset PgnIndex::lines_scanned() const { return lines_scanned_; } std::size_t PgnIndex::chunks() const { return chunks_; }
double PgnIndex::scan_milliseconds() const { return static_cast<double>(scan_ticks_) * 1000.0 / CLOCKS_PER_SEC; } const std::vector<PgnIndexEntry>& PgnIndex::entries() const { return entries_; }
std::size_t PgnIndex::complete_entry_count() const { std::size_t i, count = 0; for (i = 0; i < entries_.size(); ++i) if (entries_[i].end > entries_[i].begin) ++count; return count; }
std::size_t PgnIndex::entry_storage_bytes() const { return entries_.capacity() * sizeof(PgnIndexEntry); } std::size_t PgnIndex::scanner_buffer_bytes() const { return fast_buffer_.capacity() + general_buffer_.capacity() + fast_line_.capacity(); }
}
