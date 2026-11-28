#include "gambitree/notation_layout.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace gambitree {

namespace {

bool comment_space(char character)
{
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
}

/* Legacy ChessBase font slots, only when followed by a complete move
   reference. Roboto has no chess glyphs; ASCII piece letters remain legible
   with both the usual notation font and its existing fixed-font fallback. */
char legacy_comment_piece(const std::string& text, std::size_t at)
{
    if (at + 2 >= text.size() ||
        static_cast<unsigned char>(text[at]) != 0xc2U) return 0;
    if (at > 0 && !comment_space(text[at - 1]) &&
        std::string(".([{,:;\"'").find(text[at - 1]) == std::string::npos)
        return 0;
    char piece = 0;
    switch (static_cast<unsigned char>(text[at + 1])) {
    case 0xa2: piece = 'K'; break;
    case 0xa3: piece = 'Q'; break;
    case 0xa6: piece = 'R'; break;
    case 0xa5: piece = 'B'; break;
    case 0xa4: piece = 'N'; break;
    case 0xa7: piece = 'P'; break;
    default: return 0;
    }
    for (int prefix = 2; prefix >= 0; --prefix) {
        if (at + 2 + static_cast<std::size_t>(prefix) >= text.size()) continue;
        std::size_t end = at + 2 + static_cast<std::size_t>(prefix);
        bool valid_prefix = true;
        if (prefix >= 1) {
            char c = text[at + 2];
            valid_prefix = (c >= 'a' && c <= 'h') || (c >= '1' && c <= '8');
        }
        if (prefix == 2) {
            char c = text[at + 3];
            valid_prefix = valid_prefix && ((c >= 'a' && c <= 'h') || (c >= '1' && c <= '8'));
        }
        if (!valid_prefix || end >= text.size()) continue;
        if (text[end] == 'x') ++end;
        if (end + 1 >= text.size() || text[end] < 'a' || text[end] > 'h' ||
            text[end + 1] < '1' || text[end + 1] > '8') continue;
        end += 2;
        int suffix = 0;
        while (end < text.size() && suffix < 2 &&
               (text[end] == '+' || text[end] == '#' ||
                text[end] == '!' || text[end] == '?')) { ++end; ++suffix; }
        if (end < text.size() && !comment_space(text[end]) &&
            std::string(").,;:]}\"'").find(text[end]) == std::string::npos)
            continue;
        return piece;
    }
    return 0;
}

char legacy_comment_piece_code(const std::string& text, std::size_t at)
{
    if (at + 1 >= text.size() ||
        static_cast<unsigned char>(text[at]) != 0xc2U) return 0;
    switch (static_cast<unsigned char>(text[at + 1])) {
    case 0xa2: return 'K';
    case 0xa3: return 'Q';
    case 0xa6: return 'R';
    case 0xa5: return 'B';
    case 0xa4: return 'N';
    case 0xa7: return 'P';
    default: return 0;
    }
}

bool possible_utf8_word_byte(char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           static_cast<unsigned char>(value) >= 0x80U;
}

bool standalone_legacy_marker(const std::string& text, std::size_t at)
{
    return (at == 0 || !possible_utf8_word_byte(text[at - 1])) &&
           (at + 2 >= text.size() || !possible_utf8_word_byte(text[at + 2]));
}

unsigned int unmapped_legacy_codepoint(const std::string& text, std::size_t at)
{
    if (at + 1 >= text.size() ||
        static_cast<unsigned char>(text[at]) != 0xd7U) return 0;
    switch (static_cast<unsigned char>(text[at + 1])) {
    case 0xbb: return 0x05fbU;
    case 0xa2: return 0x05e2U;
    case 0xa7: return 0x05e7U;
    case 0xa5: return 0x05e5U;
    default: return 0;
    }
}

std::string unmapped_legacy_label(unsigned int codepoint)
{
    switch (codepoint) {
    case 0x05fbU: return "[U+05FB]";
    case 0x05e2U: return "[U+05E2]";
    case 0x05e7U: return "[U+05E7]";
    case 0x05e5U: return "[U+05E5]";
    default: return "[source symbol]";
    }
}

bool chessbase_mega_context(const PgnGame& game)
{
    const std::string* source = game.tag_value("Source");
    const std::string* record = game.tag_value("Mega2PGNRecordId");
    const std::string* eco = game.tag_value("Mega2PGNEcoIndex");
    return source && *source == "ChessBase" && record && !record->empty() &&
           eco && !eco->empty();
}

std::string remove_comment_directives(const std::string& text)
{
    std::string result;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t marker_at = text.find("[%", index);
        if (marker_at == std::string::npos) {
            result.append(text, index, std::string::npos);
            break;
        }
        result.append(text, index, marker_at - index);
        const std::size_t end = text.find(']', marker_at + 2);
        if (end == std::string::npos) {
            result.append(text, marker_at, std::string::npos);
            break;
        }
        bool valid = end > marker_at + 2;
        bool has_content = false;
        std::size_t check;
        for (check = marker_at + 2; valid && check < end; ++check) {
            if (text[check] == '[') valid = false;
            if (!comment_space(text[check])) has_content = true;
        }
        valid = valid && has_content;
        if (!valid) {
            result += text[marker_at];
            index = marker_at + 1;
            continue;
        }
        {
            const std::size_t after = end + 1;
            const bool left_space = !result.empty() && comment_space(result[result.size() - 1]);
            const bool right_space = after < text.size() && comment_space(text[after]);
            const char left = result.empty() ? '\0' : result[result.size() - 1];
            const char right = after < text.size() ? text[after] : '\0';
            const bool left_open = left == '(' || left == '[' || left == '{';
            const bool right_punctuation = right == ')' || right == ']' || right == '}' ||
                right == ',' || right == '.' || right == ';' || right == ':' ||
                right == '?' || right == '!';
            if (!result.empty() && after < text.size() && !left_space &&
                !right_space && !left_open && !right_punctuation)
                result += ' ';
            index = after;
        }
    }
    return result;
}

