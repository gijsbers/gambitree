#include "gambitree/x11_notation_renderer.h"
#include "gambitree/metadata_layout.h"
#include "gambitree/delete_dialog.h"
#include "gambitree/session_state.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <iostream>
#include <set>
#include <sstream>

namespace gambitree {

namespace {
enum PaletteColor { UI_WINDOW, UI_PANEL, UI_HEADER, UI_PRIMARY, UI_MUTED, UI_SEPARATOR, UI_SELECTED,
                    UI_SELECTED_TEXT, UI_CONTROL, UI_CONTROL_HOVER, UI_DISABLED, UI_COMMENT, UI_NAG,
                    UI_VARIATION, UI_PENDING, UI_ACTIVE };
XRenderColor to_xrender(RgbColor color) {
    XRenderColor result;
    result.red = static_cast<unsigned short>(color.red) * 257U;
    result.green = static_cast<unsigned short>(color.green) * 257U;
    result.blue = static_cast<unsigned short>(color.blue) * 257U;
    result.alpha = 65535U;
    return result;
}
void set_gc_clip(Display* display, GC graphics, int x, int y, int width, int height) {
    XRectangle clip;
    clip.x = static_cast<short>(x); clip.y = static_cast<short>(y);
    clip.width = static_cast<unsigned short>(width > 0 ? width : 0);
    clip.height = static_cast<unsigned short>(height > 0 ? height : 0);
    XSetClipRectangles(display, graphics, 0, 0, &clip, 1, Unsorted);
}

void trace_pv_fit(const std::string& text, int width, const NotationFontMetrics& metrics,
                  const UciAnalysis& analysis)
{
    static std::string last;
    std::size_t offset = 0;
    std::string accepted;
    int count = 0;
    if (!std::getenv("GAMBITREE_DIAGNOSTICS") || (text + "\n" + analysis.san_pv) == last) return;
    last = text + "\n" + analysis.san_pv;
    std::cerr << "gambitree: diagnostic: PV display snapshot_generation=" << analysis.generation
              << " depth=" << (analysis.info.depth_valid ? analysis.info.depth : 0)
              << " snapshot_pv_tokens=" << analysis.info.pv.size() << " snapshot_san='" << analysis.san_pv
              << "' available_width=" << width << "\n";
    while (offset < text.size()) {
        std::size_t start;
        std::string token;
        std::string candidate;
        while (offset < text.size() && text[offset] == ' ') ++offset;
        if (offset == text.size()) break;
        start = offset;
        while (offset < text.size() && text[offset] != ' ') ++offset;
        token = text.substr(start, offset - start);
        candidate = accepted.empty() ? token : accepted + " " + token;
        std::cerr << "gambitree: diagnostic: PV display token='" << token << "' token_width="
                  << metrics.text_width(token, true) << " cumulative_width=" << metrics.text_width(candidate, true)
                  << " " << (metrics.text_width(candidate, true) <= width ? "fit" : "reject") << "\n";
        if (metrics.text_width(candidate, true) > width) break;
        accepted = candidate;
        ++count;
    }
    std::cerr << "gambitree: diagnostic: PV display output_tokens=" << count << " output='" << accepted << "'\n";
}

int dialog_text_width(Display* display, XftFont* font,
                      const std::string& text)
{
    XGlyphInfo extents;
    if (!font || text.empty())
        return 0;
    XftTextExtentsUtf8(display, font,
                       reinterpret_cast<const FcChar8*>(text.data()),
                       static_cast<int>(text.size()), &extents);
    return static_cast<int>(extents.xOff);
}

int measured_dialog_button_width(Display* display, XftFont* font)
{
    XGlyphInfo cancel_extents;
    XGlyphInfo delete_extents;
    XftTextExtentsUtf8(display, font,
                       reinterpret_cast<const FcChar8*>("Cancel"), 6,
                       &cancel_extents);
    XftTextExtentsUtf8(display, font,
                       reinterpret_cast<const FcChar8*>("Delete"), 6,
                       &delete_extents);
    return delete_dialog_button_width(cancel_extents.xOff,
                                     delete_extents.xOff);
}

void trace_dialog_font(const char* role, XftFont* font)
{
    FcChar8* family = 0;
    FcChar8* style = 0;
    double pixel_size = 0.0;
    if (!std::getenv("GAMBITREE_DIAGNOSTICS") || !font)
        return;
    FcPatternGetString(font->pattern, FC_FAMILY, 0, &family);
    FcPatternGetString(font->pattern, FC_STYLE, 0, &style);
    FcPatternGetDouble(font->pattern, FC_PIXEL_SIZE, 0, &pixel_size);
    std::cerr << "gambitree: diagnostic: delete-dialog " << role
              << " font=" << (family ? reinterpret_cast<char*>(family) : "?")
              << ":" << (style ? reinterpret_cast<char*>(style) : "?")
              << " pixelsize=" << pixel_size
              << " ascent=" << font->ascent << " descent=" << font->descent
              << " height=" << font->height << "\n";
}

std::size_t utf8_character_size(const std::string& text, std::size_t offset)
{
    unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80)
        return 1;
    if ((first & 0xe0) == 0xc0)
        return offset + 1 < text.size() ? 2 : 1;
    if ((first & 0xf0) == 0xe0)
        return offset + 2 < text.size() ? 3 : 1;
    if ((first & 0xf8) == 0xf0)
        return offset + 3 < text.size() ? 4 : 1;
    return 1;
}

void append_wrapped_word(Display* display, XftFont* font, int available,
                         const std::string& word,
                         std::vector<std::string>& rows,
                         std::string& current)
{
    std::string candidate = current.empty() ? word : current + " " + word;
    if (dialog_text_width(display, font, candidate) <= available) {
        current = candidate;
        return;
    }
    if (!current.empty()) {
        rows.push_back(current);
        current.clear();
    }
    if (dialog_text_width(display, font, word) <= available) {
        current = word;
        return;
    }
    {
        std::size_t offset = 0;
        while (offset < word.size()) {
            std::size_t end = offset;
            std::string piece;
            while (end < word.size()) {
                std::size_t next = utf8_character_size(word, end);
                std::string proposed = piece + word.substr(end, next);
                if (!piece.empty() &&
                    dialog_text_width(display, font, proposed) > available)
                    break;
                piece = proposed;
                end += next;
            }
            if (end == offset)
                end += utf8_character_size(word, offset);
            offset = end;
            if (offset < word.size())
                rows.push_back(piece);
            else
                current = piece;
        }
    }
}

std::vector<std::string> wrap_dialog_lines(Display* display, XftFont* font,
                                            int available,
                                            const std::vector<std::string>& lines)
{
    std::vector<std::string> rows;
    std::size_t line_index;
    for (line_index = 0; line_index < lines.size(); ++line_index) {
        std::string current;
        std::size_t offset = 0;
        const std::string& source = lines[line_index];
        while (offset < source.size()) {
            while (offset < source.size() &&
                   std::isspace(static_cast<unsigned char>(source[offset])))
                ++offset;
            if (offset == source.size())
                break;
            std::size_t end = offset;
            while (end < source.size() &&
                   !std::isspace(static_cast<unsigned char>(source[end])))
                end += utf8_character_size(source, end);
            append_wrapped_word(display, font, available,
                                source.substr(offset, end - offset),
                                rows, current);
            offset = end;
        }
        if (!current.empty())
            rows.push_back(current);
        else if (source.empty())
            rows.push_back(std::string());
    }
    return rows;
}
}

SidebarBoxes::SidebarBoxes()
    : controls_top(0), controls_bottom(0), status_top(0), status_bottom(0), engine_top(0), engine_bottom(0),
      metadata_top(0), metadata_bottom(0), notation_top(0), notation_bottom(0), notation_visible_bottom(0) {}

NavigationListLayout::NavigationListLayout()
    : box_x(0), box_y(0), box_width(0), box_height(0),
      content_left(0), content_right(0), filter_top(0), filter_baseline(0),
      filter_text_bottom(0), filter_rule_y(0), filter_query_x(0),
      filter_counter_x(0), list_top(0), list_bottom(0),
      row_height(1), visible_rows(1), marker_x(0), number_x(0),
      number_width(0), description_x(0), description_width(0), footer_y(0) {}

