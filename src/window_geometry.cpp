#include "gambitree/window_geometry.h"

#include <cstdlib>
#include <sstream>

namespace gambitree {

WindowGeometry::WindowGeometry()
    : mask(NoValue), x(0), y(0), width(0), height(0), adjusted(false) {}

bool parse_window_geometry(const std::string& text, WindowGeometry& geometry)
{
    geometry = WindowGeometry();
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    const std::size_t end = text.find_last_not_of(" \t\r\n") + 1;
    std::ostringstream safe;
    // XParseGeometry accumulates digits in an int. Bound every digit run
    // before calling Xlib, including arbitrarily long or malformed input.
    for (std::size_t i = first; i < end;) {
        if (text[i] < '0' || text[i] > '9') {
            safe << text[i++];
            continue;
        }
        unsigned int value = 0;
        do {
            const unsigned int digit = static_cast<unsigned int>(text[i++] - '0');
            if (value > (32767U - digit) / 10U) {
                value = 32767U;
                geometry.adjusted = true;
            } else {
                value = value * 10U + digit;
            }
        } while (i < end && text[i] >= '0' && text[i] <= '9');
        safe << value;
    }
    geometry.mask = XParseGeometry(safe.str().c_str(), &geometry.x, &geometry.y,
                                  &geometry.width, &geometry.height);
    if (!geometry.mask) {
        geometry = WindowGeometry();
        return false;
    }
    if (geometry.mask & WidthValue) {
        const unsigned int requested = geometry.width;
        if (geometry.width < 480U) geometry.width = 480U;
        if (geometry.width > 8192U) geometry.width = 8192U;
        geometry.adjusted = geometry.adjusted || geometry.width != requested;
    }
    if (geometry.mask & HeightValue) {
        const unsigned int requested = geometry.height;
        if (geometry.height < 320U) geometry.height = 320U;
        if (geometry.height > 8192U) geometry.height = 8192U;
        geometry.adjusted = geometry.adjusted || geometry.height != requested;
    }
    return true;
}

std::string window_geometry_spec(const WindowGeometry& geometry, int width, int height)
{
    std::ostringstream spec;
    spec << width << 'x' << height;
    if (geometry.mask & XValue)
        spec << ((geometry.mask & XNegative) ? '-' : '+') << std::abs(geometry.x);
    if (geometry.mask & YValue)
        spec << ((geometry.mask & YNegative) ? '-' : '+') << std::abs(geometry.y);
    return spec.str();
}

}
