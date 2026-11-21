#include "gambitree/board_renderer.h"
#include "gambitree/asset_locator.h"
#include "gambitree/pixel_composite.h"
#include "gambitree/piece_raster_worklist.h"

#include <X11/Xutil.h>

#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <iostream>
#include <new>
#include <sys/time.h>

namespace gambitree {

#ifndef GAMBITREE_DATA_DIR
#define GAMBITREE_DATA_DIR "/usr/local/share/gambitree"
#endif

namespace {

double elapsed_milliseconds(const timeval& start, const timeval& end)
{
    return (end.tv_sec - start.tv_sec) * 1000.0 +
           (end.tv_usec - start.tv_usec) / 1000.0;
}

unsigned long component_to_mask(unsigned char component, unsigned long mask)
{
    unsigned long value = 0;
    unsigned long shifted = mask;
    int shift = 0;
    while (shifted && !(shifted & 1UL)) { ++shift; shifted >>= 1; }
    if (!shifted) return 0;
    value = (static_cast<unsigned long>(component) * shifted + 127UL) / 255UL;
    return (value << shift) & mask;
}

class XImageSurface {
public:
    explicit XImageSurface(XImage* image) : image_(image) {}
    int width() const { return image_->width; }
    int height() const { return image_->height; }
    unsigned long pixel(int x, int y) const
    {
        return XGetPixel(image_, x, y);
    }
    void set_pixel(int x, int y, unsigned long value)
    {
        XPutPixel(image_, x, y, value);
    }
private:
    XImage* image_;
};

struct PieceDrawItem {
    PiecePlacement placement;
    const PngImage* image;
    PieceDrawItem() : placement(), image(0) {}
};

uint32_t grain_hash(int x, int y, uint32_t seed)
{
    uint32_t value = seed ^ (static_cast<uint32_t>(x) * 0x9e3779b9U) ^
                     (static_cast<uint32_t>(y) * 0x85ebca6bU);
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

double grain_lattice(int x, int y, uint32_t seed)
{
    return static_cast<double>(grain_hash(x, y, seed) & 0xffffU) / 32767.5 - 1.0;
}

double smooth_noise(double x, double y, uint32_t seed)
{
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const double fx0 = x - x0;
    const double fy0 = y - y0;
    const double fx = fx0 * fx0 * (3.0 - 2.0 * fx0);
    const double fy = fy0 * fy0 * (3.0 - 2.0 * fy0);
    const double a = grain_lattice(x0, y0, seed) * (1.0 - fx) +
                      grain_lattice(x0 + 1, y0, seed) * fx;
    const double b = grain_lattice(x0, y0 + 1, seed) * (1.0 - fx) +
                      grain_lattice(x0 + 1, y0 + 1, seed) * fx;
    return a * (1.0 - fy) + b * fy;
}

unsigned char wood_component(int base, int variation)
{
    int value = base + variation;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return static_cast<unsigned char>(value);
}

RgbColor wood_pixel(int x, int y, int square_size, int file, int rank,
                    bool frame)
{
    uint32_t seed;
    double grain;
    int red;
    int green;
    int blue;
    if (frame) {
        seed = 0x6d2b79f5U;
        /* The frame has longer, gently irregular streaks than the square
           veneers, with a little finer grain to keep it from looking flat. */
        grain = smooth_noise(x * 0.045, y * 0.18, seed) * 7.0 +
                smooth_noise(x * 0.16, y * 0.42, seed + 17U) * 2.0 +
                smooth_noise(x * 0.55, y * 0.9, seed + 31U) * 0.8;
        return RgbColor(wood_component(99, static_cast<int>(grain)),
                        wood_component(48, static_cast<int>(grain * 0.72)),
                        wood_component(39, static_cast<int>(grain * 0.62)));
    }
    seed = 0x4d61706cU ^ static_cast<uint32_t>(file * 0x45d9f3b) ^
           static_cast<uint32_t>(rank * 0x119de1f3U);
    const double sx = static_cast<double>(x) / std::max(1, square_size);
    const double sy = static_cast<double>(y) / std::max(1, square_size);
    {
        /* Chessboard veneers in the reference have mostly vertical grain.
           Make the noise vary more across a file than along a rank, and
           gently warp the fibres so they do not read as straight stripes. */
        const double detail = std::min(1.0,
            std::max(0.4, static_cast<double>(square_size) / 120.0));
        const double warp = smooth_noise(sx * 4.0, sy * 2.0, seed + 53U) * 0.10;
        const double fibre_x = sx + warp;
        grain = smooth_noise(fibre_x * 10.0 * detail,
                             sy * 2.4 * detail, seed) * 5.5 +
                smooth_noise(fibre_x * 25.0 * detail,
                             sy * 6.5 * detail, seed + 101U) * 2.6 +
                smooth_noise((sx + warp * 1.7) * 54.0 * detail,
                             sy * 15.0 * detail, seed + 211U) * 1.1 +
                smooth_noise(sx * 2.3, sy * 1.8, seed + 307U) * 2.0;
    }
    {
        const int tint = static_cast<int>(grain_hash(file, rank, seed) % 13U) - 6;
        const int shade = static_cast<int>(grain);
        if ((file + rank) % 2 != 0) {
            red = 241 + tint + shade;
            green = 229 + tint + shade;
            blue = 205 + tint / 2 + shade;
        } else {
            red = 188 + tint + shade;
            green = 119 + tint + shade;
            blue = 76 + tint / 2 + shade / 2;
        }
    }
    return RgbColor(wood_component(red, 0), wood_component(green, 0),
                    wood_component(blue, 0));
}

unsigned char texture_component(double value, double scale, double offset)
{
    int result = static_cast<int>(value * scale + offset + 0.5);
    if (result < 0) result = 0;
    if (result > 255) result = 255;
    return static_cast<unsigned char>(result);
}

unsigned char soften_texture_component(double value, int neutral,
                                       double texture_strength)
{
    return texture_component(neutral + (value - neutral) * texture_strength,
                             1.0, 0.0);
}

RgbColor reduce_wood_saturation(const RgbColor& color)
{
    const double luminance = color.red * 0.2126 + color.green * 0.7152 +
                             color.blue * 0.0722;
    const double saturation = 0.78;
    return RgbColor(
        texture_component(luminance + (color.red - luminance) * saturation,
                          1.0, 0.0),
        texture_component(luminance + (color.green - luminance) * saturation,
                          1.0, 0.0),
        texture_component(luminance + (color.blue - luminance) * saturation,
                          1.0, 0.0));
}

RgbColor sample_wood_texture(const PngImage& texture, int file, int rank,
                             int x, int y, int square_size, bool dark)
{
    const double cell_width = static_cast<double>(texture.width) / 8.0;
    const double cell_height = static_cast<double>(texture.height) / 8.0;
    const double left = file * cell_width;
    const double top = rank * cell_height;
    double source_x = left + (static_cast<double>(x) + 0.5) *
                      cell_width / square_size - 0.5;
    double source_y = top + (static_cast<double>(y) + 0.5) *
                      cell_height / square_size - 0.5;
    int min_x = static_cast<int>(left);
    int max_x = static_cast<int>(left + cell_width) - 1;
    int min_y = static_cast<int>(top);
    int max_y = static_cast<int>(top + cell_height) - 1;
    int x0, x1, y0, y1;
    double fx, fy;
    double channels[3] = { 0.0, 0.0, 0.0 };
    int channel;
    RgbColor graded;
    if (source_x < min_x) source_x = min_x;
    if (source_x > max_x) source_x = max_x;
    if (source_y < min_y) source_y = min_y;
    if (source_y > max_y) source_y = max_y;
    x0 = static_cast<int>(source_x);
    y0 = static_cast<int>(source_y);
    x1 = std::min(max_x, x0 + 1);
    y1 = std::min(max_y, y0 + 1);
    fx = source_x - x0;
    fy = source_y - y0;
    for (channel = 0; channel < 3; ++channel) {
        const unsigned char* p00 = &texture.rgba[
            static_cast<std::size_t>(y0) * texture.stride + x0 * 4 + channel];
        const unsigned char* p10 = &texture.rgba[
            static_cast<std::size_t>(y0) * texture.stride + x1 * 4 + channel];
        const unsigned char* p01 = &texture.rgba[
            static_cast<std::size_t>(y1) * texture.stride + x0 * 4 + channel];
        const unsigned char* p11 = &texture.rgba[
            static_cast<std::size_t>(y1) * texture.stride + x1 * 4 + channel];
        const double top_value = *p00 * (1.0 - fx) + *p10 * fx;
        const double bottom_value = *p01 * (1.0 - fx) + *p11 * fx;
        channels[channel] = top_value * (1.0 - fy) + bottom_value * fy;
    }
    if (dark) {
        /* The generated source is intentionally richer than the final board;
           this restrained grade brings it back to the muted chestnut in the
           supplied visual references. Reduce local contrast slightly so the
           grain remains material detail rather than drawing attention away
           from the pieces. */
        graded = RgbColor(soften_texture_component(
                              texture_component(channels[0], 0.90, 14.0),
                              167, 0.72),
                          soften_texture_component(
                              texture_component(channels[1], 0.96, 28.0),
                              95, 0.72),
                          soften_texture_component(
                              texture_component(channels[2], 1.10, 42.0),
                              58, 0.72));
    } else {
        /* A small saturation reduction keeps both wood tones calmer without
           changing the mapped luminance or the white piece contrast. */
        graded = RgbColor(soften_texture_component(
                              texture_component(channels[0], 0.96, 0.0),
                              205, 0.62),
                          soften_texture_component(
                              texture_component(channels[1], 1.07, 0.0),
                              191, 0.62),
                          soften_texture_component(
                              texture_component(channels[2], 1.25, 0.0),
                              168, 0.62));
    }
    return reduce_wood_saturation(graded);
}

}

LastMoveHighlight::LastMoveHighlight() : from(-1), to(-1), valid(false) {}

BoardDrawMetrics::BoardDrawMetrics()
    : piece_instances(0), ximage_get_calls(0),
      piece_destination_readbacks(0), ximage_put_calls(0),
      ximage_get_bytes(0), ximage_put_bytes(0), board_base_ms(0.0),
      ximage_get_ms(0.0), alpha_composite_ms(0.0), ximage_put_ms(0.0),
      total_ms(0.0)
{
}

BoardRenderer::BoardRenderer(Display* display, int screen)
    : display_(display), screen_(screen), graphics_(XCreateGC(display, RootWindow(display, screen), 0, 0)),
      visual_(DefaultVisual(display, screen)), depth_(DefaultDepth(display, screen)),
      colormap_(DefaultColormap(display, screen)), board_background_cache_(0),
      board_background_square_size_(0), board_background_flipped_(false),
      board_background_rebuilds_(0), board_theme_id_("classic"),
      wood_light_texture_(), wood_dark_texture_(),
      wood_textures_load_attempted_(false),
      veneer_(), veneer_load_attempted_(false),
      coordinate_font_(0), coordinate_color_(),
      coordinate_color_allocated_(false), board_theme_(), assets_(),
      piece_set_(&default_piece_set()), piece_set_id_(default_piece_set().id),
      cache_key_(), scaled_pieces_(), candidate_scaled_pieces_(),
      temporary_pieces_(), candidate_square_size_(0),
      candidate_generation_(0), candidate_next_piece_(0),
      candidate_active_(false), candidate_resize_active_(false),
      candidate_piece_id_(0), candidate_resize_job_(),
      last_piece_instances_drawn_(0), last_draw_metrics_(),
      temporary_square_size_(0), interactive_resize_(false),
      resize_profile_enabled_(std::getenv("GAMBITREE_DIAGNOSTICS") != 0),
      scaled_cache_rebuilds_(0), scaled_piece_builds_(0),
      temporary_draws_(0), temporary_piece_builds_(0),
      resize_horizontal_ms_(0.0), resize_vertical_ms_(0.0),
      resize_total_ms_(0.0), temporary_resize_ms_(0.0),
      alpha_bounds_ms_(0.0),
      ximage_get_ms_(0.0), ximage_composite_ms_(0.0),
      ximage_put_ms_(0.0)
{
    XRenderColor color;
    coordinate_font_ = XftFontOpenName(display_, screen_, "sans:bold-10");
    if (!coordinate_font_) coordinate_font_ = XftFontOpenName(display_, screen_, "fixed");
    color.red = static_cast<unsigned short>(board_theme_.coordinate.red) * 257U;
    color.green = static_cast<unsigned short>(board_theme_.coordinate.green) * 257U;
    color.blue = static_cast<unsigned short>(board_theme_.coordinate.blue) * 257U;
    color.alpha = 65535U;
    coordinate_color_allocated_ = XftColorAllocValue(display_, visual_, colormap_, &color, &coordinate_color_) != 0;
}

BoardRenderer::~BoardRenderer()
{
    if (board_background_cache_)
        XFreePixmap(display_, board_background_cache_);
    if (coordinate_color_allocated_) XftColorFree(display_, visual_, colormap_, &coordinate_color_);
    if (coordinate_font_) XftFontClose(display_, coordinate_font_);
    if (graphics_) XFreeGC(display_, graphics_);
}

bool BoardRenderer::load_piece_theme(const std::string& theme, std::string& error)
{
    return load_piece_set(theme == "classic"
                              ? default_piece_set().id : theme, error);
}

bool BoardRenderer::load_piece_set(const std::string& id, std::string& error)
{
    const PieceSetDescriptor* descriptor = find_piece_set(id);
    const PieceSetDescriptor* layout = descriptor ? descriptor
                                                  : &default_piece_set();
    const std::string asset_directory = descriptor ? descriptor->asset_directory
                                                   : id;
    std::string root;
    std::string executable;
    AssetManager loaded_assets;
    if (id.empty() || asset_directory.find('/') != std::string::npos ||
        asset_directory == "." || asset_directory == "..") {
        error = "invalid piece set name '" + id + "'";
        return false;
    }
    error.clear();
    if (AssetLocator::find_piece_root(GAMBITREE_DATA_DIR,
                                      asset_directory,
                                      root, executable) &&
        loaded_assets.load_piece_set(
            PieceAssetDirectory(root, layout->alpha_threshold,
                                layout->crop_padding), error)) {
        if (std::getenv("GAMBITREE_DIAGNOSTICS"))
            std::cerr << "gambitree: diagnostic: executable path '" << executable
                      << "', piece asset root '" << root << "'\n";
        assets_.swap(loaded_assets);
        piece_set_ = layout;
        piece_set_id_ = descriptor ? descriptor->id : id;
        scaled_pieces_.clear();
        cancel_high_quality_rebuild();
        temporary_pieces_.clear();
        temporary_square_size_ = 0;
        interactive_resize_ = false;
        cache_key_.invalidate();
        return true;
    }
    if (std::getenv("GAMBITREE_DIAGNOSTICS"))
        std::cerr << "gambitree: diagnostic: executable path '" << executable
                  << "', no piece asset root found\n";
    if (error.empty())
        error = "cannot load all PNG assets for piece set '" + id + "'";
    return false;
}

const std::string& BoardRenderer::piece_set_id() const
{
    return piece_set_id_;
}

bool BoardRenderer::set_board_theme(const std::string& id)
{
    if (!valid_board_theme_id(id))
        return false;
    if (id == board_theme_id_)
        return true;
    board_theme_id_ = id;
    if (board_background_cache_) {
        XFreePixmap(display_, board_background_cache_);
        board_background_cache_ = 0;
    }
    board_background_square_size_ = 0;
    return true;
}

const std::string& BoardRenderer::board_theme_id() const
{
    return board_theme_id_;
}

unsigned long BoardRenderer::board_background_rebuild_count() const
{
    return board_background_rebuilds_;
}

int BoardRenderer::board_background_square_size() const
{
    return board_background_square_size_;
}

bool BoardRenderer::wood_texture_assets_loaded() const
{
    return wood_light_texture_.valid() && wood_dark_texture_.valid();
}

bool BoardRenderer::load_wood_textures()
{
    const char* const names[] = { "wood-light.png", "wood-dark.png" };
    PngImage loaded[2];
    std::string executable = AssetLocator::executable_path();
    int index;
    for (index = 0; index < 2; ++index) {
        const std::vector<std::string> paths =
            AssetLocator::board_texture_candidates(GAMBITREE_DATA_DIR,
                                                    names[index], executable);
        std::size_t path_index;
        bool found = false;
        for (path_index = 0; path_index < paths.size(); ++path_index) {
            std::string error;
            if (load_png_rgba(paths[path_index], loaded[index], error) &&
                loaded[index].valid() && loaded[index].width >= 8 &&
                loaded[index].height >= 8) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    wood_light_texture_ = loaded[0];
    wood_dark_texture_ = loaded[1];
    return true;
}

bool BoardRenderer::rebuild_textured_background(int square_size, bool flipped)
{
    // Check before arithmetic; the shared Pixmap, including frame, is <=4096px.
    // With the current 5px frame: 8*510+10=4090, but 8*511+10=4098.
    if (square_size <= 0 || square_size > (4096 - 2 * BoardLayout::frame_width()) / 8)
        return false;
    const int frame = BoardLayout::frame_width();
    const int board_size = square_size * 8;
    const int image_size = board_size + 2 * frame;
    const bool veneer = board_theme_id_ == "wood-veneer";
    PngImage veneer_board;
    XImage* image;
    Pixmap pixmap;
    int y;
    if (veneer) {
        if (!veneer_load_attempted_) {
            veneer_load_attempted_ = true;
            const std::string executable = AssetLocator::executable_path();
            std::string error;
            if (!veneer_.load(AssetLocator::board_texture_candidates(
                    GAMBITREE_DATA_DIR, "wood-veneer-light.png", executable),
                    AssetLocator::board_texture_candidates(
                    GAMBITREE_DATA_DIR, "wood-veneer-dark.png", executable), error))
                std::cerr << error << "; using classic board colors.\n";
        }
        if (!veneer_.render(square_size, flipped, veneer_board))
            return false; // Existing classic path still draws coordinates/overlays.
    } else if (!wood_textures_load_attempted_) {
        wood_textures_load_attempted_ = true;
        load_wood_textures();
    }
    image = XCreateImage(display_, visual_, static_cast<unsigned int>(depth_),
                         ZPixmap, 0, 0, static_cast<unsigned int>(image_size),
                         static_cast<unsigned int>(image_size), 32, 0);
    if (!image)
        return false;
    image->data = static_cast<char*>(std::calloc(
        static_cast<std::size_t>(image->bytes_per_line),
        static_cast<std::size_t>(image_size)));
    if (!image->data) {
        XDestroyImage(image);
        return false;
    }
    for (y = 0; y < image_size; ++y) {
        int x;
        for (x = 0; x < image_size; ++x) {
            const bool inside = x >= frame && x < frame + board_size &&
                                y >= frame && y < frame + board_size;
            unsigned long pixel_value;
            if (veneer) {
                if (!inside) {
                    pixel_value = pixel(board_theme_.frame);
                } else {
                    const unsigned char* rgba = &veneer_board.rgba[
                        (y - frame) * veneer_board.stride + (x - frame) * 4];
                    pixel_value = pixel(RgbColor(rgba[0], rgba[1], rgba[2]));
                }
            } else if (!inside) {
                pixel_value = pixel(wood_pixel(x, y, square_size, 0, 0,
                                               true));
            } else {
                const int column = (x - frame) / square_size;
                const int row = (y - frame) / square_size;
                const int file = flipped ? 7 - column : column;
                const int rank = flipped ? row : 7 - row;
                const int local_x = (x - frame) % square_size;
                const int local_y = (y - frame) % square_size;
                if (wood_light_texture_.valid() && wood_dark_texture_.valid()) {
                    const bool dark = (file + rank) % 2 == 0;
                    pixel_value = pixel(sample_wood_texture(
                        dark ? wood_dark_texture_ : wood_light_texture_,
                        file, rank, local_x, local_y, square_size, dark));
                } else {
                    pixel_value = pixel(wood_pixel(local_x, local_y,
                        square_size, file, rank, false));
                }
            }
            XPutPixel(image, x, y, pixel_value);
        }
    }
    pixmap = XCreatePixmap(display_, RootWindow(display_, screen_),
                           static_cast<unsigned int>(image_size),
                           static_cast<unsigned int>(image_size),
                           static_cast<unsigned int>(depth_));
    if (!pixmap) {
        XDestroyImage(image);
        return false;
    }
    XPutImage(display_, pixmap, graphics_, image, 0, 0, 0, 0,
              static_cast<unsigned int>(image_size),
              static_cast<unsigned int>(image_size));
    XDestroyImage(image);
    if (board_background_cache_)
        XFreePixmap(display_, board_background_cache_);
    board_background_cache_ = pixmap;
    board_background_square_size_ = square_size;
    board_background_flipped_ = flipped;
    ++board_background_rebuilds_;
    return true;
}

void BoardRenderer::set_interactive_resize(bool active)
{
    interactive_resize_ = active;
    if (!active) {
        temporary_pieces_.clear();
        temporary_square_size_ = 0;
    }
}

int BoardRenderer::high_quality_cache_square_size() const
{
    return cache_key_.matches_piece_set(piece_set_id_)
        ? cache_key_.square_size() : 0;
}

unsigned long BoardRenderer::scaled_cache_rebuild_count() const
{
    return scaled_cache_rebuilds_;
}

unsigned long BoardRenderer::scaled_piece_build_count() const
{
    return scaled_piece_builds_;
}

unsigned long BoardRenderer::temporary_piece_build_count() const
{
    return temporary_piece_builds_;
}

unsigned long BoardRenderer::temporary_draw_count() const
{
    return temporary_draws_;
}

double BoardRenderer::resize_horizontal_ms() const
{
    return resize_horizontal_ms_;
}

double BoardRenderer::resize_vertical_ms() const
{
    return resize_vertical_ms_;
}

double BoardRenderer::resize_total_ms() const
{
    return resize_total_ms_;
}

double BoardRenderer::temporary_resize_ms() const
{
    return temporary_resize_ms_;
}

double BoardRenderer::alpha_bounds_ms() const
{
    return alpha_bounds_ms_;
}

double BoardRenderer::ximage_get_ms() const
{
    return ximage_get_ms_;
}

double BoardRenderer::ximage_composite_ms() const
{
    return ximage_composite_ms_;
}

double BoardRenderer::ximage_put_ms() const
{
    return ximage_put_ms_;
}

const BoardRenderer::ScaledPiece* BoardRenderer::scaled_piece(
    int piece, const PngImage& source, int square_size)
{
    std::map<int, ScaledPiece>::iterator found = scaled_pieces_.find(piece);
    if (found != scaled_pieces_.end())
        return &found->second;
    {
        ScaledPiece scaled;
        double elapsed_ms;
        if (!build_scaled_piece(piece, source, square_size, scaled, elapsed_ms))
            return 0;
        try {
            scaled_pieces_[piece] = scaled;
        } catch (const std::bad_alloc&) {
            return 0;
        }
    }
    found = scaled_pieces_.find(piece);
    return found == scaled_pieces_.end() ? 0 : &found->second;
}

bool BoardRenderer::build_scaled_piece(int piece, const PngImage& source,
                                       int square_size, ScaledPiece& scaled,
                                       double& elapsed_ms)
{
    PngResizeProfile filter_profile;
    timeval resize_started;
    timeval resize_finished;
    timeval bounds_started;
    timeval bounds_finished;
    int target_height = PieceLayout::target_height_for_piece(
        *piece_set_, piece, square_size);
    PiecePlacement dimensions = PieceLayout::dimensions_for_target_height(
        source.width, source.height, target_height);
    elapsed_ms = 0.0;
    gettimeofday(&resize_started, 0);
    if (!dimensions.valid() || dimensions.width > square_size ||
        dimensions.height > square_size ||
        !resize_png_lanczos3(source, dimensions.width, dimensions.height,
                             scaled.image, resize_profile_enabled_
                                 ? &filter_profile : 0))
        return false;
    gettimeofday(&resize_finished, 0);
    gettimeofday(&bounds_started, 0);
    if (resize_profile_enabled_) {
        ++scaled_piece_builds_;
        resize_horizontal_ms_ += filter_profile.horizontal_ms;
        resize_vertical_ms_ += filter_profile.vertical_ms;
        resize_total_ms_ += elapsed_milliseconds(resize_started,
                                                  resize_finished);
    }
    if (!png_alpha_bounds(scaled.image, scaled.alpha_bounds))
        return false;
    gettimeofday(&bounds_finished, 0);
    if (resize_profile_enabled_) {
        alpha_bounds_ms_ += elapsed_milliseconds(bounds_started,
                                                  bounds_finished);
    }
    elapsed_ms = elapsed_milliseconds(resize_started, bounds_finished);
    return true;
}

bool BoardRenderer::begin_high_quality_rebuild(int square_size,
                                                unsigned long generation)
{
    cancel_high_quality_rebuild();
    candidate_scaled_pieces_.clear();
    candidate_square_size_ = square_size;
    candidate_generation_ = generation;
    candidate_next_piece_ = 0;
    candidate_active_ = square_size > 0;
    return candidate_active_;
}

HighQualityPieceWorkResult BoardRenderer::build_next_high_quality_piece(
    unsigned long generation, int& piece, double& elapsed_ms)
{
    std::size_t piece_count;
    const int* pieces = unique_piece_raster_worklist(piece_count);
    const PngImage* source = 0;
    if (!candidate_active_ || generation != candidate_generation_ ||
        candidate_next_piece_ >= piece_count) {
        return HQ_PIECE_WORK_FAILED;
    }
    if (!candidate_resize_active_) {
        PiecePlacement dimensions;
        piece = pieces[candidate_next_piece_];
        source = assets_.piece_image(piece);
        if (!source)
            return HQ_PIECE_WORK_FAILED;
        dimensions = PieceLayout::dimensions_for_target_height(
            source->width, source->height,
            PieceLayout::target_height_for_piece(
                *piece_set_, piece, candidate_square_size_));
        if (!dimensions.valid() ||
            dimensions.width > candidate_square_size_ ||
            dimensions.height > candidate_square_size_ ||
            !candidate_resize_job_.start(*source, dimensions.width,
                                         dimensions.height))
            return HQ_PIECE_WORK_FAILED;
        candidate_piece_id_ = piece;
        candidate_resize_active_ = true;
    }
    piece = candidate_piece_id_;
    elapsed_ms = 0.0;
    if (!candidate_resize_job_.step(16))
        return HQ_PIECE_WORK_MORE;
    {
        ScaledPiece scaled;
        PngResizeProfile profile = candidate_resize_job_.profile();
        timeval bounds_started;
        timeval bounds_finished;
        candidate_resize_job_.take_result(scaled.image);
        gettimeofday(&bounds_started, 0);
        if (!png_alpha_bounds(scaled.image, scaled.alpha_bounds))
            return HQ_PIECE_WORK_FAILED;
        gettimeofday(&bounds_finished, 0);
        elapsed_ms = profile.horizontal_ms + profile.vertical_ms +
                     elapsed_milliseconds(bounds_started, bounds_finished);
        try {
            candidate_scaled_pieces_[piece] = scaled;
        } catch (const std::bad_alloc&) {
            return HQ_PIECE_WORK_FAILED;
        }
        if (resize_profile_enabled_) {
            ++scaled_piece_builds_;
            resize_horizontal_ms_ += profile.horizontal_ms;
            resize_vertical_ms_ += profile.vertical_ms;
            resize_total_ms_ += profile.horizontal_ms + profile.vertical_ms;
            alpha_bounds_ms_ += elapsed_milliseconds(bounds_started,
                                                      bounds_finished);
        }
    }
    candidate_resize_job_.cancel();
    candidate_resize_active_ = false;
    candidate_piece_id_ = 0;
    ++candidate_next_piece_;
    return HQ_PIECE_WORK_COMPLETE;
}

bool BoardRenderer::high_quality_rebuild_ready(unsigned long generation) const
{
    return candidate_active_ && generation == candidate_generation_ &&
           candidate_next_piece_ == kUniquePieceRasterCount &&
           candidate_scaled_pieces_.size() == kUniquePieceRasterCount;
}

bool BoardRenderer::commit_high_quality_rebuild(unsigned long generation)
{
    if (!high_quality_rebuild_ready(generation))
        return false;
    scaled_pieces_.swap(candidate_scaled_pieces_);
    cache_key_.set(piece_set_id_, candidate_square_size_);
    candidate_scaled_pieces_.clear();
    temporary_pieces_.clear();
    temporary_square_size_ = 0;
    candidate_square_size_ = 0;
    candidate_generation_ = 0;
    candidate_next_piece_ = 0;
    candidate_active_ = false;
    ++scaled_cache_rebuilds_;
    return true;
}

void BoardRenderer::cancel_high_quality_rebuild()
{
    candidate_resize_job_.cancel();
    candidate_resize_active_ = false;
    candidate_piece_id_ = 0;
    candidate_scaled_pieces_.clear();
    candidate_square_size_ = 0;
    candidate_generation_ = 0;
    candidate_next_piece_ = 0;
    candidate_active_ = false;
}

std::size_t BoardRenderer::candidate_piece_count() const
{
    return candidate_scaled_pieces_.size();
}

unsigned long BoardRenderer::last_piece_instances_drawn() const
{
    return last_piece_instances_drawn_;
}

const BoardDrawMetrics& BoardRenderer::last_draw_metrics() const
{
    return last_draw_metrics_;
}

const BoardRenderer::ScaledPiece* BoardRenderer::temporary_scaled_piece(
    int piece, const PngImage& source, int square_size)
{
    std::map<int, ScaledPiece>::iterator found;
    ScaledPiece temporary;
    const PngImage* resize_source = &source;
    std::map<int, ScaledPiece>::const_iterator high_quality;
    PiecePlacement dimensions;
    timeval resize_started;
    timeval resize_finished;
    timeval bounds_started;
    timeval bounds_finished;
    int target_height;
    if (temporary_square_size_ != square_size) {
        temporary_pieces_.clear();
        temporary_square_size_ = square_size;
    }
    found = temporary_pieces_.find(piece);
    if (found != temporary_pieces_.end())
        return &found->second;
    high_quality = scaled_pieces_.find(piece);
    if (high_quality != scaled_pieces_.end())
        resize_source = &high_quality->second.image;
    target_height = PieceLayout::target_height_for_piece(
        *piece_set_, piece, square_size);
    dimensions = PieceLayout::dimensions_for_target_height(
        source.width, source.height, target_height);
    if (!dimensions.valid() || dimensions.width > square_size ||
        dimensions.height > square_size)
        return 0;
    if (resize_profile_enabled_)
        gettimeofday(&resize_started, 0);
    if (!resize_png_bilinear(*resize_source, dimensions.width,
                             dimensions.height, temporary.image))
        return 0;
    if (resize_profile_enabled_) {
        gettimeofday(&resize_finished, 0);
        ++temporary_piece_builds_;
        temporary_resize_ms_ += elapsed_milliseconds(resize_started,
                                                      resize_finished);
        gettimeofday(&bounds_started, 0);
    }
    if (!png_alpha_bounds(temporary.image, temporary.alpha_bounds))
        return 0;
    if (resize_profile_enabled_) {
        gettimeofday(&bounds_finished, 0);
        alpha_bounds_ms_ += elapsed_milliseconds(bounds_started,
                                                  bounds_finished);
    }
    try {
        temporary_pieces_[piece] = temporary;
    } catch (const std::bad_alloc&) {
        return 0;
    }
    found = temporary_pieces_.find(piece);
    return found == temporary_pieces_.end() ? 0 : &found->second;
}

unsigned long BoardRenderer::pixel(RgbColor color) const
{
    return component_to_mask(color.red, visual_->red_mask) |
           component_to_mask(color.green, visual_->green_mask) |
           component_to_mask(color.blue, visual_->blue_mask);
}

void BoardRenderer::draw_piece_fallback(Drawable drawable,
                                        const PiecePlacement& placement,
                                        const PngImage& image)
{
    XImage* target;
    timeval phase_started;
    timeval phase_finished;
    if (!placement.valid() || !image.valid()) return;
    if (resize_profile_enabled_)
        gettimeofday(&phase_started, 0);
    ++last_draw_metrics_.ximage_get_calls;
    ++last_draw_metrics_.piece_destination_readbacks;
    target = XGetImage(display_, drawable, placement.x, placement.y,
                       static_cast<unsigned int>(placement.width),
                       static_cast<unsigned int>(placement.height), AllPlanes, ZPixmap);
    if (resize_profile_enabled_) {
        double elapsed;
        gettimeofday(&phase_finished, 0);
        elapsed = elapsed_milliseconds(phase_started, phase_finished);
        last_draw_metrics_.ximage_get_ms += elapsed;
        ximage_get_ms_ += elapsed;
        gettimeofday(&phase_started, 0);
    }
    if (!target) return;
    last_draw_metrics_.ximage_get_bytes +=
        static_cast<unsigned long>(target->bytes_per_line) *
        static_cast<unsigned long>(target->height);
    if (resize_profile_enabled_)
        gettimeofday(&phase_started, 0);
    {
        XImageSurface surface(target);
        composite_rgba_image(surface, 0, 0, 0, 0, placement.width,
                             placement.height, &image.rgba[0], image.stride,
                             visual_->red_mask, visual_->green_mask,
                             visual_->blue_mask);
    }
    if (resize_profile_enabled_) {
        gettimeofday(&phase_finished, 0);
        ximage_composite_ms_ += elapsed_milliseconds(phase_started,
                                                      phase_finished);
        last_draw_metrics_.alpha_composite_ms +=
            elapsed_milliseconds(phase_started, phase_finished);
        gettimeofday(&phase_started, 0);
    }
    XPutImage(display_, drawable, graphics_, target, 0, 0, placement.x, placement.y,
              static_cast<unsigned int>(placement.width), static_cast<unsigned int>(placement.height));
    ++last_draw_metrics_.ximage_put_calls;
    last_draw_metrics_.ximage_put_bytes +=
        static_cast<unsigned long>(target->bytes_per_line) *
        static_cast<unsigned long>(target->height);
    if (resize_profile_enabled_) {
        double elapsed;
        gettimeofday(&phase_finished, 0);
        elapsed = elapsed_milliseconds(phase_started, phase_finished);
        last_draw_metrics_.ximage_put_ms += elapsed;
        ximage_put_ms_ += elapsed;
    }
    XDestroyImage(target);
}

void BoardRenderer::draw_coordinates(Drawable drawable, const BoardGeometry& geometry, bool flipped)
{
    XftDraw* draw;
    int index;
    if (!coordinate_font_ || !coordinate_color_allocated_ || geometry.square_size <= 0) return;
    draw = XftDrawCreate(display_, drawable, visual_, colormap_);
    if (!draw) return;
    for (index = 0; index < 8; ++index) {
        char file_label[2];
        char rank_label[2];
        XGlyphInfo file_extents;
        XGlyphInfo rank_extents;
        int file = flipped ? 7 - index : index;
        int rank = flipped ? index + 1 : 8 - index;
        int center_x = geometry.x + index * geometry.square_size + geometry.square_size / 2;
        int center_y = geometry.y + index * geometry.square_size + geometry.square_size / 2;
        int file_baseline;
        int rank_baseline;
        file_label[0] = static_cast<char>('a' + file); file_label[1] = '\0';
        rank_label[0] = static_cast<char>('0' + rank); rank_label[1] = '\0';
        XftTextExtentsUtf8(display_, coordinate_font_, reinterpret_cast<const FcChar8*>(file_label), 1, &file_extents);
        XftTextExtentsUtf8(display_, coordinate_font_, reinterpret_cast<const FcChar8*>(rank_label), 1, &rank_extents);
        file_baseline = geometry.y + geometry.board_size + BoardLayout::frame_width() + coordinate_font_->ascent;
        rank_baseline = center_y + (coordinate_font_->ascent - coordinate_font_->descent) / 2;
        XftDrawStringUtf8(draw, &coordinate_color_, coordinate_font_, center_x - static_cast<int>(file_extents.xOff) / 2,
                          file_baseline, reinterpret_cast<const FcChar8*>(file_label), 1);
        XftDrawStringUtf8(draw, &coordinate_color_, coordinate_font_, geometry.x - BoardLayout::frame_width() - 5 - static_cast<int>(rank_extents.xOff),
                          rank_baseline, reinterpret_cast<const FcChar8*>(rank_label), 1);
    }
    XftDrawDestroy(draw);
}

void BoardRenderer::draw(Drawable drawable, int width, int height, const Position& position,
                         bool flipped, const LastMoveHighlight& highlight, int sidebar_preferred_width)
{
    BoardGeometry geometry = BoardLayout::calculate(width, height, sidebar_preferred_width);
    PieceDrawItem piece_items[64];
    timeval draw_started;
    timeval base_started;
    timeval base_finished;
    timeval draw_finished;
    int piece_item_count = 0;
    int square;
    bool use_temporary_pieces = false;
    if (resize_profile_enabled_)
        gettimeofday(&draw_started, 0);
    last_draw_metrics_ = BoardDrawMetrics();
    last_piece_instances_drawn_ = 0;
    if (resize_profile_enabled_)
        gettimeofday(&base_started, 0);
    XSetForeground(display_, graphics_, pixel(UiPalette().window_background));
    XFillRectangle(display_, drawable, graphics_, 0, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    if (geometry.square_size <= 0) {
        if (resize_profile_enabled_) {
            gettimeofday(&draw_finished, 0);
            last_draw_metrics_.total_ms =
                elapsed_milliseconds(draw_started, draw_finished);
        }
        return;
    }
    if (interactive_resize_ &&
        !cache_key_.matches(piece_set_id_, geometry.square_size)) {
        use_temporary_pieces = true;
    } else if (!interactive_resize_ &&
               !cache_key_.matches(piece_set_id_, geometry.square_size)) {
        scaled_pieces_.clear();
        temporary_pieces_.clear();
        temporary_square_size_ = 0;
        cache_key_.set(piece_set_id_, geometry.square_size);
        if (resize_profile_enabled_)
            ++scaled_cache_rebuilds_;
    }
    if (resize_profile_enabled_ && use_temporary_pieces)
        ++temporary_draws_;
    {
        bool wood_background_ready = false;
        if (board_theme_id_ == "wood" || board_theme_id_ == "wood-veneer") {
            if (!board_background_cache_ ||
                board_background_square_size_ != geometry.square_size ||
                board_background_flipped_ != flipped)
                rebuild_textured_background(geometry.square_size, flipped);
            if (board_background_cache_ &&
                board_background_square_size_ == geometry.square_size &&
                board_background_flipped_ == flipped) {
                const int frame = BoardLayout::frame_width();
                XCopyArea(display_, board_background_cache_, drawable,
                          graphics_, 0, 0,
                          static_cast<unsigned int>(geometry.board_size +
                                                    2 * frame),
                          static_cast<unsigned int>(geometry.board_size +
                                                    2 * frame),
                          geometry.x - frame, geometry.y - frame);
                wood_background_ready = true;
            }
        }
        if (!wood_background_ready) {
            XSetForeground(display_, graphics_, pixel(board_theme_.frame));
            XFillRectangle(display_, drawable, graphics_, geometry.x - BoardLayout::frame_width(), geometry.y - BoardLayout::frame_width(),
                           static_cast<unsigned int>(geometry.board_size + 2 * BoardLayout::frame_width()),
                           static_cast<unsigned int>(geometry.board_size + 2 * BoardLayout::frame_width()));
            for (square = 0; square < 64; ++square) {
                int x;
                int y;
                RgbColor color = BoardLayout::is_light_square(square) ? board_theme_.light_square : board_theme_.dark_square;
                BoardLayout::square_rect(geometry, square, flipped, x, y);
                XSetForeground(display_, graphics_, pixel(color));
                XFillRectangle(display_, drawable, graphics_, x, y,
                               static_cast<unsigned int>(geometry.square_size),
                               static_cast<unsigned int>(geometry.square_size));
                XSetForeground(display_, graphics_, pixel(RgbColor(color.red > 8 ? color.red - 8 : 0,
                    color.green > 8 ? color.green - 8 : 0, color.blue > 8 ? color.blue - 8 : 0)));
                XDrawLine(display_, drawable, graphics_, x, y + geometry.square_size / 3,
                          x + geometry.square_size - 1, y + geometry.square_size / 3 - 2);
                XDrawLine(display_, drawable, graphics_, x, y + geometry.square_size * 2 / 3,
                          x + geometry.square_size - 1, y + geometry.square_size * 2 / 3 - 2);
            }
        }
    }
    if (highlight.valid && geometry.square_size > 10) {
        int marked[2] = { highlight.from, highlight.to };
        int index;
        for (index = 0; index < 2; ++index) {
            int x;
            int y;
            if (marked[index] < 0 || marked[index] >= 64) continue;
            BoardLayout::square_rect(geometry, marked[index], flipped, x, y);
            XSetForeground(display_, graphics_, BlackPixel(display_, screen_));
            XDrawRectangle(display_, drawable, graphics_, x + 1, y + 1,
                           static_cast<unsigned int>(geometry.square_size - 3),
                           static_cast<unsigned int>(geometry.square_size - 3));
            XSetForeground(display_, graphics_, pixel(board_theme_.highlight));
            XDrawRectangle(display_, drawable, graphics_, x + 3, y + 3,
                           static_cast<unsigned int>(geometry.square_size - 7),
                           static_cast<unsigned int>(geometry.square_size - 7));
            XSetForeground(display_, graphics_, WhitePixel(display_, screen_));
            XDrawRectangle(display_, drawable, graphics_, x + 5, y + 5,
                           static_cast<unsigned int>(geometry.square_size - 11),
                           static_cast<unsigned int>(geometry.square_size - 11));
        }
    }
    if (resize_profile_enabled_) {
        gettimeofday(&base_finished, 0);
        last_draw_metrics_.board_base_ms =
            elapsed_milliseconds(base_started, base_finished);
    }
    for (square = 0; square < 64; ++square) {
        int piece = position.piece_at(square);
        const PngImage* image = assets_.piece_image(piece);
        int x;
        int y;
        const ScaledPiece* scaled;
        if (!image) continue;
        BoardLayout::square_rect(geometry, square, flipped, x, y);
        scaled = use_temporary_pieces
            ? temporary_scaled_piece(piece, *image, geometry.square_size)
            : scaled_piece(piece, *image, geometry.square_size);
        if (scaled) {
            PiecePlacement scaled_canvas;
            scaled_canvas.width = scaled->image.width;
            scaled_canvas.height = scaled->image.height;
            PiecePlacement visible_placement =
                PieceLayout::place_alpha_bounds_for_piece(
                    *piece_set_, x, y, geometry.square_size, piece, scaled_canvas,
                    scaled->alpha_bounds);
            if (visible_placement.valid() && piece_item_count < 64) {
                piece_items[piece_item_count].placement = visible_placement;
                piece_items[piece_item_count].image = &scaled->image;
                ++piece_item_count;
                ++last_piece_instances_drawn_;
            }
        }
    }
    last_draw_metrics_.piece_instances = last_piece_instances_drawn_;
    if (board_snapshot_readbacks_for_piece_batch(
            static_cast<std::size_t>(piece_item_count)) != 0) {
        int image_x = geometry.x - BoardLayout::frame_width();
        int image_y = geometry.y - BoardLayout::frame_width();
        int image_right = image_x + geometry.board_size +
                          2 * BoardLayout::frame_width();
        int image_bottom = image_y + geometry.board_size +
                           2 * BoardLayout::frame_width();
        int item;
        for (item = 0; item < piece_item_count; ++item) {
            const PiecePlacement& placement = piece_items[item].placement;
            if (placement.x < image_x) image_x = placement.x;
            if (placement.y < image_y) image_y = placement.y;
            if (placement.x + placement.width > image_right)
                image_right = placement.x + placement.width;
            if (placement.y + placement.height > image_bottom)
                image_bottom = placement.y + placement.height;
        }
        if (image_x < 0) image_x = 0;
        if (image_y < 0) image_y = 0;
        if (image_right > width) image_right = width;
        if (image_bottom > height) image_bottom = height;
        if (image_right > image_x && image_bottom > image_y) {
            XImage* board_image;
            timeval phase_started;
            timeval phase_finished;
            if (resize_profile_enabled_)
                gettimeofday(&phase_started, 0);
            ++last_draw_metrics_.ximage_get_calls;
            board_image = XGetImage(
                display_, drawable, image_x, image_y,
                static_cast<unsigned int>(image_right - image_x),
                static_cast<unsigned int>(image_bottom - image_y),
                AllPlanes, ZPixmap);
            if (resize_profile_enabled_) {
                gettimeofday(&phase_finished, 0);
                last_draw_metrics_.ximage_get_ms =
                    elapsed_milliseconds(phase_started, phase_finished);
                ximage_get_ms_ += last_draw_metrics_.ximage_get_ms;
            }
            if (board_image) {
                last_draw_metrics_.ximage_get_bytes =
                    static_cast<unsigned long>(board_image->bytes_per_line) *
                    static_cast<unsigned long>(board_image->height);
                XImageSurface surface(board_image);
                if (resize_profile_enabled_)
                    gettimeofday(&phase_started, 0);
                for (item = 0; item < piece_item_count; ++item) {
                    const PieceDrawItem& piece_item = piece_items[item];
                    composite_rgba_image(
                        surface, image_x, image_y,
                        piece_item.placement.x, piece_item.placement.y,
                        piece_item.placement.width,
                        piece_item.placement.height,
                        &piece_item.image->rgba[0],
                        piece_item.image->stride, visual_->red_mask,
                        visual_->green_mask, visual_->blue_mask);
                }
                if (resize_profile_enabled_) {
                    gettimeofday(&phase_finished, 0);
                    last_draw_metrics_.alpha_composite_ms =
                        elapsed_milliseconds(phase_started, phase_finished);
                    ximage_composite_ms_ +=
                        last_draw_metrics_.alpha_composite_ms;
                    gettimeofday(&phase_started, 0);
                }
                XPutImage(display_, drawable, graphics_, board_image, 0, 0,
                          image_x, image_y,
                          static_cast<unsigned int>(board_image->width),
                          static_cast<unsigned int>(board_image->height));
                ++last_draw_metrics_.ximage_put_calls;
                last_draw_metrics_.ximage_put_bytes =
                    last_draw_metrics_.ximage_get_bytes;
                if (resize_profile_enabled_) {
                    gettimeofday(&phase_finished, 0);
                    last_draw_metrics_.ximage_put_ms =
                        elapsed_milliseconds(phase_started, phase_finished);
                    ximage_put_ms_ += last_draw_metrics_.ximage_put_ms;
                }
                XDestroyImage(board_image);
            } else {
                /* Keep pieces visible if the batched readback cannot be made. */
                for (item = 0; item < piece_item_count; ++item)
                    draw_piece_fallback(drawable,
                                        piece_items[item].placement,
                                        *piece_items[item].image);
            }
        }
    }
    draw_coordinates(drawable, geometry, flipped);
    if (resize_profile_enabled_) {
        gettimeofday(&draw_finished, 0);
        last_draw_metrics_.total_ms =
            elapsed_milliseconds(draw_started, draw_finished);
    }
}

}
