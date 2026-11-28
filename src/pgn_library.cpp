#include "gambitree/pgn_library.h"

#include <fstream>
#include <iterator>
#include <new>
#include <limits>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <cstring>
#include <cctype>
#include <time.h>

namespace gambitree {

namespace {

uint64_t next_pgn_file_session_identity = 1;

uint64_t monotonic_microseconds()
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * static_cast<uint64_t>(1000000) +
           static_cast<uint64_t>(now.tv_nsec / 1000L);
}

uint64_t allocate_pgn_file_session_identity()
{
    uint64_t identity = next_pgn_file_session_identity++;
    if (identity == 0)
        identity = next_pgn_file_session_identity++;
    return identity;
}

}

PgnFile::PgnFile(const std::string& file_path)
    : path(file_path), loaded_game(0), loaded_index(0), reported_empty(false),
      session_identity(allocate_pgn_file_session_identity()) {}
PgnFile::~PgnFile() { delete loaded_game; }
PgnLibrary::PgnLibrary() : file_index_(0), game_index_(0), indexing_cursor_(0) {}
PgnLibrary::~PgnLibrary() { clear(); }

std::size_t PgnLibrary::maximum_file_size() { return static_cast<std::size_t>(64) * 1024U * 1024U; }
bool PgnLibrary::acceptable_file_size(std::size_t bytes) { return bytes <= maximum_file_size(); }

void PgnLibrary::clear()
{
    std::size_t i;
    for (i = 0; i < files_.size(); ++i) delete files_[i];
    files_.clear(); file_index_ = game_index_ = indexing_cursor_ = 0;
}

bool PgnLibrary::load(const std::vector<std::string>& paths, std::vector<std::string>& errors)
{
    std::size_t i;
    clear(); errors.clear(); requested_paths_ = paths;
    for (i = 0; i < paths.size(); ++i) {
        PgnFile* file = 0;
        std::string error;
        try { file = new PgnFile(paths[i]); }
        catch (const std::bad_alloc&) { errors.push_back(paths[i] + ": insufficient memory while opening index"); continue; }
        file->delete_baseline = capture_pgn_delete_baseline(paths[i]);
        if (!file->index.open(paths[i], error)) { errors.push_back(paths[i] + ": " + error); delete file; continue; }
        try { files_.push_back(file); }
        catch (const std::bad_alloc&) { delete file; errors.push_back(paths[i] + ": insufficient memory while retaining index"); }
    }
    return !files_.empty();
}

const std::vector<std::string>& PgnLibrary::requested_paths() const
{ return requested_paths_; }