std::string hex_bytes(const std::string& text)
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    std::size_t index;
    for (index = 0; index < text.size(); ++index) {
        unsigned char value = static_cast<unsigned char>(text[index]);
        if (index) result += ' ';
        result += digits[value >> 4];
        result += digits[value & 15U];
    }
    return result;
}

}

NotationFontMetrics::NotationFontMetrics(int character_width, int line_height, const NotationTextMeasurer* measurer, unsigned int figurine_mask)
    : figurine_mask_(figurine_mask), line_height_(line_height > 0 ? line_height : 18),
      measurer_(measurer),
      measurer_generation_(measurer ? measurer->layout_generation() : 0)
{
    int index;
    if (character_width < 1) character_width = 1;
    for (index = 0; index < 256; ++index) widths_[index] = character_width;
}

void NotationFontMetrics::set_character_width(unsigned char character, int width)
{ widths_[character] = width > 0 ? width : 1; }

int NotationFontMetrics::text_width(const std::string& text, bool move_text) const
{
    std::size_t index;
    int result = 0;
    if (measurer_) return move_text ? measurer_->move_text_width(text) : measurer_->text_width(text);
    for (index = 0; index < text.size(); ++index) result += widths_[static_cast<unsigned char>(text[index])];
    return result;
}

int NotationFontMetrics::text_range_width(const std::string& text,
                                         bool move_text,
                                         std::size_t start,
                                         std::size_t length) const
{
    std::size_t index;
    int result = 0;
    if (start > text.size()) start = text.size();
    if (length > text.size() - start) length = text.size() - start;
    if (measurer_)
        return measurer_->text_range_width(text, move_text, start, length);
    for (index = start; index < start + length; ++index)
        result += widths_[static_cast<unsigned char>(text[index])];
    return result;
}

void NotationFontMetrics::text_bounds(const std::string& text, bool move_text, int& left, int& right) const
{
    if (measurer_) measurer_->text_bounds(text, move_text, left, right);
    else { left = 0; right = text_width(text, move_text); }
}

int NotationFontMetrics::space_width() const { return measurer_ ? measurer_->text_width(" ") : widths_[static_cast<unsigned char>(' ')]; }
int NotationFontMetrics::line_height() const { return line_height_; }

NotationItem::NotationItem() : kind(NOTATION_MOVE), node(0), x(0), y(0), width(0), height(0), ink_left(0), ink_right(0), move_quality(false) {}
bool NotationItem::selects_move() const
{ return node && (kind == NOTATION_MOVE || move_quality); }
NotationLayout::NotationLayout() : figurines_(false), has_cached_build_(false),
    cached_game_(0), cached_game_identity_(0), cached_figurines_(false),
    cached_chessbase_mega_(false), chessbase_mega_(false), build_generation_(0),
    width_(0), metrics_(),
    line_height_(18), cursor_x_(0), cursor_y_(0) {}

unsigned int NotationFontMetrics::figurine_mask() const { return figurine_mask_; }
bool NotationFontMetrics::same_as(const NotationFontMetrics& other) const
{
    if (figurine_mask_ != other.figurine_mask_ || line_height_ != other.line_height_ ||
        measurer_ != other.measurer_ ||
        measurer_generation_ != other.measurer_generation_) return false;
    for (int i = 0; i < 256; ++i)
        if (widths_[i] != other.widths_[i]) return false;
    return true;
}
void NotationLayout::set_figurines(bool enabled)
{ if (figurines_ != enabled) { figurines_ = enabled; invalidate(); } }
void NotationLayout::invalidate()
{ has_cached_build_ = false; cached_game_ = 0; cached_game_identity_ = 0; }
bool NotationLayout::figurines() const { return figurines_; }

