#ifndef GAMBITREE_VISITED_GAME_STATE_H
#define GAMBITREE_VISITED_GAME_STATE_H

#include <cstddef>
#include <map>
#include <new>
#include <stdint.h>
#include <utility>
#include <vector>

#include "gambitree/navigation.h"

namespace gambitree {

class VisitedGameStates {
public:
    bool save(uint64_t file_identity, std::size_t game_index,
              const PgnGame& game, const MoveNavigator& navigator)
    {
        Key key(file_identity, game_index);
        std::map<Key, Record, KeyLess>::iterator found = states_.find(key);
        try {
            std::vector<std::size_t> current_path = navigator.path();
            if (found == states_.end()) {
                Record record;
                record.path = current_path;
                record.selected_child_index =
                    navigator.selected_child_index();
                record.fingerprint = game_navigation_fingerprint(game);
                states_.insert(std::make_pair(key, record));
            } else {
                found->second.path.swap(current_path);
                found->second.selected_child_index =
                    navigator.selected_child_index();
            }
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    }

    bool restore(uint64_t file_identity, std::size_t game_index,
                 const PgnGame& game, MoveNavigator& navigator,
                 bool& exact_path)
    {
        Key key(file_identity, game_index);
        std::map<Key, Record, KeyLess>::iterator found = states_.find(key);
        exact_path = false;
        if (found == states_.end())
            return false;
        try {
            if (found->second.fingerprint !=
                game_navigation_fingerprint(game)) {
                states_.erase(found);
                return false;
            }
            exact_path = navigator.restore_path(
                found->second.path, found->second.selected_child_index);
            if (!exact_path) {
                found->second.path = navigator.path();
                found->second.selected_child_index =
                    navigator.selected_child_index();
            }
        } catch (const std::bad_alloc&) {
            states_.erase(found);
            navigator.to_begin();
            exact_path = false;
            return false;
        }
        return true;
    }

    void erase_file(uint64_t file_identity)
    {
        std::map<Key, Record, KeyLess>::iterator current = states_.begin();
        while (current != states_.end()) {
            if (current->first.file == file_identity)
                states_.erase(current++);
            else
                ++current;
        }
    }

    void erase_deleted_game_and_shift(uint64_t file_identity,
                                     std::size_t deleted_index)
    {
        std::vector<std::pair<std::size_t, Record> > shifted;
        std::map<Key, Record, KeyLess>::iterator current;
        try {
            for (current = states_.begin(); current != states_.end(); ++current)
                if (current->first.file == file_identity &&
                    current->first.game_index > deleted_index)
                    shifted.push_back(std::make_pair(
                        current->first.game_index - 1, current->second));
        } catch (const std::bad_alloc&) {
            erase_file(file_identity);
            return;
        }
        current = states_.begin();
        while (current != states_.end()) {
            if (current->first.file == file_identity &&
                current->first.game_index >= deleted_index)
                states_.erase(current++);
            else
                ++current;
        }
        try {
            std::size_t index;
            for (index = 0; index < shifted.size(); ++index)
                states_.insert(std::make_pair(
                    Key(file_identity, shifted[index].first),
                    shifted[index].second));
        } catch (const std::bad_alloc&) {
            erase_file(file_identity);
        }
    }

    std::size_t size() const { return states_.size(); }

private:
    struct Key {
        uint64_t file;
        std::size_t game_index;
        Key() : file(0), game_index(0) {}
        Key(uint64_t identity, std::size_t index)
            : file(identity), game_index(index) {}
    };

    struct KeyLess {
        bool operator()(const Key& left, const Key& right) const
        {
            if (left.file < right.file)
                return true;
            if (right.file < left.file)
                return false;
            return left.game_index < right.game_index;
        }
    };

    struct Record {
        std::vector<std::size_t> path;
        uint64_t fingerprint;
        std::size_t selected_child_index;
        Record()
            : fingerprint(0),
              selected_child_index(static_cast<std::size_t>(-1))
        {
        }
    };

    std::map<Key, Record, KeyLess> states_;
};

}

#endif
