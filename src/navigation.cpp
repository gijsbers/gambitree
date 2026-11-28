#include "gambitree/navigation.h"

#include <algorithm>
#include <new>
#include <string>

namespace gambitree {

bool is_mainline_end(const PgnGame& game, const MoveNode* node)
{
    const MoveNode* cursor = game.root;
    if (!cursor || !node) return false;
    while (!cursor->children.empty())
        cursor = cursor->children[0];
    return cursor == node;
}

MoveNavigator::MoveNavigator(const PgnGame& game)
    : game_(&game), current_(game.root)
{
    if (game.parse_status != PGN_COMPLETE && game.error_position)
        select(game.error_position);
}

const MoveNode* MoveNavigator::current() const
{
    return current_;
}

const Position& MoveNavigator::position() const
{
    return current_ == game_->root ? game_->initial_position : current_->position_after;
}

bool MoveNavigator::at_root() const
{
    return current_ == game_->root;
}

const MoveNode* MoveNavigator::selected_child(const MoveNode* parent) const
{
    std::map<const MoveNode*, const MoveNode*>::const_iterator found;
    if (parent->children.empty()) return 0;
    found = selected_children_.find(parent);
    if (found != selected_children_.end()) return found->second;
    return parent->children[0];
}

void MoveNavigator::select_child(const MoveNode* parent, const MoveNode* child)
{
    selected_children_[parent] = child;
}

std::size_t MoveNavigator::child_index(const MoveNode* parent, const MoveNode* child) const
{
    std::size_t index;
    for (index = 0; index < parent->children.size(); ++index)
        if (parent->children[index] == child) return index;
    return parent->children.size();
}

bool MoveNavigator::forward()
{
    const MoveNode* child = selected_child(current_);
    if (!child) return false;
    select_child(current_, child);
    current_ = child;
    return true;
}

bool MoveNavigator::back()
{
    const MoveNode* parent;
    std::size_t index;
    if (current_ == game_->root) return false;
    parent = current_->parent;
    index = child_index(parent, current_);
    /* Crossing out of an alternative's first move is an intentional exit.
       Prefer the played continuation next time Right is pressed. */
    if (index > 0 && index < parent->children.size())
        select_child(parent, parent->children[0]);
    current_ = parent;
    return true;
}

bool MoveNavigator::leave_variation()
{
    const MoveNode* branch_move = current_;
    while (branch_move && branch_move != game_->root) {
        const MoveNode* parent = branch_move->parent;
        std::size_t index = child_index(parent, branch_move);
        if (index > 0 && index < parent->children.size()) {
            /* Return to the position before the alternative move, then make
               Right continue along the parent's played/default continuation. */
            select_child(parent, parent->children[0]);
            current_ = parent;
            return true;
        }
        branch_move = parent;
    }
    return false;
}

void MoveNavigator::to_begin()
{
    current_ = game_->root;
}

void MoveNavigator::to_end()
{
    while (forward()) {}
}

bool MoveNavigator::enter_variation(std::size_t child_index_value)
{
    const MoveNode* child;
    if (child_index_value >= current_->children.size()) return false;
    child = current_->children[child_index_value];
    select_child(current_, child);
    current_ = child;
    return true;
}

bool MoveNavigator::select(const MoveNode* node)
{
    const MoveNode* cursor = node;
    if (!node) return false;
    while (cursor && cursor != game_->root) cursor = cursor->parent;
    if (cursor != game_->root) return false;
    cursor = node;
    while (cursor != game_->root) {
        select_child(cursor->parent, cursor);
        cursor = cursor->parent;
    }
    current_ = node;
    return true;
}

bool MoveNavigator::previous_sibling()
{
    const MoveNode* parent;
    std::size_t index;
    if (current_ == game_->root) return false;
    parent = current_->parent;
    index = child_index(parent, current_);
    if (index == 0 || index == parent->children.size()) return false;
    current_ = parent->children[index - 1];
    select_child(parent, current_);
    return true;
}

bool MoveNavigator::next_sibling()
{
    const MoveNode* parent;
    std::size_t index;
    if (current_ == game_->root) return false;
    parent = current_->parent;
    index = child_index(parent, current_);
    if (index == parent->children.size() || index + 1 >= parent->children.size()) return false;
    current_ = parent->children[index + 1];
    select_child(parent, current_);
    return true;
}

bool MoveNavigator::next_after_variation(
    const std::vector<const MoveNode*>& notation_order)
{
    /* Alternatives at this exact position take precedence at every depth.
       Only leave the enclosing RAV after exhausting these sibling choices. */
    if (next_sibling()) return true;

    const MoveNode* variation_root = 0;
    const MoveNode* cursor = current_;
    std::size_t index;
    std::size_t last_in_variation = notation_order.size();

    /* The closest non-zero child edge identifies the innermost open line. */
    while (cursor && cursor != game_->root) {
        const MoveNode* parent = cursor->parent;
        index = child_index(parent, cursor);
        if (index > 0 && index < parent->children.size()) {
            variation_root = cursor;
            break;
        }
        cursor = parent;
    }
    if (!variation_root) return false;

    /* Layout order, rather than tree order, decides where this line ends on
       the page. First locate its last displayed move, including nested RAVs. */
    for (index = 0; index < notation_order.size(); ++index) {
        const MoveNode* move = notation_order[index];
        const MoveNode* ancestor = move;
        while (ancestor && ancestor != variation_root &&
               ancestor != game_->root)
            ancestor = ancestor->parent;
        if (ancestor == variation_root) last_in_variation = index;
    }
    if (last_in_variation == notation_order.size()) return false;

    /* Comments and punctuation have no selectable node, so the first later
       move outside the current subtree is the natural Down-arrow target. */
    for (index = last_in_variation + 1; index < notation_order.size(); ++index) {
        const MoveNode* move = notation_order[index];
        const MoveNode* ancestor = move;
        while (ancestor && ancestor != variation_root &&
               ancestor != game_->root)
            ancestor = ancestor->parent;
        if (ancestor != variation_root) return select(move);
    }
    return false;
}

std::size_t MoveNavigator::sibling_index() const
{
    if (current_ == game_->root) return 0;
    return child_index(current_->parent, current_);
}

std::vector<std::size_t> MoveNavigator::path() const
{
    std::vector<std::size_t> result;
    const MoveNode* cursor = current_;
    while (cursor != game_->root) {
        std::size_t index = child_index(cursor->parent, cursor);
        if (index == cursor->parent->children.size()) {
            result.clear();
            return result;
        }
        result.push_back(index);
        cursor = cursor->parent;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

bool MoveNavigator::restore_path(const std::vector<std::size_t>& path_value)
{
    return restore_path(path_value, static_cast<std::size_t>(-1));
}

bool MoveNavigator::restore_path(const std::vector<std::size_t>& path_value,
                                 std::size_t selected_index)
{
    std::size_t index;
    selected_children_.clear();
    current_ = game_->root;
    try {
        for (index = 0; index < path_value.size(); ++index) {
            if (path_value[index] >= current_->children.size())
                return false;
            current_ = current_->children[path_value[index]];
            select_child(current_->parent, current_);
        }
        if (selected_index != static_cast<std::size_t>(-1)) {
            if (selected_index >= current_->children.size())
                return false;
            select_child(current_, current_->children[selected_index]);
        }
    } catch (const std::bad_alloc&) {
        selected_children_.clear();
        current_ = game_->root;
        return false;
    }
    return true;
}

std::size_t MoveNavigator::selected_child_index() const
{
    const MoveNode* child = selected_child(current_);
    return child ? child_index(current_, child) :
                   static_cast<std::size_t>(-1);
}

namespace {

void fingerprint_byte(uint64_t& hash, unsigned char value)
{
    hash ^= static_cast<uint64_t>(value);
    hash *= UINT64_C(1099511628211);
}

void fingerprint_number(uint64_t& hash, uint64_t value)
{
    unsigned int index;
    for (index = 0; index < 8; ++index) {
        fingerprint_byte(hash, static_cast<unsigned char>(value & 0xffU));
        value >>= 8;
    }
}

void fingerprint_string(uint64_t& hash, const std::string& value)
{
    std::size_t index;
    fingerprint_number(hash, static_cast<uint64_t>(value.size()));
    for (index = 0; index < value.size(); ++index)
        fingerprint_byte(hash, static_cast<unsigned char>(value[index]));
}

void fingerprint_node(uint64_t& hash, const MoveNode* node)
{
    std::size_t index;
    fingerprint_number(hash, static_cast<uint64_t>(node->children.size()));
    for (index = 0; index < node->children.size(); ++index) {
        const MoveNode* child = node->children[index];
        fingerprint_byte(hash, child->has_move ? 1 : 0);
        fingerprint_string(hash, child->san);
        fingerprint_string(hash, child->position_after.to_fen());
        fingerprint_node(hash, child);
    }
}

}

uint64_t game_navigation_fingerprint(const PgnGame& game)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    std::size_t index;
    fingerprint_string(hash, game.initial_position.to_fen());
    fingerprint_string(hash, game.result);
    fingerprint_number(hash, static_cast<uint64_t>(game.tags.size()));
    for (index = 0; index < game.tags.size(); ++index) {
        fingerprint_string(hash, game.tags[index].name);
        fingerprint_string(hash, game.tags[index].value);
    }
    fingerprint_node(hash, game.root);
    return hash;
}

}
