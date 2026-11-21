#include "gambitree/navigation_list.h"
#include "gambitree/notation_layout.h"

#include <cctype>
#include <sstream>

namespace gambitree {
namespace {
std::string value(const std::vector<PgnTag>& tags, const char* name)
{
    for (std::size_t i = 0; i < tags.size(); ++i)
        if (tags[i].name == name) return tags[i].value;
    return std::string();
}

void append_utf8(std::string& out, unsigned long codepoint)
{
    if (codepoint < 0x80UL) out += static_cast<char>(codepoint);
    else if (codepoint < 0x800UL) {
        out += static_cast<char>(0xc0UL | (codepoint >> 6));
        out += static_cast<char>(0x80UL | (codepoint & 0x3fUL));
    } else if (codepoint < 0x10000UL) {
        out += static_cast<char>(0xe0UL | (codepoint >> 12));
        out += static_cast<char>(0x80UL | ((codepoint >> 6) & 0x3fUL));
        out += static_cast<char>(0x80UL | (codepoint & 0x3fUL));
    } else {
        out += static_cast<char>(0xf0UL | (codepoint >> 18));
        out += static_cast<char>(0x80UL | ((codepoint >> 12) & 0x3fUL));
        out += static_cast<char>(0x80UL | ((codepoint >> 6) & 0x3fUL));
        out += static_cast<char>(0x80UL | (codepoint & 0x3fUL));
    }
}

std::string lower_text(const std::string& text)
{
    std::string result;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[offset]);
        unsigned long cp = first;
        std::size_t count = 1;
        if ((first & 0xe0U) == 0xc0U && offset + 1 < text.size()) {
            cp = (static_cast<unsigned long>(first & 0x1fU) << 6) |
                 (static_cast<unsigned char>(text[offset + 1]) & 0x3fU);
            count = 2;
        } else if ((first & 0xf0U) == 0xe0U && offset + 2 < text.size()) {
            cp = (static_cast<unsigned long>(first & 0x0fU) << 12) |
                 (static_cast<unsigned long>(
                     static_cast<unsigned char>(text[offset + 1]) & 0x3fU)
                  << 6) |
                 (static_cast<unsigned char>(text[offset + 2]) & 0x3fU);
            count = 3;
        } else if ((first & 0xf8U) == 0xf0U && offset + 3 < text.size()) {
            cp = (static_cast<unsigned long>(first & 0x07U) << 18) |
                 (static_cast<unsigned long>(
                     static_cast<unsigned char>(text[offset + 1]) & 0x3fU)
                  << 12) |
                 (static_cast<unsigned long>(
                     static_cast<unsigned char>(text[offset + 2]) & 0x3fU)
                  << 6) |
                 (static_cast<unsigned char>(text[offset + 3]) & 0x3fU);
            count = 4;
        }
        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        else if ((cp >= 0x00c0UL && cp <= 0x00d6UL) ||
                 (cp >= 0x00d8UL && cp <= 0x00deUL)) cp += 0x20UL;
        else if (cp == 0x0178UL) cp = 0x00ffUL;
        else if ((cp >= 0x0391UL && cp <= 0x03a1UL) ||
                 (cp >= 0x03a3UL && cp <= 0x03abUL)) cp += 0x20UL;
        else if (cp >= 0x0410UL && cp <= 0x042fUL) cp += 0x20UL;
        else if (cp >= 0x0400UL && cp <= 0x040fUL) cp += 0x50UL;
        append_utf8(result, cp);
        offset += count;
    }
    return result;
}

std::size_t utf8_previous(const std::string& text)
{
    if (text.empty()) return 0;
    std::size_t at = text.size() - 1;
    while (at > 0 &&
           (static_cast<unsigned char>(text[at]) & 0xc0U) == 0x80U) --at;
    return at;
}

void append_tag(std::ostringstream& out, const char* label,
                const std::string& text)
{
    if (!text.empty()) out << " · " << label << ' ' << text;
}
}

NavigationListState::NavigationListState()
    : kind_(NAVIGATION_LIST_CLOSED), selected_(0), first_(0),
      current_index_(0), visible_rows_(1) {}

void NavigationListState::open(NavigationListKind kind,
                               const std::vector<std::string>& items,
                               std::size_t current_index)
{
    kind_ = kind;
    query_.clear();
    items_ = items;
    current_index_ = current_index;
    selected_ = first_ = 0;
    rebuild_matches(current_index);
}

void NavigationListState::replace_items(const std::vector<std::string>& items,
                                        std::size_t current_index)
{
    const std::size_t preferred = selected_source_index();
    items_ = items;
    current_index_ = current_index;
    rebuild_matches(preferred == static_cast<std::size_t>(-1)
                    ? current_index : preferred);
}

void NavigationListState::append_items(const std::vector<std::string>& items,
                                       std::size_t current_index)
{
    const std::size_t preferred = selected_source_index();
    items_.insert(items_.end(), items.begin(), items.end());
    current_index_ = current_index;
    rebuild_matches(preferred == static_cast<std::size_t>(-1)
                    ? current_index : preferred);
}

