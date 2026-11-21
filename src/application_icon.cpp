#include "gambitree/application_icon.h"

#include "gambitree/asset_locator.h"

#include <climits>
#include <cstdio>
#include <limits>
#include <new>

namespace gambitree {

namespace {

const std::size_t kMaximumPropertyWords = 1000000U;
const int kMaximumIconDimension = 512;

bool png_header_matches_size(const std::string& path, int expected_size)
{
    unsigned char header[24];
    static const unsigned char signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    FILE* file = std::fopen(path.c_str(), "rb");
    std::size_t count;
    unsigned long width;
    unsigned long height;
    if (!file) return false;
    count = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    if (count != sizeof(header)) return false;
    for (count = 0; count < sizeof(signature); ++count)
        if (header[count] != signature[count]) return false;
    if (header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') return false;
    width = (static_cast<unsigned long>(header[16]) << 24) |
            (static_cast<unsigned long>(header[17]) << 16) |
            (static_cast<unsigned long>(header[18]) << 8) | header[19];
    height = (static_cast<unsigned long>(header[20]) << 24) |
             (static_cast<unsigned long>(header[21]) << 16) |
             (static_cast<unsigned long>(header[22]) << 8) | header[23];
    return width == static_cast<unsigned long>(expected_size) &&
           height == static_cast<unsigned long>(expected_size) &&
           width <= static_cast<unsigned long>(kMaximumIconDimension);
}

bool image_storage_is_safe(const PngImage& image, std::size_t& pixel_count,
                           std::string& error)
{
    std::size_t width;
    std::size_t height;
    std::size_t minimum_stride;
    std::size_t required_bytes;
    if (image.width <= 0 || image.height <= 0 || image.width > kMaximumIconDimension ||
        image.height > kMaximumIconDimension) {
        error = "invalid or excessive icon dimensions";
        return false;
    }
    width = static_cast<std::size_t>(image.width);
    height = static_cast<std::size_t>(image.height);
    if (width > std::numeric_limits<std::size_t>::max() / 4U) {
        error = "icon row size overflow";
        return false;
    }
    minimum_stride = width * 4U;
    if (image.stride < minimum_stride || image.stride == 0 ||
        height > std::numeric_limits<std::size_t>::max() / image.stride) {
        error = "invalid or overflowing icon storage";
        return false;
    }
    required_bytes = image.stride * height;
    if (required_bytes > image.rgba.size() ||
        width > std::numeric_limits<std::size_t>::max() / height) {
        error = "truncated or overflowing icon pixels";
        return false;
    }
    pixel_count = width * height;
    if (pixel_count > kMaximumPropertyWords - 2U) {
        error = "icon pixel count is excessive";
        return false;
    }
    return true;
}

}

bool rgba_to_ewmh_argb(const PngImage& image, std::vector<unsigned long>& pixels,
                       std::string& error)
{
    std::size_t count;
    std::vector<unsigned long> converted;
    std::size_t y;
    if (!image_storage_is_safe(image, count, error)) return false;
    try {
        if (count > converted.max_size()) {
            error = "icon pixel count exceeds allocation limits";
            return false;
        }
        converted.resize(count);
        for (y = 0; y < static_cast<std::size_t>(image.height); ++y) {
            std::size_t x;
            const unsigned char* row = &image.rgba[y * image.stride];
            for (x = 0; x < static_cast<std::size_t>(image.width); ++x) {
                const unsigned char* rgba = row + x * 4U;
                unsigned long argb = (static_cast<unsigned long>(rgba[3]) << 24) |
                                     (static_cast<unsigned long>(rgba[0]) << 16) |
                                     (static_cast<unsigned long>(rgba[1]) << 8) |
                                      static_cast<unsigned long>(rgba[2]);
                converted[y * static_cast<std::size_t>(image.width) + x] = argb;
            }
        }
    } catch (const std::bad_alloc&) {
        error = "insufficient memory for application icon";
        return false;
    }
    pixels.swap(converted);
    error.clear();
    return true;
}

bool append_ewmh_icon_image(const PngImage& image, std::vector<unsigned long>& property,
                            std::string& error)
{
    std::vector<unsigned long> pixels;
    std::size_t count;
    if (!image_storage_is_safe(image, count, error)) return false;
    if (property.size() > kMaximumPropertyWords - count - 2U) {
        error = "application icon property is excessive";
        return false;
    }
    if (!rgba_to_ewmh_argb(image, pixels, error)) return false;
    try {
        property.reserve(property.size() + count + 2U);
        property.push_back(static_cast<unsigned long>(image.width));
        property.push_back(static_cast<unsigned long>(image.height));
        property.insert(property.end(), pixels.begin(), pixels.end());
    } catch (const std::bad_alloc&) {
        error = "insufficient memory for application icon property";
        return false;
    }
    error.clear();
    return true;
}

std::vector<int> application_icon_sizes()
{
    std::vector<int> sizes;
    sizes.push_back(16); sizes.push_back(24); sizes.push_back(32); sizes.push_back(48);
    sizes.push_back(64); sizes.push_back(96); sizes.push_back(128); sizes.push_back(256);
    return sizes;
}

bool load_ewmh_icon_candidates(const std::vector<std::vector<std::string> >& candidates,
                               const std::vector<int>& sizes,
                               std::vector<unsigned long>& property,
                               std::string& error)
{
    std::vector<unsigned long> loaded;
    std::size_t size_index;
    error.clear();
    property.clear();
    if (candidates.size() != sizes.size()) {
        error = "invalid application icon candidate list";
        return false;
    }
    try {
        for (size_index = 0; size_index < sizes.size(); ++size_index) {
            std::size_t path_index;
            for (path_index = 0; path_index < candidates[size_index].size(); ++path_index) {
                PngImage image;
                std::string load_error;
                if (!png_header_matches_size(candidates[size_index][path_index], sizes[size_index])) continue;
                if (!load_png_rgba(candidates[size_index][path_index], image, load_error)) continue;
                if (image.width != sizes[size_index] || image.height != sizes[size_index]) continue;
                if (append_ewmh_icon_image(image, loaded, load_error)) break;
                error = load_error;
            }
        }
    } catch (const std::bad_alloc&) {
        property.clear();
        error = "insufficient memory while loading application icon";
        return false;
    }
    if (loaded.empty()) {
        property.clear();
        if (error.empty()) error = "no usable application icon assets found";
        return false;
    }
    property.swap(loaded);
    error.clear();
    return true;
}

bool load_application_icon(const std::string& installed_data_root,
                           const std::string& executable_path,
                           std::vector<unsigned long>& property,
                           std::string& error)
{
    std::vector<int> sizes = application_icon_sizes();
    std::vector<std::vector<std::string> > candidates;
    std::size_t index;
    try {
        for (index = 0; index < sizes.size(); ++index)
            candidates.push_back(AssetLocator::icon_path_candidates(installed_data_root,
                                                                     sizes[index], executable_path));
    } catch (const std::bad_alloc&) {
        property.clear();
        error = "insufficient memory while locating application icons";
        return false;
    }
    return load_ewmh_icon_candidates(candidates, sizes, property, error);
}

}