bool PgnLibrary::game_tags(std::size_t index, std::vector<PgnTag>& tags,
                           std::string& error) const
{
    const PgnFile* file = current_file();
    const PgnIndexEntry* entry;
    std::ifstream input;
    std::string bytes;
    PgnOffset length;
    std::size_t offset = 0;
    tags.clear();
    error.clear();
    if (!file || index >= game_count() || index >= file->index.entries().size()) {
        error = "game is not indexed";
        return false;
    }
    entry = &file->index.entries()[index];
    if (entry->end < entry->begin) {
        error = "invalid indexed game range";
        return false;
    }
    length = entry->end - entry->begin;
    input.open(file->path.c_str(), std::ios::in | std::ios::binary);
    if (!input) { error = "cannot reopen PGN file"; return false; }
    input.seekg(static_cast<std::streamoff>(entry->begin));
    if (!input) { error = "cannot seek to indexed game tags"; return false; }
    /* Read only the opening tag block, never the movetext or move tree. */
    {
        std::string line;
        std::size_t consumed = 0;
        bool saw_tag = false;
        try {
            while (consumed < 256U * 1024U && consumed < length &&
                   std::getline(input, line)) {
                consumed += line.size() + 1;
                if (!line.empty() && line[line.size() - 1] == '\r')
                    line.erase(line.size() - 1);
                std::size_t first = 0;
                while (first < line.size() &&
                       std::isspace(static_cast<unsigned char>(line[first])))
                    ++first;
                if (first == line.size()) continue;
                if (first == 0 && line.size() >= 3 &&
                    static_cast<unsigned char>(line[0]) == 0xef &&
                    static_cast<unsigned char>(line[1]) == 0xbb &&
                    static_cast<unsigned char>(line[2]) == 0xbf)
                    first = 3;
                if (first < line.size() && line[first] == '[') {
                    bytes += line.substr(first);
                    bytes += '\n';
                    saw_tag = true;
                } else if (saw_tag) {
                    break;
                }
            }
        } catch (const std::bad_alloc&) {
            error = "insufficient memory reading game tags";
            return false;
        }
    }
    while (offset < bytes.size()) {
        while (offset < bytes.size() &&
               std::isspace(static_cast<unsigned char>(bytes[offset]))) ++offset;
        if (offset == bytes.size() || bytes[offset] != '[') break;
        const std::size_t token_start = ++offset;
        bool quoted = false;
        bool escaped = false;
        while (offset < bytes.size()) {
            const char c = bytes[offset++];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == ']') break;
        }
        if (offset > bytes.size() || bytes[offset - 1] != ']') break;
        const std::string token = bytes.substr(token_start,
                                               offset - token_start - 1);
        std::size_t cursor = 0;
        while (cursor < token.size() &&
               std::isspace(static_cast<unsigned char>(token[cursor]))) ++cursor;
        const std::size_t name_start = cursor;
        while (cursor < token.size() &&
               !std::isspace(static_cast<unsigned char>(token[cursor]))) ++cursor;
        if (cursor == name_start) continue;
        PgnTag tag;
        tag.name = token.substr(name_start, cursor - name_start);
        while (cursor < token.size() &&
               std::isspace(static_cast<unsigned char>(token[cursor]))) ++cursor;
        if (cursor >= token.size() || token[cursor++] != '"') continue;
        while (cursor < token.size()) {
            char c = token[cursor++];
            if (c == '\\' && cursor < token.size())
                tag.value += token[cursor++];
            else if (c == '"') break;
            else tag.value += c;
        }
        tags.push_back(tag);
    }
    return true;
}

bool PgnLibrary::select_game(std::size_t index)
{
    if (empty() || index >= game_count()) return false;
    game_index_ = index;
    return true;
}

bool PgnLibrary::select_file_path(const std::string& path)
{
    std::size_t index;
    for (index = 0; index < files_.size(); ++index) {
        if (files_[index]->path == path)
            return select_file(index);
    }
    return false;
}

bool PgnLibrary::step_indexing(std::size_t maximum_lines, std::vector<std::string>& errors)
{
    PgnFile* file;
    std::string error;
    if (files_.empty() || indexing_complete()) return true;
    if (indexing_cursor_ >= files_.size()) indexing_cursor_ = 0;
    file = files_[indexing_cursor_];
    if (!file->index.complete() && !file->index.failed() && !file->index.step(maximum_lines, error))
        errors.push_back(file->path + ": indexing error: " + error);
    if (file->index.complete() && file->index.entries().empty() && !file->reported_empty) {
        errors.push_back(file->path + ": no indexable PGN games");
        file->reported_empty = true;
    }
    ++indexing_cursor_;
    return true;
}

bool PgnLibrary::step_indexing_for_milliseconds(unsigned long maximum_milliseconds, std::vector<std::string>& errors)
{
    PgnFile* file;
    std::string error;
    if (files_.empty() || indexing_complete()) return true;
    if (indexing_cursor_ >= files_.size()) indexing_cursor_ = 0;
    file = files_[indexing_cursor_];
    if (!file->index.complete() && !file->index.failed() &&
        !file->index.step_for_milliseconds(maximum_milliseconds, error))
        errors.push_back(file->path + ": indexing error: " + error);
    if (file->index.complete() && file->index.entries().empty() && !file->reported_empty) {
        errors.push_back(file->path + ": no indexable PGN games");
        file->reported_empty = true;
    }
    ++indexing_cursor_;
    return true;
}