std::string NotationLayout::format_san(const std::string& san, bool figurines,
                                      unsigned int supported_mask)
{
    if (!figurines) return san;
    static const std::string letters = "KQRBN";
    static const char* glyphs[] = {"♔", "♕", "♖", "♗", "♘"};
    std::string result;
    for (std::size_t i = 0; i < san.size(); ++i) {
        std::size_t piece = letters.find(san[i]);
        if ((i == 0 || san[i - 1] == '=') && piece != std::string::npos &&
            (supported_mask & (1U << piece))) result += glyphs[piece];
        else result += san[i];
    }
    return result;
}

namespace {
bool san_reference(const std::string& token)
{
    std::string core = token;
    if (!core.empty() && (core[core.size()-1] == '+' || core[core.size()-1] == '#'))
        core.erase(core.size()-1);
    if (core == "O-O" || core == "O-O-O") return true;
    bool piece = !core.empty() && std::string("KQRBN").find(core[0]) != std::string::npos;
    if (!piece && core.size() >= 2 && core[core.size()-2] == '=' &&
        std::string("QRBN").find(core[core.size()-1]) != std::string::npos)
        core.erase(core.size()-2);
    if (core.size() < 2) return false;
    std::size_t square = core.size()-2;
    if (core[square] < 'a' || core[square] > 'h' || core[square+1] < '1' || core[square+1] > '8') return false;
    std::string prefix = core.substr(piece ? 1 : 0, square - (piece ? 1 : 0));
    if (piece) {
        if (!prefix.empty() && prefix[prefix.size()-1] == 'x') prefix.erase(prefix.size()-1);
        if (prefix.size() > 2) return false;
        if (prefix.empty()) return true;
        if (prefix.size() == 1) return (prefix[0] >= 'a' && prefix[0] <= 'h') || (prefix[0] >= '1' && prefix[0] <= '8');
        return prefix[0] >= 'a' && prefix[0] <= 'h' && prefix[1] >= '1' && prefix[1] <= '8';
    }
    return prefix.empty() || (prefix.size() == 2 && prefix[0] >= 'a' && prefix[0] <= 'h' && prefix[1] == 'x');
}
}

std::string NotationLayout::format_san_line(const std::string& text, bool figurines,
                                            unsigned int supported_mask)
{
    if (!figurines) return text;
    std::string result;
    std::size_t at = 0;
    while (at < text.size()) {
        if (comment_space(text[at])) { result += text[at++]; continue; }
        std::size_t end = at;
        while (end < text.size() && !comment_space(text[end])) ++end;
        std::string token = text.substr(at, end-at);
        result += san_reference(token) ? format_san(token, true, supported_mask) : token;
        at = end;
    }
    return result;
}

void NotationLayout::newline() { cursor_x_ = 0; cursor_y_ += line_height_; }

namespace {
int fitting_width(const std::string& text, const NotationFontMetrics& metrics, bool move_text)
{
    if (!move_text) return metrics.text_width(text);
    int left, right;
    metrics.text_bounds(text, true, left, right);
    return right;
}
}