X11NotationRenderer::X11NotationRenderer(Display* display, int screen)
    : display_(display), graphics_(XCreateGC(display, RootWindow(display, screen), 0, 0)), screen_(screen),
      visual_(DefaultVisual(display, screen)), colormap_(DefaultColormap(display, screen)),
      ui_font_(XftFontOpenName(display, screen, "sans-11")), ui_figurine_font_(0),
      ui_annotation_font_(0),
      bold_font_(XftFontOpenName(display, screen, "sans:bold-11")),
      notation_font_(0), figurine_font_(0), annotation_font_(0), engine_font_(0),
      engine_figurine_font_(0), engine_annotation_font_(0),
      dialog_font_(XftFontOpenName(display, screen, "sans-14")),
      dialog_title_font_(XftFontOpenName(
          display, screen, "sans:weight=bold:pixelsize=21")),
      dialog_button_font_(XftFontOpenName(
          display, screen, "sans:weight=bold:pixelsize=19")),
      colors_(), colors_allocated_(), palette_(),
      measurer_(0), engine_measurer_(0), ui_measurer_(0), metrics_(), engine_metrics_(), ui_metrics_(),
      notation_points_(0), engine_points_(0), notation_ascent_(15), engine_ascent_(15),
      ascent_(15), descent_(4), text_height_(23), board_share_(0), figurines_(false),
      search_(), navigation_list_(0), navigation_list_title_(),
      navigation_list_current_(0), engine_pv_clickable_(false),
      engine_pv_left_(0), engine_pv_top_(0) {
    RgbColor palette_colors[16] = { palette_.window_background, palette_.panel_background, palette_.header_background,
        palette_.primary_text, palette_.muted_text, palette_.separator, palette_.selected_move,
        palette_.selected_move_text, palette_.control_background, palette_.control_hover, palette_.disabled_control,
        palette_.comment, palette_.nag, palette_.variation, palette_.pending_engine, palette_.active_engine };
    int index;
    if (!ui_font_) ui_font_ = XftFontOpenName(display, screen, "fixed");
    if (!bold_font_) bold_font_ = ui_font_;
    if (!dialog_font_) dialog_font_ = ui_font_;
    if (!dialog_title_font_) dialog_title_font_ = bold_font_;
    if (!dialog_button_font_) dialog_button_font_ = dialog_title_font_;
    trace_dialog_font("content", dialog_font_);
    trace_dialog_font("title", dialog_title_font_);
    trace_dialog_font("button", dialog_button_font_);
    for (index = 0; index < 16; ++index) {
        XRenderColor value = to_xrender(palette_colors[index]);
        colors_allocated_[index] = XftColorAllocValue(display_, visual_, colormap_, &value, &colors_[index]) != 0;
    }
    if (ui_font_ && ui_font_->ascent > ascent_) ascent_ = ui_font_->ascent;
    if (bold_font_ && bold_font_->ascent > ascent_) ascent_ = bold_font_->ascent;
    if (ui_font_ && ui_font_->descent > descent_) descent_ = ui_font_->descent;
    if (bold_font_ && bold_font_->descent > descent_) descent_ = bold_font_->descent;
    text_height_ = ascent_ + descent_ + 4;
    ui_figurine_font_ = open_figurine_font(display_, screen_, ui_font_, true, 100);
    ui_annotation_font_ = open_annotation_font(display_, screen_, ui_font_);
    ui_measurer_ = new XftNotationText(display_, ui_font_, ui_figurine_font_,
                                       ui_annotation_font_);
    ui_metrics_ = NotationFontMetrics(8, text_height_, ui_measurer_, ui_measurer_->figurine_mask());
    set_text_sizes(12, 12);
}

X11NotationRenderer::~X11NotationRenderer() {
    int index;
    XftFont* fonts[6];
    int font_index;
    delete measurer_;
    delete engine_measurer_;
    delete ui_measurer_;
    if (notation_font_) XftFontClose(display_, notation_font_);
    if (figurine_font_) XftFontClose(display_, figurine_font_);
    if (annotation_font_) XftFontClose(display_, annotation_font_);
    if (engine_font_) XftFontClose(display_, engine_font_);
    if (engine_figurine_font_) XftFontClose(display_, engine_figurine_font_);
    if (engine_annotation_font_) XftFontClose(display_, engine_annotation_font_);
    if (ui_annotation_font_) XftFontClose(display_, ui_annotation_font_);
    for (index = 0; index < 16; ++index) if (colors_allocated_[index]) XftColorFree(display_, visual_, colormap_, &colors_[index]);
    fonts[0] = ui_font_;
    fonts[1] = ui_figurine_font_;
    fonts[2] = bold_font_;
    fonts[3] = dialog_font_;
    fonts[4] = dialog_title_font_;
    fonts[5] = dialog_button_font_;
    for (font_index = 0; font_index < 6; ++font_index) {
        bool seen = false;
        int earlier;
        if (!fonts[font_index])
            continue;
        for (earlier = 0; earlier < font_index; ++earlier)
            if (fonts[earlier] == fonts[font_index])
                seen = true;
        if (!seen)
            XftFontClose(display_, fonts[font_index]);
    }
    if (graphics_) XFreeGC(display_, graphics_);
}

namespace {
XftFont* open_text_font(Display* display, int screen, int points, XftFont* fallback)
{
    std::ostringstream name;
    name << "Roboto-" << points;
    XftFont* font = XftFontOpenName(display, screen, name.str().c_str());
    return font ? font : (fallback ? XftFontCopy(display, fallback) : 0);
}
int text_ascent(XftFont* font, const XftNotationText& text)
{
    int result = font ? font->ascent : 15;
    if (text.ink_ascent() > result) result = text.ink_ascent();
    return result;
}
int text_descent(XftFont* font, const XftNotationText& text)
{
    int result = font ? font->descent : 4;
    if (text.ink_descent() > result) result = text.ink_descent();
    return result;
}
}

