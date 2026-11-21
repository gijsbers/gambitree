#ifndef GAMBITREE_BOARD_VENEER_H
#define GAMBITREE_BOARD_VENEER_H

#include "gambitree/png_image.h"

namespace gambitree {

/* Two source sheets, with fixed windows attached to logical chess squares.
   CPU raster preparation only; BoardRenderer owns the shared X11 Pixmap cache. */
class BoardVeneer {
public:
    bool load(const std::vector<std::string>& light_paths,
              const std::vector<std::string>& dark_paths, std::string& error);
    bool loaded() const;
    bool render(int square_size, bool flipped, PngImage& board) const;
    static PngAlphaBounds window(int square, int source_size);
private:
    PngImage light_;
    PngImage dark_;
};

}

#endif
