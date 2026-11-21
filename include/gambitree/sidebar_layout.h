#ifndef GAMBITREE_SIDEBAR_LAYOUT_H
#define GAMBITREE_SIDEBAR_LAYOUT_H

namespace gambitree {

struct SidebarGeometry {
    int left;
    int width;
    int content_left;
    int content_width;
    SidebarGeometry();
};

class SidebarLayout {
public:
    /* preferred_width is an unclamped per-window user preference; <= 0 is automatic. */
    static SidebarGeometry calculate(int window_width, int preferred_width = 0);
    /* Only manual drags produce a new share; resizing only reads it. */
    static int board_share(int window_width, int sidebar_width);
    static int width_for_share(int window_width, int board_share);
    static int automatic_width(int window_width);
    static int minimum_width();
    static int maximum_width(int window_width);
    static int clamp_width(int window_width, int preferred_width);
};

}

#endif