bool X11NotationRenderer::set_text_sizes(int notation_points, int engine_points)
{
    notation_points = validated_text_points(notation_points);
    engine_points = validated_text_points(engine_points);
    if (notation_points == notation_points_ && engine_points == engine_points_) return false;
    // Each Open/Copy owns one reference, even if Xft returns a cached pointer.
    XftFont* normal = open_text_font(display_, screen_, notation_points, ui_font_);
    XftFont* pieces = open_figurine_font(display_, screen_, normal);
    XftFont* annotations = open_annotation_font(display_, screen_, normal);
    XftFont* engine = open_text_font(display_, screen_, engine_points, ui_font_);
    XftFont* engine_pieces = open_figurine_font(display_, screen_, engine);
    XftFont* engine_annotations = open_annotation_font(display_, screen_, engine);
    XftNotationText* notation_text = new XftNotationText(display_, normal, pieces, annotations);
    XftNotationText* engine_text = new XftNotationText(display_, engine, engine_pieces,
                                                        engine_annotations);
    delete measurer_;
    delete engine_measurer_;
    if (notation_font_) XftFontClose(display_, notation_font_);
    if (figurine_font_) XftFontClose(display_, figurine_font_);
    if (annotation_font_) XftFontClose(display_, annotation_font_);
    if (engine_font_) XftFontClose(display_, engine_font_);
    if (engine_figurine_font_) XftFontClose(display_, engine_figurine_font_);
    if (engine_annotation_font_) XftFontClose(display_, engine_annotation_font_);
    notation_font_ = normal; figurine_font_ = pieces;
    annotation_font_ = annotations;
    engine_font_ = engine; engine_figurine_font_ = engine_pieces;
    engine_annotation_font_ = engine_annotations;
    measurer_ = notation_text; engine_measurer_ = engine_text;
    notation_points_ = notation_points; engine_points_ = engine_points;
    notation_ascent_ = text_ascent(normal, *measurer_);
    engine_ascent_ = text_ascent(engine, *engine_measurer_);
    metrics_ = NotationFontMetrics(8, notation_ascent_ + text_descent(normal, *measurer_) + 4,
                                    measurer_, measurer_->figurine_mask());
    engine_metrics_ = NotationFontMetrics(8, engine_ascent_ + text_descent(engine, *engine_measurer_) + 4,
                                           engine_measurer_, engine_measurer_->figurine_mask());
    if (std::getenv("GAMBITREE_DIAGNOSTICS")) {
        trace_dialog_font("notation", notation_font_);
        trace_dialog_font("figurines", figurine_font_);
        trace_dialog_font("engine", engine_font_);
        trace_dialog_font("engine-figurines", engine_figurine_font_);
        std::cerr << "gambitree: diagnostic: appearance notation_points=" << notation_points_
                  << " engine_points=" << engine_points_ << " figurine_mask=" << measurer_->figurine_mask()
                  << " notation_line=" << metrics_.line_height() << " engine_line=" << engine_metrics_.line_height() << "\n";
    }
    return true;
}
int X11NotationRenderer::notation_points() const { return notation_points_; }
int X11NotationRenderer::engine_points() const { return engine_points_; }
NotationSearch& X11NotationRenderer::search() { return search_; }
const NotationSearch& X11NotationRenderer::search() const { return search_; }
int X11NotationRenderer::search_bar_height() const
{ return ui_metrics_.line_height() + 10; }
int X11NotationRenderer::notation_viewport_height(int window_width,
                                                   int window_height,
                                                   const PgnGame& game) const
{
    SidebarBoxes section = boxes(window_width, window_height, game);
    int result = section.notation_visible_bottom - section.notation_top;
    if (search_.visible()) result -= search_bar_height();
    return result > 0 ? result : 1;
}
bool X11NotationRenderer::search_bar_contains(int window_width,
                                               int window_height,
                                               const PgnGame& game,
                                               int x, int y) const
{
    if (!search_.visible()) return false;
    SidebarGeometry sidebar = sidebar_geometry(window_width);
    SidebarBoxes section = boxes(window_width, window_height, game);
    const int top = section.notation_visible_bottom - search_bar_height();
    return x >= sidebar.content_left &&
           x < sidebar.content_left + sidebar.content_width &&
           y >= top && y < section.notation_visible_bottom;
}
const NotationFontMetrics& X11NotationRenderer::engine_metrics() const { return engine_metrics_; }
std::string X11NotationRenderer::engine_pv_text(const UciAnalysis& analysis, bool pending, int width) const
{
    std::string pv = layout_engine_pv(analysis, pending, width, engine_metrics_, figurines_).text;
    trace_pv_fit(pv, width, engine_metrics_, analysis);
    return pv;
}

int X11NotationRenderer::engine_pv_hit_test(int x, int y) const
{ return engine_pv_clickable_ ? engine_pv_layout_.hit_test(x - engine_pv_left_, y - engine_pv_top_) : -1; }
const UciAnalysis* X11NotationRenderer::engine_pv_snapshot() const
{ return engine_pv_clickable_ ? &engine_pv_snapshot_ : 0; }

SidebarGeometry X11NotationRenderer::sidebar_geometry(int window_width) const
{ return SidebarLayout::calculate(window_width, sidebar_preferred_width(window_width)); }
int X11NotationRenderer::panel_left(int window_width) const { return sidebar_geometry(window_width).left; }
int X11NotationRenderer::content_width(int window_width) const { return sidebar_geometry(window_width).content_width; }
void X11NotationRenderer::set_board_share(int share)
{ board_share_ = share > 0 && share < 10000 ? share : 0; }
int X11NotationRenderer::board_share() const { return board_share_; }
int X11NotationRenderer::sidebar_preferred_width(int window_width) const
{ return SidebarLayout::width_for_share(window_width, board_share_); }
void X11NotationRenderer::set_figurines(bool enabled) { figurines_ = enabled; }
bool X11NotationRenderer::figurines() const { return figurines_; }

int X11NotationRenderer::controls_top() const { return 8; }
int X11NotationRenderer::controls_bottom() const { return controls_top() + text_height_ + 8; }

SidebarBoxes X11NotationRenderer::boxes(int window_width, int window_height, const PgnGame& game) const {
    SidebarBoxes result;
    int available = content_width(window_width);
    std::vector<MetadataLine> metadata;
    result.controls_top = controls_top();
    result.controls_bottom = controls_bottom();
    result.status_top = result.controls_bottom + 2;
    result.status_bottom = result.status_top + text_height_ + 4;
    result.engine_top = result.status_bottom + 6;
    result.engine_bottom = result.engine_top + 3 * engine_metrics_.line_height() + 8;
    result.metadata_top = result.engine_bottom + 8;
    metadata = MetadataLayout::build(game, available, ui_metrics_, figurines_);
    result.metadata_bottom = result.metadata_top + static_cast<int>(metadata.size()) * text_height_;
    result.notation_top = result.metadata_bottom + 12;
    result.notation_bottom = window_height - 8;
    if (result.notation_top > result.notation_bottom - metrics_.line_height())
        result.notation_top = result.notation_bottom - metrics_.line_height();
    if (result.notation_top < result.engine_bottom + 8) result.notation_top = result.engine_bottom + 8;
    if (result.notation_bottom < result.notation_top) result.notation_bottom = result.notation_top;
    result.notation_visible_bottom = result.notation_top + NotationLayout::full_line_viewport_height(
        result.notation_bottom - result.notation_top, metrics_.line_height());
    return result;
}

int X11NotationRenderer::metadata_bottom(int window_width, int window_height, const PgnGame& game, int engine_lines) const {
    (void)engine_lines;
    return boxes(window_width, window_height, game).notation_top;
}
const NotationFontMetrics& X11NotationRenderer::metrics() const { return metrics_; }
unsigned long X11NotationRenderer::pixel(int color) const { return colors_allocated_[color] ? colors_[color].pixel : BlackPixel(display_, screen_); }
void X11NotationRenderer::draw_text(XftDraw* draw, XftFont* font, int color, int x, int baseline, const std::string& text) const {
    if (!draw || !font || text.empty() || !colors_allocated_[color]) return;
    XftDrawStringUtf8(draw, &colors_[color], font, x, baseline, reinterpret_cast<const FcChar8*>(text.data()), static_cast<int>(text.size()));
}
void X11NotationRenderer::set_clip(XftDraw* draw, int x, int y, int width, int height) const {
    XRectangle rectangle;
    rectangle.x = static_cast<short>(x); rectangle.y = static_cast<short>(y);
    rectangle.width = static_cast<unsigned short>(width > 0 ? width : 0);
    rectangle.height = static_cast<unsigned short>(height > 0 ? height : 0);
    XftDrawSetClipRectangles(draw, 0, 0, &rectangle, 1);
}

/* Layout y coordinates always identify the top of a line box.  Xft receives a baseline. */
int X11NotationRenderer::baseline(int line_top) const
{ return line_top + (text_height_ - ascent_ - descent_) / 2 + ascent_; }
int X11NotationRenderer::notation_baseline(int line_top) const { return line_top + 2 + notation_ascent_; }
int X11NotationRenderer::engine_baseline(int line_top) const { return line_top + 2 + engine_ascent_; }