std::vector<std::string> NotationLayout::wrap_text(const std::string& text, int width,
                                                    const NotationFontMetrics& metrics, bool move_text)
{
    std::vector<std::string> lines;
    std::size_t offset = 0;
    std::string line;
    if (width < 1) width = 1;
    while (offset < text.size()) {
        std::size_t start;
        std::string word;
        while (offset < text.size() && text[offset] == ' ') ++offset;
        if (offset == text.size()) break;
        start = offset;
        while (offset < text.size() && text[offset] != ' ') ++offset;
        word = text.substr(start, offset - start);
        if (fitting_width(word, metrics, move_text) <= width) {
            std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && fitting_width(candidate, metrics, move_text) > width) { lines.push_back(line); line = word; }
            else line = candidate;
        } else {
            std::size_t character = 0;
            if (!line.empty()) { lines.push_back(line); line.clear(); }
            while (character < word.size()) {
                std::size_t end = character;
                while (end < word.size()) {
                    std::size_t next = end + 1;
                    while (next < word.size() &&
                           (static_cast<unsigned char>(word[next]) & 0xc0U) == 0x80U) ++next;
                    if (fitting_width(word.substr(character, next - character), metrics, move_text) > width) break;
                    end = next;
                }
                if (end == character) {
                    ++end;
                    while (end < word.size() &&
                           (static_cast<unsigned char>(word[end]) & 0xc0U) == 0x80U) ++end;
                }
                lines.push_back(word.substr(character, end - character));
                character = end;
            }
        }
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

std::string NotationLayout::fit_tokens(const std::string& text, int width,
                                       const NotationFontMetrics& metrics, bool move_text)
{
    std::size_t offset = 0;
    std::string result;
    if (width < 1) return result;
    while (offset < text.size()) {
        std::size_t start;
        std::string token;
        std::string candidate;
        while (offset < text.size() && text[offset] == ' ') ++offset;
        if (offset == text.size()) break;
        start = offset;
        while (offset < text.size() && text[offset] != ' ') ++offset;
        token = text.substr(start, offset - start);
        candidate = result.empty() ? token : result + " " + token;
        if (fitting_width(candidate, metrics, move_text) > width) break;
        result = candidate;
    }
    return result;
}

std::string NotationLayout::normalize_display_text(const std::string& text)
{
    std::string result;
    std::size_t index = 0;
    bool pending_space = false;
    while (index < text.size()) {
        unsigned char byte = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        bool valid = false;
        if (byte < 0x80U) {
            if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') { pending_space = !result.empty(); ++index; continue; }
            if (byte >= 0x20U && byte != 0x7fU) { if (pending_space) result += ' '; result += static_cast<char>(byte); pending_space = false; }
            ++index;
            continue;
        }
        if (byte >= 0xc2U && byte <= 0xdfU) length = 2;
        else if (byte >= 0xe0U && byte <= 0xefU) length = 3;
        else if (byte >= 0xf0U && byte <= 0xf4U) length = 4;
        if (length && index + length <= text.size()) {
            std::size_t next;
            valid = true;
            for (next = 1; next < length; ++next)
                if ((static_cast<unsigned char>(text[index + next]) & 0xc0U) != 0x80U) valid = false;
            if ((byte == 0xe0U && static_cast<unsigned char>(text[index + 1]) < 0xa0U) ||
                (byte == 0xedU && static_cast<unsigned char>(text[index + 1]) >= 0xa0U) ||
                (byte == 0xf0U && static_cast<unsigned char>(text[index + 1]) < 0x90U) ||
                (byte == 0xf4U && static_cast<unsigned char>(text[index + 1]) > 0x8fU)) valid = false;
        }
        if (pending_space) result += ' ';
        if (valid) result.append(text, index, length);
        else result += '?';
        pending_space = false;
        index += valid ? length : 1;
    }
    if (std::getenv("GAMBITREE_DIAGNOSTICS") && result != text)
        std::cerr << "gambitree: diagnostic: comment display normalisation source_hex='" << hex_bytes(text)
                  << "' display_hex='" << hex_bytes(result) << "'\n";
    return result;
}

void NotationLayout::append(NotationItemKind kind, const MoveNode* node,
                            const std::string& text, int indent, int extra_space,
                            bool move_quality)
{
    std::vector<std::string> words;
    std::size_t offset = 0;
    int indent_x = indent * metrics_.space_width() * 2;
    int minimum_cell = metrics_.space_width();
    if (figurines_) {
        static const char* glyphs[] = {"♔", "♕", "♖", "♗", "♘"};
        for (unsigned int i = 0; i < 5; ++i) {
            if (!(metrics_.figurine_mask() & (1U << i))) continue;
            int glyph_width = metrics_.text_width(glyphs[i]);
            if (glyph_width > minimum_cell) minimum_cell = glyph_width;
        }
    }
    /* Keep the existing indentation, but its extreme right-edge clamp must
       leave room for one whole figurine rather than just one narrow space. */
    if (indent_x > width_ - minimum_cell)
        indent_x = width_ > minimum_cell ? width_ - minimum_cell : 0;
    while (offset < text.size()) {
        std::size_t start;
        while (offset < text.size() && text[offset] == ' ') ++offset;
        if (offset == text.size()) break;
        start = offset;
        while (offset < text.size() && text[offset] != ' ') ++offset;
        words.push_back(text.substr(start, offset - start));
    }
    if (words.empty()) return;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::string& word = words[index];
        int word_width = metrics_.text_width(word, kind == NOTATION_MOVE);
        int ink_left, ink_right;
        metrics_.text_bounds(word, kind == NOTATION_MOVE, ink_left, ink_right);
        const bool quality_annotation = kind == NOTATION_NAG && move_quality;
        int space = cursor_x_ > indent_x && !quality_annotation ? metrics_.space_width() : 0;
        if (space && index == 0) space += extra_space;
        int fit_width = kind == NOTATION_MOVE || quality_annotation ? ink_right : word_width;
        int available;
        if (cursor_x_ < indent_x) cursor_x_ = indent_x;
        available = width_ - cursor_x_;
        if (fit_width <= width_ - indent_x && (space + fit_width > available)) {
            newline(); cursor_x_ = indent_x; space = 0; available = width_ - cursor_x_;
        }
        if (fit_width <= width_ - indent_x) {
            NotationItem item;
            cursor_x_ += space;
            item.kind = kind; item.node = node; item.text = word; item.x = cursor_x_; item.y = cursor_y_;
            item.width = word_width; item.height = line_height_;
            item.ink_left = ink_left; item.ink_right = ink_right; item.move_quality = quality_annotation;
            items_.push_back(item); cursor_x_ += word_width;
        } else {
            std::vector<std::string> pieces;
            std::size_t piece;
            if (cursor_x_ > indent_x) { newline(); cursor_x_ = indent_x; }
            pieces = wrap_text(word, width_ - indent_x, metrics_, kind == NOTATION_MOVE || quality_annotation);
            for (piece = 0; piece < pieces.size(); ++piece) {
                NotationItem item;
                if (piece > 0) { newline(); cursor_x_ = indent_x; }
                item.kind = kind; item.node = node; item.text = pieces[piece]; item.x = cursor_x_; item.y = cursor_y_;
                item.width = metrics_.text_width(item.text, kind == NOTATION_MOVE); if (item.width > width_ - item.x) item.width = width_ - item.x;
                item.height = line_height_; item.move_quality = quality_annotation;
                metrics_.text_bounds(item.text, kind == NOTATION_MOVE, item.ink_left, item.ink_right);
                items_.push_back(item); cursor_x_ += item.width;
            }
        }
    }
}