bool PgnLibrary::indexing_complete() const
{
    std::size_t i;
    for (i = 0; i < files_.size(); ++i)
        if (!files_[i]->index.complete() && !files_[i]->index.failed()) return false;
    return true;
}

std::size_t PgnLibrary::indexed_file_count() const
{ std::size_t i, count = 0; for (i = 0; i < files_.size(); ++i) if (files_[i]->index.complete()) ++count; return count; }
std::size_t PgnLibrary::indexed_game_count() const
{ std::size_t i, count = 0; for (i = 0; i < files_.size(); ++i) count += files_[i]->index.complete_entry_count(); return count; }
PgnOffset PgnLibrary::indexed_lines() const
{ std::size_t i; PgnOffset result = 0; for (i = 0; i < files_.size(); ++i) result += files_[i]->index.lines_scanned(); return result; }
std::size_t PgnLibrary::indexing_chunks() const
{ std::size_t i, result = 0; for (i = 0; i < files_.size(); ++i) result += files_[i]->index.chunks(); return result; }
double PgnLibrary::indexing_scan_milliseconds() const
{ std::size_t i; double result = 0.0; for (i = 0; i < files_.size(); ++i) result += files_[i]->index.scan_milliseconds(); return result; }
bool PgnLibrary::empty() const { return files_.empty(); }
std::size_t PgnLibrary::file_count() const { return files_.size(); }
std::size_t PgnLibrary::game_count() const { return empty() ? 0 : files_[file_index_]->index.complete_entry_count(); }
std::size_t PgnLibrary::file_index() const { return file_index_; }
std::size_t PgnLibrary::game_index() const { return game_index_; }
const PgnFile* PgnLibrary::current_file() const { return empty() ? 0 : files_[file_index_]; }
PgnGame* PgnLibrary::current_game() const
{
    PgnFile* file = empty() ? 0 : files_[file_index_];
    return file && file->loaded_game && file->loaded_index == game_index_ ? file->loaded_game : 0;
}

