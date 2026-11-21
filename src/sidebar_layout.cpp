#include "gambitree/sidebar_layout.h"
#include "gambitree/board_layout.h"

namespace gambitree {

SidebarGeometry::SidebarGeometry() : left(0), width(0), content_left(0), content_width(0) {}

int SidebarLayout::minimum_width() { return 220; }

int SidebarLayout::board_share(int window_width, int sidebar_width)
{
    if (window_width <= 0) return 0;
    int left = window_width - clamp_width(window_width, sidebar_width);
    int share = static_cast<int>((static_cast<long>(left) * 10000 + window_width / 2) / window_width);
    if (share < 1) share = 1;
    if (share > 9999) share = 9999;
    return share;
}

int SidebarLayout::width_for_share(int window_width, int share)
{
    if (share <= 0 || share >= 10000) return 0;
    int preferred = static_cast<int>((static_cast<long>(window_width) * (10000 - share) + 5000) / 10000);
    if (preferred < 1) preferred = 1;
    return clamp_width(window_width, preferred);
}

int SidebarLayout::automatic_width(int window_width)
{
    int preferred;
    preferred = window_width * 42 / 100;
    if (preferred < 320) preferred = 320;
    if (preferred > 500) preferred = 500;
    return preferred;
}

int SidebarLayout::maximum_width(int window_width)
{
    int maximum = window_width - BoardLayout::minimum_region_width();
    if (maximum < 0) maximum = 0;
    return maximum;
}

int SidebarLayout::clamp_width(int window_width, int preferred_width)
{
    int maximum = maximum_width(window_width);
    int minimum = minimum_width();
    int width = preferred_width > 0 ? preferred_width : automatic_width(window_width);
    if (minimum > maximum) minimum = maximum;
    if (width < minimum) width = minimum;
    if (width > maximum) width = maximum;
    return width;
}

SidebarGeometry SidebarLayout::calculate(int window_width, int preferred_width)
{
    SidebarGeometry result;
    int preferred;
    if (window_width < 1) return result;
    preferred = clamp_width(window_width, preferred_width);
    result.width = preferred;
    result.left = window_width - result.width;
    result.content_left = result.left + 14;
    result.content_width = result.width - 28;
    if (result.content_width < 0) result.content_width = 0;
    return result;
}

}