const NagPresentation* NotationLayout::nag_presentation(int nag)
{
    /* NAG meanings follow PGN and the ChessBase/Informator extensions used by
       the source corpus. Keep the original code and meaning here while using
       compact chess notation for display. W/Z distinguishes the referenced
       player where the symbol itself is symmetric. */
    static const NagPresentation entries[] = {
        {1, "!", "Good move", "", "", true},
        {2, "?", "Poor move", "", "", true},
        {3, "!!", "Very good move", "", "", true},
        {4, "??", "Very poor move", "", "", true},
        {5, "!?", "Speculative or interesting move", "", "", true},
        {6, "?!", "Questionable move", "", "", true},
        {7, "□", "Forced move; other moves lose quickly", "", "forced", false},
        {8, "□", "Singular move; no reasonable alternatives", "", "singular", false},
        {10, "=", "Drawish position", "", "drawish", false},
        {11, "=", "Equal chances in a quiet position", "", "quiet", false},
        {13, "∞", "Unclear position", "", "unclear", false},
        {14, "+/=", "White has a slight advantage", "White", "slight", false},
        {15, "=/+", "Black has a slight advantage", "Black", "slight", false},
        {16, "±", "White has a moderate advantage", "White", "moderate", false},
        {17, "-/+", "Black has a moderate advantage", "Black", "moderate", false},
        {18, "+−", "White has a decisive advantage", "White", "decisive", false},
        {19, "−+", "Black has a decisive advantage", "Black", "decisive", false},
        {22, "⊙W", "White is in zugzwang", "White", "zugzwang", false},
        {23, "⊙Z", "Black is in zugzwang", "Black", "zugzwang", false},
        {30, "↻+W", "White has a slight development advantage", "White", "slight", false},
        {31, "↻+Z", "Black has a slight development advantage", "Black", "slight", false},
        {32, "↻++W", "White has a moderate development advantage", "White", "moderate", false},
        {33, "↻++Z", "Black has a moderate development advantage", "Black", "moderate", false},
        {36, "↑", "White has the initiative", "White", "initiative", false},
        {37, "↓", "Black has the initiative", "Black", "initiative", false},
        {40, "→W", "White has the attack", "White", "attack", false},
        {41, "→Z", "Black has the attack", "Black", "attack", false},
        {44, "=∞", "White has sufficient compensation for a material deficit", "White", "sufficient", false},
        {45, "∞=", "Black has sufficient compensation for a material deficit", "Black", "sufficient", false},
        {50, "⊕W", "White has a moderate center-control advantage", "White", "moderate", false},
        {131, "↔+Z", "Black has slight counterplay", "Black", "slight", false},
        {132, "↔W", "White has moderate counterplay", "White", "moderate", false},
        {133, "↔Z", "Black has moderate counterplay", "Black", "moderate", false},
        {136, "⊕+W", "White has moderate time-control pressure", "White", "moderate", false},
        {137, "⊕+Z", "Black has moderate time-control pressure", "Black", "moderate", false},
        {138, "⊕++W", "White has severe time-control pressure", "White", "severe", false},
        {140, "∆", "With the idea", "", "continuation", false},
        {141, "∇", "Aimed against", "", "counter", false},
        {142, "≥", "Better continuation", "", "alternative", false},
        {143, "≤", "Worse move", "", "alternative", false},
        {144, "≈", "Equivalent move", "", "alternative", false},
        {145, "RR", "Editor's remark", "", "editorial", false},
        {146, "N", "Novelty", "", "novelty", false}
    };
    for (std::size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i)
        if (entries[i].code == nag) return &entries[i];
    return 0;
}

std::string NotationLayout::nag_text(int nag)
{
    if (nag >= 149) return std::string();
    const NagPresentation* presentation = nag_presentation(nag);
    if (presentation) return presentation->display;
    std::ostringstream text;
    text << "$" << nag;
    return text.str();
}

