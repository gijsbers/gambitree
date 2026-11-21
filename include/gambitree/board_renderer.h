#ifndef GAMBITREE_BOARD_RENDERER_H
#define GAMBITREE_BOARD_RENDERER_H

#include <map>
#include <cstddef>
#include <string>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "gambitree/board_layout.h"
#include "gambitree/board_veneer.h"
#include "gambitree/chess.h"
#include "gambitree/piece_cache_key.h"
#include "gambitree/piece_layout.h"
#include "gambitree/piece_set.h"
#include "gambitree/png_image.h"
#include "gambitree/theme.h"

namespace gambitree {

struct LastMoveHighlight {
    int from;
    int to;
    bool valid;
    LastMoveHighlight();
};

struct BoardDrawMetrics {
    BoardDrawMetrics();
    unsigned long piece_instances;
    unsigned long ximage_get_calls;
    unsigned long piece_destination_readbacks;
    unsigned long ximage_put_calls;
    unsigned long ximage_get_bytes;
    unsigned long ximage_put_bytes;
    double board_base_ms;
    double ximage_get_ms;
    double alpha_composite_ms;
    double ximage_put_ms;
    double total_ms;
};

enum HighQualityPieceWorkResult {
    HQ_PIECE_WORK_FAILED,
    HQ_PIECE_WORK_MORE,
    HQ_PIECE_WORK_COMPLETE
};

class BoardRenderer {
public:
    BoardRenderer(Display* display, int screen);
    ~BoardRenderer();
    bool load_piece_theme(const std::string& theme, std::string& error);
    bool load_piece_set(const std::string& id, std::string& error);
    const std::string& piece_set_id() const;
    bool set_board_theme(const std::string& id);
    const std::string& board_theme_id() const;
    unsigned long board_background_rebuild_count() const;
    int board_background_square_size() const;
    bool wood_texture_assets_loaded() const;
    void set_interactive_resize(bool active);
    int high_quality_cache_square_size() const;
    unsigned long scaled_cache_rebuild_count() const;
    unsigned long scaled_piece_build_count() const;
    unsigned long temporary_draw_count() const;
    unsigned long temporary_piece_build_count() const;
    double resize_horizontal_ms() const;
    double resize_vertical_ms() const;
    double resize_total_ms() const;
    double temporary_resize_ms() const;
    double alpha_bounds_ms() const;
    double ximage_get_ms() const;
    double ximage_composite_ms() const;
    double ximage_put_ms() const;
    bool begin_high_quality_rebuild(int square_size, unsigned long generation);
    HighQualityPieceWorkResult build_next_high_quality_piece(
        unsigned long generation, int& piece, double& elapsed_ms);
    bool high_quality_rebuild_ready(unsigned long generation) const;
    bool commit_high_quality_rebuild(unsigned long generation);
    void cancel_high_quality_rebuild();
    std::size_t candidate_piece_count() const;
    unsigned long last_piece_instances_drawn() const;
    const BoardDrawMetrics& last_draw_metrics() const;
    void draw(Drawable drawable, int width, int height, const Position& position,
              bool flipped, const LastMoveHighlight& highlight, int sidebar_preferred_width);

private:
    struct ScaledPiece {
        PngImage image;
        PngAlphaBounds alpha_bounds;
    };

    Display* display_;
    int screen_;
    GC graphics_;
    Visual* visual_;
    int depth_;
    Colormap colormap_;
    Pixmap board_background_cache_;
    int board_background_square_size_;
    bool board_background_flipped_;
    unsigned long board_background_rebuilds_;
    std::string board_theme_id_;
    PngImage wood_light_texture_;
    PngImage wood_dark_texture_;
    bool wood_textures_load_attempted_;
    BoardVeneer veneer_;
    bool veneer_load_attempted_;
    XftFont* coordinate_font_;
    XftColor coordinate_color_;
    bool coordinate_color_allocated_;
    BoardTheme board_theme_;
    AssetManager assets_;
    const PieceSetDescriptor* piece_set_;
    std::string piece_set_id_;
    PieceCacheKey cache_key_;
    std::map<int, ScaledPiece> scaled_pieces_;
    std::map<int, ScaledPiece> candidate_scaled_pieces_;
    std::map<int, ScaledPiece> temporary_pieces_;
    int candidate_square_size_;
    unsigned long candidate_generation_;
    std::size_t candidate_next_piece_;
    bool candidate_active_;
    bool candidate_resize_active_;
    int candidate_piece_id_;
    PngLanczos3Job candidate_resize_job_;
    unsigned long last_piece_instances_drawn_;
    BoardDrawMetrics last_draw_metrics_;
    int temporary_square_size_;
    bool interactive_resize_;
    bool resize_profile_enabled_;
    unsigned long scaled_cache_rebuilds_;
    unsigned long scaled_piece_builds_;
    unsigned long temporary_draws_;
    unsigned long temporary_piece_builds_;
    double resize_horizontal_ms_;
    double resize_vertical_ms_;
    double resize_total_ms_;
    double temporary_resize_ms_;
    double alpha_bounds_ms_;
    double ximage_get_ms_;
    double ximage_composite_ms_;
    double ximage_put_ms_;

    unsigned long pixel(RgbColor color) const;
    const ScaledPiece* scaled_piece(int piece, const PngImage& source,
                                    int square_size);
    bool build_scaled_piece(int piece, const PngImage& source,
                            int square_size, ScaledPiece& scaled,
                            double& elapsed_ms);
    const ScaledPiece* temporary_scaled_piece(int piece,
                                              const PngImage& source,
                                              int square_size);
    void draw_coordinates(Drawable drawable, const BoardGeometry& geometry, bool flipped);
    bool rebuild_textured_background(int square_size, bool flipped);
    bool load_wood_textures();
    void draw_piece_fallback(Drawable drawable,
                             const PiecePlacement& placement,
                             const PngImage& image);
};

}

#endif