namespace {

void trace_notation_lines(Display* display, XftFont* font, const NotationLayout& layout,
                          int notation_top, int notation_bottom, int scroll_offset,
                          int ascent, int descent, int line_height)
{
    static bool reported = false;
    std::set<int> seen;
    std::size_t index;
    int count = 0;
    int previous_glyph_bottom = -2147483647;
    bool has_move = false;
    if (reported || !std::getenv("GAMBITREE_DIAGNOSTICS") || !font) return;
    for (index = 0; index < layout.items().size(); ++index)
        if (layout.items()[index].node) { has_move = true; break; }
    if (!has_move) return;
    for (index = 0; index < layout.items().size() && count < 10; ++index) {
        const NotationItem& item = layout.items()[index];
        int top, base, bottom;
        XGlyphInfo glyph;
        if (seen.find(item.y) != seen.end()) continue;
        top = notation_top + item.y - scroll_offset;
        bottom = top + line_height;
        seen.insert(item.y);
        base = top + (line_height - ascent - descent) / 2 + ascent;
        XftTextExtentsUtf8(display, font, reinterpret_cast<const FcChar8*>(item.text.data()),
                           static_cast<int>(item.text.size()), &glyph);
        std::cerr << "gambitree: notation diagnostic line=" << (item.y / line_height)
                  << " top=" << top << " baseline=" << base << " bottom=" << bottom
                  << " ascent=" << ascent << " descent=" << descent << " height=" << line_height
                  << " scroll=" << scroll_offset << " viewport_bottom=" << notation_bottom << " render_y=" << base
                  << " glyph=[" << (base - glyph.y) << ',' << (base - glyph.y + glyph.height) << ']';
        if (base - glyph.y < previous_glyph_bottom) std::cerr << " OVERLAP";
        std::cerr << " text='" << item.text << "'\n";
        previous_glyph_bottom = base - glyph.y + glyph.height;
        ++count;
    }
    if (count) reported = true;
}

}

void X11NotationRenderer::draw(Drawable drawable, int window_width, int window_height, const PgnGame& game,
                               const NotationLayout& layout, const MoveNode* current, int scroll_offset,
                               const EngineDisplay& engine_display) const {
    SidebarGeometry sidebar = sidebar_geometry(window_width);
    int left = sidebar.left, width = sidebar.width, inner_left = sidebar.content_left, inner_width = sidebar.content_width;
    SidebarBoxes section = boxes(window_width, window_height, game);
    int notation_visible_bottom = section.notation_visible_bottom;
    int line, metadata_y = section.metadata_top;
    std::vector<MetadataLine> metadata;
    std::size_t index;
    XftDraw* draw;
    engine_pv_clickable_ = false;
    if (width <= 0 || window_height <= 0) return;
    if (search_.visible()) {
        notation_visible_bottom -= search_bar_height();
        if (notation_visible_bottom < section.notation_top)
            notation_visible_bottom = section.notation_top;
    }
    draw = XftDrawCreate(display_, drawable, visual_, colormap_); if (!draw) return;
    XSetForeground(display_, graphics_, pixel(UI_PANEL));
    XFillRectangle(display_, drawable, graphics_, left, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(window_height));
    XSetForeground(display_, graphics_, pixel(UI_HEADER));
    XFillRectangle(display_, drawable, graphics_, left, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(section.status_bottom));
    XFillRectangle(display_, drawable, graphics_, left, section.engine_top - 4, static_cast<unsigned int>(width),
                   static_cast<unsigned int>(section.engine_bottom - section.engine_top + 8));
    XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
    XFillRectangle(display_, drawable, graphics_, left, section.status_bottom, static_cast<unsigned int>(width), 1);
    XFillRectangle(display_, drawable, graphics_, left, section.engine_bottom + 4, static_cast<unsigned int>(width), 1);
    set_clip(draw, inner_left, section.engine_top, inner_width, section.engine_bottom - section.engine_top);
    {
        const UciAnalysis* detail = engine_display.detail_analysis();
        const UciAnalysis* score_analysis = engine_display.score_analysis();
        const UciAnalysis* pv_analysis = engine_display.pv_analysis();
        int line_height = engine_metrics_.line_height();
        int row_one = engine_baseline(section.engine_top);
        int row_two = engine_baseline(section.engine_top + line_height);
        int row_three = engine_baseline(section.engine_top + 2 * line_height);
        int gap = engine_metrics_.space_width();
        int status_x = inner_left + engine_metrics_.text_width(engine_display.name()) + 3 * gap;
        int depth_right = inner_left + engine_metrics_.text_width("Depth 999");
        int score_x = depth_right + 3 * gap;
        int seldepth_x = score_x + engine_metrics_.text_width("-12.45 [pending]") + 3 * gap;
        draw_text(draw, engine_font_, UI_PRIMARY, inner_left, row_one, engine_display.name());
        draw_text(draw, engine_font_, UI_MUTED, status_x, row_one, engine_display.status());
        draw_text(draw, engine_font_, detail && engine_display.score_is_current() ? UI_ACTIVE : UI_PENDING,
                  inner_left, row_two, "Depth");
        if (detail && detail->info.depth_valid) {
            std::ostringstream depth; depth << detail->info.depth;
            draw_text(draw, engine_font_, engine_display.score_is_current() ? UI_ACTIVE : UI_PENDING,
                      depth_right - engine_metrics_.text_width(depth.str()), row_two, depth.str());
            if (detail->info.seldepth_valid) {
                std::ostringstream seldepth; seldepth << "sel " << detail->info.seldepth;
                draw_text(draw, engine_font_, UI_MUTED, seldepth_x, row_two, seldepth.str());
            }
        }
        if (score_analysis) {
            std::string score = UciEngine::format_score(*score_analysis);
            if (!engine_display.score_is_current()) score += " [pending]";
            draw_text(draw, engine_font_, engine_display.score_is_current() ? UI_ACTIVE : UI_PENDING, score_x, row_two, score);
        } else draw_text(draw, engine_font_, UI_MUTED, score_x, row_two, "--");
        if (pv_analysis) {
            engine_pv_snapshot_ = *pv_analysis;
            engine_pv_layout_ = layout_engine_pv(*pv_analysis, !engine_display.pv_is_current(),
                                                inner_width, engine_metrics_, figurines_);
            trace_pv_fit(engine_pv_layout_.text, inner_width, engine_metrics_, *pv_analysis);
            engine_pv_left_ = inner_left;
            engine_pv_top_ = section.engine_top + 2 * line_height;
            engine_pv_clickable_ = engine_display.pv_is_current();
            const int selected = engine_display.preview_index();
            const NotationItem* item = selected >= 0 && static_cast<std::size_t>(selected) < engine_pv_layout_.moves.size()
                ? &engine_pv_layout_.moves[selected] : 0;
            if (item) {
                set_gc_clip(display_, graphics_, inner_left, section.engine_top, inner_width,
                            section.engine_bottom - section.engine_top);
                XSetForeground(display_, graphics_, pixel(UI_SELECTED));
                XFillRectangle(display_, drawable, graphics_, inner_left + item->x + item->ink_left,
                               engine_pv_top_ + 2, static_cast<unsigned int>(item->ink_right - item->ink_left),
                               static_cast<unsigned int>(item->height - 4));
                XSetClipMask(display_, graphics_, None);
            }
            const int pv_color = engine_display.pv_is_current() ? UI_ACTIVE : UI_PENDING;
            if (item) {
                // Draw each glyph once, so selected antialiased edges match PGN text.
                for (std::size_t i = 0; i < engine_pv_layout_.moves.size(); ++i) {
                    const NotationItem& move = engine_pv_layout_.moves[i];
                    const int color = static_cast<int>(i) == selected ? UI_SELECTED_TEXT : pv_color;
                    if (colors_allocated_[color])
                        engine_measurer_->draw(draw, &colors_[color], inner_left + move.x, row_three, move.text, true);
                }
            } else if (colors_allocated_[pv_color])
                engine_measurer_->draw(draw, &colors_[pv_color], inner_left, row_three, engine_pv_layout_.text, true);
        }
    }
    set_clip(draw, inner_left, section.metadata_top, inner_width, section.metadata_bottom - section.metadata_top);
    metadata = MetadataLayout::build(game, inner_width, ui_metrics_, figurines_);
    for (line = 0; line < static_cast<int>(metadata.size()) && metadata_y < section.metadata_bottom; ++line) {
        if (colors_allocated_[metadata[line].primary ? UI_PRIMARY : UI_MUTED])
            ui_measurer_->draw(draw,
                &colors_[metadata[line].primary ? UI_PRIMARY : UI_MUTED],
                inner_left, baseline(metadata_y), metadata[line].text, false);
        metadata_y += text_height_;
    }
    XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
    XFillRectangle(display_, drawable, graphics_, inner_left, section.notation_top - 6, static_cast<unsigned int>(inner_width), 1);
    set_clip(draw, inner_left, section.notation_top, inner_width, notation_visible_bottom - section.notation_top);
    set_gc_clip(display_, graphics_, inner_left, section.notation_top, inner_width, notation_visible_bottom - section.notation_top);
    trace_notation_lines(display_, notation_font_, layout, section.notation_top, notation_visible_bottom,
                         scroll_offset, notation_ascent_, metrics_.line_height()-notation_ascent_-4, metrics_.line_height());
    std::size_t first_visible, last_visible;
    layout.item_range(scroll_offset,
                      scroll_offset + notation_visible_bottom - section.notation_top,
                      first_visible, last_visible);
    const std::vector<NotationSelectionRect> selection = layout.selection_rects(
        current, scroll_offset,
        scroll_offset + notation_visible_bottom - section.notation_top);
    XSetForeground(display_, graphics_, pixel(UI_SELECTED));
    for (std::size_t r = 0; r < selection.size(); ++r) {
        const NotationSelectionRect& rect = selection[r];
        int top = section.notation_top + rect.y - scroll_offset;
        if (top < section.notation_top || top + rect.height > notation_visible_bottom) continue;
        XFillRectangle(display_, drawable, graphics_, inner_left + rect.x, top + 2,
                       static_cast<unsigned int>(rect.width), static_cast<unsigned int>(rect.height - 4));
    }
    for (index = first_visible; index < last_visible; ++index) {
        const NotationItem& item = layout.items()[index];
        int item_top = section.notation_top + item.y - scroll_offset;
        int y = notation_baseline(item_top), color = UI_PRIMARY;
        if (item_top < section.notation_top || item_top + item.height > notation_visible_bottom) continue;
        if (item.kind == NOTATION_COMMENT) color = UI_COMMENT;
        if (item.kind == NOTATION_NAG) color = UI_NAG;
        if (item.kind == NOTATION_VARIATION) color = UI_VARIATION;
        if (item.kind == NOTATION_RESULT) color = UI_ACTIVE;
        if (item.kind == NOTATION_MOVE && !item.node) color = UI_MUTED;
        if (item.selects_move() && item.node == current) color = UI_SELECTED_TEXT;
        if (colors_allocated_[color]) measurer_->draw(draw, &colors_[color], inner_left + item.x, y,
                                                     item.text, item.kind == NOTATION_MOVE);
    }
    {
        const std::vector<NotationSearchHighlight> matches =
            search_.highlights(layout, scroll_offset,
                scroll_offset + notation_visible_bottom - section.notation_top);
        for (std::size_t i = 0; i < matches.size(); ++i) {
            const NotationTextRangeRect& rect = matches[i].rect;
            const int top = section.notation_top + rect.y - scroll_offset + 2;
            const int color = matches[i].current ? UI_ACTIVE : UI_MUTED;
            if (rect.width > 0 && rect.height > 4 && colors_allocated_[color]) {
                XSetForeground(display_, graphics_, pixel(color));
                XDrawRectangle(display_, drawable, graphics_, inner_left + rect.x,
                               top, static_cast<unsigned int>(rect.width - 1),
                               static_cast<unsigned int>(rect.height - 5));
            }
        }
    }
    if (search_.visible()) {
        const int bar_height = search_bar_height();
        const int bar_top = section.notation_visible_bottom - bar_height;
        const int bar_baseline = baseline(bar_top + 4);
        const std::string label = "Find:";
        std::ostringstream count;
        if (search_.has_current()) count << search_.current_result() + 1;
        else count << 0;
        count << " / " << search_.result_count();
        const int count_width = ui_metrics_.text_width(count.str());
        const int query_x = inner_left + 8 + ui_metrics_.text_width(label + " ");
        const int count_x = inner_left + inner_width - count_width - 8;
        const int query_available = count_x - query_x - 8;
        std::string visible_query = search_.query();
        while (!visible_query.empty() &&
               ui_metrics_.text_width(visible_query) > query_available) {
            std::size_t first = 1;
            while (first < visible_query.size() &&
                   (static_cast<unsigned char>(visible_query[first]) & 0xc0U) == 0x80U)
                ++first;
            visible_query.erase(0, first);
        }
        set_clip(draw, inner_left, bar_top, inner_width, bar_height);
        set_gc_clip(display_, graphics_, inner_left, bar_top, inner_width,
                    bar_height);
        XSetForeground(display_, graphics_, pixel(UI_HEADER));
        XFillRectangle(display_, drawable, graphics_, inner_left, bar_top,
                       static_cast<unsigned int>(inner_width),
                       static_cast<unsigned int>(bar_height));
        XSetForeground(display_, graphics_, pixel(UI_ACTIVE));
        XDrawRectangle(display_, drawable, graphics_, inner_left, bar_top,
                       static_cast<unsigned int>(inner_width - 1),
                       static_cast<unsigned int>(bar_height - 1));
        draw_text(draw, ui_font_, UI_PRIMARY, inner_left + 8, bar_baseline, label);
        if (search_.select_all() && !visible_query.empty()) {
            XSetForeground(display_, graphics_, pixel(UI_SELECTED));
            XFillRectangle(display_, drawable, graphics_, query_x,
                           bar_top + 3,
                           static_cast<unsigned int>(ui_metrics_.text_width(visible_query)),
                           static_cast<unsigned int>(bar_height - 6));
        }
        ui_measurer_->draw(draw,
            &colors_[search_.select_all() ? UI_SELECTED_TEXT : UI_PRIMARY],
            query_x, bar_baseline, visible_query, false);
        if (!search_.select_all()) {
            const int caret_x = query_x + ui_metrics_.text_width(visible_query);
            XSetForeground(display_, graphics_, pixel(UI_PRIMARY));
            XDrawLine(display_, drawable, graphics_, caret_x, bar_top + 5,
                      caret_x, bar_top + bar_height - 6);
        }
        draw_text(draw, ui_font_, UI_ACTIVE, count_x, bar_baseline, count.str());
    }
    XSetClipMask(display_, graphics_, None); XftDrawSetClip(draw, 0); XftDrawDestroy(draw);
}

