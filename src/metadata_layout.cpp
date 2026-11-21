#include "gambitree/metadata_layout.h"

namespace gambitree {

MetadataLine::MetadataLine() : text(), primary(false) {}
MetadataLine::MetadataLine(const std::string& value, bool is_primary) : text(value), primary(is_primary) {}

namespace {

std::string player(const PgnGame& game, const char* name_tag, const char* elo_tag)
{
    const std::string* name = game.tag_value(name_tag);
    const std::string* elo = game.tag_value(elo_tag);
    std::string result = name ? *name : "";
    if (elo && !elo->empty()) result += " (" + *elo + ")";
    return result;
}

std::string join(const std::string& first, const std::string& second)
{
    if (first.empty()) return second;
    if (second.empty()) return first;
    return first + " - " + second;
}

void append_wrapped(std::vector<MetadataLine>& result, const std::string& text, bool primary,
                    int width, const NotationFontMetrics& metrics)
{
    std::vector<std::string> wrapped;
    std::size_t index;
    if (text.empty()) return;
    wrapped = NotationLayout::wrap_text(text, width, metrics);
    for (index = 0; index < wrapped.size(); ++index) result.push_back(MetadataLine(wrapped[index], primary));
}

}

std::vector<MetadataLine> MetadataLayout::build(const PgnGame& game, int width,
                                                 const NotationFontMetrics& metrics,
                                                 bool figurines)
{
    std::vector<MetadataLine> result;
    std::string white = player(game, "White", "WhiteElo");
    std::string black = player(game, "Black", "BlackElo");
    const std::string* event = game.tag_value("Event");
    const std::string* round = game.tag_value("Round");
    const std::string* site = game.tag_value("Site");
    const std::string* date = game.tag_value("Date");
    const std::string* result_tag = game.tag_value("Result");
    const std::string* eco = game.tag_value("ECO");
    const std::string* annotator = game.tag_value("Annotator");
    bool survey = event && *event == "Survey" &&
        ((site && *site == "Opening") ||
         (game.tag_value("Black") &&
          NotationLayout::format_chessbase_references(
              *game.tag_value("Black"), false, metrics.figurine_mask()) !=
              *game.tag_value("Black")));
    if (survey) {
        std::string title = player(game, "White", "WhiteElo");
        std::string opening = game.tag_value("Black")
            ? NotationLayout::format_chessbase_references(
                  *game.tag_value("Black"), figurines, metrics.figurine_mask())
            : std::string();
        std::string heading = title.empty() ? "Opening survey" : title + " — Opening survey";
        if (eco && !eco->empty()) heading += " " + *eco;
        if (!opening.empty() && annotator && !annotator->empty())
            opening += " · " + *annotator;
        append_wrapped(result, heading, true, width, metrics);
        append_wrapped(result, opening, false, width, metrics);
        append_wrapped(result, join(date ? *date : "",
                                   result_tag ? *result_tag : game.result),
                       false, width, metrics);
        return result;
    }
    std::string event_round = join(event ? *event : "", round ? *round : "");
    std::string site_date_result = join(join(site ? *site : "", date ? *date : ""),
                                        result_tag ? *result_tag : game.result);
    append_wrapped(result, join(white, black), true, width, metrics);
    append_wrapped(result, event_round, false, width, metrics);
    append_wrapped(result, site_date_result, false, width, metrics);
    return result;
}

}