void NavigationListState::close()
{ kind_ = NAVIGATION_LIST_CLOSED; query_.clear(); items_.clear(); matches_.clear(); selected_ = first_ = 0; }
bool NavigationListState::visible() const { return kind_ != NAVIGATION_LIST_CLOSED; }
NavigationListKind NavigationListState::kind() const { return kind_; }
const std::string& NavigationListState::query() const { return query_; }
const std::vector<std::string>& NavigationListState::items() const { return items_; }
const std::vector<std::size_t>& NavigationListState::matches() const { return matches_; }
std::size_t NavigationListState::selected_match() const { return selected_; }
std::size_t NavigationListState::first_match() const { return first_; }
std::size_t NavigationListState::visible_rows() const { return visible_rows_; }
bool NavigationListState::has_selection() const { return selected_ < matches_.size(); }
std::size_t NavigationListState::selected_source_index() const
{ return has_selection() ? matches_[selected_] : static_cast<std::size_t>(-1); }

void NavigationListState::set_visible_rows(std::size_t rows)
{ visible_rows_ = rows ? rows : 1; keep_selection_visible(); }

void NavigationListState::set_query(const std::string& query)
{
    const std::size_t preferred = selected_source_index();
    query_ = query;
    rebuild_matches(preferred == static_cast<std::size_t>(-1)
                    ? current_index_ : preferred);
}

void NavigationListState::append_query(const std::string& text)
{ set_query(query_ + text); }

void NavigationListState::backspace_query()
{
    if (query_.empty()) return;
    query_.erase(utf8_previous(query_));
    set_query(query_);
}

void NavigationListState::rebuild_matches(std::size_t preferred_source)
{
    const std::string needle = lower_text(query_);
    matches_.clear();
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (needle.empty() || lower_text(items_[i]).find(needle) !=
                              std::string::npos)
            matches_.push_back(i);
    selected_ = 0;
    for (std::size_t i = 0; i < matches_.size(); ++i)
        if (matches_[i] == preferred_source) { selected_ = i; break; }
    first_ = 0;
    keep_selection_visible();
}

void NavigationListState::keep_selection_visible()
{
    if (matches_.empty()) { selected_ = first_ = 0; return; }
    if (selected_ >= matches_.size()) selected_ = matches_.size() - 1;
    if (selected_ < first_) first_ = selected_;
    if (selected_ >= first_ + visible_rows_)
        first_ = selected_ - visible_rows_ + 1;
    const std::size_t maximum = matches_.size() > visible_rows_
        ? matches_.size() - visible_rows_ : 0;
    if (first_ > maximum) first_ = maximum;
}

void NavigationListState::move(int amount)
{
    if (matches_.empty() || amount == 0) return;
    long next = static_cast<long>(selected_) + amount;
    if (next < 0) next = 0;
    if (next >= static_cast<long>(matches_.size()))
        next = static_cast<long>(matches_.size() - 1);
    selected_ = static_cast<std::size_t>(next);
    keep_selection_visible();
}

void NavigationListState::page(int direction)
{ move(direction * static_cast<int>(visible_rows_ > 1 ? visible_rows_ - 1 : 1)); }
void NavigationListState::home() { if (!matches_.empty()) { selected_ = 0; keep_selection_visible(); } }
void NavigationListState::end() { if (!matches_.empty()) { selected_ = matches_.size() - 1; keep_selection_visible(); } }

bool NavigationListState::select_match_at(std::size_t visible_row)
{
    const std::size_t target = first_ + visible_row;
    if (target >= matches_.size() || visible_row >= visible_rows_) return false;
    selected_ = target;
    return true;
}

std::string navigation_game_label(std::size_t game_number,
                                  const std::vector<PgnTag>& tags,
                                  bool figurines,
                                  unsigned int figurine_mask)
{
    std::ostringstream out;
    const std::string white = value(tags, "White");
    const std::string black_source = value(tags, "Black");
    const std::string black = NotationLayout::format_chessbase_references(
        black_source, figurines, figurine_mask);
    const std::string event = value(tags, "Event");
    const std::string site = value(tags, "Site");
    const std::string eco = value(tags, "ECO");
    const bool survey = lower_text(event) == "survey" ||
                        lower_text(site) == "opening";
    out << game_number << ". ";
    if (!white.empty() && !black.empty() && !survey)
        out << white << " – " << black;
    else if (!white.empty())
        out << white;
    else if (!event.empty())
        out << event;
    else
        out << "Game " << game_number;
    if (survey) {
        out << " — Opening survey";
        if (!black.empty()) out << " · " << black;
    }
    append_tag(out, "Event", event == "Survey" ? std::string() : event);
    append_tag(out, "Date", value(tags, "Date"));
    append_tag(out, "Round", value(tags, "Round"));
    if (!eco.empty()) out << " · " << eco;
    append_tag(out, "Opening", value(tags, "Opening"));
    append_tag(out, "Variation", value(tags, "Variation"));
    return out.str();
}

}