std::string NotationLayout::format_comment_text(const std::string& text,
                                               bool figurines,
                                               bool chessbase_mega,
                                               unsigned int supported_mask)
{
    const std::string normalized = normalize_display_text(remove_comment_directives(text));
    std::string result;
    std::size_t index = 0;
    while (index < normalized.size()) {
        const char piece = legacy_comment_piece_code(normalized, index);
        if (piece) {
            static const std::string pieces = "KQRBNP";
            static const char* glyphs[] = {"♔", "♕", "♖", "♗", "♘", "♙"};
            const std::size_t piece_index = pieces.find(piece);
            if (figurines && piece_index != std::string::npos &&
                (supported_mask & (1U << piece_index)))
                result += glyphs[piece_index];
            else result += piece;
            index += 2;
            continue;
        }
        if (chessbase_mega && index + 1 < normalized.size() &&
            static_cast<unsigned char>(normalized[index]) == 0xc2U &&
            standalone_legacy_marker(normalized, index)) {
            const unsigned char marker =
                static_cast<unsigned char>(normalized[index + 1]);
            if (marker == 0xadU) {
                if (figurines && (supported_mask & (1U << 3)))
                    result += "♗♗";
                else
                    result += "bishop pair";
                index += 2;
                continue;
            }
            if (marker == 0xacU) {
                result += "endgame";
                index += 2;
                continue;
            }
        }
        const unsigned int unmapped = unmapped_legacy_codepoint(normalized,
                                                                 index);
        if (unmapped) {
            const std::string label = unmapped_legacy_label(unmapped);
            if (!result.empty() &&
                ((result[result.size() - 1] >= 'A' && result[result.size() - 1] <= 'Z') ||
                 (result[result.size() - 1] >= 'a' && result[result.size() - 1] <= 'z') ||
                 (result[result.size() - 1] >= '0' && result[result.size() - 1] <= '9') ||
                 result[result.size() - 1] == ']')) result += ' ';
            result += label;
            if (index + 2 < normalized.size()) {
                const char next = normalized[index + 2];
                if ((next >= 'A' && next <= 'Z') ||
                    (next >= 'a' && next <= 'z') ||
                    (next >= '0' && next <= '9')) result += ' ';
            }
            index += 2;
            continue;
        }
        if (normalized[index] == '$' && (index == 0 ||
            normalized[index - 1] == ' ' || normalized[index - 1] == '(' ||
            normalized[index - 1] == '[' || normalized[index - 1] == '{' ||
            normalized[index - 1] == '<' || normalized[index - 1] == '\'' ||
            normalized[index - 1] == '"')) {
            std::size_t end = index + 1;
            while (end < normalized.size() && normalized[end] >= '0' && normalized[end] <= '9') ++end;
            if (end == index + 2 && normalized[index + 1] >= '1' && normalized[index + 1] <= '6' &&
                (end == normalized.size() || normalized[end] == ' ' || normalized[end] == ')' ||
                 normalized[end] == ']' || normalized[end] == '}' || normalized[end] == '>' ||
                 normalized[end] == ',' || normalized[end] == '.' || normalized[end] == ';' ||
                 normalized[end] == ':' || normalized[end] == '?' || normalized[end] == '!' ||
                 normalized[end] == '\'' || normalized[end] == '"')) {
                result += nag_text(normalized[index + 1] - '0');
                index = end;
                continue;
            }
        }
        result += normalized[index++];
    }
    return result;
}

std::string NotationLayout::format_chessbase_references(
    const std::string& text, bool figurines, unsigned int supported_mask)
{
    static const std::string pieces = "KQRBNP";
    static const char* glyphs[] = {"♔", "♕", "♖", "♗", "♘", "♙"};
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        char piece = legacy_comment_piece(text, i);
        if (!piece) { result += text[i++]; continue; }
        std::size_t index = pieces.find(piece);
        if (figurines && index != std::string::npos &&
            (supported_mask & (1U << index))) result += glyphs[index];
        else result += piece;
        i += 2;
    }
    return result;
}

void NotationLayout::append_comments(const MoveNode* node, int indent)
{
    std::size_t index;
    for (index = 0; index < node->comments.size(); ++index)
        append(NOTATION_COMMENT, 0,
               format_comment_text(node->comments[index], figurines_,
                                   chessbase_mega_, metrics_.figurine_mask()), indent);
}

void NotationLayout::append_leading_comments(const MoveNode* node, int indent)
{
    std::size_t index;
    for (index = 0; index < node->leading_comments.size(); ++index)
        append(NOTATION_COMMENT, 0,
               format_comment_text(node->leading_comments[index], figurines_,
                                   chessbase_mega_, metrics_.figurine_mask()), indent);
}

void NotationLayout::render_move(const MoveNode* node, const Position& before,
                                 int indent, bool explicit_number, bool clickable)
{
    std::ostringstream number; std::size_t index;
    bool adjacent = !items_.empty() && items_.back().node == node->parent &&
        (items_.back().kind == NOTATION_MOVE || items_.back().kind == NOTATION_NAG);
    int pair_gap = adjacent ? metrics_.space_width() : 0;
    int reply_gap = adjacent ? (metrics_.space_width() + 1) / 2 : 0;
    if (explicit_number || before.side_to_move() == WHITE) {
        number << before.fullmove_number() << (before.side_to_move() == WHITE ? "." : "...");
        append(NOTATION_MOVE, 0, number.str(), indent, before.side_to_move() == WHITE ? pair_gap : 0);
    }
    append(NOTATION_MOVE, clickable ? node : 0, format_san(node->san, figurines_, metrics_.figurine_mask()), indent,
           before.side_to_move() == BLACK && !explicit_number ? reply_gap : 0);
    for (index = 0; index < node->nags.size(); ++index) {
        const NagPresentation* presentation = nag_presentation(node->nags[index]);
        const std::string display = nag_text(node->nags[index]);
        if (!display.empty())
            append(NOTATION_NAG, clickable ? node : 0, display, indent, 0,
                   presentation && presentation->move_quality);
    }
    if (node->following_items.empty()) append_comments(node, indent);
}

