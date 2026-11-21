#ifndef GAMBITREE_NOTATION_SEARCH_H
#define GAMBITREE_NOTATION_SEARCH_H

#include <string>
#include <vector>

#include "gambitree/notation_layout.h"

namespace gambitree {

struct NotationSearchHighlight {
    NotationTextRangeRect rect;
    bool current;
};

/* Search projection for the already formatted, ordered notation items. */
class NotationSearch {
public:
    NotationSearch();

    bool visible() const;
    void open();
    void close();
    bool select_all() const;
    void set_select_all(bool selected);
    const std::string& query() const;
    void set_query(const std::string& query, const NotationLayout& layout,
                   int scroll_offset, int viewport_height);
    void refresh(const NotationLayout& layout);
    bool move(int direction);
    bool has_current() const;
    std::size_t result_count() const;
    std::size_t current_result() const;
    int scroll_to_current(const NotationLayout& layout, int scroll_offset,
                          int viewport_height) const;
    std::vector<NotationSearchHighlight> highlights(
        const NotationLayout& layout, int first_y, int last_y) const;

    /* Search folds ASCII/Latin-1 case and chess figurines to SAN letters. */
    static std::string normalized_query(const std::string& text);

private:
    struct SourceByte {
        std::size_t item;
        std::size_t begin;
        std::size_t end;
        SourceByte();
    };
    struct Match {
        std::size_t begin;
        std::size_t end;
        int top;
        int bottom;
        Match();
    };

    bool visible_;
    bool select_all_;
    std::string query_;
    std::string projection_;
    std::vector<SourceByte> source_bytes_;
    std::vector<Match> matches_;
    std::size_t current_;
    bool has_current_;
    uint64_t layout_generation_;

    void build_projection(const NotationLayout& layout);
    void build_matches(const NotationLayout& layout, int scroll_offset,
                       int viewport_height, bool preserve_current);
    static bool is_move_number(const NotationItem& item);
};

}

#endif
