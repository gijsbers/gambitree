#ifndef GAMBITREE_XFT_NOTATION_TEXT_H
#define GAMBITREE_XFT_NOTATION_TEXT_H

#include <X11/Xft/Xft.h>
#include "gambitree/notation_layout.h"

namespace gambitree {

/* Caller owns the returned font. Fontconfig may supply another suitable family. */
XftFont* open_figurine_font(Display* display, int screen, XftFont* text_font,
                             bool bold = true, int scale_percent = 110);
/* Same-size DejaVu Sans fallback for the compact chess-annotation symbols. */
XftFont* open_annotation_font(Display* display, int screen,
                               XftFont* text_font);

/* Measurement and drawing share exactly the same font runs and advances. */
class XftNotationText : public NotationTextMeasurer {
public:
    XftNotationText(Display* display, XftFont* text_font, XftFont* figurine_font,
                    XftFont* annotation_font = 0);
    uint64_t layout_generation() const;
    int text_width(const std::string& text) const;
    int move_text_width(const std::string& text) const;
    int text_range_width(const std::string& text, bool move_text,
                         std::size_t start, std::size_t length) const;
    void text_bounds(const std::string& text, bool move_text, int& left, int& right) const;
    int draw(XftDraw* target, const XftColor* color, int x, int baseline,
             const std::string& text, bool move_text = false) const;
    unsigned int figurine_mask() const;
    int ink_ascent() const;
    int ink_descent() const;
private:
    Display* display_;
    XftFont* text_font_;
    XftFont* figurine_font_;
    XftFont* annotation_font_;
    unsigned int mask_;
    int ink_ascent_;
    int ink_descent_;
    double points_;
    uint64_t generation_;
    int figurine_at(const std::string& text, std::size_t at) const;
    unsigned int annotation_symbol_at(const std::string& text,
                                      std::size_t at) const;
    XftFont* font_at(const std::string& text, std::size_t at) const;
    int optical_adjustment(const std::string& text, std::size_t at) const;
    int prefix_width(const std::string& text, std::size_t offset,
                     bool move_text) const;
    int render(XftDraw* target, const XftColor* color, int x, int baseline,
               const std::string& text, bool move_text, int& left, int& right) const;
};

}
#endif
