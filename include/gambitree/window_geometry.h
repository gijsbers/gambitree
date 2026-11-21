#ifndef GAMBITREE_WINDOW_GEOMETRY_H
#define GAMBITREE_WINDOW_GEOMETRY_H

#include <X11/Xutil.h>
#include <string>

namespace gambitree {

struct WindowGeometry {
    int mask;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    bool adjusted;
    WindowGeometry();
};

bool parse_window_geometry(const std::string& text, WindowGeometry& geometry);
std::string window_geometry_spec(const WindowGeometry& geometry, int width, int height);

}
#endif
