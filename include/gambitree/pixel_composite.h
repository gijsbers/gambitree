#ifndef GAMBITREE_PIXEL_COMPOSITE_H
#define GAMBITREE_PIXEL_COMPOSITE_H

#include <cstddef>

namespace gambitree {

inline unsigned int board_snapshot_readbacks_for_piece_batch(
    std::size_t piece_count)
{
    return piece_count == 0 ? 0U : 1U;
}

inline unsigned int per_piece_readbacks_for_piece_batch(std::size_t)
{
    return 0U;
}

namespace detail {

inline unsigned char pixel_component(unsigned long value,
                                     unsigned long mask)
{
    unsigned long shifted = mask;
    unsigned long component;
    int shift = 0;
    while (shifted && !(shifted & 1UL)) {
        ++shift;
        shifted >>= 1;
    }
    if (!shifted)
        return 0;
    component = (value & mask) >> shift;
    return static_cast<unsigned char>(
        (component * 255UL + shifted / 2UL) / shifted);
}

inline unsigned long component_pixel(unsigned char component,
                                     unsigned long mask)
{
    unsigned long shifted = mask;
    unsigned long value;
    int shift = 0;
    while (shifted && !(shifted & 1UL)) {
        ++shift;
        shifted >>= 1;
    }
    if (!shifted)
        return 0;
    value = (static_cast<unsigned long>(component) * shifted + 127UL) /
            255UL;
    return (value << shift) & mask;
}

} // namespace detail

inline unsigned long composite_rgba_pixel(unsigned long background,
                                          const unsigned char* rgba,
                                          unsigned long red_mask,
                                          unsigned long green_mask,
                                          unsigned long blue_mask)
{
    unsigned int alpha = rgba[3];
    unsigned char red;
    unsigned char green;
    unsigned char blue;
    if (alpha == 0)
        return background;
    red = static_cast<unsigned char>(
        (rgba[0] * alpha + detail::pixel_component(background, red_mask) *
         (255 - alpha)) / 255);
    green = static_cast<unsigned char>(
        (rgba[1] * alpha + detail::pixel_component(background, green_mask) *
         (255 - alpha)) / 255);
    blue = static_cast<unsigned char>(
        (rgba[2] * alpha + detail::pixel_component(background, blue_mask) *
         (255 - alpha)) / 255);
    return detail::component_pixel(red, red_mask) |
           detail::component_pixel(green, green_mask) |
           detail::component_pixel(blue, blue_mask);
}

/* Surface supplies width(), height(), pixel(x,y), and set_pixel(x,y,value).
   This keeps clipping and channel math identical for XImage and pure tests. */
template <typename Surface>
std::size_t composite_rgba_image(Surface& surface, int origin_x, int origin_y,
                                 int destination_x, int destination_y,
                                 int source_width, int source_height,
                                 const unsigned char* rgba,
                                 std::size_t source_stride,
                                 unsigned long red_mask,
                                 unsigned long green_mask,
                                 unsigned long blue_mask)
{
    int source_y;
    std::size_t blended = 0;
    if (!rgba || source_width <= 0 || source_height <= 0)
        return 0;
    for (source_y = 0; source_y < source_height; ++source_y) {
        int target_y = destination_y + source_y - origin_y;
        int source_x;
        if (target_y < 0 || target_y >= surface.height())
            continue;
        for (source_x = 0; source_x < source_width; ++source_x) {
            int target_x = destination_x + source_x - origin_x;
            std::size_t offset = static_cast<std::size_t>(source_y) *
                                     source_stride +
                                 static_cast<std::size_t>(source_x) * 4;
            const unsigned char* source = rgba + offset;
            if (target_x < 0 || target_x >= surface.width() || !source[3])
                continue;
            surface.set_pixel(
                target_x, target_y,
                composite_rgba_pixel(surface.pixel(target_x, target_y),
                                     source, red_mask, green_mask,
                                     blue_mask));
            ++blended;
        }
    }
    return blended;
}

} // namespace gambitree

#endif
