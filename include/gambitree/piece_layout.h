#ifndef GAMBITREE_PIECE_LAYOUT_H
#define GAMBITREE_PIECE_LAYOUT_H

#include "gambitree/png_image.h"
#include "gambitree/piece_set.h"

namespace gambitree {

struct PiecePlacement {
    int x;
    int y;
    int width;
    int height;
    PiecePlacement();
    bool valid() const;
};

class PieceLayout {
public:
    static PiecePlacement centered(int square_x, int square_y, int square_size,
                                   int image_width, int image_height);
    static int target_height_for_piece(int piece, int square_size);
    static int target_height_for_piece(const PieceSetDescriptor& set,
                                       int piece, int square_size);
    // Signed optical Y offset, in square-scaled pixels. Positive moves down.
    // Reference offsets are documented beside the table in piece_layout.cpp.
    static int vertical_offset_for_piece(int piece, int square_size);
    static int vertical_offset_for_piece(const PieceSetDescriptor& set,
                                         int piece, int square_size);
    static PiecePlacement dimensions_for_target_height(int image_width,
                                                       int image_height,
                                                       int target_height);
    static PiecePlacement center_alpha_bounds(int square_x, int square_y,
                                              int square_size,
                                              const PiecePlacement& image,
                                              const PngAlphaBounds& alpha);
    static PiecePlacement place_alpha_bounds_for_piece(
        int square_x, int square_y, int square_size,
        int piece, const PiecePlacement& image,
        const PngAlphaBounds& alpha);
    static PiecePlacement place_alpha_bounds_for_piece(
        const PieceSetDescriptor& set,
        int square_x, int square_y, int square_size,
        int piece, const PiecePlacement& image,
        const PngAlphaBounds& alpha);
    static int source_coordinate(int destination_coordinate, int destination_size,
                                 int source_size);
};

}

#endif
