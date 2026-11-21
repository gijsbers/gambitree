#ifndef GAMBITREE_NOTATION_LAYOUT_H
#define GAMBITREE_NOTATION_LAYOUT_H

#include <string>
#include <vector>

#include "gambitree/pgn.h"

namespace gambitree {

class NotationTextMeasurer {
public:
    virtual ~NotationTextMeasurer() {}
    virtual uint64_t layout_generation() const { return 0; }
    virtual int text_width(const std::string& text) const = 0;
    virtual int move_text_width(const std::string& text) const { return text_width(text); }
    virtual int text_range_width(const std::string& text, bool move_text,
                                 std::size_t start, std::size_t length) const
    {
        if (start > text.size()) start = text.size();
        if (length > text.size() - start) length = text.size() - start;
        return (move_text ? move_text_width(text.substr(0, start + length))
                          : text_width(text.substr(0, start + length))) -
               (move_text ? move_text_width(text.substr(0, start))
                          : text_width(text.substr(0, start)));
    }
    virtual void text_bounds(const std::string& text, bool move_text, int& left, int& right) const
    { left = 0; right = move_text ? move_text_width(text) : text_width(text); }
};

enum NotationItemKind { NOTATION_MOVE, NOTATION_COMMENT, NOTATION_NAG, NOTATION_VARIATION, NOTATION_RESULT };

class NotationFontMetrics {
public:
    NotationFontMetrics(int character_width = 8, int line_height = 18,
                        const NotationTextMeasurer* measurer = 0,
                        unsigned int figurine_mask = 63);
    void set_character_width(unsigned char character, int width);
    int text_width(const std::string& text, bool move_text = false) const;
    int text_range_width(const std::string& text, bool move_text,
                         std::size_t start, std::size_t length) const;
    void text_bounds(const std::string& text, bool move_text, int& left, int& right) const;
    int space_width() const;
    int line_height() const;
    unsigned int figurine_mask() const;
    bool same_as(const NotationFontMetrics& other) const;
private:
    unsigned int figurine_mask_;
    int widths_[256];
    int line_height_;
    const NotationTextMeasurer* measurer_;
    uint64_t measurer_generation_;
};

struct NotationItem {
    NotationItemKind kind;
    const MoveNode* node;
    std::string text;
    int x;
    int y;
    int width;
    int height;
    int ink_left;
    int ink_right;
    bool move_quality;
    bool selects_move() const;
    NotationItem();
};

struct NotationSelectionRect {
    int x, y, width, height;
};

struct NotationTextRangeRect {
    int x, y, width, height;
};

struct NagPresentation {
    int code;
    const char* display;
    const char* meaning;
    const char* side;
    const char* nuance;
    bool move_quality;
};

class NotationLayout {
public:
    NotationLayout();
    void set_figurines(bool enabled);
    void invalidate();
    bool figurines() const;
    /* Only leading SAN pieces and promotion pieces; never arbitrary prose. */
    static std::string format_san(const std::string& san, bool figurines,
                                  unsigned int supported_mask = 31);
    /* Presentation of a SAN PV only; whitespace and unrecognized tokens stay intact. */
    static std::string format_san_line(const std::string& text, bool figurines,
                                       unsigned int supported_mask = 31);
    void build(const PgnGame& game, int width, int character_width, int line_height);
    void build(const PgnGame& game, int width, const NotationFontMetrics& metrics);
    const std::vector<NotationItem>& items() const;
    uint64_t layout_generation() const;
    bool text_range_rect(std::size_t item_index, std::size_t byte_start,
                         std::size_t byte_length,
                         NotationTextRangeRect& rect) const;
    std::vector<NotationSelectionRect> selection_rects(const MoveNode* node) const;
    std::vector<NotationSelectionRect> selection_rects(const MoveNode* node,
                                                      int first_y, int last_y) const;
    void item_range(int first_y, int last_y, std::size_t& first,
                    std::size_t& last) const;
    int content_height() const;
    int max_scroll(int viewport_height) const;
    int ensure_visible(const MoveNode* node, int scroll_offset, int viewport_height) const;
    int ensure_end_visible(int scroll_offset, int viewport_height) const;
    const MoveNode* hit_test(int x, int y, int scroll_offset) const;
    const MoveNode* hit_test_visible(int x, int y, int scroll_offset, int viewport_height) const;
    static int full_line_viewport_height(int available_height, int line_height);
    static const NagPresentation* nag_presentation(int nag);
    static std::string nag_text(int nag);
    static std::vector<std::string> wrap_text(const std::string& text, int width,
                                              const NotationFontMetrics& metrics, bool move_text = false);
    /* Keeps only whole whitespace-delimited tokens that fit in one measured row. */
    static std::string fit_tokens(const std::string& text, int width,
                                  const NotationFontMetrics& metrics, bool move_text = false);
    /* Presentation-only UTF-8 and whitespace normalisation for PGN comments. */
    static std::string normalize_display_text(const std::string& text);
    /* Formats standalone conventional $1..$6 tokens in normalized comment text. */
    static std::string format_comment_text(const std::string& text,
                                           bool figurines = false,
                                           bool chessbase_mega = false,
                                           unsigned int supported_mask = 63);
    /* Presentation-only conversion of recognizable legacy ChessBase move
       references. Non-move currency and prose are returned unchanged. */
    static std::string format_chessbase_references(const std::string& text,
                                                   bool figurines,
                                                   unsigned int supported_mask = 63);

private:
    bool figurines_;
    bool has_cached_build_;
    const PgnGame* cached_game_;
    uint64_t cached_game_identity_;
    bool cached_figurines_;
    bool cached_chessbase_mega_;
    bool chessbase_mega_;
    uint64_t build_generation_;
    std::vector<NotationItem> items_;
    int width_;
    NotationFontMetrics metrics_;
    int line_height_;
    int cursor_x_;
    int cursor_y_;

    void newline();
    void append(NotationItemKind kind, const MoveNode* node, const std::string& text,
                int indent, int extra_space = 0, bool move_quality = false);
    void append_comments(const MoveNode* node, int indent);
    void append_leading_comments(const MoveNode* node, int indent);
    void render_move(const MoveNode* node, const Position& before, int indent,
                     bool explicit_number, bool clickable);
    void render_following(const MoveNode* node, const Position& before, int indent,
                          bool clickable);
    void render_rav(const MoveNode* first, const Position& before, int indent,
                    bool clickable);
    void render_mainline(const MoveNode* parent, const Position& before,
                         int indent, bool clickable, bool explicit_number);
    void render_trailing_variation(const MoveNode* root, int indent);
};

}

#endif
