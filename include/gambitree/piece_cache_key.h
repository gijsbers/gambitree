#ifndef GAMBITREE_PIECE_CACHE_KEY_H
#define GAMBITREE_PIECE_CACHE_KEY_H

#include <string>

namespace gambitree {

/* The scaled-piece cache depends only on active set and final board-square
   size. Window geometry changes that preserve both keys reuse the cache. */
class PieceCacheKey {
public:
    PieceCacheKey() : set_id_(), square_size_(0) {}

    bool matches(const std::string& set_id, int square_size) const
    {
        return !set_id_.empty() && set_id_ == set_id &&
               square_size_ == square_size;
    }

    bool matches_piece_set(const std::string& set_id) const
    {
        return !set_id_.empty() && set_id_ == set_id;
    }

    int square_size() const { return square_size_; }

    void set(const std::string& set_id, int square_size)
    {
        set_id_ = set_id;
        square_size_ = square_size;
    }

    void invalidate()
    {
        set_id_.clear();
        square_size_ = 0;
    }

private:
    std::string set_id_;
    int square_size_;
};

}

#endif