void NotationLayout::render_rav(const MoveNode* first, const Position& before,
                                int indent, bool clickable)
{
    if (!first) return;
    append(NOTATION_VARIATION, 0, "(", indent);
    append_leading_comments(first, indent + 1);
    render_move(first, before, indent + 1, true, clickable);
    render_following(first, before, indent + 1, clickable);
    if (!first->children.empty())
        render_mainline(first, first->position_after, indent + 1, clickable,
                        false);
    append(NOTATION_VARIATION, 0, ")", indent);
    newline();
}

void NotationLayout::render_following(const MoveNode* node,
                                      const Position& before, int indent,
                                      bool clickable)
{
    std::size_t index;
    for (index = 0; index < node->following_items.size(); ++index) {
        const PgnMoveItem& item = node->following_items[index];
        if (item.type == PgnMoveItem::COMMENT)
            append(NOTATION_COMMENT, 0,
                   format_comment_text(item.comment, figurines_, chessbase_mega_,
                                       metrics_.figurine_mask()), indent);
        else if (item.variation_root)
            render_rav(item.variation_root, before, indent + 1, clickable);
    }
}

void NotationLayout::render_mainline(const MoveNode* parent,
                                     const Position& before, int indent,
                                     bool clickable, bool explicit_number)
{
    if (parent->children.empty()) return;
    const MoveNode* main = parent->children[0];
    render_move(main, before, indent, explicit_number, clickable);
    render_following(main, before, indent, clickable);
    render_mainline(main, main->position_after, indent, clickable, false);
}

void NotationLayout::render_trailing_variation(const MoveNode* root, int indent)
{
    if (!root || root->children.empty()) return;
    render_rav(root->children[0], root->position_after, indent, false);
}

void NotationLayout::build(const PgnGame& game, int width, int character_width, int line_height)
{ build(game, width, NotationFontMetrics(character_width, line_height)); }

void NotationLayout::build(const PgnGame& game, int width, const NotationFontMetrics& metrics)
{
    const int normalized_width = width > 0 ? width : 1;
    const bool has_chessbase_mega_context = chessbase_mega_context(game);
    if (has_cached_build_ && cached_game_ == &game &&
        cached_game_identity_ == game.instance_identity() && width_ == normalized_width &&
        cached_figurines_ == figurines_ &&
        cached_chessbase_mega_ == has_chessbase_mega_context &&
        metrics_.same_as(metrics)) return;
    items_.clear(); width_ = normalized_width; metrics_ = metrics; line_height_ = metrics_.line_height(); cursor_x_ = 0; cursor_y_ = 0;
    chessbase_mega_ = has_chessbase_mega_context;
    append_comments(game.root, 0);
    render_mainline(game.root, game.initial_position, 0, true, true);
    if (game.parse_status != PGN_PARTIAL && !game.result.empty())
        append(NOTATION_RESULT, 0, game.result, 0);
    if (game.parse_status != PGN_COMPLETE)
        append(NOTATION_COMMENT, 0, game.parse_warning(), 0);
    {
        std::size_t index;
        for (index = 0; index < game.trailing_items.size(); ++index) {
            const PgnTrailingItem& item = game.trailing_items[index];
            if (item.type == PgnTrailingItem::COMMENT)
                append(NOTATION_COMMENT, 0,
                       format_comment_text(item.comment, figurines_, chessbase_mega_,
                                           metrics_.figurine_mask()), 0);
            else
                render_trailing_variation(item.variation_root, 1);
        }
    }
    cached_game_ = &game;
    cached_game_identity_ = game.instance_identity();
    cached_figurines_ = figurines_;
    cached_chessbase_mega_ = chessbase_mega_;
    ++build_generation_;
    if (!build_generation_) ++build_generation_;
    has_cached_build_ = true;
}

const std::vector<NotationItem>& NotationLayout::items() const { return items_; }
uint64_t NotationLayout::layout_generation() const { return build_generation_; }

bool NotationLayout::text_range_rect(std::size_t item_index,
                                     std::size_t byte_start,
                                     std::size_t byte_length,
                                     NotationTextRangeRect& rect) const
{
    if (item_index >= items_.size() || !byte_length) return false;
    const NotationItem& item = items_[item_index];
    if (byte_start > item.text.size()) return false;
    if (byte_length > item.text.size() - byte_start)
        byte_length = item.text.size() - byte_start;
    if (!byte_length) return false;
    rect.x = item.x + metrics_.text_range_width(item.text,
        item.kind == NOTATION_MOVE, 0, byte_start);
    rect.width = metrics_.text_range_width(item.text,
        item.kind == NOTATION_MOVE, byte_start, byte_length);
    rect.y = item.y;
    rect.height = item.height;
    return rect.width > 0;
}
std::vector<NotationSelectionRect> NotationLayout::selection_rects(const MoveNode* node) const
{ return selection_rects(node, -1, 2147483647); }