void X11NotationRenderer::draw_controls(Drawable drawable, int window_width, const std::string& label,
                                        bool previous_file, bool next_file, bool previous_game, bool next_game) const {
    const char* captions[] = { "<F", "F>", "<G", "G>" };
    bool enabled[] = { previous_file, next_file, previous_game, next_game };
    SidebarGeometry sidebar = sidebar_geometry(window_width);
    int left = sidebar.content_left, index;
    int top = controls_top();
    XftDraw* draw = XftDrawCreate(display_, drawable, visual_, colormap_);
    if (!draw) return;
    set_clip(draw, left, top, sidebar.content_width, controls_bottom() - top);
    for (index = 0; index < 4; ++index) {
        int x = left + index * 31;
        XSetForeground(display_, graphics_, pixel(enabled[index] ? UI_CONTROL : UI_DISABLED));
        XFillRectangle(display_, drawable, graphics_, x, top, 27, static_cast<unsigned int>(text_height_ + 2));
        XSetForeground(display_, graphics_, pixel(UI_SEPARATOR)); XDrawRectangle(display_, drawable, graphics_, x, top, 26, static_cast<unsigned int>(text_height_ + 1));
        draw_text(draw, ui_font_, enabled[index] ? UI_PRIMARY : UI_MUTED, x + 5, baseline(top + 1), captions[index]);
    }
    draw_text(draw, bold_font_, UI_PRIMARY, left + 132, baseline(top + 1), label);
    XftDrawSetClip(draw, 0); XftDrawDestroy(draw);
}

void X11NotationRenderer::draw_status(Drawable drawable, int window_width, const std::string& text) const {
    SidebarGeometry sidebar = sidebar_geometry(window_width); XftDraw* draw = XftDrawCreate(display_, drawable, visual_, colormap_);
    if (!draw) return;
    int top = controls_bottom() + 2;
    set_clip(draw, sidebar.content_left, top, sidebar.content_width, text_height_ + 4);
    draw_text(draw, ui_font_, UI_MUTED, sidebar.content_left, baseline(top + 2), text);
    XftDrawSetClip(draw, 0); XftDrawDestroy(draw);
}

