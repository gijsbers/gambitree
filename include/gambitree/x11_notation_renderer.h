#ifndef GAMBITREE_X11_NOTATION_RENDERER_H
#define GAMBITREE_X11_NOTATION_RENDERER_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <vector>
#include "gambitree/engine_display.h"
#include "gambitree/xft_notation_text.h"
#include "gambitree/notation_layout.h"
#include "gambitree/notation_search.h"
#include "gambitree/sidebar_layout.h"
#include "gambitree/theme.h"
#include "gambitree/keyboard_help.h"
#include "gambitree/navigation_list.h"

namespace gambitree {

struct SidebarBoxes {
    int controls_top;
    int controls_bottom;
    int status_top;
    int status_bottom;
    int engine_top;
    int engine_bottom;
    int metadata_top;
    int metadata_bottom;
    int notation_top;
    int notation_bottom;
    int notation_visible_bottom;
    SidebarBoxes();
};

struct NavigationListLayout {
    int box_x;
    int box_y;
    int box_width;
    int box_height;
    int content_left;
    int content_right;
    int filter_top;
    int filter_baseline;
    int filter_text_bottom;
    int filter_rule_y;
    int filter_query_x;
    int filter_query_width;
    int filter_counter_x;
    int list_top;
    int list_bottom;
    int row_height;
    int visible_rows;
    int marker_x;
    int number_x;
    int number_width;
    int description_x;
    int description_width;
    int footer_y;
    NavigationListLayout();
};

class X11NotationRenderer {
public:
    X11NotationRenderer(Display* display, int screen);
    ~X11NotationRenderer();
    int panel_left(int window_width) const;
    int content_width(int window_width) const;
    SidebarGeometry sidebar_geometry(int window_width) const;
    void set_board_share(int share);
    int board_share() const;
    int sidebar_preferred_width(int window_width) const;
    void set_figurines(bool enabled);
    bool figurines() const;
    bool set_text_sizes(int notation_points, int engine_points);
    int notation_points() const;
    int engine_points() const;
    NotationSearch& search();
    const NotationSearch& search() const;
    int search_bar_height() const;
    int notation_viewport_height(int window_width, int window_height,
                                 const PgnGame& game) const;
    bool search_bar_contains(int window_width, int window_height,
                             const PgnGame& game, int x, int y) const;
    const NotationFontMetrics& engine_metrics() const;
    std::string engine_pv_text(const UciAnalysis& analysis, bool pending, int width) const;
    int engine_pv_hit_test(int x, int y) const;
    const UciAnalysis* engine_pv_snapshot() const;
    SidebarBoxes boxes(int window_width, int window_height, const PgnGame& game) const;
    int controls_top() const;
    int controls_bottom() const;
    int metadata_bottom(int window_width, int window_height, const PgnGame& game, int engine_lines) const;
    const NotationFontMetrics& metrics() const;
    void draw_controls(Drawable drawable, int window_width, const std::string& label,
                       bool previous_file, bool next_file, bool previous_game, bool next_game) const;
    void draw_status(Drawable drawable, int window_width, const std::string& text) const;
    void draw_delete_dialog(Drawable drawable, int width, int height, bool error,
                            const std::vector<std::string>& lines,
                            int focused_button, int hovered_button) const;
    int delete_dialog_height(int width, bool error,
                             const std::vector<std::string>& lines) const;
    int delete_dialog_button_at(bool error, int width, int height,
                                int x, int y) const;
    KeyboardHelpLayout keyboard_help_layout(int width, int height) const;
    void draw_keyboard_help(Drawable drawable, int width, int height, int& scroll) const;
    std::size_t navigation_list_visible_rows(int width, int height) const;
    NavigationListLayout navigation_list_layout(
        int width, int height, const NavigationListState& list) const;
    int navigation_list_row_at(int width, int height, int x, int y) const;
    void draw_navigation_list(Drawable drawable, int width, int height,
                              const NavigationListState& list,
                              const std::string& title,
                              std::size_t current_index) const;
    void set_navigation_list(NavigationListState* list,
                             const std::string& title,
                             std::size_t current_index);
    void draw_navigation_list_overlay(Drawable drawable, int width,
                                      int height) const;
    void draw(Drawable drawable, int window_width, int window_height, const PgnGame& game,
              const NotationLayout& layout, const MoveNode* current, int scroll_offset,
              const EngineDisplay& engine_display) const;
private:
    Display* display_;
    GC graphics_;
    int screen_;
    Visual* visual_;
    Colormap colormap_;
    XftFont* ui_font_;
    XftFont* ui_figurine_font_;
    XftFont* ui_annotation_font_;
    XftFont* bold_font_;
    XftFont* notation_font_;
    XftFont* figurine_font_;
    XftFont* annotation_font_;
    XftFont* engine_font_;
    XftFont* engine_figurine_font_;
    XftFont* engine_annotation_font_;
    XftFont* dialog_font_;
    XftFont* dialog_title_font_;
    XftFont* dialog_button_font_;
    XftColor colors_[16];
    bool colors_allocated_[16];
    UiPalette palette_;
    XftNotationText* measurer_;
    XftNotationText* engine_measurer_;
    XftNotationText* ui_measurer_;
    NotationFontMetrics metrics_;
    NotationFontMetrics engine_metrics_;
    NotationFontMetrics ui_metrics_;
    int notation_points_;
    int engine_points_;
    int notation_ascent_;
    int engine_ascent_;
    int ascent_;
    int descent_;
    int text_height_;
    int board_share_;
    bool figurines_;
    NotationSearch search_;
    NavigationListState* navigation_list_;
    std::string navigation_list_title_;
    std::size_t navigation_list_current_;
    /* Hitboxes and UCI data belong to the last painted row, not a newer poll. */
    mutable EnginePvLayout engine_pv_layout_;
    mutable UciAnalysis engine_pv_snapshot_;
    mutable bool engine_pv_clickable_;
    mutable int engine_pv_left_, engine_pv_top_;
    void draw_text(XftDraw* draw, XftFont* font, int color, int x, int baseline,
                   const std::string& text) const;
    void set_clip(XftDraw* draw, int x, int y, int width, int height) const;
    unsigned long pixel(int color) const;
    int baseline(int line_top) const;
    int notation_baseline(int line_top) const;
    int engine_baseline(int line_top) const;
};

}

#endif
