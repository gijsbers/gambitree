#include "gambitree/board_layout.h"
#include "gambitree/sidebar_layout.h"

namespace gambitree {

BoardGeometry::BoardGeometry() : x(0), y(0), square_size(0), board_size(0) {}

int BoardLayout::outer_margin() { return 18; }
int BoardLayout::coordinate_space() { return 22; }
int BoardLayout::sidebar_gutter() { return 18; }
int BoardLayout::frame_width() { return 5; }
int BoardLayout::minimum_square_size() { return 16; }
int BoardLayout::minimum_region_width()
{ return outer_margin() + coordinate_space() + frame_width() + 8 * minimum_square_size() + sidebar_gutter(); }

BoardGeometry BoardLayout::calculate(int window_width, int window_height, int sidebar_preferred_width)
{
    const int margin = outer_margin();
    const int labels = coordinate_space();
    SidebarGeometry sidebar = SidebarLayout::calculate(window_width, sidebar_preferred_width);
    int board_region_right = sidebar.left - sidebar_gutter();
    int board_left = margin + labels;
    int available_width = board_region_right - board_left - frame_width();
    int available_height = window_height - 2 * margin - labels;
    int size = available_width < available_height ? available_width : available_height;
    BoardGeometry geometry;
    if (size < 0) size = 0;
    geometry.square_size = size / 8;
    geometry.board_size = geometry.square_size * 8;
    geometry.x = board_left;
    /* Centre a height-limited board inside the left board region, never in the whole window. */
    if (geometry.board_size < available_width)
        geometry.x += (available_width - geometry.board_size) / 2;
    geometry.y = (window_height - geometry.board_size) / 2 - labels / 2;
    if (geometry.x < margin) geometry.x = margin;
    if (geometry.y < margin) geometry.y = margin;
    return geometry;
}

void BoardLayout::square_rect(const BoardGeometry& geometry, int square, bool flipped,
                              int& x, int& y)
{
    int file = square % 8;
    int rank = square / 8;
    int column = flipped ? 7 - file : file;
    int row = flipped ? rank : 7 - rank;
    x = geometry.x + column * geometry.square_size;
    y = geometry.y + row * geometry.square_size;
}

int BoardLayout::square_at(const BoardGeometry& geometry, int x, int y, bool flipped)
{
    int column;
    int row;
    int file;
    int rank;
    if (geometry.square_size <= 0 || x < geometry.x || y < geometry.y ||
        x >= geometry.x + geometry.board_size || y >= geometry.y + geometry.board_size)
        return -1;
    column = (x - geometry.x) / geometry.square_size;
    row = (y - geometry.y) / geometry.square_size;
    file = flipped ? 7 - column : column;
    rank = flipped ? row : 7 - row;
    return rank * 8 + file;
}

bool BoardLayout::is_light_square(int square)
{
    int file;
    int rank;
    if (square < 0 || square >= 64) return false;
    file = square % 8;
    rank = square / 8;
    return (file + rank) % 2 != 0;
}

}
