#include "gambitree/piece_layout.h"

#include <algorithm>
#include <cstring>

namespace gambitree {

PiecePlacement::PiecePlacement() : x(0), y(0), width(0), height(0) {}
bool PiecePlacement::valid() const { return width > 0 && height > 0; }

PiecePlacement PieceLayout::centered(int square_x, int square_y, int square_size,
                                     int image_width, int image_height)
{
    PiecePlacement placement;
    int maximum;
    if (square_size <= 0 || image_width <= 0 || image_height <= 0) return placement;
    maximum = square_size * 9 / 10;
    if (maximum <= 0) return placement;
    if (image_width >= image_height) {
        placement.width = maximum;
        placement.height = (image_height * maximum + image_width / 2) / image_width;
    } else {
        placement.height = maximum;
        placement.width = (image_width * maximum + image_height / 2) / image_height;
    }
    if (placement.width <= 0) placement.width = 1;
    if (placement.height <= 0) placement.height = 1;
    placement.x = square_x + (square_size - placement.width) / 2;
    placement.y = square_y + (square_size - placement.height) / 2;
    return placement;
}

int PieceLayout::target_height_for_piece(int piece, int square_size)
{
    return target_height_for_piece(default_piece_set(), piece, square_size);
}

int PieceLayout::target_height_for_piece(const PieceSetDescriptor& set,
                                         int piece, int square_size)
{
    int piece_type = piece < 0 ? -piece : piece;
    int target;
    int maximum;
    if (square_size <= 0 || piece_type < 1 || piece_type > 6)
        return 0;
    target = (square_size * set.target_heights_at_84[piece_type] + 42) / 84;
    maximum = square_size * 9 / 10;
    return std::max(1, std::min(target, maximum));
}

int PieceLayout::vertical_offset_for_piece(int piece, int square_size)
{
    return vertical_offset_for_piece(default_piece_set(), piece, square_size);
}

int PieceLayout::vertical_offset_for_piece(const PieceSetDescriptor& set,
                                           int piece, int square_size)
{
    int piece_type = piece < 0 ? -piece : piece;
    int reference_offset;
    int magnitude;
    int whole_squares;
    int remainder;
    int scaled;
    if (square_size <= 0)
        return 0;
    if (piece_type < 1 || piece_type > 6)
        return 0;
    reference_offset = set.vertical_offsets_at_84[piece_type];
    magnitude = reference_offset < 0 ? -reference_offset : reference_offset;
    whole_squares = square_size / 84;
    remainder = square_size % 84;
    scaled = magnitude * whole_squares + (magnitude * remainder + 42) / 84;
    int offset = reference_offset < 0 ? -scaled : scaled;
    if (set.id && std::strcmp(set.id, "cburnett") == 0) {
        /* Common lift, applied after the existing per-type rounding.
           Split quotient/remainder to avoid multiplying a large square size. */
        offset -= (square_size / 115) * 2 +
                  ((square_size % 115) * 2 + 57) / 115;
    }
    return offset;
}

PiecePlacement PieceLayout::dimensions_for_target_height(int image_width,
                                                         int image_height,
                                                         int target_height)
{
    PiecePlacement dimensions;
    if (image_width <= 0 || image_height <= 0 || target_height <= 0)
        return dimensions;
    dimensions.height = target_height;
    dimensions.width = (image_width * target_height + image_height / 2) /
                       image_height;
    if (dimensions.width <= 0)
        dimensions.width = 1;
    if (dimensions.height <= 0)
        dimensions.height = 1;
    return dimensions;
}

PiecePlacement PieceLayout::center_alpha_bounds(int square_x, int square_y,
                                                int square_size,
                                                const PiecePlacement& image,
                                                const PngAlphaBounds& alpha)
{
    PiecePlacement placement = image;
    if (!image.valid() || !alpha.valid() || square_size <= 0 ||
        alpha.x < 0 || alpha.y < 0 ||
        alpha.x + alpha.width > image.width ||
        alpha.y + alpha.height > image.height)
        return PiecePlacement();
    placement.x = square_x + (square_size - alpha.width) / 2 - alpha.x;
    placement.y = square_y + (square_size - alpha.height) / 2 - alpha.y;
    return placement;
}

PiecePlacement PieceLayout::place_alpha_bounds_for_piece(
    int square_x, int square_y, int square_size,
    int piece, const PiecePlacement& image, const PngAlphaBounds& alpha)
{
    return place_alpha_bounds_for_piece(default_piece_set(), square_x,
                                        square_y, square_size, piece, image,
                                        alpha);
}

PiecePlacement PieceLayout::place_alpha_bounds_for_piece(
    const PieceSetDescriptor& set,
    int square_x, int square_y, int square_size,
    int piece, const PiecePlacement& image, const PngAlphaBounds& alpha)
{
    PiecePlacement placement = center_alpha_bounds(square_x, square_y,
                                                    square_size, image, alpha);
    if (placement.valid())
        placement.y += vertical_offset_for_piece(set, piece, square_size);
    return placement;
}

int PieceLayout::source_coordinate(int destination_coordinate, int destination_size,
                                   int source_size)
{
    int source;
    if (destination_coordinate < 0 || destination_coordinate >= destination_size ||
        destination_size <= 0 || source_size <= 0) return -1;
    if (destination_size == 1) return 0;
    source = destination_coordinate * (source_size - 1) / (destination_size - 1);
    return source < source_size ? source : source_size - 1;
}

}
