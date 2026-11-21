#include "gambitree/xft_notation_text.h"

namespace gambitree {

namespace {
uint64_t next_xft_layout_generation = 1;
const FcChar32 annotation_symbols[] = {
    0x25a1, 0x2299, 0x21bb, 0x2192, 0x221e, 0x2295,
    0x2206, 0x2207, 0x2194, 0x2264, 0x2248, 0x2265
};

unsigned int utf8_codepoint(const std::string& text, std::size_t at)
{
    if (at >= text.size()) return 0;
    const unsigned char first = static_cast<unsigned char>(text[at]);
    if ((first & 0xe0U) == 0xc0U && at + 1 < text.size())
        return ((first & 0x1fU) << 6) |
               (static_cast<unsigned char>(text[at + 1]) & 0x3fU);
    if ((first & 0xf0U) == 0xe0U && at + 2 < text.size())
        return ((first & 0x0fU) << 12) |
               ((static_cast<unsigned char>(text[at + 1]) & 0x3fU) << 6) |
               (static_cast<unsigned char>(text[at + 2]) & 0x3fU);
    if ((first & 0xf8U) == 0xf0U && at + 3 < text.size())
        return ((first & 0x07U) << 18) |
               ((static_cast<unsigned char>(text[at + 1]) & 0x3fU) << 12) |
               ((static_cast<unsigned char>(text[at + 2]) & 0x3fU) << 6) |
               (static_cast<unsigned char>(text[at + 3]) & 0x3fU);
    return first;
}

bool is_annotation_symbol(unsigned int codepoint)
{
    for (std::size_t i = 0; i < sizeof(annotation_symbols) /
                                sizeof(annotation_symbols[0]); ++i)
        if (annotation_symbols[i] == codepoint) return true;
    return false;
}

XftFont* match_figurine_font(Display* display, int screen, double size, bool bold)
{
    FcPattern* request = FcPatternCreate();
    FcCharSet* chars = FcCharSetCreate();
    if (!request || !chars) {
        if (request) FcPatternDestroy(request);
        if (chars) FcCharSetDestroy(chars);
        return 0;
    }
    for (unsigned int cp = 0x2654; cp <= 0x2659; ++cp) FcCharSetAddChar(chars, cp);
    FcPatternAddString(request, FC_FAMILY, reinterpret_cast<const FcChar8*>("DejaVu Sans"));
    FcPatternAddDouble(request, FC_PIXEL_SIZE, size);
    FcPatternAddInteger(request, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    // DejaVu's outlined chess glyphs are otherwise equally thin in Book/Bold.
    if (bold) FcPatternAddBool(request, FC_EMBOLDEN, FcTrue);
    FcPatternAddCharSet(request, FC_CHARSET, chars);
    FcResult result;
    FcPattern* match = XftFontMatch(display, screen, request, &result);
    FcPatternDestroy(request);
    FcCharSetDestroy(chars);
    if (!match) return 0;
    XftFont* font = XftFontOpenPattern(display, match);
    if (!font) FcPatternDestroy(match);
    return font;
}

XftFont* match_annotation_font(Display* display, int screen,
                                XftFont* text_font)
{
    if (!display || !text_font) return 0;
    FcPattern* request = FcPatternCreate();
    FcCharSet* chars = FcCharSetCreate();
    if (!request || !chars) {
        if (request) FcPatternDestroy(request);
        if (chars) FcCharSetDestroy(chars);
        return 0;
    }
    for (std::size_t i = 0; i < sizeof(annotation_symbols) /
                                sizeof(annotation_symbols[0]); ++i)
        FcCharSetAddChar(chars, annotation_symbols[i]);
    FcPatternAddString(request, FC_FAMILY,
                       reinterpret_cast<const FcChar8*>("DejaVu Sans"));
    double size = 12.0;
    FcPatternGetDouble(text_font->pattern, FC_PIXEL_SIZE, 0, &size);
    FcPatternAddDouble(request, FC_PIXEL_SIZE, size);
    FcPatternAddInteger(request, FC_WEIGHT, FC_WEIGHT_REGULAR);
    FcPatternAddCharSet(request, FC_CHARSET, chars);
    FcResult result;
    FcPattern* match = XftFontMatch(display, screen, request, &result);
    FcPatternDestroy(request);
    FcCharSetDestroy(chars);
    if (!match) return 0;
    XftFont* font = XftFontOpenPattern(display, match);
    if (!font) FcPatternDestroy(match);
    if (font) {
        for (std::size_t i = 0; i < sizeof(annotation_symbols) /
                                    sizeof(annotation_symbols[0]); ++i) {
            if (!XftCharExists(display, font, annotation_symbols[i])) {
                XftFontClose(display, font);
                return 0;
            }
        }
    }
    return font;
}
}

XftFont* open_annotation_font(Display* display, int screen,
                               XftFont* text_font)
{ return match_annotation_font(display, screen, text_font); }

XftFont* open_figurine_font(Display* display, int screen, XftFont* text_font,
                             bool bold, int scale_percent)
{
    double size = 16.0;
    if (text_font) FcPatternGetDouble(text_font->pattern, FC_PIXEL_SIZE, 0, &size);
    XftFont* font = match_figurine_font(display, screen, size * scale_percent / 100.0, bold);
    if (!bold) return font;
    bool complete = font != 0;
    int weight = 0;
    if (font) FcPatternGetInteger(font->pattern, FC_WEIGHT, 0, &weight);
    for (unsigned int cp = 0x2654; complete && cp <= 0x2659; ++cp)
        complete = XftCharExists(display, font, cp) != 0;
    if (complete && weight >= FC_WEIGHT_DEMIBOLD) return font;
    if (font) XftFontClose(display, font);
    // Preserve the established Book font at text size when bold is unavailable.
    return match_figurine_font(display, screen, size, false);
}

XftNotationText::XftNotationText(Display* display, XftFont* text_font,
                                 XftFont* figurine_font,
                                 XftFont* annotation_font)
    : display_(display), text_font_(text_font), figurine_font_(figurine_font),
      annotation_font_(annotation_font),
      mask_(0), ink_ascent_(0), ink_descent_(0), points_(12.0),
      generation_(next_xft_layout_generation++)
{
    if (text_font_) FcPatternGetDouble(text_font_->pattern, FC_SIZE, 0, &points_);
    for (unsigned int i = 0; figurine_font_ && i < 6; ++i) {
        if (!XftCharExists(display_, figurine_font_, 0x2654 + i)) continue;
        mask_ |= 1U << i;
        FcChar32 cp = 0x2654 + i;
        XGlyphInfo ink;
        XftTextExtents32(display_, figurine_font_, &cp, 1, &ink);
        if (ink.y > ink_ascent_) ink_ascent_ = ink.y;
        int below = static_cast<int>(ink.height) - ink.y;
        if (below > ink_descent_) ink_descent_ = below;
    }
    for (std::size_t i = 0; annotation_font_ &&
         i < sizeof(annotation_symbols) / sizeof(annotation_symbols[0]); ++i) {
        if (text_font_ && XftCharExists(display_, text_font_, annotation_symbols[i]))
            continue;
        if (!XftCharExists(display_, annotation_font_, annotation_symbols[i]))
            continue;
        FcChar32 cp = annotation_symbols[i];
        XGlyphInfo ink;
        XftTextExtents32(display_, annotation_font_, &cp, 1, &ink);
        if (ink.y > ink_ascent_) ink_ascent_ = ink.y;
        int below = static_cast<int>(ink.height) - ink.y;
        if (below > ink_descent_) ink_descent_ = below;
    }
}

uint64_t XftNotationText::layout_generation() const { return generation_; }

int XftNotationText::figurine_at(const std::string& text, std::size_t at) const
{
    if (at + 2 < text.size() &&
        static_cast<unsigned char>(text[at]) == 0xe2U &&
        static_cast<unsigned char>(text[at + 1]) == 0x99U) {
        unsigned char last = static_cast<unsigned char>(text[at + 2]);
        if (last >= 0x94U && last <= 0x99U && (mask_ & (1U << (last - 0x94U))))
            return last - 0x94U;
    }
    return -1;
}

unsigned int XftNotationText::annotation_symbol_at(const std::string& text,
                                                    std::size_t at) const
{
    const unsigned int codepoint = utf8_codepoint(text, at);
    return is_annotation_symbol(codepoint) ? codepoint : 0;
}

XftFont* XftNotationText::font_at(const std::string& text, std::size_t at) const
{
    if (figurine_at(text, at) >= 0) return figurine_font_;
    const unsigned int symbol = annotation_symbol_at(text, at);
    if (annotation_font_ && symbol &&
        (!text_font_ || !XftCharExists(display_, text_font_, symbol)) &&
        XftCharExists(display_, annotation_font_, symbol))
        return annotation_font_;
    return text_font_;
}

int XftNotationText::optical_adjustment(const std::string& text, std::size_t at) const
{
    // Only a leading piece in an explicitly marked move/PV, never prose or a
    // promotion piece. The next character belongs to the same SAN token.
    int piece = figurine_at(text, at);
    if (!figurine_font_ || !text_font_ || piece < 0 ||
        at + 3 >= text.size() ||
        (at && text[at-1] != ' ' && text[at-1] != '\t' && text[at-1] != '\n' && text[at-1] != '\r')) return 0;
    unsigned char next = static_cast<unsigned char>(text[at+3]);
    if (!((next >= 'a' && next <= 'h') || (next >= '1' && next <= '8') || next == 'x')) return 0;
    static const int corrections[] = {0, 1, 1, 2, 2, 0}; // K Q R B N P, at 12 pt.
    int correction = static_cast<int>(corrections[piece] * points_ / 12.0 + 0.5);
    XGlyphInfo glyph, following;
    XftTextExtentsUtf8(display_, figurine_font_,
        reinterpret_cast<const FcChar8*>(text.data()+at), 3, &glyph);
    XftTextExtentsUtf8(display_, text_font_, &next, 1, &following);
    // Ink rectangles may meet, but must not overlap, even at 8 pt.
    int safe = glyph.xOff - (static_cast<int>(glyph.width) - glyph.x) - following.x;
    if (correction > safe) correction = safe;
    return correction > 0 ? correction : 0;
}

int XftNotationText::render(XftDraw* target, const XftColor* color, int x, int baseline,
                           const std::string& text, bool move_text, int& left, int& right) const
{
    int advance = 0;
    left = right = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        XftFont* font = font_at(text, at);
        std::size_t end = at;
        do {
            ++end;
            while (end < text.size() &&
                   (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) ++end;
        } while (end < text.size() && font_at(text, end) == font &&
                 !(move_text && (figurine_at(text, at) >= 0 || figurine_at(text, end) >= 0)));
        if (font) {
            const FcChar8* bytes = reinterpret_cast<const FcChar8*>(text.data() + at);
            int length = static_cast<int>(end - at);
            XGlyphInfo extents;
            XftTextExtentsUtf8(display_, font, bytes, length, &extents);
            if (target && color)
                XftDrawStringUtf8(target, color, font, x + advance, baseline, bytes, length);
            int ink_left = advance - extents.x;
            int ink_right = ink_left + extents.width;
            if (ink_left < left) left = ink_left;
            if (ink_right > right) right = ink_right;
            advance += extents.xOff;
            if (move_text) advance -= optical_adjustment(text, at);
        }
        at = end;
    }
    if (advance > right) right = advance;
    return advance;
}

int XftNotationText::text_width(const std::string& text) const
{ int left, right; return render(0, 0, 0, 0, text, false, left, right); }
int XftNotationText::move_text_width(const std::string& text) const
{ int left, right; return render(0, 0, 0, 0, text, true, left, right); }
int XftNotationText::prefix_width(const std::string& text, std::size_t offset,
                                  bool move_text) const
{
    if (offset > text.size()) offset = text.size();
    if (move_text && offset == 3 && text.size() > 3 &&
        figurine_at(text, 0) >= 0 && optical_adjustment(text, 0) > 0) {
        /* The optical advance belongs between the leading figurine and its
           SAN continuation. Include that next SAN byte while measuring the
           prefix, then remove its own advance. */
        return move_text_width(text.substr(0, 4)) -
               text_width(text.substr(3, 1));
    }
    return move_text ? move_text_width(text.substr(0, offset))
                     : text_width(text.substr(0, offset));
}
int XftNotationText::text_range_width(const std::string& text, bool move_text,
                                      std::size_t start,
                                      std::size_t length) const
{
    if (start > text.size()) start = text.size();
    if (length > text.size() - start) length = text.size() - start;
    return prefix_width(text, start + length, move_text) -
           prefix_width(text, start, move_text);
}
void XftNotationText::text_bounds(const std::string& text, bool move_text, int& left, int& right) const
{ render(0, 0, 0, 0, text, move_text, left, right); }
int XftNotationText::draw(XftDraw* target, const XftColor* color, int x, int baseline,
                         const std::string& text, bool move_text) const
{ int left, right; return render(target, color, x, baseline, text, move_text, left, right); }
unsigned int XftNotationText::figurine_mask() const { return mask_; }
int XftNotationText::ink_ascent() const { return ink_ascent_; }
int XftNotationText::ink_descent() const { return ink_descent_; }

}