bool PgnLibrary::materialize_current(std::string& error)
{
    error.clear();
    PgnFile* file;
    const PgnIndexEntry* entry;
    std::ifstream input;
    std::string data;
    PgnDocument document;
    if (empty() || game_index_ >= game_count()) { error = "selected game is not indexed yet"; return false; }
    file = files_[file_index_]; entry = &file->index.entries()[game_index_];
    PgnOffset range;
    bool diagnostics = std::getenv("GAMBITREE_DIAGNOSTICS") != 0;
    uint64_t started = monotonic_microseconds();
    uint64_t read_finished = 0;
    uint64_t parse_finished = 0;
    if (entry->end < entry->begin) { error = "invalid indexed game range"; return false; }
    range = entry->end - entry->begin;
    if (range > static_cast<PgnOffset>(maximum_file_size())) { error = "game exceeds 64 MiB materialization limit"; return false; }
    if (range > static_cast<PgnOffset>(std::numeric_limits<std::size_t>::max()) ||
        range > static_cast<PgnOffset>(std::numeric_limits<std::streamsize>::max()) ||
        entry->begin > static_cast<PgnOffset>(std::numeric_limits<std::streamoff>::max())) {
        error = "indexed game range is not representable on this platform"; return false;
    }
    if (file->loaded_game && file->loaded_index == game_index_) return true;
    try {
        data.resize(static_cast<std::size_t>(range));
    } catch (const std::bad_alloc&) { error = "insufficient memory while loading game"; return false; }
    input.open(file->path.c_str(), std::ios::in | std::ios::binary);
    if (!input) { error = "cannot reopen file"; return false; }
    input.seekg(static_cast<std::streamoff>(entry->begin));
    if (!input || (!data.empty() && !input.read(&data[0], static_cast<std::streamsize>(data.size())))) { error = "cannot read indexed game range"; return false; }
    read_finished = monotonic_microseconds();
    try {
        PgnParser::parse_for_viewing(data, document, entry->begin == 0);
    } catch (const std::bad_alloc&) {
        error = "insufficient memory while parsing game";
        return false;
    }
    parse_finished = monotonic_microseconds();
    const bool already_reported = file->warning_reported_games.find(game_index_) !=
                                  file->warning_reported_games.end();
    if (document.skipped_variation_count != 0) {
        if (!already_reported) {
            std::size_t warning_index;
            std::size_t displayed = document.warnings.size();
            for (warning_index = 0; warning_index < displayed; ++warning_index) {
                const PgnWarning& warning = document.warnings[warning_index];
                std::cerr << "gambitree: " << file->path << ": game "
                          << game_index_ + 1 << ", line " << warning.line
                          << ", column " << warning.column << ": "
                          << warning.message << "\n";
            }
            if (document.skipped_variation_count > displayed) {
                std::cerr << "gambitree: " << file->path << ": game "
                          << game_index_ + 1 << ": skipped "
                          << document.skipped_variation_count - displayed
                          << " additional variation(s) truncated after an illegal move\n";
            }
            file->warning_reported_games.insert(game_index_);
        }
    }
    if (document.games.empty() || (file->index.provisional() && !document.errors.empty())) {
        if (file->index.provisional()) {
            std::string fallback_error;
            if (!file->index.fallback_to_general(fallback_error)) {
                error = "provisional game boundary could not be validated; " + fallback_error;
                return false;
            }
            game_index_ = 0;
            error = "provisional game boundary could not be validated; switched to robust reindexing";
            return false;
        }
        if (!document.errors.empty()) {
            const PgnError& e = document.errors[0];
            const std::size_t slash = file->path.find_last_of('/');
            std::ostringstream text;
            text << file->path.substr(slash == std::string::npos ? 0 : slash + 1)
                 << ": game " << game_index_ + 1 << ": " << e.message
                 << " (game line " << e.line << ", column " << e.column
                 << "; game starts at byte " << entry->begin << ")";
            error = text.str();
        }
        else error = "no valid PGN game in indexed range";
        return false;
    }
    delete file->loaded_game;
    file->loaded_game = document.games[0];
    document.games[0] = 0;
    file->loaded_index = game_index_;
    if (file->loaded_game->parse_status != PGN_COMPLETE &&
        game_index_ < static_cast<std::size_t>(std::numeric_limits<int>::max()))
        file->loaded_game->parse_error.game_number = static_cast<int>(game_index_ + 1);
    if (file->loaded_game->parse_status == PGN_PARTIAL && !already_reported) {
        const PgnGame& game = *file->loaded_game;
        const Position& before = game.error_position == game.root ?
            game.initial_position : game.error_position->position_after;
        std::cerr << "gambitree: " << file->path << ": game " << game_index_ + 1
                  << ": " << game.parse_warning() << " FEN " << before.to_fen() << "\n";
        file->warning_reported_games.insert(game_index_);
    }
    if (diagnostics) {
        double read_ms = static_cast<double>(read_finished - started) / 1000.0;
        double parse_ms = static_cast<double>(parse_finished - read_finished) / 1000.0;
        double milliseconds = static_cast<double>(parse_finished - started) / 1000.0;
        std::cerr << "gambitree: diagnostic: lazy game materialization " << milliseconds << " ms, "
                  << "read_ms=" << read_ms << " parse_ms=" << parse_ms << ", "
                  << range << " bytes, file " << file->path << ", game " << game_index_ + 1 << "\n";
    }
    return true;
}

