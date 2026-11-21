#include "gambitree/notation_search.h"

namespace gambitree {

namespace {

const std::size_t no_item = static_cast<std::size_t>(-1);

std::size_t utf8_size(const std::string& text, std::size_t at)
{
    const unsigned char first = static_cast<unsigned char>(text[at]);
    if (first < 0x80U) return 1;
    if ((first & 0xe0U) == 0xc0U && at + 1 < text.size()) return 2;
    if ((first & 0xf0U) == 0xe0U && at + 2 < text.size()) return 3;
    if ((first & 0xf8U) == 0xf0U && at + 3 < text.size()) return 4;
    return 1;
}

char figurine_letter(const std::string& text, std::size_t at)
{
    if (at + 2 >= text.size() ||
        static_cast<unsigned char>(text[at]) != 0xe2U ||
        static_cast<unsigned char>(text[at + 1]) != 0x99U) return 0;
    const unsigned char last = static_cast<unsigned char>(text[at + 2]);
    static const char letters[] = "KQRBNP";
    return last >= 0x94U && last <= 0x99U ? letters[last - 0x94U] : 0;
}

char fold_ascii(char value)
{
    if (value >= 'A' && value <= 'Z')
        return static_cast<char>(value - 'A' + 'a');
    return value;
}

}

NotationSearch::SourceByte::SourceByte() : item(no_item), begin(0), end(0) {}
NotationSearch::Match::Match() : begin(0), end(0), top(0), bottom(0) {}

NotationSearch::NotationSearch()
    : visible_(false), select_all_(false), current_(0), has_current_(false),
      layout_generation_(0) {}

bool NotationSearch::visible() const { return visible_; }
void NotationSearch::open() { visible_ = true; select_all_ = true; }
void NotationSearch::close() { visible_ = false; select_all_ = false; }
bool NotationSearch::select_all() const { return select_all_; }
void NotationSearch::set_select_all(bool selected) { select_all_ = selected; }
const std::string& NotationSearch::query() const { return query_; }

std::string NotationSearch::normalized_query(const std::string& text)
{
    std::string result;
    for (std::size_t at = 0; at < text.size();) {
        const char piece = figurine_letter(text, at);
        if (piece) {
            result += fold_ascii(piece);
            at += 3;
            continue;
        }
        const std::size_t length = utf8_size(text, at);
        if (length == 1) result += fold_ascii(text[at]);
        else if (length == 2 &&
                 static_cast<unsigned char>(text[at]) == 0xc3U) {
            const unsigned char second =
                static_cast<unsigned char>(text[at + 1]);
            if ((second >= 0x80U && second <= 0x96U) ||
                (second >= 0x98U && second <= 0x9eU)) {
                result += text[at];
                result += static_cast<char>(second + 0x20U);
            } else result.append(text, at, length);
        } else result.append(text, at, length);
        at += length;
    }
    return result;
}

bool NotationSearch::is_move_number(const NotationItem& item)
{
    if (item.kind != NOTATION_MOVE || item.node != 0 || item.text.empty())
        return false;
    bool saw_digit = false;
    for (std::size_t i = 0; i < item.text.size(); ++i) {
        if (item.text[i] >= '0' && item.text[i] <= '9') saw_digit = true;
        else if (item.text[i] != '.' || !saw_digit) return false;
    }
    return saw_digit;
}

void NotationSearch::build_projection(const NotationLayout& layout)
{
    const std::vector<NotationItem>& items = layout.items();
    projection_.clear();
    source_bytes_.clear();
    for (std::size_t item_index = 0; item_index < items.size(); ++item_index) {
        const NotationItem& item = items[item_index];
        if (item_index) {
            const NotationItem& previous = items[item_index - 1];
            const bool number_to_san = is_move_number(previous) &&
                item.kind == NOTATION_MOVE && item.node != 0;
            const bool quality_suffix = item.kind == NOTATION_NAG &&
                item.move_quality && previous.node == item.node;
            bool separate = true;
            if (number_to_san || quality_suffix) separate = false;
            else if (previous.y == item.y &&
                     item.x <= previous.x + previous.width)
                separate = false;
            if (separate) {
                projection_ += ' ';
                source_bytes_.push_back(SourceByte());
            }
        }

        for (std::size_t at = 0; at < item.text.size();) {
            const char piece = figurine_letter(item.text, at);
            const std::size_t source_length = piece ? 3 : utf8_size(item.text, at);
            const std::size_t output_start = projection_.size();
            if (piece) projection_ += fold_ascii(piece);
            else if (source_length == 1)
                projection_ += fold_ascii(item.text[at]);
            else if (source_length == 2 &&
                     static_cast<unsigned char>(item.text[at]) == 0xc3U) {
                const unsigned char second =
                    static_cast<unsigned char>(item.text[at + 1]);
                if ((second >= 0x80U && second <= 0x96U) ||
                    (second >= 0x98U && second <= 0x9eU)) {
                    projection_ += item.text[at];
                    projection_ += static_cast<char>(second + 0x20U);
                } else projection_.append(item.text, at, source_length);
            } else projection_.append(item.text, at, source_length);
            for (std::size_t byte = output_start; byte < projection_.size(); ++byte) {
                SourceByte source;
                source.item = item_index;
                source.begin = at;
                source.end = at + source_length;
                source_bytes_.push_back(source);
            }
            at += source_length;
        }
    }
    layout_generation_ = layout.layout_generation();
}

void NotationSearch::build_matches(const NotationLayout& layout,
                                   int scroll_offset, int viewport_height,
                                   bool preserve_current)
{
    const std::size_t old_current = current_;
    matches_.clear();
    has_current_ = false;
    const std::string needle = normalized_query(query_);
    if (needle.empty()) return;

    std::size_t found = projection_.find(needle);
    while (found != std::string::npos) {
        Match match;
        match.begin = found;
        match.end = found + needle.size();
        match.top = 2147483647;
        match.bottom = 0;
        const std::vector<NotationItem>& items = layout.items();
        for (std::size_t i = match.begin; i < match.end &&
             i < source_bytes_.size(); ++i) {
            const SourceByte& source = source_bytes_[i];
            if (source.item == no_item || source.item >= items.size()) continue;
            const NotationItem& item = items[source.item];
            if (item.y < match.top) match.top = item.y;
            if (item.y + item.height > match.bottom)
                match.bottom = item.y + item.height;
        }
        if (match.top != 2147483647)
            matches_.push_back(match);
        found = projection_.find(needle, found + 1);
    }

    if (matches_.empty()) return;
    if (preserve_current) current_ = old_current % matches_.size();
    else {
        current_ = 0;
        int first_after = -1;
        for (std::size_t i = 0; i < matches_.size(); ++i) {
            if (matches_[i].bottom > scroll_offset) {
                current_ = i;
                first_after = 1;
                break;
            }
        }
        if (first_after < 0) current_ = 0;
        (void)viewport_height;
    }
    has_current_ = true;
}

void NotationSearch::set_query(const std::string& query,
                               const NotationLayout& layout,
                               int scroll_offset, int viewport_height)
{
    if (layout_generation_ != layout.layout_generation())
        build_projection(layout);
    query_ = query;
    select_all_ = false;
    build_matches(layout, scroll_offset, viewport_height, false);
}

void NotationSearch::refresh(const NotationLayout& layout)
{
    if (layout_generation_ == layout.layout_generation()) return;
    build_projection(layout);
    if (!query_.empty()) build_matches(layout, 0, 0, true);
}

bool NotationSearch::move(int direction)
{
    if (matches_.empty() || !direction) return false;
    if (!has_current_) {
        current_ = direction > 0 ? 0 : matches_.size() - 1;
        has_current_ = true;
        return true;
    }
    if (direction > 0) current_ = (current_ + 1) % matches_.size();
    else current_ = current_ == 0 ? matches_.size() - 1 : current_ - 1;
    return true;
}

bool NotationSearch::has_current() const { return has_current_; }
std::size_t NotationSearch::result_count() const { return matches_.size(); }
std::size_t NotationSearch::current_result() const
{ return has_current_ ? current_ : 0; }

int NotationSearch::scroll_to_current(const NotationLayout& layout,
                                      int scroll_offset,
                                      int viewport_height) const
{
    if (!has_current_ || current_ >= matches_.size()) return scroll_offset;
    if (viewport_height < 1) viewport_height = 1;
    const Match& match = matches_[current_];
    if (match.bottom - match.top > viewport_height)
        scroll_offset = match.top;
    else if (match.top < scroll_offset)
        scroll_offset = match.top;
    else if (match.bottom > scroll_offset + viewport_height)
        scroll_offset = match.bottom - viewport_height;
    if (scroll_offset < 0) scroll_offset = 0;
    const int maximum = layout.max_scroll(viewport_height);
    if (scroll_offset > maximum) scroll_offset = maximum;
    return scroll_offset;
}

std::vector<NotationSearchHighlight> NotationSearch::highlights(
    const NotationLayout& layout, int first_y, int last_y) const
{
    std::vector<NotationSearchHighlight> result;
    const std::vector<NotationItem>& items = layout.items();
    std::size_t low = 0, high = matches_.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (matches_[middle].bottom <= first_y) low = middle + 1;
        else high = middle;
    }
    for (std::size_t match_index = low; match_index < matches_.size(); ++match_index) {
        const Match& match = matches_[match_index];
        if (match.top >= last_y) break;
        bool active = false;
        std::size_t active_item = 0, span_begin = 0, span_end = 0;
        for (std::size_t at = match.begin; at < match.end &&
             at < source_bytes_.size(); ++at) {
            const SourceByte& source = source_bytes_[at];
            if (source.item == no_item || source.item >= items.size()) {
                if (active) {
                    NotationSearchHighlight highlight;
                    if (layout.text_range_rect(active_item, span_begin,
                            span_end - span_begin, highlight.rect)) {
                        highlight.current = has_current_ &&
                                            match_index == current_;
                        result.push_back(highlight);
                    }
                    active = false;
                }
                continue;
            }
            if (active && source.item == active_item &&
                source.begin <= span_end) {
                if (source.end > span_end) span_end = source.end;
                continue;
            }
            if (active) {
                NotationSearchHighlight highlight;
                if (layout.text_range_rect(active_item, span_begin,
                        span_end - span_begin, highlight.rect)) {
                    highlight.current = has_current_ &&
                                        match_index == current_;
                    result.push_back(highlight);
                }
            }
            active = true;
            active_item = source.item;
            span_begin = source.begin;
            span_end = source.end;
        }
        if (active) {
            NotationSearchHighlight highlight;
            if (layout.text_range_rect(active_item, span_begin,
                    span_end - span_begin, highlight.rect)) {
                highlight.current = has_current_ && match_index == current_;
                result.push_back(highlight);
            }
        }
    }
    return result;
}

}
