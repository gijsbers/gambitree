#ifndef GAMBITREE_PGN_LIBRARY_H
#define GAMBITREE_PGN_LIBRARY_H

#include <cstddef>
#include <stdint.h>
#include <set>
#include <string>
#include <vector>

#include "gambitree/pgn_index.h"
#include "gambitree/pgn_delete.h"

namespace gambitree {

class PgnFile {
public:
    std::string path;
    PgnIndex index;
    PgnGame* loaded_game;
    std::size_t loaded_index;
    bool reported_empty;
    uint64_t session_identity;
    PgnDeleteBaseline delete_baseline;
    std::set<std::size_t> warning_reported_games;
    explicit PgnFile(const std::string& file_path);
    ~PgnFile();
private:
    PgnFile(const PgnFile&);
    PgnFile& operator=(const PgnFile&);
};

class PgnLibrary {
public:
    PgnLibrary();
    ~PgnLibrary();
    bool load(const std::vector<std::string>& paths, std::vector<std::string>& errors);
    const std::vector<std::string>& requested_paths() const;
    bool game_tags(std::size_t index, std::vector<PgnTag>& tags,
                   std::string& error) const;
    bool select_game(std::size_t index);
    bool select_file_path(const std::string& path);
    bool step_indexing(std::size_t maximum_lines, std::vector<std::string>& errors);
    bool step_indexing_for_milliseconds(unsigned long maximum_milliseconds, std::vector<std::string>& errors);
    bool indexing_complete() const;
    std::size_t indexed_file_count() const;
    std::size_t indexed_game_count() const;
    PgnOffset indexed_lines() const;
    std::size_t indexing_chunks() const;
    double indexing_scan_milliseconds() const;
    bool empty() const;
    std::size_t file_count() const;
    std::size_t game_count() const;
    std::size_t file_index() const;
    std::size_t game_index() const;
    const PgnFile* current_file() const;
    PgnGame* current_game() const;
    bool materialize_current(std::string& error);
    bool current_game_deletion_eligible(std::string& reason) const;
    bool delete_current_game(bool& committed, bool& file_removed,
                             bool& sync_warning, std::string& error);
    bool previous_file(); bool next_file(); bool previous_game(); bool next_game();
    bool can_previous_file() const; bool can_next_file() const;
    bool can_previous_game() const; bool can_next_game() const;
    static std::size_t maximum_file_size();
    static bool acceptable_file_size(std::size_t bytes);
private:
    std::vector<PgnFile*> files_;
    std::vector<std::string> requested_paths_;
    std::size_t file_index_;
    std::size_t game_index_;
    std::size_t indexing_cursor_;
    void clear();
    bool select_file(std::size_t index);
    PgnLibrary(const PgnLibrary&);
    PgnLibrary& operator=(const PgnLibrary&);
};

}

#endif
