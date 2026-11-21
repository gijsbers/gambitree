#ifndef GAMBITREE_NAVIGATION_H
#define GAMBITREE_NAVIGATION_H

#include <cstddef>
#include <map>
#include <stdint.h>
#include <vector>

#include "gambitree/pgn.h"

namespace gambitree {

/* True when node is the last move reached by following child zero from the
   game root (the played main line), rather than a terminal RAV move. */
bool is_mainline_end(const PgnGame& game, const MoveNode* node);

/*
 * A non-owning cursor over one PgnGame move tree. The PgnGame must outlive the
 * navigator. Child zero is the main line; later children are variations.
 */
class MoveNavigator {
public:
    explicit MoveNavigator(const PgnGame& game);

    const MoveNode* current() const;
    const Position& position() const;
    bool at_root() const;

    /* Move to the selected child, or child zero when no child was selected. */
    bool forward();
    /* Go back one move; crossing out of an alternative resets that choice. */
    bool back();
    /* Leave the closest alternative line and return before its first move. */
    bool leave_variation();
    void to_begin();
    void to_end();

    /* Select and enter a child of the current node. Index zero is main line. */
    bool enter_variation(std::size_t child_index);
    bool select(const MoveNode* node);

    /* Select the immediately preceding or following child of the parent. */
    bool previous_sibling();
    bool next_sibling();
    /* First choose the next sibling at the same branch point, at any depth.
       Otherwise, inside a variation, select the first move in notation order
       after that complete variation. With no destination, keep the cursor. */
    bool next_after_variation(const std::vector<const MoveNode*>& notation_order);

    std::size_t sibling_index() const;

    /* Child-index path from the game root to the current move node. */
    std::vector<std::size_t> path() const;
    /* Restore the longest valid prefix; return true only for an exact path. */
    bool restore_path(const std::vector<std::size_t>& path);
    bool restore_path(const std::vector<std::size_t>& path,
                      std::size_t selected_child_index);
    std::size_t selected_child_index() const;

private:
    const PgnGame* game_;
    const MoveNode* current_;
    std::map<const MoveNode*, const MoveNode*> selected_children_;

    const MoveNode* selected_child(const MoveNode* parent) const;
    void select_child(const MoveNode* parent, const MoveNode* child);
    std::size_t child_index(const MoveNode* parent, const MoveNode* child) const;

    MoveNavigator(const MoveNavigator&);
    MoveNavigator& operator=(const MoveNavigator&);
};

/* Session identity for matching a saved navigation path to a reparsed game. */
uint64_t game_navigation_fingerprint(const PgnGame& game);

}

#endif