bool PgnLibrary::current_game_deletion_eligible(std::string& reason) const
{
    reason.clear();
    const PgnFile* file = current_file();
    if (!file) {
        reason = "No PGN file is selected.";
        return false;
    }
    if (!file->index.complete()) {
        reason = "Wait until this PGN file has finished indexing before deleting a game.";
        return false;
    }
    if (game_index_ >= game_count()) {
        reason = "There is no indexed game selected.";
        return false;
    }
    if (!current_game()) {
        reason = "The selected game must be loaded successfully before deletion.";
        return false;
    }
    if (!file->delete_baseline.eligible) {
        reason = file->delete_baseline.reason;
        return false;
    }
    if (file->delete_baseline.size != file->index.file_size()) {
        reason = "The PGN file changed while it was being indexed; reopen it before deleting.";
        return false;
    }
    if (!file->index.boundaries_reliable(game_index_) ||
        (game_count() == 1 && !file->index.whole_file_is_single_game())) {
        reason = "Gambitree cannot safely tell where this game starts and ends. It has not been removed.";
        return false;
    }
    return true;
}

bool PgnLibrary::delete_current_game(bool& committed, bool& file_removed,
                                     bool& sync_warning, std::string& error)
{
    PgnFile* file;
    std::size_t old_file_index;
    std::size_t old_game_index;
    std::size_t old_game_count;
    PgnDeleteResult result;
    std::string reason;
    committed = false;
    file_removed = false;
    sync_warning = false;
    error.clear();
    if (!current_game_deletion_eligible(reason)) {
        error = reason;
        return false;
    }
    file = files_[file_index_];
    old_file_index = file_index_;
    old_game_index = game_index_;
    old_game_count = game_count();
    if (old_game_count == 0 || old_game_index >= file->index.entries().size()) {
        error = "The selected game has no reliable indexed byte range.";
        return false;
    }
    {
        const PgnIndexEntry& entry = file->index.entries()[old_game_index];
        // Whole-file removal also requires the indexer's independent coverage proof.
        const bool whole_file = old_game_count == 1 && file->index.whole_file_is_single_game();
        const PgnOffset begin = whole_file ? 0 : entry.begin;
        if (!pgn_delete_range(file->path, file->delete_baseline, begin,
                              entry.end, whole_file, result)) {
            error = result.message;
            return false;
        }
    }
    committed = result.committed;
    file_removed = result.file_removed;
    sync_warning = result.sync_warning;
    error = result.message;
    delete file->loaded_game;
    file->loaded_game = 0;
    file->warning_reported_games.clear();
    file->loaded_index = 0;
    if (file_removed) {
        delete file;
        files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(old_file_index));
        if (files_.empty()) {
            file_index_ = game_index_ = indexing_cursor_ = 0;
            return true;
        }
        if (old_file_index < files_.size())
            file_index_ = old_file_index;
        else
            file_index_ = files_.size() - 1;
        game_index_ = 0;
        if (files_[file_index_]->index.complete_entry_count() == 0) {
            std::size_t distance;
            bool found = false;
            for (distance = 0; distance < files_.size(); ++distance) {
                if (file_index_ + distance < files_.size() &&
                    files_[file_index_ + distance]->index.complete_entry_count() != 0) {
                    file_index_ += distance;
                    found = true;
                    break;
                }
                if (distance != 0 && file_index_ >= distance &&
                    files_[file_index_ - distance]->index.complete_entry_count() != 0) {
                    file_index_ -= distance;
                    found = true;
                    break;
                }
            }
            if (!found)
                file_index_ = old_file_index < files_.size()
                    ? old_file_index : files_.size() - 1;
        }
        game_index_ = 0;
        indexing_cursor_ = files_.empty() ? 0 : file_index_;
        return true;
    }

    {
        std::string open_error;
        file->delete_baseline = capture_pgn_delete_baseline(file->path);
        bool replacement_matches = result.has_replacement_fingerprint &&
            file->delete_baseline.eligible &&
            file->delete_baseline.device == result.replacement_device &&
            file->delete_baseline.inode == result.replacement_inode &&
            file->delete_baseline.size == result.replacement_size &&
            file->delete_baseline.mode == result.replacement_mode &&
            file->delete_baseline.mtime_seconds ==
                result.replacement_mtime_seconds &&
            file->delete_baseline.mtime_nanoseconds ==
                result.replacement_mtime_nanoseconds &&
            std::memcmp(file->delete_baseline.sha256,
                        result.replacement_sha256,
                        sizeof(result.replacement_sha256)) == 0;
        if (!file->delete_baseline.eligible || !replacement_matches ||
            !file->index.open(file->path, open_error)) {
            std::string reindex_error;
            if (!file->delete_baseline.eligible)
                reindex_error = file->delete_baseline.reason;
            else if (!replacement_matches)
                reindex_error = "the committed file changed before it could "
                                "become the new trusted baseline";
            else
                reindex_error = open_error;
            delete file;
            files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(old_file_index));
            if (!error.empty())
                error += "; ";
            error += "game was removed from disk, but the changed source could "
                     "not be reindexed and was removed from the loaded list: " +
                     reindex_error;
            if (files_.empty()) {
                file_index_ = game_index_ = indexing_cursor_ = 0;
                return true;
            }
            file_index_ = old_file_index < files_.size()
                ? old_file_index : files_.size() - 1;
            game_index_ = 0;
            indexing_cursor_ = file_index_;
            return true;
        }
        while (!file->index.complete() && !file->index.failed()) {
            std::string index_error;
            if (!file->index.step(static_cast<std::size_t>(-1), index_error)) {
                open_error = index_error;
                break;
            }
        }
        if (file->index.complete() && !file->index.failed() &&
            !pgn_delete_baseline_matches(file->path,
                                         file->delete_baseline,
                                         open_error)) {
            open_error = "the PGN changed while the committed file was "
                         "being reindexed";
        }
        if (file->index.failed() || !file->index.complete() ||
            file->index.complete_entry_count() == 0) {
            std::string reindex_error = open_error.empty()
                ? "no valid games remain" : open_error;
            delete file;
            files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(old_file_index));
            if (!error.empty())
                error += "; ";
            error += "game was removed from disk, but the changed source "
                     "could not be reindexed and was removed from the loaded "
                     "list: " + reindex_error;
            if (files_.empty()) {
                file_index_ = game_index_ = indexing_cursor_ = 0;
                return true;
            }
            file_index_ = old_file_index < files_.size()
                ? old_file_index : files_.size() - 1;
            game_index_ = 0;
            indexing_cursor_ = file_index_;
            return true;
        }
        file_index_ = old_file_index;
        game_index_ = old_game_index < file->index.complete_entry_count()
            ? old_game_index : file->index.complete_entry_count() - 1;
        indexing_cursor_ = old_file_index;
    }
    return true;
}

