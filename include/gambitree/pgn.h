#ifndef GAMBITREE_PGN_H
#define GAMBITREE_PGN_H

#include <string>
#include <vector>
#include <stdint.h>

#include "gambitree/chess.h"

namespace gambitree {

struct PgnTag {
    std::string name;
    std::string value;
    PgnTag();
    PgnTag(const std::string& tag_name, const std::string& tag_value);
};

struct PgnError {
    int game_number;
    int line;
    int column;
    std::string message;
    PgnError();
    PgnError(int game, int error_line, int error_column, const std::string& error_message);
};

struct PgnWarning {
    int game_number;
    int line;
    int column;
    std::string message;
    PgnWarning();
    PgnWarning(int game, int warning_line, int warning_column,
               const std::string& warning_message);
};

struct MoveNode;

struct PgnMoveItem {
    enum Type { COMMENT, VARIATION };
    Type type;
    std::string comment;
    MoveNode* variation_root;
    PgnMoveItem(Type item_type, const std::string& item_comment,
                MoveNode* root)
        : type(item_type), comment(item_comment), variation_root(root) {}
};

struct MoveNode {
    MoveNode* parent;
    std::vector<MoveNode*> children;
    Move move;
    std::string san;
    std::vector<std::string> leading_comments;
    std::vector<std::string> comments;
    std::vector<int> nags;
    /* Comments and RAVs in the source order after this move. Variation roots
       are non-owning references; the regular children tree owns every node. */
    std::vector<PgnMoveItem> following_items;
    Position position_after;
    bool has_move;

    MoveNode();
    ~MoveNode();

private:
    MoveNode(const MoveNode&);
    MoveNode& operator=(const MoveNode&);
};

struct PgnTrailingItem {
    enum Type { COMMENT, HYPOTHETICAL_VARIATION };
    Type type;
    std::string comment;
    MoveNode* variation_root;
    PgnTrailingItem(Type item_type, const std::string& item_comment,
                    MoveNode* root)
        : type(item_type), comment(item_comment), variation_root(root) {}
};

enum PgnParseStatus { PGN_COMPLETE, PGN_RECOVERED_VARIATION, PGN_PARTIAL };

class PgnGame {
public:
    std::vector<PgnTag> tags;
    MoveNode* root;
    Position initial_position;
    std::string result;
    std::vector<PgnTrailingItem> trailing_items;
    PgnParseStatus parse_status;
    PgnError parse_error;
    // Non-owning, always a navigable node in root's validated tree.
    const MoveNode* error_position;
    // Length of the validated path to error_position, excluding side branches.
    std::size_t available_plies;
    std::string parse_warning() const;
    uint64_t instance_identity() const;

    PgnGame();
    ~PgnGame();
    const std::string* tag_value(const std::string& name) const;

private:
    PgnGame(const PgnGame&);
    PgnGame& operator=(const PgnGame&);
    uint64_t instance_identity_;
};

class PgnDocument {
public:
    std::vector<PgnGame*> games;
    std::vector<PgnError> errors;
    std::vector<PgnWarning> warnings;
    std::size_t skipped_variation_count;

    PgnDocument();
    ~PgnDocument();
    void clear();

private:
    PgnDocument(const PgnDocument&);
    PgnDocument& operator=(const PgnDocument&);
};

class PgnParser {
public:
    /* Parses all recoverable games. On an invalid game, tokens are skipped until
       the next tag pair, which is the parser's reliable resynchronization point. */
    static bool parse(const std::string& text, PgnDocument& document,
                      bool allow_initial_bom = true);
    /* Viewer mode retains validated prefixes after movetext errors. False still
       reports a parse error; inspect each retained game's explicit status. */
    static bool parse_for_viewing(const std::string& text, PgnDocument& document,
                                 bool allow_initial_bom = true);
private:
    static bool parse_impl(const std::string& text, PgnDocument& document,
                           bool allow_initial_bom, bool retain_prefix);
};

}

#endif
