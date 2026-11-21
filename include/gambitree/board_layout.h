#ifndef GAMBITREE_BOARD_LAYOUT_H
#define GAMBITREE_BOARD_LAYOUT_H

namespace gambitree {

struct BoardGeometry {
    int x;
    int y;
    int square_size;
    int board_size;
    BoardGeometry();
};

class BoardLayout {
public:
    static BoardGeometry calculate(int window_width, int window_height, int sidebar_preferred_width = 0);
    static int outer_margin();
    static int coordinate_space();
    static int sidebar_gutter();
    static int frame_width();
    static int minimum_square_size();
    static int minimum_region_width();
    static int square_at(const BoardGeometry& geometry, int x, int y, bool flipped);
    static void square_rect(const BoardGeometry& geometry, int square, bool flipped,
                            int& x, int& y);
    /* True for the conventional light colour of a logical chess square. */
    static bool is_light_square(int square);
};

}

#endif