void NotationLayout::item_range(int first_y, int last_y, std::size_t& first,
                                std::size_t& last) const
{
    std::size_t low = 0, high = items_.size();
    while (low < high) {
        std::size_t mid = low + (high - low) / 2;
        if (items_[mid].y < first_y) low = mid + 1;
        else high = mid;
    }
    first = low;
    high = items_.size();
    while (low < high) {
        std::size_t mid = low + (high - low) / 2;
        if (items_[mid].y < last_y) low = mid + 1;
        else high = mid;
    }
    last = low;
}

std::vector<NotationSelectionRect> NotationLayout::selection_rects(
    const MoveNode* node, int first_y, int last_y) const
{
    std::vector<NotationSelectionRect> result;
    bool adjacent = false;
    int previous_right = 0;
    std::size_t first, last;
    item_range(first_y, last_y, first, last);
    for (std::size_t i = first; i < last; ++i) {
        const NotationItem& item = items_[i];
        if (!item.selects_move() || item.node != node) { adjacent = false; continue; }
        int left = item.x + item.ink_left, right = item.x + item.ink_right;
        if (adjacent && result.back().y == item.y && item.x == previous_right) {
            NotationSelectionRect& rect = result.back();
            int end = rect.x + rect.width;
            if (right > end) end = right;
            if (left < rect.x) rect.x = left;
            rect.width = end - rect.x;
        } else {
            NotationSelectionRect rect;
            rect.x = left; rect.y = item.y; rect.width = right-left; rect.height = item.height;
            result.push_back(rect);
        }
        adjacent = true; previous_right = item.x + item.width;
    }
    return result;
}
int NotationLayout::content_height() const { return cursor_y_ + line_height_; }
int NotationLayout::max_scroll(int viewport_height) const { int result = content_height() - viewport_height; return result > 0 ? result : 0; }
int NotationLayout::full_line_viewport_height(int available_height, int line_height)
{
    if (available_height < 1 || line_height < 1) return 0;
    return (available_height / line_height) * line_height;
}
int NotationLayout::ensure_visible(const MoveNode* node, int scroll_offset, int viewport_height) const
{
    std::size_t index;
    if (viewport_height < 1) viewport_height = 1;
    int maximum = max_scroll(viewport_height);
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > maximum) scroll_offset = maximum;
    int first = -1, bottom = 0;
    for (index = 0; index < items_.size(); ++index) {
        const NotationItem& item = items_[index];
        if (!item.selects_move() || item.node != node) continue;
        if (first < 0) first = item.y;
        bottom = item.y + item.height;
    }
    if (first >= 0) {
        // In an exceptionally tall split token, retain the established SAN-first
        // behavior. Otherwise keep its complete quality annotation visible too.
        if (bottom-first > viewport_height) bottom = first + line_height_;
        if (first < scroll_offset) scroll_offset = first;
        else if (bottom > scroll_offset + viewport_height) scroll_offset = bottom - viewport_height;
        if (scroll_offset < 0) scroll_offset = 0;
        if (scroll_offset > maximum) scroll_offset = maximum;
    }
    return scroll_offset;
}
int NotationLayout::ensure_end_visible(int scroll_offset, int viewport_height) const
{
    int maximum = max_scroll(viewport_height);
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > maximum) scroll_offset = maximum;
    return maximum;
}
const MoveNode* NotationLayout::hit_test(int x, int y, int scroll_offset) const
{
    std::size_t index, first, last;
    int content_y = y + scroll_offset;
    item_range(content_y - line_height_ + 1, content_y + 1, first, last);
    for (index = first; index < last; ++index) { const NotationItem& item = items_[index];
        if (item.selects_move() && x >= item.x + item.ink_left && x < item.x + item.ink_right && content_y >= item.y && content_y < item.y + item.height) return item.node; }
    return 0;
}
const MoveNode* NotationLayout::hit_test_visible(int x, int y, int scroll_offset, int viewport_height) const
{
    std::size_t index, first, last;
    int content_y = y + scroll_offset;
    if (viewport_height < 1 || y < 0 || y >= viewport_height) return 0;
    item_range(content_y - line_height_ + 1, content_y + 1, first, last);
    for (index = first; index < last; ++index) {
        const NotationItem& item = items_[index];
        int screen_top = item.y - scroll_offset;
        if (!item.selects_move() || screen_top < 0 ||
            screen_top + item.height > viewport_height) continue;
        if (x >= item.x + item.ink_left && x < item.x + item.ink_right && content_y >= item.y && content_y < item.y + item.height)
            return item.node;
    }
    return 0;
}

}