KeyboardHelpLayout X11NotationRenderer::keyboard_help_layout(int width, int height) const
{
    XftNotationText body_text(display_, dialog_font_, 0);
    XftNotationText key_text(display_, dialog_button_font_, 0);
    XftNotationText title_text(display_, dialog_title_font_, 0);
    int line_height = std::max(dialog_font_->ascent + dialog_font_->descent,
                               dialog_button_font_->ascent + dialog_button_font_->descent) + 6;
    NotationFontMetrics body(8, line_height, &body_text);
    NotationFontMetrics keys(8, line_height, &key_text);
    NotationFontMetrics title(8, dialog_title_font_->ascent + dialog_title_font_->descent + 6, &title_text);
    return KeyboardHelpLayout::build(width, height, body, keys, title);
}

void X11NotationRenderer::draw_keyboard_help(Drawable drawable, int width, int height, int& scroll) const
{
    if (width < 1 || height < 1) return;
    KeyboardHelpLayout help = keyboard_help_layout(width, height);
    if (scroll < 0) scroll = 0;
    if (scroll > help.max_scroll()) scroll = help.max_scroll();
    XSetClipMask(display_, graphics_, None);
    XSetForeground(display_, graphics_, pixel(UI_WINDOW));
    XFillRectangle(display_, drawable, graphics_, 0, 0, width, height);
    XSetForeground(display_, graphics_, pixel(UI_PANEL));
    XFillRectangle(display_, drawable, graphics_, help.x, help.y, help.width, help.height);
    XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
    XDrawRectangle(display_, drawable, graphics_, help.x, help.y, help.width-1, help.height-1);
    XftDraw* draw = XftDrawCreate(display_, drawable, visual_, colormap_);
    if (!draw) return;
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<KeyboardHelpText>& lines = pass ? help.content : help.fixed;
        int clip_top = pass ? help.body_top : help.y;
        int clip_bottom = pass ? help.body_bottom : help.y+help.height;
        set_clip(draw, help.x, clip_top, help.width, clip_bottom-clip_top);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const KeyboardHelpText& line = lines[i];
            int top = pass ? help.body_top+line.y-scroll : line.y;
            if (top < clip_top || top+line.height > clip_bottom) continue;
            XftFont* font = line.kind == HELP_TITLE ? dialog_title_font_ :
                (line.kind == HELP_KEY || line.kind == HELP_GROUP ? dialog_button_font_ : dialog_font_);
            int color = line.kind == HELP_KEY ? UI_ACTIVE :
                (line.kind == HELP_CLOSE || line.kind == HELP_FOOTER ? UI_MUTED : UI_PRIMARY);
            int baseline = top+(line.height-font->ascent-font->descent)/2+font->ascent;
            draw_text(draw, font, color, line.x, baseline, line.text);
        }
    }
    XftDrawSetClip(draw, 0);
    XftDrawDestroy(draw);
}

std::size_t X11NotationRenderer::navigation_list_visible_rows(int width,
                                                               int height) const
{
    NavigationListState empty;
    return static_cast<std::size_t>(navigation_list_layout(width, height,
                                                           empty).visible_rows);
}

NavigationListLayout X11NotationRenderer::navigation_list_layout(
    int width, int height, const NavigationListState& list) const
{
    NavigationListLayout result;
    result.box_x = std::max(8, width / 12);
    result.box_y = std::max(8, height / 12);
    result.box_width = std::max(1, width - 2 * result.box_x);
    result.box_height = std::max(1, height - 2 * result.box_y);
    const int margin = std::min(24, std::max(8, result.box_width / 16));
    result.content_left = result.box_x + margin;
    result.content_right = std::max(result.content_left,
                                    result.box_x + result.box_width - margin);
    result.filter_top = result.box_y + 54;
    result.filter_baseline = result.filter_top + dialog_button_font_->ascent;
    result.filter_text_bottom = result.filter_baseline +
                                dialog_button_font_->descent;
    result.filter_rule_y = result.filter_text_bottom + 5;
    result.list_top = std::max(result.box_y + 92,
                              result.filter_rule_y + 10);
    result.list_bottom = std::max(result.list_top,
                                  result.box_y + result.box_height - 28);
    const int ui_ink_height = ui_measurer_
        ? ui_measurer_->ink_ascent() + ui_measurer_->ink_descent()
        : ui_metrics_.line_height();
    result.row_height = std::max(dialog_font_->height, ui_ink_height) + 8;
    result.visible_rows = std::max(1, (result.list_bottom - result.list_top) /
                                      std::max(1, result.row_height));
    result.footer_y = result.box_y + result.box_height - 8;

    const int filter_label_width = dialog_text_width(
        display_, dialog_button_font_, "Filter: ");
    std::ostringstream counter_stream;
    counter_stream << list.matches().size() << " of " << list.items().size();
    const std::string counter = counter_stream.str();
    const int counter_width = dialog_text_width(display_, dialog_button_font_,
                                                 counter);
    const int gap = 12;
    result.filter_query_x = result.content_left + filter_label_width;
    result.filter_counter_x = std::max(result.filter_query_x,
        result.content_right - counter_width);
    result.filter_query_width = std::max(0, result.filter_counter_x - gap -
                                             result.filter_query_x);
    result.marker_x = result.content_left;
    const int marker_width = ui_measurer_ ? ui_measurer_->text_width("•") : 12;
    result.number_x = result.marker_x + marker_width + 8;
    if (list.kind() == NAVIGATION_LIST_GAMES) {
        std::ostringstream number;
        number << list.items().size() << ".";
        result.number_width = ui_measurer_
            ? ui_measurer_->text_width(number.str()) : 24;
        result.description_x = result.number_x + result.number_width + 10;
    } else {
        result.number_width = 0;
        result.description_x = result.number_x;
    }
    result.description_width = std::max(0, result.content_right -
                                            result.description_x);
    return result;
}

int X11NotationRenderer::navigation_list_row_at(int width, int height,
                                                int x, int y) const
{
    NavigationListState empty;
    const NavigationListLayout layout = navigation_list_layout(width, height,
                                                                empty);
    if (x < layout.content_left || x >= layout.content_right ||
        y < layout.list_top || y >= layout.list_bottom)
        return -1;
    return (y - layout.list_top) / layout.row_height;
}

