#include "gambitree/board_veneer.h"
#include "gambitree/board_layout.h"

#include <algorithm>
#include <new>

namespace gambitree {

namespace {

bool usable_sheet(const PngImage& image)
{
    if (!image.valid() || image.width < 8 || image.width != image.height ||
        image.width % 8 != 0)
        return false;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            if (image.rgba[y * image.stride + x * 4 + 3] != 255)
                return false;
    return true;
}

bool load_sheet(const std::vector<std::string>& paths, PngImage& image,
                std::string& error)
{
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (load_png_rgba(paths[i], image, error) && usable_sheet(image))
            return true;
    }
    error = "Missing or invalid opaque square wood-veneer texture";
    return false;
}

void take_image(PngImage& source, PngImage& destination)
{
    destination.width = source.width;
    destination.height = source.height;
    destination.stride = source.stride;
    destination.rgba.swap(source.rgba);
}

}

bool BoardVeneer::load(const std::vector<std::string>& light_paths,
                       const std::vector<std::string>& dark_paths,
                       std::string& error)
{
    PngImage light, dark;
    if (!load_sheet(light_paths, light, error) ||
        !load_sheet(dark_paths, dark, error))
        return false;
    take_image(light, light_);
    take_image(dark, dark_);
    error.clear();
    return true;
}

bool BoardVeneer::loaded() const
{
    return light_.valid() && dark_.valid();
}

PngAlphaBounds BoardVeneer::window(int square, int source_size)
{
    PngAlphaBounds bounds;
    if (square < 0 || square >= 64 || source_size < 8 || source_size % 8 != 0)
        return bounds;
    const int index = square / 2;
    // Odd multiplier permutes the 64 sheet windows without collisions.
    // Each colour uses 32 different windows, with no flips or grain rotations.
    const int slot = (index * 17 + (BoardLayout::is_light_square(square) ? 11 : 37)) % 64;
    bounds.width = bounds.height = source_size / 8;
    bounds.x = (slot % 8) * bounds.width;
    bounds.y = (slot / 8) * bounds.height;
    return bounds;
}

bool BoardVeneer::render(int square_size, bool flipped, PngImage& board) const
{
    board = PngImage();
    // 511 keeps the unframed raster below 4096px (8*511=4088).
    // BoardRenderer reserves the frame separately, allowing at most 510.
    if (!loaded() || square_size <= 0 || square_size > 511)
        return false;
    const int size = square_size * 8;
    PngImage sheets[2], result;
    // Filter each continuous sheet once, before cutting the windows. This
    // preserves the normalized crop and avoids ringing at artificial cut edges.
    if (!resize_png_lanczos3(dark_, size, size, sheets[0]) ||
        !resize_png_lanczos3(light_, size, size, sheets[1]))
        return false;
    try {
        result.width = result.height = size;
        result.stride = static_cast<std::size_t>(size) * 4;
        result.rgba.resize(result.stride * size);
        for (int square = 0; square < 64; ++square) {
            const PngImage& sheet = sheets[BoardLayout::is_light_square(square) ? 1 : 0];
            const PngAlphaBounds crop = window(square, size);
            const int file = square % 8, rank = square / 8;
            const int column = flipped ? 7 - file : file;
            const int row = flipped ? rank : 7 - rank;
            for (int y = 0; y < square_size; ++y) {
                for (int x = 0; x < square_size; ++x) {
                    const int sx = crop.x + (flipped ? square_size - 1 - x : x);
                    const int sy = crop.y + (flipped ? square_size - 1 - y : y);
                    const std::size_t from = sy * sheet.stride + sx * 4;
                    const std::size_t to = (row * square_size + y) * result.stride +
                                          (column * square_size + x) * 4;
                    std::copy(sheet.rgba.begin() + from, sheet.rgba.begin() + from + 4,
                              result.rgba.begin() + to);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    take_image(result, board);
    return true;
}

}
