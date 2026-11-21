#ifndef GAMBITREE_PIECE_RASTER_WORKLIST_H
#define GAMBITREE_PIECE_RASTER_WORKLIST_H

#include <cstddef>

#include "gambitree/chess.h"

namespace gambitree {

const std::size_t kUniquePieceRasterCount = 12;

inline const int* unique_piece_raster_worklist(std::size_t& count)
{
    static const int pieces[kUniquePieceRasterCount] = {
        WHITE_PAWN, WHITE_KNIGHT, WHITE_BISHOP, WHITE_ROOK, WHITE_QUEEN,
        WHITE_KING, BLACK_PAWN, BLACK_KNIGHT, BLACK_BISHOP, BLACK_ROOK,
        BLACK_QUEEN, BLACK_KING
    };
    count = kUniquePieceRasterCount;
    return pieces;
}

} // namespace gambitree

#endif
