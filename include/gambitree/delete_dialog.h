#ifndef GAMBITREE_DELETE_DIALOG_H
#define GAMBITREE_DELETE_DIALOG_H

#include <string>
#include <cstdlib>

namespace gambitree {

enum DeleteDialogKey {
    DELETE_DIALOG_KEY_OTHER,
    DELETE_DIALOG_KEY_TAB,
    DELETE_DIALOG_KEY_ENTER,
    DELETE_DIALOG_KEY_ESCAPE,
    DELETE_DIALOG_KEY_CONFIRM
};

enum DeleteDialogAction {
    DELETE_DIALOG_NO_ACTION,
    DELETE_DIALOG_CANCEL,
    DELETE_DIALOG_CONFIRM
};

struct DeleteDialogInputState {
    int focused_button;
    bool error;
    bool action_taken;

    DeleteDialogInputState(bool is_error = false)
        : focused_button(1), error(is_error), action_taken(false)
    {
    }
};

inline DeleteDialogAction handle_delete_dialog_key(
    DeleteDialogInputState& state, DeleteDialogKey key, bool shift)
{
    if (key == DELETE_DIALOG_KEY_ESCAPE)
        return DELETE_DIALOG_CANCEL;
    if (state.action_taken)
        return DELETE_DIALOG_NO_ACTION;
    if (key == DELETE_DIALOG_KEY_TAB && !state.error) {
        /* Forward and reverse traversal both select the other of two buttons. */
        state.focused_button = 1 - state.focused_button;
        return DELETE_DIALOG_NO_ACTION;
    }
    if (key == DELETE_DIALOG_KEY_ENTER) {
        state.action_taken = true;
        if (state.error || state.focused_button == 0)
            return DELETE_DIALOG_CANCEL;
        return DELETE_DIALOG_CONFIRM;
    }
    if (key == DELETE_DIALOG_KEY_CONFIRM && !state.error) {
        state.action_taken = true;
        return DELETE_DIALOG_CONFIRM;
    }
    (void)shift;
    return DELETE_DIALOG_NO_ACTION;
}

struct DeleteDialogTextExtents {
    int x;
    int y;
    int width;
    int height;
    int advance;
    int ascent;
    int descent;
    DeleteDialogTextExtents()
        : x(0), y(0), width(0), height(0), advance(0), ascent(0), descent(0)
    {
    }
};

struct DeleteDialogTextOrigin {
    int x;
    int baseline;
};

inline int delete_dialog_title_bar_height(int font_height)
{
    return font_height + 18;
}

inline int delete_dialog_title_bar_height(int font_height, int line_count)
{
    int lines = line_count > 0 ? line_count : 1;
    return lines * (font_height + 2) + 16;
}

inline int delete_dialog_button_height(int font_height)
{
    int requested = font_height + 18;
    return requested < 50 ? 50 : requested;
}

inline int delete_dialog_button_width(int cancel_advance,
                                      int delete_advance)
{
    int widest = cancel_advance > delete_advance
        ? cancel_advance : delete_advance;
    int requested = widest + 36;
    return requested < 112 ? 112 : requested;
}

inline int delete_dialog_button_y(int dialog_height, int button_height)
{
    return dialog_height - button_height - 14;
}

inline int delete_dialog_height_for_content(int title_height,
                                            int body_lines,
                                            int body_line_height,
                                            int button_height)
{
    int height = title_height + 12 + body_lines * body_line_height +
                 12 + button_height + 14;
    return height < 220 ? 220 : height;
}

inline DeleteDialogTextOrigin center_delete_dialog_label(
    int button_x, int button_y, int button_width, int button_height,
    const DeleteDialogTextExtents& extents)
{
    DeleteDialogTextOrigin origin;
    int left = extents.x < 0 ? extents.x : 0;
    int right = extents.x + extents.width;
    if (right < extents.advance)
        right = extents.advance;
    origin.x = button_x + (button_width - (right - left)) / 2 - left;
    origin.baseline = button_y +
        (button_height - (extents.ascent + extents.descent)) / 2 +
        extents.ascent;
    return origin;
}

inline bool is_ascii_digits(const std::string& value)
{
    std::size_t index;
    if (value.empty())
        return false;
    for (index = 0; index < value.size(); ++index)
        if (value[index] < '0' || value[index] > '9')
            return false;
    return true;
}

inline std::string format_pgn_game_date(const std::string& value)
{
    std::string parts[3];
    std::size_t first = value.find('.');
    std::size_t second;
    std::size_t third;
    std::string result;
    unsigned long year = 0;
    unsigned long month = 0;
    unsigned long day = 0;
    std::size_t index;
    if (first != 4 || value.size() != 10 || value[7] != '.')
        return std::string();
    second = 5;
    third = 8;
    parts[0] = value.substr(0, first);
    parts[1] = value.substr(second, 2);
    parts[2] = value.substr(third, 2);
    for (index = 0; index < 3; ++index)
        if (parts[index] != "??" && !is_ascii_digits(parts[index]))
            return std::string();
    if (is_ascii_digits(parts[0])) {
        year = static_cast<unsigned long>(std::strtoul(parts[0].c_str(), 0, 10));
        if (year == 0)
            return std::string();
    }
    if (is_ascii_digits(parts[1])) {
        month = static_cast<unsigned long>(std::strtoul(parts[1].c_str(), 0, 10));
        if (month < 1 || month > 12)
            return std::string();
    }
    if (is_ascii_digits(parts[2])) {
        day = static_cast<unsigned long>(std::strtoul(parts[2].c_str(), 0, 10));
        if (day < 1 || day > 31)
            return std::string();
    }
    if (month && day) {
        static const unsigned int month_days[12] = {
            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
        };
        unsigned int maximum = month_days[month - 1];
        if (month == 2 && year &&
            (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)))
            maximum = 29;
        if (day > maximum)
            return std::string();
    }
    /* PGN date components are ordered.  Once a component is unknown,
       later components cannot be shown without inventing their meaning. */
    for (index = 0; index < 3; ++index) {
        if (parts[index] == "??")
            break;
        if (!result.empty())
            result += '.';
        result += parts[index];
    }
    return result;
}

inline std::string format_pgn_elo(const std::string& value)
{
    std::size_t first = 0;
    std::size_t last = value.size();
    unsigned int rating = 0;
    std::size_t index;
    while (first < last && (value[first] == ' ' || value[first] == '\t'))
        ++first;
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t'))
        --last;
    if (first == last || last - first > 4)
        return std::string();
    for (index = first; index < last; ++index) {
        if (value[index] < '0' || value[index] > '9')
            return std::string();
        rating = rating * 10U +
            static_cast<unsigned int>(value[index] - '0');
        if (rating > 4000U)
            return std::string();
    }
    return value.substr(first, last - first);
}

inline std::string format_pgn_player(const std::string& side,
                                     const std::string& name,
                                     const std::string& elo)
{
    std::string result = side + ": " +
        (name.empty() ? std::string("(unknown)") : name);
    std::string rating = format_pgn_elo(elo);
    if (!rating.empty())
        result += " (" + rating + ")";
    return result;
}

}

#endif
