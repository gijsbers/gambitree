#ifndef GAMBITREE_THEME_H
#define GAMBITREE_THEME_H

#include <map>
#include <string>

#include "gambitree/chess.h"
#include "gambitree/png_image.h"

namespace gambitree {

bool valid_board_theme_id(const std::string& id);
std::string next_board_theme_id(const std::string& id);
const char* board_theme_name(const std::string& id);

struct RgbColor {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
    RgbColor();
    RgbColor(unsigned char red_value, unsigned char green_value, unsigned char blue_value);
};

class UiPalette {
public:
    UiPalette();
    RgbColor window_background;
    RgbColor panel_background;
    RgbColor header_background;
    RgbColor primary_text;
    RgbColor muted_text;
    RgbColor separator;
    RgbColor selected_move;
    RgbColor selected_move_text;
    RgbColor control_background;
    RgbColor control_hover;
    RgbColor disabled_control;
    RgbColor comment;
    RgbColor nag;
    RgbColor variation;
    RgbColor pending_engine;
    RgbColor active_engine;
};

class BoardTheme {
public:
    BoardTheme();
    RgbColor light_square;
    RgbColor dark_square;
    RgbColor frame;
    RgbColor coordinate;
    RgbColor highlight;
};

class PieceAssetDirectory {
public:
    explicit PieceAssetDirectory(const std::string& directory,
                                 unsigned char alpha_threshold = 4,
                                 int crop_padding = 8);
    std::string path_for(int piece) const;
    unsigned char alpha_threshold() const;
    int crop_padding() const;
private:
    std::string directory_;
    unsigned char alpha_threshold_;
    int crop_padding_;
};

/* Compatibility name for existing callers and tests. */
typedef PieceAssetDirectory PieceTheme;

class AssetManager {
public:
    AssetManager();
    bool load_piece_set(const PieceAssetDirectory& assets,
                        std::string& error);
    bool load_piece_theme(const PieceTheme& theme, std::string& error);
    void swap(AssetManager& other);
    const PngImage* piece_image(int piece) const;
private:
    std::map<int, PngImage> pieces_;
};

}

#endif
