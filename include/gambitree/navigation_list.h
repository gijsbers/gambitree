#ifndef GAMBITREE_NAVIGATION_LIST_H
#define GAMBITREE_NAVIGATION_LIST_H

#include <cstddef>
#include <string>
#include <vector>
#include "gambitree/pgn.h"

namespace gambitree {

enum NavigationListKind { NAVIGATION_LIST_CLOSED, NAVIGATION_LIST_GAMES,
                          NAVIGATION_LIST_FILES };

class NavigationListState {
public:
    NavigationListState();
    void open(NavigationListKind kind, const std::vector<std::string>& items,
              std::size_t current_index);
    void replace_items(const std::vector<std::string>& items,
                      std::size_t current_index);
    void append_items(const std::vector<std::string>& items,
                      std::size_t current_index);
    void close();
    bool visible() const;
    NavigationListKind kind() const;
    const std::string& query() const;
    void set_query(const std::string& query);
    void append_query(const std::string& text);
    void backspace_query();
    const std::vector<std::string>& items() const;
    const std::vector<std::size_t>& matches() const;
    std::size_t selected_match() const;
    std::size_t first_match() const;
    std::size_t selected_source_index() const;
    bool has_selection() const;
    void set_visible_rows(std::size_t rows);
    std::size_t visible_rows() const;
    void move(int amount);
    void page(int direction);
    void home();
    void end();
    bool select_match_at(std::size_t visible_row);
private:
    NavigationListKind kind_;
    std::string query_;
    std::vector<std::string> items_;
    std::vector<std::size_t> matches_;
    std::size_t selected_;
    std::size_t first_;
    std::size_t current_index_;
    std::size_t visible_rows_;
    void rebuild_matches(std::size_t preferred_source);
    void keep_selection_visible();
};

std::string navigation_game_label(std::size_t game_number,
                                  const std::vector<PgnTag>& tags,
                                  bool figurines = false,
                                  unsigned int figurine_mask = 0);

}

#endif