bool PgnLibrary::select_file(std::size_t index) { if (index >= files_.size()) return false; file_index_ = index; game_index_ = 0; return true; }
bool PgnLibrary::previous_file()
{
    std::size_t index = file_index_;
    while (index > 0) { --index; if (files_[index]->index.complete_entry_count() != 0) return select_file(index); }
    return false;
}
bool PgnLibrary::next_file()
{
    std::size_t index;
    for (index = file_index_ + 1; index < files_.size(); ++index)
        if (files_[index]->index.complete_entry_count() != 0) return select_file(index);
    return false;
}
bool PgnLibrary::previous_game() { if (!can_previous_game()) return false; --game_index_; return true; }
bool PgnLibrary::next_game() { if (!can_next_game()) return false; ++game_index_; return true; }
bool PgnLibrary::can_previous_file() const { std::size_t i = file_index_; while (i > 0) { --i; if (files_[i]->index.complete_entry_count() != 0) return true; } return false; }
bool PgnLibrary::can_next_file() const { std::size_t i; for (i = file_index_ + 1; i < files_.size(); ++i) if (files_[i]->index.complete_entry_count() != 0) return true; return false; }
bool PgnLibrary::can_previous_game() const { return !empty() && game_index_ > 0; }
bool PgnLibrary::can_next_game() const { return !empty() && game_index_ + 1 < game_count(); }

}