void X11NotationRenderer::draw_navigation_list(
    Drawable drawable, int width, int height,
    const NavigationListState& list, const std::string& title,
    std::size_t current_index) const
{
    if (!list.visible() || width < 1 || height < 1) return;
    const NavigationListLayout layout = navigation_list_layout(width, height,
                                                                list);
    const int box_x = layout.box_x, box_y = layout.box_y;
    const int box_width = layout.box_width, box_height = layout.box_height;
    const int row_height = layout.row_height;
    const int ui_ink_ascent = ui_measurer_ ? ui_measurer_->ink_ascent()
                                           : ui_font_->ascent;
    const int ui_ink_height = ui_measurer_
        ? ui_measurer_->ink_ascent() + ui_measurer_->ink_descent()
        : ui_metrics_.line_height();
    const int list_top = layout.list_top, list_bottom = layout.list_bottom;
    const int visible = layout.visible_rows;
    const std::vector<std::string>& items = list.items();
    const std::vector<std::size_t>& matches = list.matches();
    XftDraw* draw;
    XSetClipMask(display_, graphics_, None);
    XSetForeground(display_, graphics_, pixel(UI_WINDOW));
    XFillRectangle(display_, drawable, graphics_, 0, 0,
                   static_cast<unsigned int>(width),
                   static_cast<unsigned int>(height));
    XSetForeground(display_, graphics_, pixel(UI_PANEL));
    XFillRectangle(display_, drawable, graphics_, box_x, box_y,
                   static_cast<unsigned int>(box_width),
                   static_cast<unsigned int>(box_height));
    XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
    XDrawRectangle(display_, drawable, graphics_, box_x, box_y,
                   static_cast<unsigned int>(box_width - 1),
                   static_cast<unsigned int>(box_height - 1));
    XSetForeground(display_, graphics_, pixel(UI_HEADER));
    XFillRectangle(display_, drawable, graphics_, box_x + 1, box_y + 1,
                   static_cast<unsigned int>(box_width - 2), 42);
    draw = XftDrawCreate(display_, drawable, visual_, colormap_);
    if (!draw) return;
    set_clip(draw, layout.content_left, box_y,
             std::max(0, layout.content_right - layout.content_left), 42);
    draw_text(draw, dialog_title_font_, UI_PRIMARY, layout.content_left,
              box_y + 29, title);
    XftDrawSetClip(draw, 0);
    {
        const int baseline = layout.filter_baseline;
        set_clip(draw, layout.content_left, layout.filter_top,
                 std::max(0, layout.filter_query_x - layout.content_left),
                 dialog_button_font_->height);
        draw_text(draw, dialog_button_font_, UI_ACTIVE, layout.content_left,
                  baseline, "Filter: ");
        XftDrawSetClip(draw, 0);
        set_clip(draw, layout.filter_query_x, layout.filter_top,
                 layout.filter_query_width, dialog_button_font_->height);
        draw_text(draw, dialog_button_font_, UI_PRIMARY,
                  layout.filter_query_x, baseline, list.query());
        XftDrawSetClip(draw, 0);
        std::ostringstream count;
        count << matches.size() << " of " << items.size();
        const std::string counter = count.str();
        const int counter_x = layout.filter_counter_x;
        set_clip(draw, counter_x, layout.filter_top,
                 std::max(0, layout.content_right - counter_x),
                 dialog_button_font_->height);
        draw_text(draw, dialog_button_font_, UI_MUTED, counter_x, baseline,
                  counter);
        XftDrawSetClip(draw, 0);
        XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
        XDrawLine(display_, drawable, graphics_, layout.content_left,
                  layout.filter_rule_y, layout.content_right,
                  layout.filter_rule_y);
    }
    set_clip(draw, layout.content_left, list_top,
             layout.content_right - layout.content_left,
             std::max(0, list_bottom - list_top));
    for (int row = 0; row < visible; ++row) {
        const std::size_t match = list.first_match() +
                                  static_cast<std::size_t>(row);
        if (match >= matches.size()) break;
        const std::size_t source = matches[match];
        const int top = list_top + row * row_height;
        const bool selected = match == list.selected_match();
        if (selected) {
            XSetForeground(display_, graphics_, pixel(UI_SELECTED));
            XFillRectangle(display_, drawable, graphics_, layout.content_left - 8, top,
                           static_cast<unsigned int>(std::max(0,
                               layout.content_right - layout.content_left + 16)),
                           static_cast<unsigned int>(row_height));
        }
        const std::string marker = source == current_index ? "•" : "";
        const int marker_color = source == current_index ? UI_ACTIVE : UI_MUTED;
        const int baseline = top + (row_height - ui_ink_height) / 2 +
                             ui_ink_ascent;
        if (!marker.empty())
            ui_measurer_->draw(draw,
                &colors_[selected ? UI_SELECTED_TEXT : marker_color],
                layout.marker_x, baseline, marker, false);
        std::string description = items[source];
        std::string number;
        if (list.kind() == NAVIGATION_LIST_GAMES) {
            std::ostringstream expected;
            expected << source + 1 << ". ";
            number = expected.str();
            if (description.compare(0, number.size(), number) == 0)
                description.erase(0, number.size());
        }
        if (!number.empty()) {
            const int number_width = ui_measurer_->text_width(number.substr(0,
                number.size() - 1));
            ui_measurer_->draw(draw,
                &colors_[selected ? UI_SELECTED_TEXT : UI_MUTED],
                layout.number_x + std::max(0, layout.number_width - number_width),
                baseline, number.substr(0, number.size() - 1), false);
        }
        std::string shown = description;
        const int available = layout.description_width;
        const std::string ellipsis("…");
        const std::string dots("...");
        const std::string suffix = ui_measurer_->text_width(ellipsis) <= available
            ? ellipsis : dots;
        if (ui_measurer_->text_width(shown) > available) {
            while (!shown.empty() &&
                   ui_measurer_->text_width(shown + suffix) > available) {
                std::size_t cut = shown.size() - 1;
                while (cut > 0 &&
                       (static_cast<unsigned char>(shown[cut]) & 0xc0U) ==
                           0x80U) --cut;
                shown.erase(cut);
            }
            if (ui_measurer_->text_width(suffix) <= available)
                shown += suffix;
            else
                shown.clear();
        }
        set_clip(draw, layout.description_x, top,
                 layout.description_width, row_height);
        ui_measurer_->draw(
            draw, &colors_[selected ? UI_SELECTED_TEXT : UI_PRIMARY],
            layout.description_x, baseline, shown, false);
        XftDrawSetClip(draw, 0);
    }
    XftDrawSetClip(draw, 0);
    set_clip(draw, layout.content_left, layout.footer_y - dialog_font_->ascent,
             std::max(0, layout.content_right - layout.content_left),
             dialog_font_->height);
    draw_text(draw, dialog_font_, UI_MUTED, layout.content_left,
              layout.footer_y,
              "Type to filter · Up/Down select · Enter opens · Esc cancels");
    XftDrawSetClip(draw, 0);
    XftDrawDestroy(draw);
}

void X11NotationRenderer::set_navigation_list(
    NavigationListState* list, const std::string& title,
    std::size_t current_index)
{
    navigation_list_ = list;
    navigation_list_title_ = title;
    navigation_list_current_ = current_index;
}

void X11NotationRenderer::draw_navigation_list_overlay(
    Drawable drawable, int width, int height) const
{
    if (!navigation_list_ || !navigation_list_->visible()) return;
    navigation_list_->set_visible_rows(
        navigation_list_visible_rows(width, height));
    std::ostringstream title;
    title << navigation_list_title_;
    if (navigation_list_->has_selection())
        title << " — " << navigation_list_->selected_source_index() + 1
              << " / " << navigation_list_->items().size();
    draw_navigation_list(drawable, width, height, *navigation_list_,
                         title.str(), navigation_list_current_);
}

