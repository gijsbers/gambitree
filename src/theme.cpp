#include "gambitree/theme.h"

#include <new>

namespace gambitree {

bool valid_board_theme_id(const std::string& id)
{
    return id == "classic" || id == "wood" || id == "wood-veneer";
}

std::string next_board_theme_id(const std::string& id)
{
    if (id == "classic") return "wood";
    if (id == "wood") return "wood-veneer";
    return "classic";
}

const char* board_theme_name(const std::string& id)
{
    if (id == "wood") return "Wood";
    if (id == "wood-veneer") return "Wood Veneer";
    return "Classic";
}

RgbColor::RgbColor() : red(0), green(0), blue(0) {}
RgbColor::RgbColor(unsigned char red_value, unsigned char green_value, unsigned char blue_value)
    : red(red_value), green(green_value), blue(blue_value) {}

UiPalette::UiPalette()
    : window_background(35, 39, 43), panel_background(43, 47, 52), header_background(53, 58, 64),
      primary_text(234, 232, 225), muted_text(172, 179, 184), separator(79, 85, 91),
      selected_move(190, 143, 61), selected_move_text(31, 29, 24), control_background(66, 72, 78),
      control_hover(85, 92, 98), disabled_control(91, 96, 101), comment(181, 188, 184),
      nag(151, 199, 126), variation(217, 181, 119), pending_engine(174, 159, 125), active_engine(157, 207, 184) {}

BoardTheme::BoardTheme()
    : light_square(222, 184, 129), dark_square(128, 82, 48),
      frame(70, 42, 25), coordinate(224, 205, 171), highlight(231, 191, 65) {}

PieceAssetDirectory::PieceAssetDirectory(const std::string& directory,
                                         unsigned char alpha_threshold,
                                         int crop_padding)
    : directory_(directory), alpha_threshold_(alpha_threshold),
      crop_padding_(crop_padding) {}

unsigned char PieceAssetDirectory::alpha_threshold() const
{
    return alpha_threshold_;
}

int PieceAssetDirectory::crop_padding() const
{
    return crop_padding_;
}

std::string PieceAssetDirectory::path_for(int piece) const
{
    const char* name = 0;
    switch (piece) {
    case WHITE_KING: name = "white-king"; break;
    case WHITE_QUEEN: name = "white-queen"; break;
    case WHITE_ROOK: name = "white-rook"; break;
    case WHITE_BISHOP: name = "white-bishop"; break;
    case WHITE_KNIGHT: name = "white-knight"; break;
    case WHITE_PAWN: name = "white-pawn"; break;
    case BLACK_KING: name = "black-king"; break;
    case BLACK_QUEEN: name = "black-queen"; break;
    case BLACK_ROOK: name = "black-rook"; break;
    case BLACK_BISHOP: name = "black-bishop"; break;
    case BLACK_KNIGHT: name = "black-knight"; break;
    case BLACK_PAWN: name = "black-pawn"; break;
    default: return "";
    }
    return directory_ + "/" + name + ".png";
}

AssetManager::AssetManager() {}

void AssetManager::swap(AssetManager& other)
{
    pieces_.swap(other.pieces_);
}

bool AssetManager::load_piece_theme(const PieceTheme& theme, std::string& error)
{
    return load_piece_set(theme, error);
}

bool AssetManager::load_piece_set(const PieceAssetDirectory& theme,
                                  std::string& error)
{
    int piece;
    std::map<int, PngImage> loaded;
    for (piece = BLACK_KING; piece <= WHITE_KING; ++piece) {
        PngImage image;
        PngImage visible_image;
        std::string path;
        if (piece == EMPTY) continue;
        path = theme.path_for(piece);
        if (!load_png_rgba(path, image, error))
            return false;
        if (!crop_png_to_alpha_threshold(image, theme.alpha_threshold(),
                                         theme.crop_padding(), visible_image)) {
            error = "piece PNG has no visible alpha content: '" + path + "'";
            return false;
        }
        try {
            loaded[piece] = visible_image;
        } catch (const std::bad_alloc&) {
            error = "insufficient memory while loading piece assets";
            return false;
        }
    }
    pieces_.swap(loaded);
    error.clear();
    return true;
}

const PngImage* AssetManager::piece_image(int piece) const
{
    std::map<int, PngImage>::const_iterator found = pieces_.find(piece);
    return found == pieces_.end() ? 0 : &found->second;
}

}
