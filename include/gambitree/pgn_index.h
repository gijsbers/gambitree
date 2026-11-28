#ifndef GAMBITREE_PGN_INDEX_H
#define GAMBITREE_PGN_INDEX_H

#include <cstddef>
#include <ctime>
#include <stdint.h>
#include <string>
#include <vector>

#include "gambitree/pgn.h"

namespace gambitree {

typedef uint64_t PgnOffset;

struct PgnIndexEntry {
    PgnOffset begin;
    PgnOffset end;
    PgnIndexEntry();
};

/*
 * Streaming game-range index.  Conventional databases at least 128 MiB use a
 * provisional 1 MiB block scanner; entries are only two 64-bit offsets so
 * no tags or movetext accumulate in that route.  The general scanner tracks
 * names only within the current header block to detect ambiguous boundaries.
 * Exact tags, moves and PGN semantics remain the responsibility of lazy
 * materialization.
 */
class PgnIndex {
public:
    PgnIndex();
    ~PgnIndex();
    bool open(const std::string& path, std::string& error);
    bool step(std::size_t maximum_lines, std::string& error);
    bool step_for_milliseconds(unsigned long maximum_milliseconds, std::string& error);
    bool complete() const;
    bool failed() const;
    PgnOffset bytes_scanned() const;
    PgnOffset file_size() const;
    PgnOffset lines_scanned() const;
    std::size_t chunks() const;
    double scan_milliseconds() const;
    const std::vector<PgnIndexEntry>& entries() const;
    std::size_t complete_entry_count() const;
    bool provisional() const;
    bool boundaries_reliable(std::size_t game) const;
    bool whole_file_is_single_game() const;
    bool fallback_to_general(std::string& error);
    std::size_t entry_storage_bytes() const;
    std::size_t scanner_buffer_bytes() const;
    static PgnOffset provisional_threshold(); /* 128 MiB */
private:
    std::string path_;
    void* input_;
    PgnOffset file_size_;
    PgnOffset bytes_scanned_;
    std::vector<PgnIndexEntry> entries_;
    // Kept separately so the large-file index retains its 16-byte entries.
    std::vector<bool> reliable_ranges_;
    bool complete_;
    bool failed_;
    bool in_brace_comment_;
    bool have_game_;
    bool saw_movetext_;
    bool saw_result_;
    bool saw_tag_like_;
    unsigned int rav_depth_;
    bool in_tag_;
    bool tag_quoted_;
    bool tag_escaped_;
    bool prefix_trivia_;
    bool framing_content_;
    bool framing_movetext_;
    bool framing_result_;
    unsigned long framing_move_number_;
    bool framing_have_move_number_;
    bool framing_black_number_;
    bool framing_move_since_number_;
    std::vector<std::string> framing_tags_;
    PgnOffset line_number_;
    PgnOffset lines_scanned_;
    std::size_t chunks_;
    std::clock_t started_;
    std::clock_t scan_ticks_;
    bool fast_mode_;
    bool fast_previous_blank_;
    bool fast_have_candidate_;
    PgnOffset fast_candidate_begin_;
    PgnOffset fast_line_begin_;
    std::string fast_line_;
    bool fast_line_overflow_;
    std::vector<char> fast_buffer_;
    std::vector<char> general_buffer_;
    PgnOffset next_diagnostic_bytes_;
    bool framing_neutral() const;
    void mark_uncertain();
    bool finish_framing_token(const std::string& token);
    bool scan_structure(const std::string& line, bool first_line);
    void finish_current(PgnOffset end);
    bool append_entry(PgnOffset begin, std::string& error);
    bool step_fast(unsigned long maximum_milliseconds, std::string& error);
    bool process_fast_line(const std::string& line, bool overflow, PgnOffset begin,
                           bool previous_blank, std::string& error);
    bool choose_fast_path(const std::string& path) const;
    void report_diagnostics(bool final) const;
    bool step_internal(std::size_t maximum_lines, unsigned long maximum_milliseconds, std::string& error);
    PgnIndex(const PgnIndex&);
    PgnIndex& operator=(const PgnIndex&);
};

}

#endif