void X11NotationRenderer::draw_delete_dialog(
    Drawable drawable, int width, int height, bool error,
    const std::vector<std::string>& lines, int focused_button,
    int hovered_button) const
{
    XftDraw* draw;
    int index;
    const char* title = error ? "Cannot delete game" :
                                "Delete this game permanently?";
    std::vector<std::string> title_input(1, title);
    std::vector<std::string> title_rows = wrap_dialog_lines(
        display_, dialog_title_font_, width - 44, title_input);
    const int title_height = delete_dialog_title_bar_height(
        dialog_title_font_->height, static_cast<int>(title_rows.size()));
    const int button_height =
        delete_dialog_button_height(dialog_button_font_->height);
    const int button_width = measured_dialog_button_width(
        display_, dialog_button_font_);
    const int button_y = delete_dialog_button_y(height, button_height);
    const int button_gap = 16;
    const int cancel_x = (width - (button_width * 2 + button_gap)) / 2;
    const int delete_x = cancel_x + button_width + button_gap;
    const int ok_x = (width - button_width) / 2;
    const int line_height = dialog_font_->height + 6;
    const int body_top = title_height + 12;
    const int body_bottom = button_y - 12;
    std::vector<std::string> rows = wrap_dialog_lines(
        display_, dialog_font_, width - 48, lines);
    XSetForeground(display_, graphics_, pixel(UI_WINDOW));
    XFillRectangle(display_, drawable, graphics_, 0, 0,
                   static_cast<unsigned int>(width),
                   static_cast<unsigned int>(height));
    XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
    XDrawRectangle(display_, drawable, graphics_, 1, 1,
                   static_cast<unsigned int>(width - 3),
                   static_cast<unsigned int>(height - 3));
    XSetForeground(display_, graphics_, pixel(UI_HEADER));
    XFillRectangle(display_, drawable, graphics_, 2, 2,
                   static_cast<unsigned int>(width - 4),
                   static_cast<unsigned int>(title_height - 4));
    draw = XftDrawCreate(display_, drawable, visual_, colormap_);
    if (!draw)
        return;
    XftDrawSetClip(draw, 0);
    {
        int title_line_height = dialog_title_font_->height + 2;
        int title_top = (title_height -
            static_cast<int>(title_rows.size()) * title_line_height) / 2;
        for (index = 0; index < static_cast<int>(title_rows.size()); ++index) {
            XGlyphInfo extents;
            DeleteDialogTextExtents text_extents;
            DeleteDialogTextOrigin origin;
            const std::string& row = title_rows[index];
            XftTextExtentsUtf8(display_, dialog_title_font_,
                               reinterpret_cast<const FcChar8*>(row.data()),
                               static_cast<int>(row.size()), &extents);
            text_extents.x = extents.x;
            text_extents.y = extents.y;
            text_extents.width = extents.width;
            text_extents.height = extents.height;
            text_extents.advance = extents.xOff;
            text_extents.ascent = dialog_title_font_->ascent;
            text_extents.descent = dialog_title_font_->descent;
            origin = center_delete_dialog_label(
                22, title_top + index * title_line_height, width - 44,
                title_line_height, text_extents);
            draw_text(draw, dialog_title_font_, UI_PRIMARY, origin.x,
                      origin.baseline, row);
        }
    }
    set_clip(draw, 22, body_top, width - 44, body_bottom - body_top);
    for (index = 0; index < static_cast<int>(rows.size()); ++index) {
        int line_top = body_top + index * line_height;
        if (line_top + line_height > body_bottom)
            break;
        draw_text(draw, dialog_font_, UI_PRIMARY, 24,
                  line_top + (line_height - dialog_font_->ascent -
                              dialog_font_->descent) / 2 +
                              dialog_font_->ascent,
                  rows[index]);
    }
    XftDrawSetClip(draw, 0);
    XftDrawDestroy(draw);
    if (error) {
        XSetForeground(display_, graphics_, pixel(hovered_button == 1
                                                   ? UI_CONTROL_HOVER
                                                   : UI_CONTROL));
        XFillRectangle(display_, drawable, graphics_, ok_x, button_y,
                       static_cast<unsigned int>(button_width),
                       static_cast<unsigned int>(button_height));
        XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
        XDrawRectangle(display_, drawable, graphics_, ok_x, button_y,
                       static_cast<unsigned int>(button_width - 1),
                       static_cast<unsigned int>(button_height - 1));
        if (focused_button == 1) {
            XSetForeground(display_, graphics_, pixel(UI_ACTIVE));
            XDrawRectangle(display_, drawable, graphics_, ok_x + 2,
                           button_y + 2,
                           static_cast<unsigned int>(button_width - 5),
                           static_cast<unsigned int>(button_height - 5));
        }
    } else {
        const int button_x[2] = { cancel_x, delete_x };
        for (index = 0; index < 2; ++index) {
            XSetForeground(display_, graphics_, pixel(
                hovered_button == index ? UI_CONTROL_HOVER : UI_CONTROL));
            XFillRectangle(display_, drawable, graphics_, button_x[index],
                           button_y,
                           static_cast<unsigned int>(button_width),
                           static_cast<unsigned int>(button_height));
            XSetForeground(display_, graphics_, pixel(UI_SEPARATOR));
            XDrawRectangle(display_, drawable, graphics_, button_x[index],
                           button_y,
                           static_cast<unsigned int>(button_width - 1),
                           static_cast<unsigned int>(button_height - 1));
            if (focused_button == index) {
                XSetForeground(display_, graphics_, pixel(UI_ACTIVE));
                XDrawRectangle(display_, drawable, graphics_,
                               button_x[index] + 2, button_y + 2,
                               static_cast<unsigned int>(button_width - 5),
                               static_cast<unsigned int>(button_height - 5));
                XSetForeground(display_, graphics_, pixel(UI_WINDOW));
                XDrawRectangle(display_, drawable, graphics_,
                               button_x[index] + 4, button_y + 4,
                               static_cast<unsigned int>(button_width - 9),
                               static_cast<unsigned int>(button_height - 9));
            }
        }
    }
    draw = XftDrawCreate(display_, drawable, visual_, colormap_);
    if (!draw)
        return;
    if (error) {
        const char* label = "OK";
        XGlyphInfo extents;
        DeleteDialogTextExtents text_extents;
        DeleteDialogTextOrigin origin;
        XftTextExtentsUtf8(display_, dialog_button_font_,
                           reinterpret_cast<const FcChar8*>(label), 2,
                           &extents);
        text_extents.x = extents.x;
        text_extents.y = extents.y;
        text_extents.width = extents.width;
        text_extents.height = extents.height;
        text_extents.advance = extents.xOff;
        text_extents.ascent = dialog_button_font_->ascent;
        text_extents.descent = dialog_button_font_->descent;
        origin = center_delete_dialog_label(ok_x, button_y, button_width,
                                           button_height, text_extents);
        draw_text(draw, dialog_button_font_, UI_PRIMARY, origin.x,
                  origin.baseline, label);
    } else {
        const char* labels[2] = { "Cancel", "Delete" };
        const int button_x[2] = { cancel_x, delete_x };
        for (index = 0; index < 2; ++index) {
            XGlyphInfo extents;
            DeleteDialogTextExtents text_extents;
            DeleteDialogTextOrigin origin;
            XftTextExtentsUtf8(display_, dialog_button_font_,
                               reinterpret_cast<const FcChar8*>(labels[index]),
                               static_cast<int>(std::strlen(labels[index])),
                               &extents);
            text_extents.x = extents.x;
            text_extents.y = extents.y;
            text_extents.width = extents.width;
            text_extents.height = extents.height;
            text_extents.advance = extents.xOff;
            text_extents.ascent = dialog_button_font_->ascent;
            text_extents.descent = dialog_button_font_->descent;
            origin = center_delete_dialog_label(button_x[index], button_y,
                                               button_width, button_height,
                                               text_extents);
            draw_text(draw, dialog_button_font_, UI_PRIMARY, origin.x,
                      origin.baseline, labels[index]);
        }
    }
    XftDrawSetClip(draw, 0);
    XftDrawDestroy(draw);
}

int X11NotationRenderer::delete_dialog_height(
    int width, bool error, const std::vector<std::string>& lines) const
{
    std::vector<std::string> rows = wrap_dialog_lines(
        display_, dialog_font_, width - 48, lines);
    const char* title = error ? "Cannot delete game" :
                                "Delete this game permanently?";
    std::vector<std::string> title_input(1, title);
    std::vector<std::string> title_rows = wrap_dialog_lines(
        display_, dialog_title_font_, width - 44, title_input);
    int title_height = delete_dialog_title_bar_height(
        dialog_title_font_->height, static_cast<int>(title_rows.size()));
    int button_height =
        delete_dialog_button_height(dialog_button_font_->height);
    int height = delete_dialog_height_for_content(
        title_height, static_cast<int>(rows.size()), dialog_font_->height + 6,
        button_height);
    if (error && height < 190)
        height = 190;
    return height;
}

int X11NotationRenderer::delete_dialog_button_at(bool error, int width,
                                                int height, int x, int y) const
{
    int button_height =
        delete_dialog_button_height(dialog_button_font_->height);
    int button_width = measured_dialog_button_width(
        display_, dialog_button_font_);
    int button_y = delete_dialog_button_y(height, button_height);
    if (y < button_y || y >= button_y + button_height)
        return -1;
    if (error) {
        int left = (width - button_width) / 2;
        return x >= left && x < left + button_width ? 1 : -1;
    }
    int cancel_x =
        (width - (button_width * 2 + 16)) / 2;
    int delete_x = cancel_x + button_width + 16;
    if (x >= cancel_x && x < cancel_x + button_width)
        return 0;
    if (x >= delete_x && x < delete_x + button_width)
        return 1;
    return -1;
}

}
