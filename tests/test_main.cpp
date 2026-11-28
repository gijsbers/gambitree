#include <cstring>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <cmath>
#include <sys/stat.h>
#include <dirent.h>
#include <vector>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>
#include <ctime>
#include <X11/keysym.h>

#include "gambitree/chess.h"
#include "gambitree/command_line.h"
#include "gambitree/configure_event_batch.h"
#include "gambitree/complete_frame_buffer.h"
#include "gambitree/delete_dialog.h"
#include "gambitree/deferred_piece_resize.h"
#include "gambitree/piece_cache_key.h"
#include "gambitree/keyboard_navigation.h"
#include "gambitree/keyboard_help.h"
#include "gambitree/navigation_list.h"
#include "gambitree/board_layout.h"
#include "gambitree/board_veneer.h"
#include "gambitree/navigation.h"
#include "gambitree/visited_game_state.h"
#include "gambitree/piece_layout.h"
#include "gambitree/piece_raster_worklist.h"
#include "gambitree/piece_set.h"
#include "gambitree/pixel_composite.h"
#include "gambitree/sidebar_layout.h"
#include "gambitree/notation_layout.h"
#include "gambitree/notation_search.h"
#include "gambitree/metadata_layout.h"
#include "gambitree/png_image.h"
#include "gambitree/application_icon.h"
#include "gambitree/pgn.h"
#include "gambitree/pgn_library.h"
#include "gambitree/pgn_index.h"
#include "gambitree/pgn_delete.h"
#include "gambitree/theme.h"
#include "gambitree/asset_locator.h"
#include "gambitree/uci_engine.h"
#include "gambitree/engine_display.h"
#include "gambitree/version.h"
#include "gambitree/session_state.h"

#include "test_paths.h"

using gambitree::Move;
using gambitree::BoardGeometry;
using gambitree::BoardLayout;
using gambitree::Position;
using gambitree::Undo;
using gambitree::MoveNode;
using gambitree::MoveNavigator;
using gambitree::PgnDocument;
using gambitree::PgnGame;
using gambitree::PngImage;
using gambitree::PieceTheme;
using gambitree::PieceLayout;
using gambitree::PiecePlacement;
using gambitree::NotationLayout;
using gambitree::NotationFontMetrics;
using gambitree::MetadataLayout;
using gambitree::MetadataLine;
using gambitree::PgnLibrary;
using gambitree::PgnIndex;
using gambitree::PgnOffset;
using gambitree::UciAnalysis;
using gambitree::UciEngine;
using gambitree::UciInfo;
using gambitree::UCI_SCORE_CENTIPAWNS;
using gambitree::UCI_SCORE_MATE;
using gambitree::UCI_UNAVAILABLE;
using gambitree::UCI_STARTING;
using gambitree::UCI_WAITING_READY;
using gambitree::UCI_READY;
using gambitree::UCI_ANALYZING;
using gambitree::UCI_STOPPING;
using gambitree::EngineUiThrottle;
using gambitree::EngineDisplay;
using gambitree::EngineDisplayGeometry;
using gambitree::CommandLineOptions;
using gambitree::COMMAND_LINE_RUN;
using gambitree::COMMAND_LINE_ERROR;
using gambitree::SidebarGeometry;
using gambitree::SidebarLayout;
using gambitree::AssetLocator;
using gambitree::WHITE;
using gambitree::BLACK;

namespace {

int failures = 0;
std::string test_scratch_directory;
std::string source_directory;

class VariableTextMeasurer : public gambitree::NotationTextMeasurer {
public:
    int text_width(const std::string& text) const {
        int width = 0;
        std::size_t index;
        for (index = 0; index < text.size(); ++index)
            width += text[index] == 'W' ? 11 : (text[index] == 'i' ? 3 : (text[index] == ' ' ? 4 : 6));
        return width;
    }
};

std::vector<const MoveNode*> displayed_move_order(const NotationLayout& layout)
{
    std::vector<const MoveNode*> result;
    const std::vector<gambitree::NotationItem>& items = layout.items();
    std::size_t index;
    for (index = 0; index < items.size(); ++index)
        if (items[index].kind == gambitree::NOTATION_MOVE && items[index].node &&
            (result.empty() || result.back() != items[index].node))
            result.push_back(items[index].node);
    return result;
}

class TestPixelSurface {
public:
    TestPixelSurface(int width, int height, unsigned long initial)
        : width_(width), height_(height), pixels_(
              static_cast<std::size_t>(width) * height, initial)
    {
    }
    int width() const { return width_; }
    int height() const { return height_; }
    unsigned long pixel(int x, int y) const
    {
        return pixels_[static_cast<std::size_t>(y) * width_ + x];
    }
    void set_pixel(int x, int y, unsigned long value)
    {
        pixels_[static_cast<std::size_t>(y) * width_ + x] = value;
    }
    const std::vector<unsigned long>& pixels() const { return pixels_; }
private:
    int width_;
    int height_;
    std::vector<unsigned long> pixels_;
};

unsigned char reference_pixel_component(unsigned long value,
                                        unsigned long mask)
{
    unsigned long shifted = mask;
    unsigned long component;
    int shift = 0;
    while (shifted && !(shifted & 1UL)) {
        ++shift;
        shifted >>= 1;
    }
    if (!shifted)
        return 0;
    component = (value & mask) >> shift;
    return static_cast<unsigned char>(
        (component * 255UL + shifted / 2UL) / shifted);
}

unsigned long reference_composite_pixel(unsigned long background,
                                        const unsigned char* rgba,
                                        unsigned long red_mask,
                                        unsigned long green_mask,
                                        unsigned long blue_mask)
{
    unsigned int alpha = rgba[3];
    unsigned char channels[3];
    unsigned long masks[3];
    unsigned long result = 0;
    int index;
    masks[0] = red_mask;
    masks[1] = green_mask;
    masks[2] = blue_mask;
    if (alpha == 0)
        return background;
    channels[0] = reference_pixel_component(background, red_mask);
    channels[1] = reference_pixel_component(background, green_mask);
    channels[2] = reference_pixel_component(background, blue_mask);
    for (index = 0; index < 3; ++index) {
        unsigned long shifted = masks[index];
        unsigned long value;
        int shift = 0;
        unsigned char blended = static_cast<unsigned char>(
            (rgba[index] * alpha + channels[index] * (255 - alpha)) / 255);
        while (shifted && !(shifted & 1UL)) {
            ++shift;
            shifted >>= 1;
        }
        if (shifted) {
            value = (static_cast<unsigned long>(blended) * shifted + 127UL) /
                    255UL;
            result |= (value << shift) & masks[index];
        }
    }
    return result;
}

void expect_true(bool value, const char* description)
{
    if (!value) {
        std::cerr << "FAIL: " << description << "\n";
        ++failures;
    }
}

Position position_from_fen(const char* fen)
{
    Position position;
    std::string error;
    expect_true(Position::from_fen(fen, position, &error), fen);
    return position;
}

bool play_san(Position& position, const char* text, Undo* undo)
{
    Move move;
    Undo local;
    if (!position.parse_san(text, move)) return false;
    return position.make_move(move, undo ? *undo : local);
}

unsigned long perft(Position& position, int depth)
{
    std::vector<Move> moves;
    std::size_t index;
    unsigned long nodes = 0;
    if (depth == 0) return 1;
    moves = position.legal_moves();
    for (index = 0; index < moves.size(); ++index) {
        Undo undo;
        expect_true(position.make_move(moves[index], undo), "perft move is legal");
        nodes += perft(position, depth - 1);
        position.unmake_move(undo);
    }
    return nodes;
}

void test_scratch_paths()
{
    expect_true(test_paths::scratch_root(0) == source_directory + "/build" &&
                test_paths::scratch_root("") == source_directory + "/build",
                "unset or empty TMPDIR uses the portable build-directory default");
    expect_true(test_paths::scratch_root("relative scratch") == source_directory + "/relative scratch" &&
                test_paths::scratch_root(test_scratch_directory.c_str()) == test_scratch_directory,
                "scratch paths accept relative and absolute roots including spaces");
    const std::string parent = test_scratch_directory + "/parent with spaces";
    expect_true(mkdir(parent.c_str(), 0700) == 0, "create a portable scratch parent");
    const std::string first = test_paths::make_directory("child", parent);
    const std::string second = test_paths::make_directory("child", parent);
    expect_true(!first.empty() && !second.empty() && first != second &&
                first.find(parent + "/child.") == 0 && second.find(parent + "/child.") == 0,
                "each test run gets a distinct directory under its requested scratch parent");
    expect_true(test_paths::make_directory("child", parent + "/absent").empty(),
                "unusable scratch roots fail without silently writing elsewhere");
    if (!first.empty() && !second.empty()) {
        const std::string nested = first + "/captures";
        expect_true(mkdir(nested.c_str(), 0700) == 0, "create nested scratch captures");
        { std::ofstream file((nested + "/.capture").c_str()); file << "capture"; }
        { std::ofstream file((second + "/keep").c_str()); file << "outside scratch tree"; }
        expect_true(symlink(second.c_str(), (first + "/linked-directory").c_str()) == 0 &&
                    symlink((second + "/absent").c_str(), (first + "/broken-link").c_str()) == 0,
                    "create scratch links without adding external test files");
        expect_true(test_paths::remove_tree(first) && access(first.c_str(), F_OK) != 0,
                    "scratch cleanup removes nested captures, hidden files and symbolic links");
        std::ifstream kept((second + "/keep").c_str());
        std::string contents;
        std::getline(kept, contents);
        expect_true(contents == "outside scratch tree",
                    "scratch cleanup never follows links outside its owned directory");
        expect_true(test_paths::remove_tree(second), "remove the isolated cleanup reference directory");
    }
    rmdir(first.c_str()); rmdir(second.c_str()); rmdir(parent.c_str());
}

void test_fen()
{
    Position start = Position::standard();
    const char* standard = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    Position parsed;
    std::string error;
    expect_true(start.to_fen() == standard, "standard FEN serialization");
    expect_true(start.legal_moves().size() == 20, "standard position has twenty legal moves");
    expect_true(Position::from_fen("r3k2r/ppp2ppp/2n5/3pp3/8/2N2N2/PPPP1PPP/R3K2R b KQkq e3 17 42", parsed, &error), "complete FEN parse");
    expect_true(parsed.to_fen() == "r3k2r/ppp2ppp/2n5/3pp3/8/2N2N2/PPPP1PPP/R3K2R b KQkq e3 17 42", "complete FEN roundtrip");
    expect_true(!Position::from_fen("invalid", parsed, &error), "short FEN rejected");
    expect_true(!Position::from_fen("8/8/8/8/8/8/8/9 w - - 0 1", parsed, &error), "wide FEN rank rejected");
    expect_true(!Position::from_fen("8/8/8/8/8/8/8/8 x - - 0 1", parsed, &error), "invalid FEN side rejected");
    expect_true(!Position::from_fen("8/8/8/8/8/8/8/4K3 w - - 0 1", parsed, &error), "FEN without black king rejected");
    expect_true(!Position::from_fen("4k3/8/8/8/8/8/4K3/7R w K - 0 1", parsed, &error), "FEN with displaced castling king rejected");
    expect_true(!Position::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 999999999999999999999 1", parsed, &error), "overflowing FEN clock rejected");
    expect_true(perft(start, 3) == 8902UL, "start-position perft depth 3");
    parsed = position_from_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    expect_true(perft(parsed, 2) == 2039UL, "Kiwipete perft depth 2");
}

void test_nonstandard_start_positions()
{
    Position position;
    Position original;
    std::string error;
    Undo undo;
    Move move;
    const char* odds = "4k3/8/8/8/8/8/8/4K2R w K - 7 12";
    expect_true(Position::from_fen(odds, position, &error),
                "orthodox odds FEN permits a missing queenside rook without Q right");
    expect_true(!Position::from_fen("4k3/8/8/8/8/8/8/4K2R w KQ - 7 12", position, &error),
                "orthodox odds FEN rejects a castling right without its rook");
    expect_true(Position::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1", position, &error),
                "orthodox FEN permits missing minor pieces");
    expect_true(Position::from_fen("4k3/8/8/8/8/8/8/4K3 b - - 13 42", position, &error) &&
                position.side_to_move() == BLACK && position.halfmove_clock() == 13 &&
                position.fullmove_number() == 42,
                "orthodox odds FEN preserves black to move and counters");
    expect_true(position.to_fen() == "4k3/8/8/8/8/8/8/4K3 b - - 13 42",
                "orthodox odds FEN roundtrips");
    original = position;
    expect_true(position.parse_san("Kd7", move) && position.make_move(move, undo),
                "legal move generation works from an odds position");
    position.unmake_move(undo);
    expect_true(position.equals(original), "odds-position make/unmake restores all state");
    {
        PgnDocument document;
        const char* pgn = "[SetUp \"1\"]\n[FEN \"4k3/8/8/8/8/8/8/4K2R b - - 3 9\"]\n"
                          "[White \"Odds\"]\n\n9... Kd7 *\n";
        expect_true(gambitree::PgnParser::parse(pgn, document) && document.games.size() == 1,
                    "PGN SetUp/FEN odds game parses");
        if (!document.games.empty()) {
            expect_true(document.games[0]->initial_position.side_to_move() == BLACK &&
                        document.games[0]->initial_position.fullmove_number() == 9,
                        "PGN odds FEN supplies the actual starting position");
            MoveNavigator navigator(*document.games[0]);
            expect_true(navigator.forward() && navigator.current()->san == "Kd7",
                        "navigation traverses an odds/FEN game");
        }
    }
}

bool load_variant_fen(const char* fen, Position& position, std::string* error = 0)
{
    return Position::from_fen(fen, position, error, gambitree::CHESS960);
}

void test_chess960_castling()
{
    Position position;
    Position original;
    std::string error;
    Undo undo;
    Move move;

    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/RK4R1 w KQ - 0 1", position, &error),
                "Chess960 X-FEN KQ finds outer rooks around a non-e-file king");
    expect_true(position.variant() == gambitree::CHESS960 &&
                position.to_fen() == "4k3/8/8/8/8/8/8/RK4R1 w AG - 0 1",
                "Chess960 X-FEN serializes canonically as Shredder-FEN file letters");
    original = position;
    expect_true(position.parse_san("O-O", move) && position.san(move) == "O-O" &&
                position.make_move(move, undo), "Chess960 kingside SAN castling parses and makes");
    expect_true(position.to_fen() == "4k3/8/8/8/8/8/8/R4RK1 b - - 1 1",
                "Chess960 kingside castle finishes king g1 and rook f1");
    position.unmake_move(undo);
    expect_true(position.equals(original), "Chess960 kingside castle undo restores homes and rights");
    expect_true(position.parse_san("O-O-O", move) && position.make_move(move, undo),
                "Chess960 queenside SAN castling parses and makes");
    expect_true(position.piece_at(Position::square_from_string("c1")) == gambitree::WHITE_KING &&
                position.piece_at(Position::square_from_string("d1")) == gambitree::WHITE_ROOK,
                "Chess960 queenside castle finishes king c1 and rook d1");
    position.unmake_move(undo);
    expect_true(position.equals(original), "Chess960 queenside castle undo is exact");

    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/6KR w H - 0 1", position, &error),
                "Shredder-FEN accepts explicit rook file");
    expect_true(position.parse_san("O-O", move) && position.make_move(move, undo) &&
                position.piece_at(Position::square_from_string("g1")) == gambitree::WHITE_KING &&
                position.piece_at(Position::square_from_string("f1")) == gambitree::WHITE_ROOK,
                "king already on its Chess960 destination remains in place");
    position.unmake_move(undo);
    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/4KR2 w F - 0 1", position, &error) &&
                position.parse_san("O-O", move) && position.make_move(move, undo) &&
                position.piece_at(Position::square_from_string("g1")) == gambitree::WHITE_KING &&
                position.piece_at(Position::square_from_string("f1")) == gambitree::WHITE_ROOK,
                "rook already on its Chess960 destination is not overwritten");
    position.unmake_move(undo);
    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/5KR1 w G - 0 1", position, &error) &&
                position.parse_san("O-O", move) && position.make_move(move, undo) &&
                position.piece_at(Position::square_from_string("g1")) == gambitree::WHITE_KING &&
                position.piece_at(Position::square_from_string("f1")) == gambitree::WHITE_ROOK,
                "adjacent king and rook cross without overwriting either");
    position.unmake_move(undo);
    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/1K1R4 w D - 0 1", position, &error) &&
                position.parse_san("O-O", move) && position.make_move(move, undo) &&
                position.piece_at(Position::square_from_string("g1")) == gambitree::WHITE_KING &&
                position.piece_at(Position::square_from_string("f1")) == gambitree::WHITE_ROOK,
                "king may pass through the registered rook's start square");
    position.unmake_move(undo);
    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/5K1R w H - 0 1", position, &error) &&
                position.parse_san("O-O", move) && position.make_move(move, undo) &&
                position.piece_at(Position::square_from_string("g1")) == gambitree::WHITE_KING &&
                position.piece_at(Position::square_from_string("f1")) == gambitree::WHITE_ROOK,
                "rook may finish on the king's vacated starting square");
    position.unmake_move(undo);

    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/RK4R1 w KQ - 0 1", position, &error),
                "X-FEN fixture reloads for rights tests");
    expect_true(position.parse_san("Kb2", move) && position.make_move(move, undo) &&
                position.castling_rights() == 0,
                "moving the Chess960 king loses both castling rights");
    position.unmake_move(undo);
    expect_true(position.parse_san("Rg2", move) && position.make_move(move, undo) &&
                position.castling_rights() == gambitree::WHITE_QUEENSIDE,
                "moving the registered kingside rook loses only its right");

    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/RK4R1 w KQ - 0 1", position, &error) &&
                position.parse_san("Ra2", move) && position.make_move(move, undo) &&
                position.castling_rights() == gambitree::WHITE_KINGSIDE,
                "moving the queenside rook preserves the unrelated kingside right");

    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/1K1N3R w H - 0 1", position, &error) &&
                !position.parse_san("O-O", move),
                "a piece on the king route blocks Chess960 castling");
    expect_true(load_variant_fen("1r5k/8/8/8/8/8/8/1K5R w H - 0 1", position, &error) &&
                !position.parse_san("O-O", move),
                "Chess960 castling is forbidden while the king is in check");
    expect_true(load_variant_fen("5r1k/8/8/8/8/8/8/1K5R w H - 0 1", position, &error) &&
                !position.parse_san("O-O", move),
                "Chess960 king cannot cross an attacked transit square");
    expect_true(load_variant_fen("6rk/8/8/8/8/8/8/1K5R w H - 0 1", position, &error) &&
                !position.parse_san("O-O", move),
                "Chess960 king cannot finish on an attacked destination");

    expect_true(load_variant_fen("6kr/8/8/8/8/8/8/K7 b k - 0 1", position, &error) &&
                position.parse_san("O-O", move) && position.make_move(move, undo) &&
                position.piece_at(Position::square_from_string("g8")) == gambitree::BLACK_KING &&
                position.piece_at(Position::square_from_string("f8")) == gambitree::BLACK_ROOK,
                "black Chess960 kingside castling uses the same final squares");
    position.unmake_move(undo);
    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/2K4R b H - 0 1", position, &error) &&
                position.side_to_move() == BLACK,
                "Chess960 FEN supports black to move");

    expect_true(load_variant_fen("4k3/8/8/8/8/8/8/2K4R w H - 0 1", position, &error),
                "Chess960 position reloads for UCI conversion");
    {
        Move uci_castle;
        expect_true(position.move_from_uci(Position::square_from_string("c1"),
                                           Position::square_from_string("h1"),
                                           gambitree::NO_PIECE, uci_castle),
                    "Chess960 UCI castling coordinate targets the registered rook");
        expect_true((uci_castle.flags & gambitree::MOVE_CASTLE_KINGSIDE) != 0 &&
                    position.san(uci_castle) == "O-O",
                    "Chess960 UCI rook-target move maps to the canonical SAN castle");
    }
    {
        Position explicit_files;
        expect_true(load_variant_fen("4k3/8/8/8/8/8/8/RK4R1 w AG - 0 1", explicit_files, &error) &&
                    explicit_files.to_fen() == "4k3/8/8/8/8/8/8/RK4R1 w AG - 0 1",
                    "Shredder-FEN explicit rook files parse and roundtrip");
        expect_true(!load_variant_fen("4k3/8/8/8/8/8/8/RK4R1 w AAG - 0 1", explicit_files, &error),
                    "duplicate Chess960 castling rights are rejected");
        expect_true(!load_variant_fen("4k3/8/8/8/8/8/8/RK4R1 w B - 0 1", explicit_files, &error),
                    "Chess960 rook file cannot name the king square");
        expect_true(!Position::from_fen("4k3/8/8/8/8/8/8/RK4R1 w KQ - 0 1", explicit_files, &error),
                    "Chess960 X-FEN is not implicitly enabled for standard FEN");
    }
    {
        PgnDocument document;
        expect_true(gambitree::PgnParser::parse(
                    "[Variant \"cHeSs 960\"]\n[SetUp \"1\"]\n"
                    "[FEN \"4k3/8/8/8/8/8/8/RK4R1 w KQ - 0 1\"]\n\n1. O-O *\n",
                    document) && document.errors.empty() && document.games.size() == 1 &&
                    document.games[0]->initial_position.variant() == gambitree::CHESS960,
                    "PGN Variant selects Chess960 before interpreting FEN and SAN");
        expect_true(gambitree::PgnParser::parse(
                    "[Variant \"Fischer Random\"]\n[SetUp \"1\"]\n"
                    "[FEN \"4k3/8/8/8/8/8/8/RK4R1 w KQ - 0 1\"]\n\n1. O-O-O *\n",
                    document) && document.errors.empty() && document.games.size() == 1,
                    "Fischer Random alias is recognized case-insensitively");
        expect_true(gambitree::PgnParser::parse(
                    "[Variant \"Fischerandom\"]\n[SetUp \"1\"]\n"
                    "[FEN \"4k3/8/8/8/8/8/8/RK4R1 w KQ - 0 1\"]\n\n"
                    "1. O-O {main castle} (1. O-O-O $1 {alternate castle}) *\n",
                    document) && document.errors.empty() && document.games.size() == 1 &&
                    document.games[0]->root->children.size() == 2 &&
                    document.games[0]->root->children[0]->comments.size() == 1 &&
                    document.games[0]->root->children[1]->nags.size() == 1,
                    "Chess960 PGN retains SAN castling, comments, and RAV/NAG structure");
        expect_true(!gambitree::PgnParser::parse("[Variant \"Unknown Variant\"]\n\n1. e4 *\n", document) &&
                    !document.errors.empty() &&
                    document.errors[0].message.find("unsupported PGN Variant") != std::string::npos,
                    "unknown PGN variants fail clearly rather than becoming Chess960");
    }
    expect_true(load_variant_fen("5k2/8/8/8/8/8/8/1K5R w H - 0 1", position, &error) &&
                position.parse_san("O-O+", move) && position.san(move) == "O-O+",
                "Chess960 SAN retains the check suffix after castling");
    expect_true(load_variant_fen("4k3/8/8/8/8/8/7r/1K5R b H - 0 1", position, &error) &&
                position.parse_san("Rxh1+", move) && position.make_move(move, undo) &&
                position.castling_rights() == 0,
                "capturing the registered rook removes its Chess960 castling right");
}

void test_real_chess960_pgn()
{
    std::ifstream input("tests/data/freestyle_shorts.pgn");
    std::string pgn((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    PgnDocument document;
    std::string error;
    PgnLibrary library;
    std::vector<std::string> paths;
    std::vector<std::string> errors;
    std::set<std::string> starts;
    std::size_t index;
    expect_true(input.good() || input.eof(), "real Chess960 integration PGN is readable");
    expect_true(gambitree::PgnParser::parse(pgn, document) && document.errors.empty() &&
                document.games.size() == 14,
                "all fourteen games in freestyle_shorts.pgn parse completely");
    for (index = 0; index < document.games.size(); ++index) {
        const std::string* fen = document.games[index]->tag_value("FEN");
        expect_true(document.games[index]->initial_position.variant() == gambitree::CHESS960,
                    "freestyle_shorts.pgn game starts under Chess960 rules");
        expect_true(document.games[index]->tag_value("Variant") != 0 &&
                    *document.games[index]->tag_value("Variant") == "Chess960",
                    "Chess960 tag remains attached to parsed game");
        expect_true(fen != 0 && fen->find(" KQkq ") != std::string::npos,
                    "freestyle_shorts.pgn uses X-FEN symbolic KQkq castling rights");
        if (fen) starts.insert(*fen);
        expect_true(!document.games[index]->root->children.empty(),
                    "every real Chess960 game has a parsed movetext start");
        MoveNavigator navigator(*document.games[index]);
        expect_true(navigator.position().variant() == gambitree::CHESS960,
                    "navigation root retains the Chess960 starting position");
        while (navigator.forward()) { }
    }
    expect_true(starts.size() > 1, "freestyle_shorts.pgn contains multiple distinct Chess960 start positions");
    paths.push_back("tests/data/freestyle_shorts.pgn");
    expect_true(library.load(paths, errors), "real freestyle_shorts.pgn loads into lazy PGN library");
    while (!library.indexing_complete()) library.step_indexing(100, errors);
    expect_true(library.game_count() == 14, "lazy index finds all fourteen real Chess960 games");
    for (index = 0; index < library.game_count(); ++index) {
        if (index != library.game_index()) expect_true(library.next_game(), "lazy Chess960 navigation selects next game");
        expect_true(library.materialize_current(error), "lazy Chess960 game materializes");
        if (library.current_game())
            expect_true(library.current_game()->initial_position.variant() == gambitree::CHESS960,
                        "lazy materialization preserves Chess960 rules");
    }
    expect_true(errors.empty(), "real Chess960 PGN indexes without warnings");
}

void test_moves_and_undo()
{
    Position position = Position::standard();
    Position original = position;
    Undo undo;
    expect_true(play_san(position, "e4", &undo), "pawn double move");
    expect_true(position.en_passant_square() == Position::square_from_string("e3"), "double move sets en-passant square");
    expect_true(position.side_to_move() == gambitree::BLACK, "move changes turn");
    position.unmake_move(undo);
    expect_true(position.equals(original), "undo restores every start-position field");
    expect_true(play_san(position, "Nf3", &undo), "knight move");
    position.unmake_move(undo);
    expect_true(position.equals(original), "undo after knight move");
    {
        Move illegal(Position::square_from_string("e2"), Position::square_from_string("e5"), gambitree::NO_PIECE, gambitree::MOVE_NORMAL);
        expect_true(!position.make_move(illegal, undo), "illegal structural move rejected");
        expect_true(position.equals(original), "rejected move preserves position");
    }
    position = position_from_fen("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1");
    expect_true(play_san(position, "exd5", &undo), "pawn capture");
    position.unmake_move(undo);
    expect_true(position.to_fen() == "4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "undo capture");
    position = position_from_fen("4k3/8/8/8/8/8/3Q4/4K3 w - - 0 1");
    expect_true(play_san(position, "Qd7+", &undo), "queen move and check");
    position = position_from_fen("4k3/8/8/8/8/8/4B3/4K3 w - - 0 1");
    expect_true(play_san(position, "Bb5+", &undo), "bishop move");
    position = position_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    expect_true(play_san(position, "Kd2", &undo), "king move");
}

void test_special_moves()
{
    Position position = position_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    Undo undo;
    expect_true(play_san(position, "O-O", &undo), "white kingside castling");
    expect_true(position.to_fen() == "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1", "kingside castling state");
    position.unmake_move(undo);
    expect_true(play_san(position, "O-O-O", &undo), "white queenside castling");
    position.unmake_move(undo);
    expect_true(position.to_fen() == "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "undo castling");
    position = position_from_fen("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1");
    expect_true(play_san(position, "O-O", &undo), "black kingside castling");
    expect_true(position.to_fen() == "r4rk1/8/8/8/8/8/8/R3K2R w KQ - 1 2", "black kingside castling state");
    position.unmake_move(undo);
    position = position_from_fen("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1");
    expect_true(play_san(position, "O-O-O", &undo), "black queenside castling");
    position.unmake_move(undo);
    position = position_from_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 5 8");
    expect_true(play_san(position, "exd6", &undo), "en-passant capture");
    expect_true(position.piece_at(Position::square_from_string("d5")) == gambitree::EMPTY, "en-passant removes captured pawn");
    position.unmake_move(undo);
    expect_true(position.to_fen() == "4k3/8/8/3pP3/8/8/8/4K3 w - d6 5 8", "undo en-passant restores state");
    position = position_from_fen("4k3/8/8/8/3Pp3/8/8/4K3 b - d3 5 8");
    expect_true(play_san(position, "exd3", &undo), "black en-passant capture");
    position.unmake_move(undo);
    expect_true(position.to_fen() == "4k3/8/8/8/3Pp3/8/8/4K3 b - d3 5 8", "undo black en-passant restores state");
    position = position_from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    expect_true(play_san(position, "a8=Q+", &undo), "queen promotion with check");
    position.unmake_move(undo);
    expect_true(position.to_fen() == "4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "undo queen promotion");
    expect_true(play_san(position, "a8=R+", &undo), "rook promotion");
    position.unmake_move(undo);
    expect_true(play_san(position, "a8=B", &undo), "bishop promotion");
    position.unmake_move(undo);
    expect_true(play_san(position, "a8=N", &undo), "knight promotion");
    position = position_from_fen("4k2r/6P1/8/8/8/8/8/4K3 w - - 0 1");
    expect_true(play_san(position, "gxh8=Q+", &undo), "capturing promotion");
    position.unmake_move(undo);
    expect_true(position.to_fen() == "4k2r/6P1/8/8/8/8/8/4K3 w - - 0 1", "undo capturing promotion");
}

void test_legality_and_status()
{
    Position position = position_from_fen("4r1k1/8/8/8/8/8/4R3/4K3 w - - 0 1");
    Move move;
    expect_true(!position.parse_san("Rd2", move), "pinned rook cannot expose king");
    expect_true(position.parse_san("Rxe8+", move), "pinned rook may capture checking line piece");
    position = position_from_fen("8/8/8/KPp4r/8/8/8/4k3 w - c6 0 1");
    expect_true(!position.parse_san("bxc6", move), "en-passant that exposes own king is illegal");
    position = position_from_fen("3rk2r/8/8/8/8/8/8/R3K3 w Qk - 0 1");
    expect_true(!position.parse_san("O-O-O", move), "castling through attacked square is illegal");
    position = position_from_fen("4r1k1/8/8/8/8/8/8/R3K2R w KQ - 0 1");
    expect_true(!position.parse_san("O-O", move), "castling out of check is illegal");
    position = position_from_fen("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
    expect_true(position.in_check(gambitree::BLACK), "check detection");
    expect_true(position.is_checkmate(), "checkmate detection");
    position = position_from_fen("7k/5Q2/7K/8/8/8/8/8 b - - 0 1");
    expect_true(position.is_stalemate(), "stalemate detection");
    position = position_from_fen("4k3/8/8/8/8/8/4B3/4R1K1 w - - 0 1");
    expect_true(play_san(position, "Bd3+", 0), "discovered line check generation");
    position = position_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    expect_true(position.piece_at(Position::square_from_string("e1")) == gambitree::WHITE_KING, "king representation");
}

void test_san()
{
    Position position = position_from_fen("4k3/8/8/8/8/2N1N3/8/4K3 w - - 0 1");
    Move move;
    Undo undo;
    expect_true(position.parse_san("Ncd5", move), "SAN file disambiguation parsing");
    expect_true(position.san(move) == "Ncd5", "SAN file disambiguation generation");
    position = position_from_fen("4k3/8/8/8/8/R7/8/R3K3 w - - 0 1");
    expect_true(position.parse_san("R1a2", move), "SAN rank disambiguation parsing");
    expect_true(position.san(move) == "R1a2", "SAN rank disambiguation generation");
    position = Position::standard();
    expect_true(!position.parse_san("Qz7", move), "invalid SAN rejected");
    expect_true(!position.parse_san("e5", move), "illegal SAN rejected");
    expect_true(position.parse_san("e4+", move) && position.san(move) == "e4",
                "incorrect check annotation does not invalidate a legal quiet move");
    position = position_from_fen("r2k3r/ppp2p2/2p4p/5b2/2P2P2/2P5/P5PP/R3KB1R w KQ - 0 14");
    expect_true(position.parse_san("O-O-O", move) && position.san(move) == "O-O-O+",
                "a legal ChessBase long castle may omit its terminal check marker");
    expect_true(position.parse_san("O-O-O#", move) && position.san(move) == "O-O-O+",
                "an incorrect mate annotation does not invalidate a legal checking castle");
    position = position_from_fen("r2k3r/pp3pbp/2p2np1/4pb2/2P5/BPN1P3/P3BPPP/R3K2R w KQ - 2 12");
    expect_true(position.parse_san("O-O-O", move) && position.san(move) == "O-O-O+",
                "the second reported Cifuentes long castle accepts the omitted plus");
    expect_true(position.parse_san("O-O-O#", move) && position.san(move) == "O-O-O+",
                "the second reported position also tolerates a false mate annotation");
    expect_true(position.parse_san("O-O-O", move) && position.make_move(move, undo) &&
                position.to_fen() ==
                    "r2k3r/pp3pbp/2p2np1/4pb2/2P5/BPN1P3/P3BPPP/2KR3R b - - 3 12",
                "the second omitted-suffix castle reaches its exact reference position");
    position.unmake_move(undo);
    {
        const std::string omitted_plus_rav =
            "[SetUp \"1\"]\n[FEN \"r2k3r/ppp2p2/2p4p/5b2/2P2P2/2P5/P5PP/R3KB1R w KQ - 0 14\"]\n\n"
            "14. O-O-O (14. Rb1 (14. Rc1)) (14. Rc1) *";
        PgnDocument castle_document;
        expect_true(gambitree::PgnParser::parse(omitted_plus_rav, castle_document) &&
                    castle_document.games.size() == 1 &&
                    castle_document.warnings.empty() &&
                    castle_document.games[0]->root->children.size() == 4 &&
                    castle_document.games[0]->root->children[0]->san == "O-O-O" &&
                    castle_document.games[0]->root->children[0]->position_after.to_fen() ==
                        "r2k3r/ppp2p2/2p4p/5b2/2P2P2/2P5/P5PP/2KR1B1R b - - 1 14",
                    "source-spelled long castle with omitted check marker preserves sibling and nested RAV structure");
    }
    position = position_from_fen("rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2");
    expect_true(position.parse_san("Qh4#", move), "SAN checkmate parsing");
    expect_true(position.san(move) == "Qh4#", "SAN checkmate generation");
    expect_true(position.parse_san("Qh4", move),
                "a legal mating move may omit its terminal mate marker in source SAN");
    expect_true(position.parse_san("Qh4+", move) && position.san(move) == "Qh4#",
                "a check annotation is tolerated when the legal move is checkmate");
    position = position_from_fen("4k3/8/8/8/8/8/4R3/K7 w - - 0 1");
    expect_true(!position.parse_san("Rxe8#", move), "SAN cannot capture the opposing king");

    position = position_from_fen("4k3/7b/8/8/2N5/3N4/2K5/8 w - - 0 1");
    expect_true(position.parse_san("Nb2", move) &&
                move.from == Position::square_from_string("c4") &&
                position.san(move) == "Nb2",
                "canonical SAN selects the only legal knight when the geometric rival is pinned");
    expect_true(position.parse_san("Ncb2", move) &&
                move.from == Position::square_from_string("c4"),
                "redundant departure file is accepted only for the matching legal piece");
    expect_true(position.parse_san("N4b2", move) &&
                move.from == Position::square_from_string("c4"),
                "redundant departure rank is accepted only for the matching legal piece");
    expect_true(position.parse_san("Nc4b2", move) &&
                move.from == Position::square_from_string("c4"),
                "full redundant departure square is accepted for the legal move");
    expect_true(!position.parse_san("Ndb2", move) &&
                !position.parse_san("Nab2", move),
                "a pinned rival and a nonexistent origin cannot be used to fabricate a move");
    expect_true(position.parse_san("Ncb2", move) && position.make_move(move, undo) &&
                position.to_fen() == "4k3/7b/8/8/8/3N4/1NK5/8 b - - 1 1",
                "the selected legal origin reaches the expected post-move position");

    position = position_from_fen("6k1/8/8/8/8/2N1N3/8/6K1 w - - 0 1");
    expect_true(position.parse_san("Ncd5", move) &&
                move.from == Position::square_from_string("c3"),
                "required file disambiguation remains valid when two candidates are legal");
    expect_true(position.parse_san("Nc3d5", move) &&
                move.from == Position::square_from_string("c3"),
                "full origin remains valid when canonical SAN itself needs disambiguation");
    expect_true(!position.parse_san("N4d5", move),
                "an insufficient rank hint that leaves two legal candidates is rejected");

    position = position_from_fen("8/7n/6KP/8/k3nR2/8/8/8 b - - 7 84");
    expect_true(position.parse_san("Nhg5", move) &&
                move.from == Position::square_from_string("h7") &&
                position.san(move) == "Ng5",
                "redundant origin is accepted when the geometric rival knight is absolutely pinned");
    expect_true(position.parse_san("Nhg5", move) && position.make_move(move, undo) &&
                position.to_fen() == "8/8/6KP/6n1/k3nR2/8/8/8 w - - 8 85",
                "the additional pinned-knight case produces its expected FEN");

    const std::string pinned_rav =
        "[SetUp \"1\"]\n[FEN \"4k3/7b/8/8/2N5/3N4/2K5/8 w - - 0 1\"]\n\n"
        "1. Ncb2 (1. Nc4b2 (1. N4b2)) *";
    PgnDocument pinned_document;
    expect_true(gambitree::PgnParser::parse(pinned_rav, pinned_document) &&
                pinned_document.games.size() == 1,
                "redundantly specified legal SAN parses in nested RAVs");
    if (pinned_document.games.size() == 1) {
        MoveNode* root = pinned_document.games[0]->root;
        MoveNode* main_move = root->children.size() > 0 ? root->children[0] : 0;
        MoveNode* rav_move = root->children.size() > 1 ? root->children[1] : 0;
        MoveNode* nested_move = root->children.size() > 2 ? root->children[2] : 0;
        expect_true(main_move && main_move->san == "Ncb2" &&
                    main_move->move.from == Position::square_from_string("c4") &&
                    main_move->position_after.to_fen() ==
                        "4k3/7b/8/8/8/3N4/1NK5/8 b - - 1 1",
                    "mainline retains source SAN and the legal move position");
        expect_true(root->children.size() == 3 && rav_move && nested_move &&
                    rav_move->move.from == Position::square_from_string("c4") &&
                    nested_move->move.from == Position::square_from_string("c4") &&
                    nested_move->position_after.to_fen() ==
                        "4k3/7b/8/8/8/3N4/1NK5/8 b - - 1 1" &&
                    pinned_document.games[0]->initial_position.to_fen() ==
                        "4k3/7b/8/8/2N5/3N4/2K5/8 w - - 0 1",
                    "nested variation restores its parent position and keeps the same legal origin");
    }
}

MoveNode* child(MoveNode* node, std::size_t index, const char* description);


void test_missing_pawn_capture_file()
{
    using namespace gambitree;
    const char* before = "1r1q3r/p2b1pk1/1p1p1np1/n1pP2Bp/1RPN1Q2/2P3P1/P4PBP/5RK1 w - c6 0 22";
    const char* after = "1r1q3r/p2b1pk1/1pPp1np1/n5Bp/1RPN1Q2/2P3P1/P4PBP/5RK1 b - - 0 22";
    const char* final = "8/p1r3k1/1p1p4/3B1R2/2P5/2P5/r6P/1R4K1 b - - 0 39";
    struct Case { const char* fen; const char* supplied; const char* canonical; };
    const Case cases[] = {
        { before, "xc6", "dxc6" }, { before, "xc6+", "dxc6" },
        { before, "xc6#", "dxc6" }, { before, "dxc6", "dxc6" },
        { "4k3/8/8/4p3/3P4/8/8/4K3 w - - 0 1", "xe5", "dxe5" },
        { "4k3/8/8/4p3/3P4/8/8/4K3 w - - 0 1", "xe5#", "dxe5" },
        { "4k3/8/8/4p3/5P2/8/8/4K3 b - - 0 1", "xf4", "exf4" },
        { "4k3/8/8/4p3/3P1P2/8/8/4K3 w - - 0 1", "xe5", 0 },
        { "4k3/8/8/4p3/3P4/5N2/8/4K3 w - - 0 1", "xe5", 0 },
        { "k2r4/8/8/4p3/3P1P2/8/8/3K4 w - - 0 1", "xe5", "fxe5" },
        { "4k3/8/2p5/8/1N6/8/8/4K3 w - - 0 1", "xc6", 0 },
        { "1r1q3r/p2b1pk1/1p1p1np1/n1pP2Bp/1RPN1Q2/2P3P1/P4PBP/5RK1 w - - 0 22", "xc6", 0 },
        { "8/8/8/KPp4r/8/8/8/4k3 w - c6 0 1", "xc6", 0 },
        { "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1", "xb8=Q#", "axb8=Q+" },
        { "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1", "xb8", 0 },
        { "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1", "xb8=K", 0 },
        { before, "xc6++", 0 }, { before, "xxc6", 0 }, { before, "c6", 0 }
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Position position;
        expect_true(Position::from_fen(cases[i].fen, position, 0), "missing-file test has a valid initial FEN");
        const std::string unchanged = position.to_fen();
        Move move;
        const bool accepted = position.parse_san(cases[i].supplied, move);
        expect_true(accepted == (cases[i].canonical != 0) && position.to_fen() == unchanged,
              "a missing pawn file requires exactly one legal capture, including competing piece captures");
        if (accepted) {
            expect_true(position.san(move) == cases[i].canonical && (move.flags & MOVE_CAPTURE) &&
                  (position.piece_at(move.from) == WHITE_PAWN || position.piece_at(move.from) == BLACK_PAWN),
                  "the unique candidate is a pawn capture with unchanged canonical SAN and promotion rules");
            Undo undo;
            expect_true(position.make_move(move, undo), "the recognized pawn capture is executable");
            if (i < 4)
                expect_true((move.flags & MOVE_EN_PASSANT) && move.from == Position::square_from_string("d5") &&
                      move.to == Position::square_from_string("c6") && position.to_fen() == after &&
                      position.piece_at(Position::square_from_string("d5")) == EMPTY &&
                      position.piece_at(Position::square_from_string("c5")) == EMPTY &&
                      position.piece_at(Position::square_from_string("c6")) == WHITE_PAWN,
                      "en passant moves d5 to c6, removes c5 and reaches the exact reference FEN");
            position.unmake_move(undo);
            expect_true(position.to_fen() == unchanged, "undo restores the complete pre-capture position");
        }

    }
    std::ifstream source("tests/data/lima-souza-2024-missing-pawn-file.pgn", std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::string corrected = original;
    const std::size_t replacement = corrected.find("22. xc6");
    expect_true(!original.empty() && replacement != std::string::npos, "load the untouched reported regression game");
    if (replacement == std::string::npos) return;
    corrected.replace(replacement, 7, "22. dxc6");
    PgnDocument raw, canonical;
    expect_true(PgnParser::parse(original, raw) && raw.games.size() == 1 && raw.errors.empty(),
          "the entire reported game parses without an invalid SAN error");
    expect_true(PgnParser::parse(corrected, canonical) && canonical.games.size() == 1 && canonical.errors.empty(),
          "a copy changing only xc6 to dxc6 also parses");
    if (raw.games.size() != 1 || canonical.games.size() != 1) return;
    std::ifstream references("tests/data/lima-souza-2024-fens.txt");
    const MoveNode* a = raw.games[0]->root;
    const MoveNode* b = canonical.games[0]->root;
    std::string fen;
    int plies = 0;
    while (std::getline(references, fen)) {
        if (fen.empty() || fen[0] == '#') continue;
        if (a->children.empty() || b->children.empty()) { expect_true(false, "both games retain every reference ply"); break; }
        a = a->children[0]; b = b->children[0]; ++plies;
        expect_true(a->position_after.to_fen() == fen && b->position_after.to_fen() == fen &&
              a->move.from == b->move.from && a->move.to == b->move.to &&
              a->move.flags == b->move.flags && a->move.promotion == b->move.promotion,
              "original and corrected games have identical moves and independent python-chess FENs at every ply");
        if (plies == 43)
            expect_true(a->san == "xc6" && b->san == "dxc6" && (a->move.flags & MOVE_EN_PASSANT) &&
                  a->parent->position_after.to_fen() == before && a->position_after.to_fen() == after,
                  "the original en-passant spelling is preserved independently of its internal move");
    }
    expect_true(plies == 77 && a->children.empty() && b->children.empty() &&
          a->position_after.to_fen() == final && b->position_after.to_fen() == final,
          "both complete games have exactly 77 legal plies and the independently verified final FEN");

    const std::string setup = std::string("[SetUp \"1\"]\n[FEN \"") + before + "\"]\n\n";
    PgnDocument rav;
    expect_true(PgnParser::parse(setup + "22. dxc6 (22. xc6 Bxc6 23. Nxc6 Nxc6) Bxc6 23. Nxc6 Nxc6 *\n\n"
                          "[Event \"Following\"]\n\n1. e4 e5 *\n", rav) &&
          rav.games.size() == 2 && rav.errors.empty() && rav.games[0]->root->children.size() == 2,
          "an omitted pawn file inside a RAV preserves its mainline and the following game");
    if (rav.games.size() == 2 && rav.games[0]->root->children.size() == 2) {
        for (int branch = 0; branch < 2; ++branch) {
            const MoveNode* node = rav.games[0]->root->children[branch];
            expect_true(node->parent == rav.games[0]->root && node->san == (branch ? "xc6" : "dxc6") &&
                  node->position_after.to_fen() == after && (node->move.flags & MOVE_EN_PASSANT),
                  "both alternative captures retain their source SAN, parent and en-passant state");
            int count = 1;
            while (!node->children.empty()) { node = node->children[0]; ++count; }
            expect_true(count == 4 && node->position_after.to_fen() == "1r1q3r/p4pk1/1pnp1np1/6Bp/1RP2Q2/2P3P1/P4PBP/5RK1 w - - 0 24",
                  "mainline and RAV resume independently through the same four legal plies");
        }
    }
    PgnLibrary library;
    std::vector<std::string> paths(1, "tests/data/lima-souza-2024-missing-pawn-file.pgn"), errors;
    std::string error;
    expect_true(library.load(paths, errors), "the reported game opens through the viewer library");
    while (!library.indexing_complete()) library.step_indexing(100, errors);
    expect_true(library.materialize_current(error) && library.current_game()->parse_status == PGN_COMPLETE,
          "the viewer now loads the complete game instead of its former 42-ply prefix");
    if (!library.current_game()) return;
    const MoveNode* ep = library.current_game()->root;
    for (int i = 0; i < 43 && !ep->children.empty(); ++i) ep = ep->children[0];
    MoveNavigator navigation(*library.current_game());
    expect_true(navigation.select(ep) && ep->san == "xc6" && navigation.position().to_fen() == after &&
          navigation.back() && navigation.position().to_fen() == before &&
          navigation.forward() && navigation.current() == ep,
          "viewer selection and navigation retain the source-spelled en-passant move and exact positions");
    navigation.to_end();
    expect_true(navigation.position().to_fen() == final, "viewer navigation reaches the full 77-ply final position");
    for (int figurines = 0; figurines < 2; ++figurines) {
        NotationLayout layout;
        layout.set_figurines(figurines != 0);
        layout.build(*library.current_game(), 320, NotationFontMetrics());
        bool displayed = false;
        for (std::size_t i = 0; i < layout.items().size(); ++i) {
            const NotationItem& item = layout.items()[i];
            if (item.kind == NOTATION_MOVE && item.node == ep)
                displayed = item.text == "xc6" && layout.hit_test(item.x + item.width / 2, item.y + 1, 0) == ep &&
                    !layout.selection_rects(ep).empty();
        }
        expect_true(displayed, "the original pawn capture remains visible and selectable with and without figurines");
    }
}

void test_lan()
{
    using namespace gambitree;
    std::ifstream lan_file("tests/data/spice-cup-2025-lan.pgn", std::ios::binary);
    std::ifstream san_file("tests/data/spice-cup-2025-san-fens.pgn", std::ios::binary);
    const std::string lan_text((std::istreambuf_iterator<char>(lan_file)), std::istreambuf_iterator<char>());
    const std::string san_text((std::istreambuf_iterator<char>(san_file)), std::istreambuf_iterator<char>());
    PgnDocument lan, san;
    expect_true(!lan_text.empty() && PgnParser::parse(lan_text, lan) && lan.games.size() == 2 &&
          lan.errors.empty(), "both complete reported LAN games parse without errors");
    expect_true(!san_text.empty() && PgnParser::parse(san_text, san) && san.games.size() == 2 &&
          san.errors.empty(), "independent canonical SAN/FEN references parse");
    const int counts[] = {25, 26};
    const char* finals[] = {
        "rnbqr1k1/5pbp/3p1np1/p1pP4/Pp2P3/1P1B1N2/1B1N1PPP/R2Q1RK1 b - - 2 13",
        "r4rk1/2pqbppp/2npbn2/1p1Np3/4P3/1BPP1N2/1P3PPP/R1BQR1K1 w - - 3 14"
    };
    if (lan.games.size() == 2 && san.games.size() == 2) for (std::size_t g = 0; g < 2; ++g) {
        const MoveNode* l = lan.games[g]->root;
        const MoveNode* s = san.games[g]->root;
        int plies = 0;
        while (!l->children.empty() && !s->children.empty()) {
            l = l->children[0]; s = s->children[0]; ++plies;
            expect_true(s->comments.size() == 1, "every SAN reference move has a fixed independent FEN comment");
            std::istringstream words(s->comments.size() == 1 ? s->comments[0] : "");
            std::string expected, word;
            while (words >> word) { if (!expected.empty()) expected += ' '; expected += word; }
            expect_true(l->position_after.to_fen() == expected && s->position_after.to_fen() == expected &&
                  l->move.from == s->move.from && l->move.to == s->move.to &&
                  l->move.flags == s->move.flags && l->move.promotion == s->move.promotion,
                  "LAN and SAN agree with the independent fixed FEN and exact move after every ply");
            expect_true((l->parent->has_move ? l->parent->position_after : lan.games[g]->initial_position).san(l->move) == s->san,
                  "LAN acceptance leaves conventional SAN generation unchanged");
            if (plies == 1) expect_true(l->san == (g ? "e2e4" : "d2d4"),
                                  "the original LAN source notation is retained");
        }
        expect_true(plies == counts[g] && l->children.empty() && s->children.empty() &&
              l->position_after.to_fen() == finals[g] && lan.games[g]->result == san.games[g]->result,
              "reported games have exactly 25/26 plies, independent final FENs and unchanged results");
        PgnLibrary library;
        std::vector<std::string> paths(1, "tests/data/spice-cup-2025-lan.pgn"), errors;
        std::string error;
        expect_true(library.load(paths, errors), "LAN fixture opens through the indexed viewer library");
        while (!library.indexing_complete()) library.step_indexing(100, errors);
        const bool loaded = library.select_game(g) && library.materialize_current(error);
        expect_true(loaded && library.game_count() == 2,
                    "both LAN games materialize through the actual lazy viewer load path");
        if (loaded) {
            MoveNavigator cursor(*library.current_game()); cursor.to_end();
            expect_true(cursor.position().to_fen() == finals[g], "indexed LAN replay retains the exact final FEN");
        }
        MoveNavigator navigator(*lan.games[g]);
        navigator.to_end();
        expect_true(navigator.current() == l && navigator.position().to_fen() == finals[g],
              "viewer navigation reaches the exact original LAN end node and FEN");
        navigator.to_begin();
        int navigated = 0;
        while (navigator.forward()) ++navigated;
        expect_true(navigated == counts[g] && navigator.current() == l,
              "viewer can replay every LAN move including captures and castling");
        for (int figurines = 0; figurines < 2; ++figurines) {
            NotationLayout layout;
            layout.set_figurines(figurines != 0);
            layout.build(*lan.games[g], 300, NotationFontMetrics());
            bool first = false;
            for (std::size_t i = 0; i < layout.items().size(); ++i) {
                const NotationItem& item = layout.items()[i];
                if (item.kind != NOTATION_MOVE || item.node != lan.games[g]->root->children[0]) continue;
                first = item.text == (g ? "e2e4" : "d2d4") &&
                    layout.hit_test(item.x + item.width / 2, item.y + 1, 0) == item.node;
            }
            expect_true(first, "LAN remains visible verbatim with correct hitboxes in letter and figurine modes");
        }
    }

    struct Case { const char* fen; const char* text; const char* canonical; };
    const Case cases[] = {
        {0, "d2d4", "d4"}, {0, "Pd2d4", "d4"}, {0, "d2-d4", "d4"},
        {0, "Ng1f3", "Nf3"}, {0, "g1f3", "Nf3"}, {0, "Ng1-f3#", "Nf3"},
        {0, "e2e4+", "e4"}, {0, "Nb1c3", "Nc3"},
        {"4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4xd5", "exd5"},
        {"4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "Pe4xd5#", "exd5"},
        {"4k3/8/8/3p4/8/2N5/8/4K3 w - - 0 1", "Nc3xd5", "Nxd5"},
        {"4k3/8/8/3p4/8/2N5/8/4K3 w - - 0 1", "c3xd5+", "Nxd5"},
        {"7k/1P6/8/8/8/8/8/7K w - - 0 1", "b7b8=Q#", "b8=Q+"},
        {"7k/1P6/8/8/8/8/8/7K w - - 0 1", "Pb7-b8=N+", "b8=N"},
        {"k6r/6P1/8/8/8/8/8/7K w - - 0 1", "g7xh8=Q", "gxh8=Q+"},
        {"k6r/6P1/8/8/8/8/8/7K w - - 0 1", "Pg7xh8=N#", "gxh8=N"},
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 10", "e5xd6+", "exd6"},
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "O-O", "O-O"},
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "Ke1g1", "O-O"},
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1c1", "O-O-O"},
        {0, "d2d5", 0}, {0, "d2xd4", 0}, {0, "e3e4", 0},
        {0, "Ne2e4", 0}, {0, "Bd2d4", 0}, {0, "Ng8f6", 0},
        {0, "Bc1h6", 0}, {0, "b1d2", 0}, {0, "Ke1g1", 0},
        {"4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", 0},
        {"4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4-d5", 0},
        {"k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1", "Re2f2", 0},
        {"k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1", "e2f2", 0},
        {"4r1k1/8/8/8/8/8/8/R3K3 w Q - 0 1", "Ra1a2", 0},
        {"7k/1P6/8/8/8/8/8/7K w - - 0 1", "b7b8", 0},
        {"7k/1P6/8/8/8/8/8/7K w - - 0 1", "b7b8=K", 0},
        {0, "d2d4++", 0}, {0, "Qd2d4", 0}, {0, "d9d4", 0}
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Position position = Position::standard();
        if (cases[i].fen) expect_true(Position::from_fen(cases[i].fen, position, 0), "LAN test FEN is valid");
        const std::string before = position.to_fen();
        Move move;
        const bool valid = position.parse_san(cases[i].text, move);
        expect_true(valid == (cases[i].canonical != 0), "LAN accepts exactly legal, correctly specified moves");
        std::string after;
        if (valid) {
            expect_true(position.san(move) == cases[i].canonical, "LAN resolves to the expected canonical SAN move");
            Undo undo;
            const bool made = position.make_move(move, undo);
            expect_true(made, "a parsed LAN move passes ordinary move application");
            if (made) { after = position.to_fen(); position.unmake_move(undo); }
        }
        expect_true(position.to_fen() == before, "LAN lookup and undo leave the source position unchanged");
        std::ostringstream game_text;
        game_text << "[SetUp \"1\"]\n[FEN \"" << before << "\"]\n\n"
                  << position.fullmove_number() << ". " << cases[i].text << " *";
        PgnDocument one;
        const bool parsed = PgnParser::parse(game_text.str(), one);
        expect_true(parsed == valid, "LAN tokenization preserves the same legality as direct move parsing");
        if (parsed && one.games.size() == 1 && !one.games[0]->root->children.empty()) {
            const MoveNode* node = one.games[0]->root->children[0];
            expect_true(node->san == cases[i].text && node->position_after.to_fen() == after,
                  "LAN tokenization and tree construction preserve exact source text and final position");
        }


    }
    PgnDocument rav;
    expect_true(PgnParser::parse("1. d2d4 (1. e2e4 e7e5 (1... c7c5 2. Ng1f3) 2. Ng1f3 Nb8c6) "
                           "Ng8f6 2. c2c4 e7e6 *", rav) && rav.games.size() == 1,
          "LAN parses through nested RAVs and resumes the main line");
    if (rav.games.size() == 1) {
        const MoveNode* root = rav.games[0]->root;
        expect_true(root->children.size() == 2 && root->children[1]->children.size() == 2,
              "LAN preserves the exact nested variation topology");
        const MoveNode* main = root;
        while (!main->children.empty()) main = main->children[0];
        expect_true(main->position_after.to_fen() ==
              "rnbqkb1r/pppp1ppp/4pn2/8/2PP4/8/PP2PPPP/RNBQKBNR w KQkq - 0 3",
              "LAN RAV processing restores the correct parent mainline position");
        if (root->children.size() == 2 && root->children[1]->children.size() == 2) {
            const MoveNode* branch = root->children[1];
            const MoveNode* nested = branch->children[1];
            expect_true(nested->children.size() == 1 && nested->children[0]->position_after.to_fen() ==
                  "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
                  "the nested LAN RAV keeps its independent exact position");
            while (!branch->children.empty()) branch = branch->children[0];
            expect_true(branch->position_after.to_fen() ==
                  "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
                  "LAN also resumes the enclosing RAV after its nested alternative");
        }
    }
    PgnDocument nulls;
    expect_true(PgnParser::parse("1. e2e4 -- 2. Ng1f3 (2. d2d4) -- 3. Bf1c4 *", nulls) &&
          nulls.games.size() == 1, "LAN interoperates with analytical null moves and RAVs");
    if (nulls.games.size() == 1) {
        const MoveNode* node = nulls.games[0]->root;
        int plies = 0;
        while (!node->children.empty()) { node = node->children[0]; ++plies; }
        expect_true(plies == 5 && node->position_after.to_fen() ==
              "rnbqkbnr/pppppppp/8/8/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 3",
              "null moves in LAN preserve occupancy, turn, clocks and the exact final position");
    }
}

void test_san_check_annotations()
{
    using namespace gambitree;
    const char* before = "8/8/8/4B3/2b3p1/6R1/5K2/7k w - - 7 78";
    const char* after = "8/8/8/4B3/2b3p1/8/5K2/6Rk b - - 8 78";
    const char* spellings[] = {"Rg1+", "Rg1#", "Rg1", "Rg3g1+", "R3g1#"};
    for (std::size_t i = 0; i < sizeof(spellings) / sizeof(spellings[0]); ++i) {
        Position position;
        Move move;
        Undo undo;
        expect_true(Position::from_fen(before, position, 0) &&
              position.parse_san(spellings[i], move) && position.san(move) == "Rg1#" &&
              position.make_move(move, undo) && position.to_fen() == after &&
              position.is_checkmate(),
              "a legal mating move accepts plus, hash or no annotation with exact FEN and mate");
    }
    Position position;
    Move move;
    expect_true(Position::from_fen(before, position, 0) &&
          !position.parse_san("Rh2+", move) && !position.parse_san("Rxg1#", move) &&
          !position.parse_san("Rhg1+", move) && !position.parse_san("Rg1++", move),
          "ignoring check annotations does not permit impossible moves, false captures or wrong origins");
    expect_true(Position::from_fen("4k3/8/8/8/8/2N1N3/8/4K3 w - - 0 1", position, 0) &&
          !position.parse_san("Nd5+", move) && position.parse_san("Ncd5#", move),
          "check annotations neither resolve ambiguity nor prevent correct disambiguation");
    expect_true(Position::from_fen("4k3/8/8/8/8/8/4R3/K7 w - - 0 1", position, 0) &&
          position.parse_san("Re7+", move) && position.san(move) == "Re7+" &&
          position.parse_san("Re7#", move) && position.parse_san("Re7", move),
          "ordinary checks accept correct, incorrect and omitted annotations without changing canonical SAN");
    position = Position::standard();
    expect_true(position.parse_san("e4+", move) && position.san(move) == "e4" &&
          position.parse_san("e4#", move) && !position.parse_san("e5#", move) &&
          !position.parse_san("e4+#", move),
          "quiet moves tolerate a check annotation but malformed or illegal move bodies remain invalid");

    const std::string setup = std::string("[SetUp \"1\"]\n[FEN \"") + before + "\"]\n\n";
    PgnDocument variations;
    expect_true(PgnParser::parse(setup +
              "78. Rg1+ (78. Rg1# (78. Rg1)) (78. Rg1) 1-0\n\n"
              "[Event \"Following game\"]\n\n1. e4# e5+ 2. Nf3 Nc6 *\n", variations) &&
          variations.games.size() == 2 && variations.errors.empty(),
          "wrong check annotations in mainline, nested and sibling RAVs preserve the following game");
    if (variations.games.size() == 2) {
        MoveNode* root = variations.games[0]->root;
        expect_true(root->children.size() == 4,
              "all four independently annotated mating branches are retained");
        const char* source_san[] = {"Rg1+", "Rg1#", "Rg1", "Rg1"};
        for (std::size_t i = 0; i < root->children.size(); ++i) {
            MoveNode* node = root->children[i];
            expect_true(i < 4 && node->parent == root && node->san == source_san[i] &&
                  node->position_after.to_fen() == after &&
                  node->position_after.is_checkmate(),
                  "each RAV preserves its source SAN, exact parent and mating position");
        }
        if (root->children.size() == 4) {
            expect_true(root->children[1]->following_items.size() == 1 &&
                  root->children[1]->following_items[0].type == PgnMoveItem::VARIATION &&
                  root->children[1]->following_items[0].variation_root == root->children[2],
                  "nested RAV remains attached to the correct source move");
        }
        MoveNode* node = variations.games[1]->root;
        int plies = 0;
        while (!node->children.empty()) { node = node->children[0]; ++plies; }
        expect_true(plies == 4 && node->position_after.to_fen() ==
                  "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
              "the next game has four independent legal plies and the expected FEN");
    }

    PgnDocument continuation;
    expect_true(PgnParser::parse(
        "1. e4# (1. d4+ d5# (1... Nf6+) 2. c4# e6+) e5+ 2. Nf3# Nc6+ *", continuation) &&
        continuation.games.size() == 1,
        "inaccurate annotations preserve continuation after nested RAVs");
    if (continuation.games.size() == 1) {
        MoveNode* root = continuation.games[0]->root;
        expect_true(root->children.size() == 2 && root->children[1]->children.size() == 2,
              "mainline and both black RAV replies retain exact branch structure");
        MoveNode* main = root;
        int plies = 0;
        while (!main->children.empty()) { main = main->children[0]; ++plies; }
        expect_true(plies == 4 && main->position_after.to_fen() ==
              "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
              "resuming the mainline after a RAV keeps the exact four-ply position");
        if (root->children.size() == 2 && root->children[1]->children.size() == 2) {
            MoveNode* branch = root->children[1];
            MoveNode* nested = branch->children[1];
            expect_true(nested->san == "Nf6+" && nested->parent == branch &&
                  nested->position_after.to_fen() ==
                  "rnbqkb1r/pppppppp/5n2/8/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 1 2",
                  "nested reply uses its own parent position and retains source SAN");
            int branch_plies = 1;
            while (!branch->children.empty()) { branch = branch->children[0]; ++branch_plies; }
            expect_true(branch_plies == 4 && branch->position_after.to_fen() ==
                  "rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/8/PP2PPPP/RNBQKBNR w KQkq - 0 3",
                  "parent RAV also resumes with its independent exact four-ply position");
        }
    }

    std::ifstream input("tests/data/guarin-malagon-2024-mate-suffix.pgn", std::ios::binary);
    const std::string pgn((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    PgnDocument document;
    expect_true(input.good() && !pgn.empty() && PgnParser::parse(pgn, document) &&
          document.games.size() == 1 && document.errors.empty(),
          "the complete reported PGN loads without an invalid-move error");
    if (document.games.size() == 1) {
        MoveNode* node = document.games[0]->root;
        int plies = 0;
        while (!node->children.empty()) { node = node->children[0]; ++plies; }
        expect_true(plies == 155 && node->san == "Rg1+" && node->position_after.to_fen() == after &&
              node->position_after.is_checkmate(),
              "the reported PGN retains 155 plies, original final SAN and the exact checkmate FEN");
        MoveNavigator navigator(*document.games[0]);
        navigator.to_end();
        expect_true(navigator.current() == node && navigator.position().to_fen() == after &&
              navigator.back() && navigator.position().to_fen() == before &&
              navigator.forward() && navigator.current() == node,
              "navigation reaches and leaves the source-spelled mating move without changing node identity");
        for (int figurines = 0; figurines < 2; ++figurines) {
            NotationLayout layout;
            layout.set_figurines(figurines != 0);
            layout.build(*document.games[0], 300, NotationFontMetrics());
            bool displayed = false;
            for (std::size_t i = 0; i < layout.items().size(); ++i) {
                const NotationItem& item = layout.items()[i];
                if (item.kind != NOTATION_MOVE || item.node != node) continue;
                displayed = item.text == (figurines ? "♖g1+" : "Rg1+") &&
                    layout.hit_test(item.x + item.width / 2, item.y + 1, 0) == node &&
                    !layout.selection_rects(node).empty();
            }
            expect_true(displayed, "source SAN, figurines, selection and hitboxes retain the mating move");
        }
    }
}


void test_san_error_diagnostics()
{
    PgnLibrary source;
    std::vector<std::string> source_paths(1, "tests/data/guarin-malagon-2024-mate-suffix.pgn");
    std::vector<std::string> source_errors;
    std::string source_error;
    expect_true(source.load(source_paths, source_errors), "the reported file opens through the viewer library");
    while (!source.indexing_complete()) source.step_indexing(100, source_errors);
    const bool loaded = source.materialize_current(source_error);
    expect_true(loaded && source.game_count() == 1 && source_errors.empty(),
                "lazy viewer loading retains the complete reported game");
    if (loaded) {
        MoveNavigator navigator(*source.current_game());
        navigator.to_end();
        expect_true(navigator.current()->san == "Rg1+" && navigator.position().is_checkmate() &&
                    navigator.position().to_fen() == "8/8/8/4B3/2b3p1/8/5K2/6Rk b - - 8 78",
                    "the viewer library and navigator reach the exact reported final checkmate");
    }
    std::ifstream input("tests/data/guarin-malagon-2024-mate-suffix.pgn", std::ios::binary);
    std::string pgn((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::size_t move = pgn.rfind("Rg1+");
    expect_true(move != std::string::npos, "diagnostic fixture contains the reported move");
    if (move == std::string::npos) return;
    pgn.replace(move, 4, "Rh2+");
    const std::string path = test_scratch_directory + "/gambitree-san-error.pgn";
    std::ofstream output(path.c_str(), std::ios::binary);
    output << "[Event \"Valid first game\"]\n\n1. e4 *\n\n" << pgn;
    output.close();
    PgnLibrary library;
    std::vector<std::string> paths(1, path), errors;
    std::string error;
    expect_true(library.load(paths, errors), "invalid-move diagnostic file can be indexed");
    while (!library.indexing_complete()) library.step_indexing(100, errors);
    expect_true(library.materialize_current(error) && library.select_game(1) &&
                library.materialize_current(error) && error.empty(),
                "a real illegal move retains the valid prefix of the second game");
    const PgnGame* partial = library.current_game();
    const std::string warning = partial ? partial->parse_warning() : "";
    expect_true(partial && partial->parse_status == gambitree::PGN_PARTIAL &&
                partial->parse_error.game_number == 2 && partial->available_plies == 154 &&
                warning.find("78.White") != std::string::npos &&
                warning.find("SAN move \"Rh2+\"") != std::string::npos &&
                warning.find("no unique legal move matches this notation") != std::string::npos &&
                warning.find("game line 22, column 933") != std::string::npos,
                "partial-game diagnostics retain human game and move, exact SAN, reason and location");
    std::remove(path.c_str());
    PgnDocument document;
    expect_true(!gambitree::PgnParser::parse(
        "[SetUp \"1\"]\n[FEN \"8/8/8/4B3/2b3p1/8/5K2/6Rk b - - 8 78\"]\n\n"
        "78... Kh2# *", document) && document.errors.size() == 1 &&
        document.errors[0].message.find("78.Black") == 0,
        "invalid black-move diagnostic uses the actual fullmove number and side from the FEN");
}

void test_analytical_null_moves()
{
    Position position = position_from_fen(
        "r3k2r/8/8/3pP3/8/8/8/R3K2R w KQkq d6 12 7");
    Undo white_null_undo, black_null_undo;
    expect_true(position.make_null_move(white_null_undo) &&
                position.to_fen() ==
                    "r3k2r/8/8/3pP3/8/8/8/R3K2R b KQkq - 13 7",
                "white analytical null clears en-passant, advances halfmove, preserves rights and occupancy");
    expect_true(position.make_null_move(black_null_undo) &&
                position.to_fen() ==
                    "r3k2r/8/8/3pP3/8/8/8/R3K2R w KQkq - 14 8",
                "black analytical null advances fullmove number and flips side");
    position.unmake_move(black_null_undo);
    expect_true(position.to_fen() ==
                    "r3k2r/8/8/3pP3/8/8/8/R3K2R b KQkq - 13 7",
                "null-move undo restores the exact preceding FEN");
    position.unmake_move(white_null_undo);
    expect_true(position.to_fen() ==
                    "r3k2r/8/8/3pP3/8/8/8/R3K2R w KQkq d6 12 7",
                "a second undo restores en-passant, clocks, castling and the original board");
    position = position_from_fen("4k3/8/8/8/8/8/8/4R1K1 b - - 0 1");
    Undo illegal_null_undo;
    expect_true(!position.make_null_move(illegal_null_undo),
                "an analytical null move is rejected while the mover is in check");

    struct RustemovNullCase { const char* before; const char* after; };
    const RustemovNullCase rustemov_nulls[] = {
        {"1rbqr1k1/1p1n1pbp/p2p2p1/2pP3n/P3P3/2NBBN1P/1P1Q1PP1/R4RK1 w - - 9 14",
         "1rbqr1k1/1p1n1pbp/p2p2p1/2pP3n/P3P3/2NBBN1P/1P1Q1PP1/R4RK1 b - - 10 14"},
        {"r1b1r1k1/1p1nqpbp/p2p2p1/2pP3n/P3P3/2NBBN1P/1P3PP1/R2QR1K1 w - - 9 14",
         "r1b1r1k1/1p1nqpbp/p2p2p1/2pP3n/P3P3/2NBBN1P/1P3PP1/R2QR1K1 b - - 10 14"},
        {"r1b1r1k1/1p1nqpbp/p2p1np1/2pP4/P3P3/2NBBN1P/1P3PP1/R2QR1K1 w - - 11 15",
         "r1b1r1k1/1p1nqpbp/p2p1np1/2pP4/P3P3/2NBBN1P/1P3PP1/R2QR1K1 b - - 12 15"},
        {"r4rk1/1p1bqpb1/p2p1npp/2pP4/P3P2B/2N2B1P/1P3PP1/R2QR1K1 w - - 2 18",
         "r4rk1/1p1bqpb1/p2p1npp/2pP4/P3P2B/2N2B1P/1P3PP1/R2QR1K1 b - - 3 18"},
        {"r1b1r1k1/1p1nqpbp/p2p1np1/2pP4/P3P3/2N1B2P/1P1NBPP1/R2Q1RK1 w - - 11 15",
         "r1b1r1k1/1p1nqpbp/p2p1np1/2pP4/P3P3/2N1B2P/1P1NBPP1/R2Q1RK1 b - - 12 15"}
    };
    for (std::size_t i = 0; i < sizeof(rustemov_nulls) /
                                sizeof(rustemov_nulls[0]); ++i) {
        std::string error;
        Position rustemov_position;
        Undo rustemov_undo;
        expect_true(Position::from_fen(rustemov_nulls[i].before,
                                       rustemov_position, &error) &&
                    rustemov_position.make_null_move(rustemov_undo) &&
                    rustemov_position.to_fen() == rustemov_nulls[i].after,
                    "the corresponding one of Rustemov-Korneev's five -- positions null-moves to the exact FEN");
        rustemov_position.unmake_move(rustemov_undo);
        expect_true(rustemov_position.to_fen() == rustemov_nulls[i].before,
                    "each Rustemov -- position restores exactly after undo");
    }

    const std::string pgn =
        "1. e4 e5 (1... -- 2. Nf3 (2. d4)) 2. Nf3 *";
    PgnDocument document;
    expect_true(gambitree::PgnParser::parse(pgn, document) &&
                document.games.size() == 1 && document.warnings.empty(),
                "analytical null move parses in a sibling and nested RAV");
    if (document.games.size() == 1) {
        MoveNode* e4 = child(document.games[0]->root, 0,
                             "null-move fixture e4 exists");
        MoveNode* e5 = child(e4, 0, "main-line e5 exists after null RAV");
        MoveNode* null_node = child(e4, 1, "null RAV remains a sibling of e5");
        MoveNode* nf3 = child(null_node, 0, "null RAV continuation remains present");
        MoveNode* d4 = child(null_node, 1, "nested RAV remains present");
        expect_true(null_node && null_node->san == "--" &&
                    null_node->position_after.to_fen() ==
                        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 1 2" &&
                    nf3 && d4 && e5 &&
                    e5->position_after.to_fen() ==
                        "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2" &&
                    document.games[0]->initial_position.to_fen() ==
                        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                    "null RAV retains source token and its position without contaminating siblings or root");
    }
}

MoveNode* child(MoveNode* node, std::size_t index, const char* description)
{
    expect_true(node != 0 && index < node->children.size(), description);
    return node != 0 && index < node->children.size() ? node->children[index] : 0;
}

void test_pgn()
{
    const char* simple =
        "[Event \"Event name\"]\n[Site \"Brussels\"]\n[Date \"2026.09.17\"]\n"
        "[Round \"7\"]\n[White \"White player\"]\n[Black \"Black player\"]\n"
        "[Result \"*\"]\n[WhiteElo \"2100\"]\n[ECO \"C20\"]\n"
        "\n{before first move} 1. e4 {between moves} e5 $1 2. Nf3 {after move}\n";
    PgnDocument document;
    MoveNode* e4;
    MoveNode* e5;
    MoveNode* nf3;
    expect_true(gambitree::PgnParser::parse(simple, document), "simple PGN parses");
    expect_true(document.games.size() == 1, "simple PGN has one game");
    if (document.games.empty()) return;
    PgnGame* game = document.games[0];
    expect_true(game->tags.size() == 9, "arbitrary standard tags retained");
    expect_true(game->tag_value("White") != 0 && *game->tag_value("White") == "White player", "tag lookup retains value");
    expect_true(game->result == "*", "Result tag supplies game result when movetext omits it");
    expect_true(game->root->comments.size() == 1 && game->root->comments[0] == "before first move", "comment before move belongs to root");
    e4 = child(game->root, 0, "e4 node exists");
    if (!e4) return;
    expect_true(e4->san == "e4" && e4->position_after.to_fen() == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", "e4 node stores correct position");
    expect_true(e4->comments.size() == 1 && e4->comments[0] == "between moves", "between-move comment retained");
    e5 = child(e4, 0, "e5 node exists");
    if (!e5) return;
    expect_true(e5->nags.size() == 1 && e5->nags[0] == 1, "NAG retained on proper move");
    nf3 = child(e5, 0, "Nf3 node exists");
    if (!nf3) return;
    expect_true(nf3->comments.size() == 1 && nf3->comments[0] == "after move", "trailing move comment retained");

    document.clear();
    expect_true(gambitree::PgnParser::parse("1. e4 (1. d4 d5) e5 2. Nf3 *", document), "single RAV parses");
    game = document.games[0]; e4 = child(game->root, 0, "main e4 exists");
    MoveNode* d4 = child(game->root, 1, "variation d4 branches from root");
    if (e4 && d4) {
        expect_true(e4->san == "e4" && d4->san == "d4", "main line and variation are siblings");
        expect_true(d4->position_after.to_fen() == "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq d3 0 1", "RAV starts before e4");
        expect_true(child(d4, 0, "d5 in variation exists")->san == "d5", "variation continuation retained");
        expect_true(child(e4, 0, "main e5 exists")->san == "e5", "main line remains intact after RAV");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse("1. e4 e5 (1... c5 $5 {inside c5} (1... d5 {nested} (1... Nf6 {deep})) 2. Nf3) (1... e6 {sibling}) 2. Nf3 {after variations} *", document), "nested and sibling RAVs parse");
    game = document.games[0]; e4 = child(game->root, 0, "e4 for nested RAV exists");
    if (e4) {
        MoveNode* main_e5 = child(e4, 0, "main e5 exists after nested RAV");
        MoveNode* c5 = child(e4, 1, "first sibling variation exists");
        MoveNode* d5 = child(e4, 2, "nested variation branches before c5");
        MoveNode* nf6 = child(e4, 3, "deep variation branches before d5");
        MoveNode* e6 = child(e4, 4, "second sibling variation exists");
        if (main_e5 && c5 && d5 && nf6 && e6) {
            expect_true(c5->nags.size() == 1 && c5->nags[0] == 5 && c5->comments[0] == "inside c5", "variation NAG and comment retained");
            expect_true(d5->san == "d5" && d5->position_after.to_fen() == "rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 2", "nested RAV starts before c5");
            expect_true(nf6->san == "Nf6" && nf6->position_after.to_fen() == "rnbqkb1r/pppppppp/5n2/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 1 2", "deep RAV starts before d5");
            expect_true(nf6->comments.size() == 1 && nf6->comments[0] == "deep", "deep variation comment retained");
            expect_true(e6->san == "e6", "sibling variation retained separately");
            expect_true(child(main_e5, 0, "main Nf3 exists")->comments[0] == "after variations", "comment after variation returns to main line");
        }
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse("[SetUp \"1\"]\n[FEN \"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1\"]\n1. O-O *", document), "SetUp/FEN PGN parses");
    expect_true(document.games[0]->root->children[0]->position_after.to_fen() == "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1", "SetUp FEN supplies root position");

    document.clear();
    expect_true(gambitree::PgnParser::parse("1. e4 d5 2. exd5 *\n\n1. f3 e5 2. g4 Qh4# 0-1", document), "captures and checkmate PGNs parse");
    expect_true(document.games.size() == 2, "multiple games in one input retained");
    expect_true(document.games[1]->root->children[0]->children[0]->children[0]->children[0]->san == "Qh4#", "checkmate SAN stored");
    expect_true(document.games[1]->result == "0-1", "movetext result retained");

    document.clear();
    expect_true(gambitree::PgnParser::parse("1.e4 e5 2.Nf3 Nc6 3.Bb5 a6 1-0", document), "compact move-number notation parses");
    expect_true(document.games.size() == 1 && document.games[0]->result == "1-0", "compact notation retains result token");

    document.clear();
    const char* bare_number =
        "[Event \"Separated move number\"]\n\n"
        "1. e4 c5 2 Nf3 $5 {the move number has no period} e6 "
        "(2... d6 3. d4) 3. d4 cxd4 *\n";
    expect_true(gambitree::PgnParser::parse(bare_number, document),
                "a bare expected move number before a legal SAN move is tolerated");
    if (document.games.size() == 1) {
        PgnGame* numbered_game = document.games[0];
        MoveNode* main_e4 = child(numbered_game->root, 0, "numbered line e4 exists");
        MoveNode* main_c5 = main_e4 ? child(main_e4, 0, "numbered line c5 exists") : 0;
        MoveNode* main_nf3 = main_c5 ? child(main_c5, 0, "numbered line Nf3 exists") : 0;
        expect_true(main_nf3 && main_nf3->san == "Nf3" &&
                    main_nf3->position_after.to_fen() ==
                    "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
                    "the SAN after an omitted number period is played at the expected position");
        expect_true(main_nf3 && main_nf3->nags.size() == 1 &&
                    main_nf3->nags[0] == 5 && main_nf3->comments.size() == 1,
                    "NAGs and comments after the tolerated number remain attached");
        MoveNode* variation_d6 = main_nf3 ? child(main_nf3, 1, "RAV after tolerated number exists") : 0;
        expect_true(variation_d6 && variation_d6->san == "d6" &&
                    variation_d6->position_after.side_to_move() == gambitree::WHITE,
                    "RAV parsing and its start position remain correct");
    }

    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4 e5 99 Nf3 *", document),
                "an unrelated standalone number is not silently discarded");
    expect_true(document.errors.size() == 1 && document.errors[0].line == 1 &&
                document.errors[0].message.find("standalone number") != std::string::npos,
                "an unrecognized bare number receives a located diagnostic");

    document.clear();
    const char* later_bare_number =
        "[SetUp \"1\"]\n[FEN \"4k3/8/8/8/8/8/P7/4K3 w - - 0 14\"]\n\n"
        "14 a4 *\n";
    expect_true(gambitree::PgnParser::parse(later_bare_number, document) &&
                document.games.size() == 1 &&
                document.games[0]->root->children[0]->san == "a4" &&
                document.games[0]->root->children[0]->position_after.to_fen() ==
                    "4k3/8/8/8/P7/8/8/4K3 b - a3 0 14",
                "later bare move numbers are accepted only when expected by the current FEN");

    document.clear();
    const char* separated_period_number =
        "[SetUp \"1\"]\n"
        "[FEN \"4k3/8/8/8/8/6N1/8/4K3 w - - 0 21\"]\n\n"
        "21 .Nf5 21... Kf8 *\n";
    expect_true(gambitree::PgnParser::parse(separated_period_number, document) &&
                document.games.size() == 1,
                "a separated white move-number period is recognized in context");
    if (document.games.size() == 1) {
        MoveNode* nf5 = child(document.games[0]->root, 0,
                              "SAN after separated period exists");
        expect_true(nf5 && nf5->san == "Nf5" &&
                    nf5->position_after.to_fen() ==
                        "4k3/8/8/5N2/8/8/8/4K3 b - - 1 21",
                    "separated punctuation does not change the expected position");
        expect_true(nf5 && child(nf5, 0, "black reply after Nf5 exists") &&
                    child(nf5, 0, "black reply after Nf5 exists")->san == "Kf8",
                    "the move after separated notation is still parsed normally");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "[SetUp \"1\"]\n"
                    "[FEN \"4k3/8/8/8/8/6N1/8/4K3 w - - 0 21\"]\n\n"
                    "21. Kf2 (21 .Nf5) *", document) &&
                document.games.size() == 1,
                "separated period is recognized relative to the local RAV position");
    if (document.games.size() == 1) {
        MoveNode* main_move = child(document.games[0]->root, 0,
                                    "main move before separated-number RAV exists");
        MoveNode* rav_move = child(document.games[0]->root, 1,
                                   "RAV begins from the expected parent position");
        expect_true(main_move && main_move->san == "Kf2" && rav_move &&
                    rav_move->san == "Nf5" &&
                    rav_move->position_after.to_fen() ==
                        "4k3/8/8/5N2/8/8/8/4K3 b - - 1 21",
                    "separated-number RAV keeps the correct branch position");
    }

    document.clear();
    expect_true(!gambitree::PgnParser::parse(
                    "[SetUp \"1\"]\n"
                    "[FEN \"4k3/8/8/8/8/6N1/8/4K3 w - - 0 21\"]\n\n"
                    "20 .Nf5 *", document),
                "a separated period after the wrong move number remains an error");

    document.clear();
    expect_true(!gambitree::PgnParser::parse(
                    "[SetUp \"1\"]\n"
                    "[FEN \"4k3/8/8/8/8/6N1/8/4K3 w - - 0 21\"]\n\n"
                    "21 .Qh5 *", document),
                "an illegal SAN after a separated period remains an error");

    document.clear();
    expect_true(!gambitree::PgnParser::parse(
                    "[SetUp \"1\"]\n"
                    "[FEN \"4k3/8/8/8/8/6N1/8/4K3 w - - 0 21\"]\n\n"
                    "21 {comment} .Nf5 *", document),
                "separated-number recovery does not cross a comment");

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 (1... c5 2. Nf3 Kb8) (1... d5) 2. Nf3 *",
                    document),
                "an illegal RAV is truncated while the main line remains valid");
    expect_true(document.errors.empty() && document.games.size() == 1 &&
                document.skipped_variation_count == 1 &&
                document.warnings.size() == 1 &&
                document.warnings[0].game_number == 1 &&
                document.warnings[0].line == 1 &&
                document.warnings[0].column == 26 &&
                document.warnings[0].message.find("Kb8") != std::string::npos &&
                document.warnings[0].message.find("RAV depth 1") != std::string::npos,
                "a truncated RAV produces one located warning with its SAN and depth");
    if (document.games.size() == 1) {
        PgnGame* recovered = document.games[0];
        MoveNode* recovered_e4 = child(recovered->root, 0,
                                       "recovered main e4 exists");
        MoveNode* recovered_e5 = recovered_e4 ? child(recovered_e4, 0,
                                       "recovered main e5 exists") : 0;
        MoveNode* recovered_nf3 = recovered_e5 ? child(recovered_e5, 0,
                                       "main line continues after invalid RAV") : 0;
        expect_true(recovered_nf3 && recovered_nf3->san == "Nf3" &&
                    recovered_nf3->position_after.to_fen() ==
                    "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
                    "skipping a RAV restores the exact main-line position");
        expect_true(recovered_e4 && recovered_e4->children.size() == 3 &&
                    recovered_e4->children[1]->san == "c5" &&
                    recovered_e4->children[1]->children.size() == 1 &&
                    recovered_e4->children[1]->children[0]->san == "Nf3" &&
                    recovered_e4->children[1]->children[0]->children.empty() &&
                    recovered_e4->children[2]->san == "d5",
                    "the legal RAV prefix survives, the illegal tail is absent, and its sibling remains");
        if (recovered_e4 && recovered_e4->children.size() == 3) {
            MoveNavigator recovered_navigator(*recovered);
            MoveNode* valid_rav = recovered_e4->children[1]->children[0];
            expect_true(recovered_navigator.select(valid_rav) &&
                        recovered_navigator.position().equals(valid_rav->position_after),
                        "the last legal prefix move remains selectable with its stored position");
        }
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 (1... c5 $5 {first} 2. Nf3 $6 {second} Kb8 "
                    "3. d4 {untrusted tail}) (1... d5) 2. Nf3 *", document) &&
                document.games.size() == 1 && document.skipped_variation_count == 1,
                "two verified RAV moves survive an illegal later move");
    if (document.games.size() == 1) {
        MoveNode* prefix_c5 = child(document.games[0]->root->children[0], 1,
                                    "legal RAV prefix c5 exists");
        MoveNode* prefix_nf3 = prefix_c5 ? child(prefix_c5, 0,
                                    "legal RAV prefix Nf3 exists") : 0;
        expect_true(prefix_c5 && prefix_c5->nags.size() == 1 &&
                    prefix_c5->nags[0] == 5 && prefix_c5->comments.size() == 1 &&
                    prefix_c5->comments[0] == "first",
                    "comment and NAG on the first retained RAV move stay attached");
        expect_true(prefix_nf3 && prefix_nf3->nags.size() == 1 &&
                    prefix_nf3->nags[0] == 6 && prefix_nf3->comments.size() == 1 &&
                    prefix_nf3->comments[0] == "second" &&
                    prefix_nf3->children.empty(),
                    "comment and NAG on the final verified move stay, with no illegal tail node");
        expect_true(prefix_nf3 && prefix_nf3->position_after.to_fen() ==
                    "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
                    "the retained prefix ends at its verified chess position");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 (1... c5 2. Nf3 (2... Kb8) 2... d6) "
                    "(1... d5) 2. Nf3 *", document),
                "an illegal nested RAV is truncated without losing its valid parent RAV");
    if (document.games.size() == 1) {
        MoveNode* e4_node = child(document.games[0]->root, 0,
                                  "nested recovery e4 exists");
        MoveNode* c5_node = e4_node ? child(e4_node, 1,
                                  "valid outer c5 RAV remains") : 0;
        MoveNode* nf3_node = c5_node ? child(c5_node, 0,
                                  "valid outer RAV continues") : 0;
        expect_true(document.skipped_variation_count == 1 && c5_node &&
                    nf3_node && nf3_node->children.size() == 1 &&
                    nf3_node->children[0]->san == "d6",
                    "only the malformed nested branch is discarded");
        expect_true(c5_node && c5_node->position_after.to_fen() ==
                    "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2",
                    "valid variation retains its parent castling and en-passant state");
        expect_true(e4_node && e4_node->children.size() == 3 &&
                    e4_node->children[2]->san == "d5",
                    "a later valid sibling RAV remains navigable");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 (1... c5 {literal ) and ( in braces} "
                    "2. Nf3 ; literal ) in line comment\n"
                    "% ) ignored escape-line text\n2... Kb8) 2. Nf3 *",
                    document),
                "comments and escape lines do not confuse the invalid-RAV boundary scan");
    expect_true(document.skipped_variation_count == 1 &&
                document.games.size() == 1 && document.errors.empty(),
                "a structurally closed invalid RAV with opaque comments is recoverable");
    if (document.games.size() == 1) {
        MoveNode* e4_comments = child(document.games[0]->root, 0,
                                      "comment-boundary recovery e4 exists");
        MoveNode* c5_comments = e4_comments ? child(e4_comments, 1,
                                      "comment-boundary legal c5 prefix exists") : 0;
        MoveNode* nf3_comments = c5_comments ? child(c5_comments, 0,
                                      "comment-boundary legal Nf3 prefix exists") : 0;
        expect_true(c5_comments && c5_comments->comments.size() == 1 &&
                    c5_comments->comments[0] == "literal ) and ( in braces" &&
                    nf3_comments && nf3_comments->comments.size() == 1 &&
                    nf3_comments->comments[0] == " literal ) in line comment",
                    "literal parentheses in comments remain attached and escape lines stay ignored");
        expect_true(nf3_comments && nf3_comments->children.empty(),
                    "comment-aware recovery leaves no node for the illegal move or skipped tail");
    }

    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4 e5 (1... Kb8 *", document),
                "an unbalanced RAV containing invalid SAN remains a fatal error");

    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4 e5 Kb8 *", document),
                "illegal SAN in the main line remains fatal");
    expect_true(document.errors.size() == 1 &&
                document.errors[0].message.find("RAV depth 0") != std::string::npos,
                "main-line illegal SAN retains its fatal diagnostic");

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 (1... Kb8) (1... Qh4) (1... d5) 2. Nf3 *",
                    document),
                "multiple invalid sibling RAVs are truncated independently");
    expect_true(document.skipped_variation_count == 2 &&
                document.games.size() == 1,
                "valid sibling after multiple invalid RAVs remains in the tree");
    if (document.games.size() == 1) {
        MoveNode* e4_after_invalids = child(document.games[0]->root, 0,
                                            "multiple recovery e4 exists");
        MoveNode* d5_after_invalids = child(e4_after_invalids, 1,
                                            "valid sibling after two invalid RAVs exists");
        expect_true(e4_after_invalids && e4_after_invalids->children.size() == 2 &&
                    d5_after_invalids && d5_after_invalids->san == "d5",
                    "the valid sibling SAN is retained after two discarded RAVs");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 (1... c5 (1... d5) 2. Kxf5) "
                    "(1... e6) 2. Nf3 *", document) &&
                document.skipped_variation_count == 1 &&
                document.games.size() == 1,
                "an invalid outer RAV retains its legal prefix and completed nested branch");
    if (document.games.size() == 1) {
        MoveNode* e4_outer = child(document.games[0]->root, 0,
                                   "outer rollback e4 exists");
        expect_true(e4_outer && e4_outer->children.size() == 4 &&
                    e4_outer->children[0]->san == "e5" &&
                    e4_outer->children[1]->san == "c5" &&
                    e4_outer->children[2]->san == "d5" &&
                    e4_outer->children[3]->san == "e6",
                    "truncation preserves the legal outer prefix, its nested sibling, and later sibling");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 (1... c5 (1... d6 2. d4 Qh9) "
                    "2. Nf3 Nc6) (1... e6) 2. Nf3 *", document) &&
                document.games.size() == 1 && document.skipped_variation_count == 1,
                "an illegal deep nested RAV move preserves verified moves at both outer levels");
    if (document.games.size() == 1) {
        MoveNode* e4_nested_prefix = child(document.games[0]->root, 0,
                                           "deep nested recovery e4 exists");
        MoveNode* c5_nested_prefix = e4_nested_prefix ? child(e4_nested_prefix, 1,
                                           "deep nested recovery c5 exists") : 0;
        MoveNode* d6_nested_prefix = e4_nested_prefix ? child(e4_nested_prefix, 2,
                                           "deep nested recovery d6 exists") : 0;
        expect_true(c5_nested_prefix && c5_nested_prefix->children.size() == 1 &&
                    c5_nested_prefix->children[0]->san == "Nf3" && d6_nested_prefix &&
                    d6_nested_prefix->san == "d6" && d6_nested_prefix->children.size() == 1 &&
                    d6_nested_prefix->children[0]->san == "d4" &&
                    d6_nested_prefix->children[0]->children.empty(),
                    "only the illegal deep tail is absent; outer and inner legal prefixes survive");
        MoveNode* sibling = e4_nested_prefix ? child(e4_nested_prefix, 3,
                                           "deep nested sibling e6 exists") : 0;
        expect_true(sibling && sibling->san == "e6",
                    "a valid sibling RAV remains available after nested recovery");
        expect_true(d6_nested_prefix && d6_nested_prefix->children[0]->position_after.to_fen() ==
                    "rnbqkbnr/ppp1pppp/3p4/8/3PP3/8/PPP2PPP/RNBQKBNR b KQkq d3 0 2",
                    "deep retained prefix has the expected FEN before its discarded illegal move");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
                    "[Variant \"Chess960\"]\n1. e4 e5 (1... Kb8) *",
                    document) && document.skipped_variation_count == 1 &&
                document.games.size() == 1,
                "invalid RAV recovery also preserves a valid Chess960 main line");

    document.clear();
    {
        std::string many_invalid_variations = "1. e4";
        int variation_index;
        for (variation_index = 0; variation_index < 12; ++variation_index)
            many_invalid_variations += " (1... Kb8)";
        many_invalid_variations += " *";
        expect_true(gambitree::PgnParser::parse(many_invalid_variations, document) &&
                    document.skipped_variation_count == 12 &&
                    document.warnings.size() == 10,
                    "variation warnings retain a bounded sample and count all skipped RAVs");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse("1. e4 e5 ?? 2. Nf3 !? *", document), "symbolic engine NAGs parse without being treated as SAN");
    expect_true(document.games[0]->root->children[0]->children[0]->nags[0] == 4 &&
                document.games[0]->root->children[0]->children[0]->children[0]->nags[0] == 5,
                "symbolic engine NAGs map to their conventional numeric values");

    document.clear();
    expect_true(!gambitree::PgnParser::parse("[Event \"broken\"]\n1. e4 Qz7\n[Event \"good\"]\n1. d4 d5 *", document), "invalid SAN reports parse failure");
    expect_true(document.errors.size() == 1 && document.errors[0].line == 2, "invalid SAN records location");
    expect_true(document.errors[0].message.find("RAV depth") != std::string::npos &&
                document.errors[0].message.find("FEN") != std::string::npos,
                "invalid SAN diagnostics retain variation depth and position context");
    expect_true(document.games.size() == 1 && document.games[0]->tag_value("Event") != 0 && *document.games[0]->tag_value("Event") == "good", "parser resynchronizes at next tag pair");

    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4 (1. d4", document), "unexpected EOF inside variation rejected");
    expect_true(!document.errors.empty(), "unexpected EOF produces diagnostic");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4 {unterminated", document), "unterminated comment rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4 ) e5", document), "unexpected variation end rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4 $x", document), "malformed NAG rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("[SetUp \"1\"]\n1. e4 *", document), "SetUp without FEN rejected");
}

void test_pgn_symbolic_suffixes()
{
    const char* san[] = {"e4", "e5", "Nf3", "Nc6", "Bb5", "a6"};
    const int nag[] = {1, 2, 3, 4, 5, 6};
    PgnDocument document;
    const std::string main_line =
        "1.e4! e5? 2. Nf3!! Nc6?? 3. Bb5!? a6?! *";
    expect_true(gambitree::PgnParser::parse(main_line, document),
                "all six attached symbolic suffixes parse in the main line");
    if (document.games.size() == 1) {
        MoveNode* node = document.games[0]->root;
        int i;
        for (i = 0; i < 6; ++i) {
            node = child(node, 0, "attached-symbol main-line move exists");
            if (!node) break;
            expect_true(node->san == san[i],
                        "attached suffix is removed from stored SAN");
            expect_true(node->nags.size() == 1 && node->nags[0] == nag[i],
                        "attached suffix maps to the correct move NAG");
        }
    }

    document.clear();
    {
        const std::string ravs =
            "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 "
            "(3... Nf6! 4. O-O) (3... Bc5? 4. c3) "
            "(3... d6!! 4. d4) (3... g6?? 4. c3) "
            "(3... a5!? 4. O-O) (3... Be7?! 4. O-O) "
            "4. Ba4 Nf6 *";
        const char* rav_san[] = {"Nf6", "Bc5", "d6", "g6", "a5", "Be7"};
        MoveNode* node;
        MoveNode* bb5;
        MoveNode* a6;
        int i;
        expect_true(gambitree::PgnParser::parse(ravs, document),
                    "all six attached symbolic suffixes parse in legal RAVs");
        expect_true(document.skipped_variation_count == 0 &&
                    document.warnings.empty(),
                    "valid annotated RAVs are neither truncated nor warned about");
        if (document.games.size() == 1) {
            node = document.games[0]->root;
            for (i = 0; i < 5 && node; ++i)
                node = child(node, 0, "RAV opening move exists");
            bb5 = node;
            a6 = bb5 ? child(bb5, 0, "played a6 remains the main child") : 0;
            if (bb5 && a6) {
                expect_true(bb5->children.size() == 7,
                            "all six sibling RAVs remain in the move tree");
                for (i = 0; i < 6; ++i) {
                    MoveNode* variation = child(
                        bb5, static_cast<std::size_t>(i + 1),
                        "annotated sibling RAV exists");
                    if (!variation) continue;
                    expect_true(variation->san == rav_san[i],
                                "RAV SAN is stored without its suffix");
                    expect_true(variation->nags.size() == 1 &&
                                variation->nags[0] == nag[i],
                                "RAV suffix is retained on its exact move");
                    expect_true(variation->children.size() == 1,
                                "continuation after an annotated RAV move remains");
                }
                expect_true(a6->children.size() == 1 &&
                            a6->children[0]->san == "Ba4",
                            "main line continues after the annotated sibling RAVs");
            }
        }
    }

    document.clear();
    {
        const std::string numeric =
            "1. e4 $1 e5 $2 2. Nf3 $3 Nc6 $4 3. Bb5 $5 a6 $6 "
            "(3... Nf6 $6) 4. Ba4 *";
        MoveNode* node;
        int i;
        expect_true(gambitree::PgnParser::parse(numeric, document),
                    "numeric NAGs $1 through $6 still parse");
        if (document.games.size() == 1) {
            node = document.games[0]->root;
            for (i = 0; i < 6; ++i) {
                node = child(node, 0, "numeric-NAG main-line move exists");
                if (!node) break;
                expect_true(node->nags.size() == 1 && node->nags[0] == i + 1,
                            "numeric NAG remains attached to its main-line move");
            }
            if (node && node->parent) {
                MoveNode* bb5 = node->parent;
                MoveNode* variation = child(bb5, 1, "numeric-NAG RAV exists");
                expect_true(variation && variation->nags.size() == 1 &&
                            variation->nags[0] == 6,
                            "numeric NAG remains attached inside the RAV");
            }
        }
    }

    document.clear();
    {
        const std::string combined =
            "1. e4!? {comment after attached suffix} $5 e5 *";
        expect_true(gambitree::PgnParser::parse(combined, document),
                    "suffix, following comment, and numeric NAG parse together");
        if (document.games.size() == 1) {
            MoveNode* e4 = child(document.games[0]->root, 0,
                                 "combined-annotation e4 exists");
            expect_true(e4 && e4->nags.size() == 2 && e4->nags[0] == 5 &&
                        e4->nags[1] == 5,
                        "symbolic and numeric NAGs both remain on e4");
            expect_true(e4 && e4->comments.size() == 1 &&
                        e4->comments[0] == "comment after attached suffix",
                        "comment remains attached to the annotated move");
        }
    }

    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e5?! *", document),
                "an illegal SAN with valid suffix remains rejected");
    expect_true(document.errors.size() == 1 &&
                document.errors[0].message.find("SAN move \"e5\"") != std::string::npos,
                "illegal SAN diagnostic uses the suffix-free SAN token");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e5+/- *", document),
                "illegal SAN with attached evaluation remains rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4!!! *", document),
                "unsupported repeated suffix is rejected");
    expect_true(document.errors.size() == 1 &&
                document.errors[0].message.find("e4!!!") != std::string::npos,
                "unsupported suffix is not silently normalized");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4?" "?! *", document),
                "unsupported mixed suffix is rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4+/-+ *", document),
                "an undefined evaluation spelling is not treated as an annotation");
}

void test_pgn_evaluations_and_post_result_analysis()
{
    const std::string evaluations =
        "1. e4+/- e5-/+ 2. Nf3+- Nc6-+ *";
    PgnDocument document;
    expect_true(gambitree::PgnParser::parse(evaluations, document) &&
                document.games.size() == 1,
                "all four attached evaluation suffixes parse in the main line");
    if (document.games.size() == 1) {
        MoveNode* node = document.games[0]->root;
        const int expected_nags[] = {16, 17, 18, 19};
        const char* expected_san[] = {"e4", "e5", "Nf3", "Nc6"};
        int index;
        for (index = 0; index < 4; ++index) {
            node = child(node, 0, "evaluation main-line move exists");
            if (!node) break;
            expect_true(node->san == expected_san[index] &&
                        node->nags.size() == 1 &&
                        node->nags[0] == expected_nags[index],
                        "evaluation suffix becomes its exact NAG and leaves SAN intact");
        }
        expect_true(document.games[0]->result == "*",
                    "evaluation annotations do not alter the result");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
        "1. e4 e5 (1... c5+/- 2. d4) (1... d5-/+ 2. exd5) "
        "(1... Nf6+- 2. e5) (1... e6-+ 2. d4) 2. Nf3 *", document) &&
        document.games.size() == 1,
        "all four attached evaluation suffixes parse inside ordinary RAVs");
    if (document.games.size() == 1) {
        MoveNode* e4 = child(document.games[0]->root, 0,
                             "evaluation RAV main e4 exists");
        const int expected_nags[] = {16, 17, 18, 19};
        const char* expected_san[] = {"c5", "d5", "Nf6", "e6"};
        int index;
        expect_true(e4 && e4->children.size() == 5,
                    "four evaluated alternatives and main e5 are retained");
        for (index = 0; index < 4 && e4; ++index) {
            MoveNode* variation = child(e4, static_cast<std::size_t>(index + 1),
                                        "evaluated RAV exists");
            expect_true(variation && variation->san == expected_san[index] &&
                        variation->nags.size() == 1 &&
                        variation->nags[0] == expected_nags[index] &&
                        !variation->children.empty(),
                        "RAV evaluation NAG is kept on its legal first move");
        }
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
        "[SetUp \"1\"]\n[FEN \"7k/8/5KQ1/8/8/8/8/8 w - - 0 1\"]\n\n"
        "1. Qh5++/- *", document) && document.games.size() == 1,
        "check suffix remains SAN when followed by attached evaluation");
    if (document.games.size() == 1) {
        MoveNode* move = child(document.games[0]->root, 0,
                               "checking evaluated queen move exists");
        expect_true(move && move->san == "Qh5+" && move->nags.size() == 1 &&
                    move->nags[0] == 16,
                    "check marker is preserved and evaluation maps to NAG 16");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse(
        "[SetUp \"1\"]\n[FEN \"7k/8/5KQ1/8/8/8/8/6R1 w - - 0 1\"]\n\n"
        "1. Qh5#+- *", document) && document.games.size() == 1,
        "mate suffix remains SAN when followed by attached evaluation");
    if (document.games.size() == 1) {
        MoveNode* move = child(document.games[0]->root, 0,
                               "mating evaluated queen move exists");
        expect_true(move && move->san == "Qh5#" && move->nags.size() == 1 &&
                    move->nags[0] == 18,
                    "mate marker is preserved and evaluation maps to NAG 18");
    }

    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e5+/- *", document),
                "illegal SAN with a valid evaluation remains fatal");
    document.clear();
    expect_true(!gambitree::PgnParser::parse("1. e4+-- *", document),
                "an unsupported evaluation suffix is not normalized");

    const std::string tail =
        "1. e4 e5 2. Nf3 Nc6 0-1\n"
        "{The game is over; analysis follows.} (3. Bb5 a6) "
        "{Another line:} (3. Bc4 (3. d4) Nf6)\n"
        "[Site \"Next game without Event\"]\n\n"
        "1. d4 d5 1-0";
    document.clear();
    expect_true(gambitree::PgnParser::parse(tail, document) &&
                document.games.size() == 2,
                "tail comments and hypothetical RAVs do not create an extra game");
    if (document.games.size() == 2) {
        PgnGame* first = document.games[0];
        MoveNode* end = first->root;
        while (!end->children.empty()) end = end->children[0];
        expect_true(first->result == "0-1" &&
                    end->position_after.to_fen() ==
                        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
                    "post-result analysis preserves the actual result and final FEN");
        expect_true(first->trailing_items.size() == 4 &&
                    first->trailing_items[0].type ==
                        gambitree::PgnTrailingItem::COMMENT &&
                    first->trailing_items[1].type ==
                        gambitree::PgnTrailingItem::HYPOTHETICAL_VARIATION &&
                    first->trailing_items[2].type ==
                        gambitree::PgnTrailingItem::COMMENT &&
                    first->trailing_items[3].type ==
                        gambitree::PgnTrailingItem::HYPOTHETICAL_VARIATION,
                    "post-result comments and variants retain their source order");
        MoveNode* tail_root = first->trailing_items[1].variation_root;
        expect_true(tail_root && tail_root->position_after.to_fen() ==
                    end->position_after.to_fen() &&
                    tail_root->children.size() == 1 &&
                    tail_root->children[0]->san == "Bb5" &&
                    tail_root->children[0]->children[0]->san == "a6",
                    "hypothetical variation starts at the played final position");
        expect_true(first->trailing_items[3].variation_root->children.size() == 2 &&
                    first->trailing_items[3].variation_root->children[0]->san == "Bc4" &&
                    first->trailing_items[3].variation_root->children[0]->children[0]->san == "Nf6" &&
                    first->trailing_items[3].variation_root->children[1]->san == "d4",
                    "nested post-result RAV remains separate from the main line");
        expect_true(document.games[1]->tag_value("Site") != 0 &&
                    document.games[1]->root->children.size() == 1,
                    "a later tag section starts a second game without requiring Event");
    }

    const char* results[] = {"1-0", "0-1", "1/2-1/2", "*"};
    int result_index;
    for (result_index = 0; result_index < 4; ++result_index) {
        std::string pgn = "1. e4 e5 " + std::string(results[result_index]) +
                          " {tail} (2. Nf3)";
        document.clear();
        expect_true(gambitree::PgnParser::parse(pgn, document) &&
                    document.games.size() == 1 &&
                    document.games[0]->result == results[result_index] &&
                    document.games[0]->trailing_items.size() == 2,
                    "all result markers preserve same-game tail comments and RAVs");
    }

    document.clear();
    expect_true(!gambitree::PgnParser::parse(
                    "1. e4 e5 1-0 2. Nf3", document),
                "unparenthesized movetext after a result is rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse(
                    "1. e4 e5 1-0 (2. Kb8)", document),
                "illegal post-result hypothetical SAN is rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse(
                    "1. f3 e5 2. g4 Qh4# 0-1 (3. Ke2)", document),
                "a hypothetical continuation after actual checkmate is rejected");
    document.clear();
    expect_true(!gambitree::PgnParser::parse(
                    "1. e4 e5 1-0 ()", document),
                "an empty post-result RAV is rejected");
}

void test_adversarial_pgn_file()
{
    std::ifstream input("tests/data/adversarial.pgn");
    std::ostringstream content;
    PgnDocument document;
    content << input.rdbuf();
    expect_true(input.good() || input.eof(), "adversarial PGN file opened");
    expect_true(gambitree::PgnParser::parse(content.str(), document), "adversarial nested PGN parses");
    expect_true(document.games.size() == 1 && document.games[0]->root->children.size() >= 2, "adversarial PGN retains root variations");
}

void test_navigation()
{
    PgnGame empty;
    MoveNavigator empty_navigator(empty);
    PgnDocument document;
    PgnGame* game;
    MoveNode* e4;
    MoveNode* d4;

    expect_true(empty_navigator.at_root(), "empty navigator starts at root");
    expect_true(empty_navigator.current() == empty.root, "empty navigator selects root node");
    expect_true(empty_navigator.position().equals(empty.initial_position), "root navigator exposes initial position");
    expect_true(!empty_navigator.forward(), "forward at empty root does not move");
    expect_true(!empty_navigator.back(), "back at root does not move");
    expect_true(!empty_navigator.enter_variation(0), "empty root has no variation to enter");
    expect_true(!empty_navigator.next_sibling() && !empty_navigator.previous_sibling(), "root has no siblings");
    empty_navigator.to_end();
    expect_true(empty_navigator.at_root(), "end at empty root does not move");

    expect_true(gambitree::PgnParser::parse("1. e4 e5 2. Nf3 *", document), "navigation main-line PGN parses");
    game = document.games[0];
    MoveNavigator main_navigator(*game);
    expect_true(main_navigator.forward() && main_navigator.current()->san == "e4", "forward enters main-line first move");
    expect_true(main_navigator.position().to_fen() == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", "forward exposes e4 position");
    expect_true(main_navigator.forward() && main_navigator.current()->san == "e5", "forward follows main line");
    expect_true(main_navigator.back() && main_navigator.current()->san == "e4", "back returns to parent node");
    main_navigator.to_end();
    expect_true(main_navigator.current()->san == "Nf3", "end follows the current main line to its leaf");
    expect_true(main_navigator.position().to_fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2", "end exposes final main-line position");
    expect_true(!main_navigator.forward(), "forward at line end does not move");
    expect_true(gambitree::is_mainline_end(*game, main_navigator.current()),
                "the played-line leaf is recognized as the notation end");
    main_navigator.to_begin();
    expect_true(main_navigator.at_root() && main_navigator.position().equals(game->initial_position), "begin returns to root position");

    {
        PgnDocument branch_document;
        const std::string branch_pgn =
            "1. e4 e5 (1... c5 2. Nf3 d6 (2... Nc6 3. d4)) "
            "2. Nf3 (2. Bc4) Nc6 3. Bb5 a6 *";
        expect_true(gambitree::PgnParser::parse(branch_pgn, branch_document) &&
                    branch_document.games.size() == 1,
                    "nested navigation fixture parses");
        if (!branch_document.games.empty()) {
            PgnGame& branch_game = *branch_document.games[0];
            MoveNode* branch_e4 = branch_game.root->children[0];
            MoveNode* branch_e5 = branch_e4->children[0];
            MoveNode* branch_c5 = branch_e4->children[1];
            MoveNode* side_nf3 = branch_c5->children[0];
            MoveNode* side_d6 = side_nf3->children[0];
            MoveNode* side_nc6 = side_nf3->children[1];
            MoveNode* side_d4 = side_nc6->children[0];
            MoveNode* main_bc4 = branch_e5->children[1];
            MoveNavigator up_from_first(branch_game);
            MoveNavigator up_from_middle(branch_game);
            MoveNavigator up_from_last(branch_game);
            MoveNavigator nested_up(branch_game);
            MoveNavigator main_up(branch_game);
            MoveNavigator left_then_right(branch_game);
            MoveNavigator selected_side(branch_game);
            MoveNavigator end_main(branch_game);
            MoveNavigator end_side(branch_game);
            MoveNavigator end_side_middle(branch_game);
            MoveNavigator unrelated_choice(branch_game);

            expect_true(up_from_first.select(branch_c5) &&
                        up_from_first.leave_variation() &&
                        up_from_first.current() == branch_e4 &&
                        up_from_first.position().equals(branch_e4->position_after),
                        "Up from the first alternative move returns to its branching position");
            expect_true(up_from_middle.select(side_nf3) &&
                        up_from_middle.leave_variation() &&
                        up_from_middle.current() == branch_e4,
                        "Up from the middle of an alternative leaves that line");
            expect_true(up_from_last.select(side_d6) &&
                        up_from_last.leave_variation() &&
                        up_from_last.current() == branch_e4,
                        "Up from the last move of an alternative leaves that line");
            expect_true(nested_up.select(side_d4) &&
                        nested_up.leave_variation() &&
                        nested_up.current() == side_nf3 &&
                        nested_up.position().equals(side_nf3->position_after) &&
                        nested_up.leave_variation() &&
                        nested_up.current() == branch_e4 &&
                        nested_up.forward() && nested_up.current() == branch_e5,
                        "Up leaves one nested level at a time and Right resumes its parent continuation");
            expect_true(main_up.select(branch_e5) && !main_up.leave_variation() &&
                        main_up.current() == branch_e5,
                        "Up at the main-game level does not jump");
            expect_true(left_then_right.select(branch_c5) &&
                        left_then_right.back() && left_then_right.current() == branch_e4 &&
                        left_then_right.selected_child_index() == 0 &&
                        left_then_right.forward() && left_then_right.current() == branch_e5,
                        "Left out of an alternative clears that branch preference for Right");
            expect_true(left_then_right.back() &&
                        left_then_right.current() == branch_e4 &&
                        left_then_right.enter_variation(1) &&
                        left_then_right.current() == branch_c5,
                        "an exited alternative can still be deliberately selected again");

            end_main.to_end();
            expect_true(end_main.current()->san == "a6" &&
                        end_main.position().equals(end_main.current()->position_after),
                        "End from the main game reaches the default played-line leaf");
            expect_true(end_side.select(branch_c5), "side line can be selected before End");
            end_side.to_end();
            expect_true(end_side.current() == side_d6,
                        "End in a side line follows its default continuation");
            expect_true(end_side_middle.select(side_nf3),
                        "middle of a side line can be selected before End");
            end_side_middle.to_end();
            expect_true(end_side_middle.current() == side_d6,
                        "End in the middle of a side line reaches that line's last move");
            expect_true(end_side.select(side_nc6), "nested alternative can be selected before End");
            end_side.to_end();
            expect_true(end_side.current() == side_d4,
                        "End follows a selected nested continuation");
            end_side.to_end();
            expect_true(end_side.current() == side_d4,
                        "End at an existing leaf keeps the same selected move");
            expect_true(nested_up.select(side_d4) && nested_up.leave_variation() &&
                        nested_up.current() == side_nf3 && nested_up.forward() &&
                        nested_up.current() == side_d6,
                        "Right after leaving a nested alternative follows the parent line");

            expect_true(unrelated_choice.select(branch_e5) &&
                        unrelated_choice.enter_variation(1) &&
                        unrelated_choice.current() == main_bc4 &&
                        unrelated_choice.select(branch_c5) &&
                        unrelated_choice.leave_variation() &&
                        unrelated_choice.current() == branch_e4 &&
                        unrelated_choice.forward() &&
                        unrelated_choice.current() == branch_e5 &&
                        unrelated_choice.selected_child_index() == 1 &&
                        unrelated_choice.forward() &&
                        unrelated_choice.current() == main_bc4,
                        "leaving one alternative preserves a remembered choice at an unrelated branch");
        }
    }

    {
        PgnDocument down_document;
        NotationLayout down_layout;
        const std::string down_pgn =
            "1. e4 e5 (1... c5 2. Nf3 d6 (2... Nc6 3. d4 "
            "{A note after the nested line.}) 3. Bb5+ Bd7) "
            "{A note between sibling lines.} "
            "(1... e6 2. d4 d5) 2. Nf3 (2. Bc4) Nc6 3. Bb5 a6 *";
        expect_true(gambitree::PgnParser::parse(down_pgn, down_document) &&
                    down_document.games.size() == 1,
                    "Down-arrow display-order fixture parses");
        if (!down_document.games.empty()) {
            PgnGame& down_game = *down_document.games[0];
            MoveNode* down_e4 = down_game.root->children[0];
            MoveNode* down_e5 = down_e4->children[0];
            MoveNode* c5 = down_e4->children[1];
            MoveNode* e6 = down_e4->children.size() > 2 ? down_e4->children[2] : 0;
            MoveNode* side_nf3 = c5->children[0];
            MoveNode* side_d6 = side_nf3->children[0];
            MoveNode* inner_nc6 = side_nf3->children[1];
            MoveNode* inner_d4 = inner_nc6->children[0];
            MoveNode* outer_bb5 = side_d6->children.empty()
                ? 0 : side_d6->children[0];
            MoveNode* outer_bd7 = outer_bb5 && !outer_bb5->children.empty()
                ? outer_bb5->children[0] : 0;
            MoveNode* outer_resume = down_e5->children[0];
            std::vector<const MoveNode*> order;
            MoveNavigator down_from_first(down_game);
            MoveNavigator down_from_middle(down_game);
            MoveNavigator down_from_last(down_game);
            MoveNavigator down_nested(down_game);
            MoveNavigator down_mainline(down_game);
            int sibling_line_y = -1;
            int sibling_line_scroll;
            std::size_t item_index;

            down_layout.build(down_game, 240, 8, 18);
            order = displayed_move_order(down_layout);
            for (item_index = 0; item_index < down_layout.items().size();
                 ++item_index)
                if (down_layout.items()[item_index].node == e6 &&
                    down_layout.items()[item_index].text == e6->san)
                    sibling_line_y = down_layout.items()[item_index].y;
            expect_true(e6 && outer_bb5 &&
                        outer_bd7 && outer_resume,
                        "Down-arrow fixture exposes expected tree nodes");

            expect_true(down_from_first.select(c5) &&
                        down_from_first.next_after_variation(order) &&
                        down_from_first.current() == e6 &&
                        down_from_first.position().equals(e6->position_after),
                        "Down from a variation start skips its full subtree to the next displayed sibling line");
            sibling_line_scroll = down_layout.ensure_visible(
                e6, down_layout.max_scroll(36), 36);
            expect_true(sibling_line_y >= sibling_line_scroll &&
                        sibling_line_y + 18 <= sibling_line_scroll + 36,
                        "the selected Down-arrow destination can be kept visible by the shared scroll helper");
            expect_true(down_from_middle.select(side_nf3) &&
                        down_from_middle.next_after_variation(order) &&
                        down_from_middle.current() == e6,
                        "Down from the middle of a variation reaches the same next outside move");
            expect_true(down_from_last.select(c5) &&
                        down_from_last.forward() &&
                        down_from_last.forward() &&
                        down_from_last.forward() &&
                        down_from_last.next_after_variation(order) &&
                        down_from_last.current() == e6,
                        "Down from the last move of a variation reaches the same next outside move");
            expect_true(down_nested.select(inner_nc6) &&
                        down_nested.next_after_variation(order) &&
                        down_nested.current() == outer_bb5 &&
                        down_nested.position().equals(outer_bb5->position_after),
                        "Down from a nested line skips its nested alternatives and selects the next move in the enclosing line");
            expect_true(down_nested.select(inner_d4) &&
                        down_nested.next_after_variation(order) &&
                        down_nested.current() == outer_bb5 &&
                        down_nested.position().equals(outer_bb5->position_after),
                        "Down from the last nested-line move skips comments and returns to the enclosing line");
            expect_true(down_nested.select(inner_nc6) &&
                        down_nested.next_after_variation(order) &&
                        down_nested.current() == outer_bb5 &&
                        down_nested.forward() &&
                        down_nested.current() == outer_bd7,
                        "Right after a Down jump follows the target line preference");
            expect_true(down_mainline.select(down_e5) &&
                        down_mainline.next_after_variation(order) &&
                        down_mainline.current() == c5,
                        "Down on the played line retains the existing next-alternative behavior");
            expect_true(down_game.root->children[0] == down_e4 &&
                        down_e4->children[0] == down_e5 &&
                        down_e5->children[0] == outer_resume,
                        "Down display-order navigation does not change the played move tree");
        }
    }

    {
        PgnDocument terminal_document;
        NotationLayout terminal_layout;
        const std::string terminal_pgn =
            "1. e4 e5 (1... c5) *";
        expect_true(gambitree::PgnParser::parse(terminal_pgn, terminal_document) &&
                    terminal_document.games.size() == 1,
                    "terminal alternative fixture parses");
        if (!terminal_document.games.empty()) {
            PgnGame& terminal_game = *terminal_document.games[0];
            MoveNode* e4_move = terminal_game.root->children[0];
            MoveNode* c5_move = e4_move->children.size() > 1
                ? e4_move->children[1] : 0;
            MoveNavigator terminal_cursor(terminal_game);
            std::vector<const MoveNode*> order;
            terminal_layout.build(terminal_game, 240, 8, 18);
            order = displayed_move_order(terminal_layout);
            expect_true(c5_move && terminal_cursor.select(c5_move) &&
                        !terminal_cursor.next_after_variation(order) &&
                        terminal_cursor.current() == c5_move &&
                        terminal_cursor.position().equals(c5_move->position_after),
                        "Down with no later selectable move leaves selection and board unchanged");
        }
    }

    {
        PgnDocument scroll_document;
        NotationLayout scroll_layout;
        const std::string scroll_pgn =
            "1. e4 e5 {A long explanation that wraps across several lines "
            "before the played line continues and leaves the early variation "
            "well above the end of the notation pane.} (1... c5) "
            "2. Nf3 Nc6 3. Bb5 a6 4. Ba4 Nf6 5. O-O Be7 "
            "6. Re1 b5 7. Bb3 d6 *";
        expect_true(gambitree::PgnParser::parse(scroll_pgn, scroll_document) &&
                    scroll_document.games.size() == 1,
                    "navigation scroll regression fixture parses");
        if (!scroll_document.games.empty()) {
            PgnGame& scroll_game = *scroll_document.games[0];
            MoveNode* e4_move = scroll_game.root->children[0];
            MoveNode* e5_move = e4_move->children[0];
            MoveNode* c5_move = e4_move->children[1];
            MoveNode* nf3_move = e5_move->children[0];
            MoveNode* nc6_move = nf3_move->children[0];
            MoveNode* mainline_end = scroll_game.root;
            MoveNavigator scroll_navigator(scroll_game);
            int scroll_offset;
            int c5_y = -1;
            int c5_x = 0;
            int nf3_y = -1;
            int nf3_x = 0;
            bool moved;
            bool show_end;
            std::size_t item_index;
            const MoveNode* clicked;
            const int viewport_height = 36;
            scroll_layout.build(scroll_game, 128, 8, 18);
            while (!mainline_end->children.empty())
                mainline_end = mainline_end->children[0];
            for (item_index = 0; item_index < scroll_layout.items().size();
                 ++item_index) {
                if (scroll_layout.items()[item_index].node == c5_move) {
                    c5_y = scroll_layout.items()[item_index].y;
                    c5_x = scroll_layout.items()[item_index].x;
                }
                if (scroll_layout.items()[item_index].node == nf3_move) {
                    nf3_y = scroll_layout.items()[item_index].y;
                    nf3_x = scroll_layout.items()[item_index].x;
                }
            }
            expect_true(!gambitree::is_mainline_end(scroll_game, c5_move),
                        "the end of a short RAV is not the played-line notation end");
            expect_true(gambitree::is_mainline_end(scroll_game, mainline_end),
                        "the last played move remains recognized after a RAV and long comment");
            scroll_offset = scroll_layout.ensure_visible(nf3_move, 0,
                                                         viewport_height);
            clicked = scroll_layout.hit_test_visible(
                nf3_x + 1, nf3_y - scroll_offset + 1, scroll_offset,
                viewport_height);
            expect_true(clicked == nf3_move &&
                        scroll_navigator.select(clicked) &&
                        scroll_navigator.forward() &&
                        scroll_navigator.current() == nc6_move &&
                        scroll_navigator.position().equals(nc6_move->position_after),
                        "forward after direct selection follows the selected move's real continuation and FEN");
            scroll_offset = scroll_layout.ensure_visible(
                c5_move, scroll_layout.max_scroll(viewport_height),
                viewport_height);
            expect_true(c5_y >= scroll_offset &&
                        c5_y + 18 <= scroll_offset + viewport_height &&
                        scroll_offset < scroll_layout.max_scroll(viewport_height),
                        "selecting an early RAV from the end scrolls toward its move without an end jump");
            clicked = scroll_layout.hit_test_visible(
                c5_x + 1, c5_y - scroll_offset + 1, scroll_offset,
                viewport_height);
            expect_true(clicked == c5_move && scroll_navigator.select(clicked),
                        "the visible RAV move click selects its own tree identity");
            moved = scroll_navigator.forward();
            show_end = !moved && gambitree::is_mainline_end(
                scroll_game, scroll_navigator.current());
            if (show_end)
                scroll_offset = scroll_layout.ensure_end_visible(
                    scroll_offset, viewport_height);
            expect_true(!moved && !show_end &&
                        scroll_navigator.current() == c5_move &&
                        scroll_navigator.position().equals(c5_move->position_after) &&
                        c5_y >= scroll_offset &&
                        c5_y + 18 <= scroll_offset + viewport_height,
                        "forward at a RAV leaf leaves cursor visible and board at its legal position");
            expect_true(scroll_navigator.back() &&
                        scroll_navigator.current() == e4_move &&
                        scroll_navigator.forward() &&
                        scroll_navigator.current() == e5_move &&
                        scroll_navigator.position().equals(e5_move->position_after),
                        "back/forward at a RAV boundary exits to the played continuation");
            expect_true(scroll_navigator.select(mainline_end),
                        "the actual played-line end can be selected independently of the RAV leaf");
            moved = scroll_navigator.forward();
            show_end = !moved && gambitree::is_mainline_end(
                scroll_game, scroll_navigator.current());
            if (show_end)
                scroll_offset = scroll_layout.ensure_visible(
                    scroll_navigator.current(), scroll_offset,
                    viewport_height);
            expect_true(!moved && show_end &&
                        scroll_offset == scroll_layout.max_scroll(viewport_height) &&
                        scroll_navigator.position().equals(
                            mainline_end->position_after),
                        "selection-aware visibility remains bounded when the selected end move is already at the document end");
        }
    }

    {
        PgnDocument trailing_document;
        NotationLayout trailing_layout;
        std::ostringstream trailing_pgn;
        int scroll;
        int end_y = -1;
        std::size_t item_index;
        const int viewport = 42;
        trailing_pgn << "1. e4 e5 2. Nf3 Nc6 * {";
        for (int comment_line = 0; comment_line < 80; ++comment_line)
            trailing_pgn << "Long post-game commentary remains readable after the final move. ";
        trailing_pgn << "}";
        expect_true(gambitree::PgnParser::parse(trailing_pgn.str(), trailing_document) &&
                    trailing_document.games.size() == 1,
                    "post-result commentary navigation fixture parses as one game");
        if (!trailing_document.games.empty()) {
            PgnGame& trailing_game = *trailing_document.games[0];
            MoveNavigator cursor(trailing_game);
            const MoveNode* final_move = trailing_game.root->children[0]
                ->children[0]->children[0]->children[0];
            trailing_layout.build(trailing_game, 128, 8, 18);
            for (item_index = 0; item_index < trailing_layout.items().size();
                 ++item_index)
                if (trailing_layout.items()[item_index].node == final_move)
                    end_y = trailing_layout.items()[item_index].y;
            scroll = trailing_layout.max_scroll(viewport);
            expect_true(final_move != 0,
                        "final played move is present before End visibility check");
            cursor.to_end();
            scroll = trailing_layout.ensure_visible(cursor.current(), scroll,
                                                    viewport);
            expect_true(cursor.current() == final_move && end_y >= scroll &&
                        end_y + 18 <= scroll + viewport &&
                        scroll < trailing_layout.max_scroll(viewport),
                        "End keeps the final chess move visible instead of scrolling through trailing comments");
            scroll = trailing_layout.max_scroll(viewport);
            expect_true(!cursor.forward() && cursor.current() == final_move,
                        "Right at the final move does not select text after the game");
            scroll = trailing_layout.ensure_visible(cursor.current(), scroll,
                                                    viewport);
            expect_true(end_y >= scroll && end_y + 18 <= scroll + viewport &&
                        scroll < trailing_layout.max_scroll(viewport),
                        "Right at a leaf brings the selected move back into view without jumping to trailing text");
        }
    }

    document.clear();
    {
        std::ifstream input("tests/data/adversarial.pgn");
        std::ostringstream content;
        content << input.rdbuf();
        expect_true(gambitree::PgnParser::parse(content.str(), document), "adversarial PGN parses for navigation");
    }
    game = document.games[0];
    e4 = game->root->children[0];
    d4 = game->root->children[1];
    {
        MoveNode* nested = d4->children.size() > 1 ? d4->children[1] : 0;
        NotationLayout real_layout;
        MoveNavigator mouse_selected(*game);
        real_layout.build(*game, 120, 8, 18);
        expect_true(nested && nested->comments.size() == 1 && nested->comments[0] == "nested comment",
                    "real regression PGN retains nested RAV comment for navigation");
        expect_true(d4->nags.size() == 1 && d4->nags[0] == 5,
                    "real regression PGN retains variation NAG for navigation");
        expect_true(nested && mouse_selected.select(nested) && mouse_selected.position().equals(nested->position_after),
                    "mouse-equivalent selection enters nested RAV with its stored position");
        expect_true(mouse_selected.forward() && mouse_selected.current()->san == "c4" && mouse_selected.back() &&
                    mouse_selected.forward() && mouse_selected.current()->san == "c4",
                    "keyboard forward/back after real-PGN variation selection preserves branch context");
        expect_true(real_layout.ensure_visible(nested, 0, 18) >= 0,
                    "real regression notation remains navigable with comments and NAGs present");
    }
    MoveNavigator complex_navigator(*game);
    expect_true(complex_navigator.enter_variation(1) && complex_navigator.current() == d4, "enter variation selects root sibling");
    expect_true(complex_navigator.position().to_fen() == "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq d3 0 1", "root variation exposes d4 position");
    expect_true(complex_navigator.forward() && complex_navigator.current()->san == "d5", "variation forward follows its main child");
    expect_true(complex_navigator.position().to_fen() == "rnbqkbnr/ppp1pppp/8/3p4/3P4/8/PPP1PPPP/RNBQKBNR w KQkq d6 0 2", "variation main child exposes d5 position");
    expect_true(complex_navigator.next_sibling() && complex_navigator.current()->san == "Nf6", "next sibling enters nested variation");
    expect_true(complex_navigator.sibling_index() == 1, "nested variation has sibling index one");
    expect_true(complex_navigator.position().to_fen() == "rnbqkb1r/pppppppp/5n2/8/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 1 2", "nested variation exposes Nf6 position");
    expect_true(complex_navigator.forward() && complex_navigator.current()->san == "c4", "forward enters subvariation continuation");
    expect_true(complex_navigator.position().to_fen() == "rnbqkb1r/pppppppp/5n2/8/2PP4/8/PP2PPPP/RNBQKBNR b KQkq c3 0 2", "subvariation exposes c4 position");
    expect_true(complex_navigator.forward() && complex_navigator.current()->san == "e6", "forward enters nested main line");
    expect_true(complex_navigator.next_sibling() && complex_navigator.current()->san == "g6", "next sibling enters deeply nested variation");
    expect_true(complex_navigator.position().to_fen() == "rnbqkb1r/pppppp1p/5np1/8/2PP4/8/PP2PPPP/RNBQKBNR w KQkq - 0 3", "deep sibling exposes g6 position");
    expect_true(!complex_navigator.next_sibling(), "last sibling has no next sibling");
    expect_true(complex_navigator.previous_sibling() && complex_navigator.current()->san == "e6", "previous sibling restores nested main line");
    expect_true(complex_navigator.position().to_fen() == "rnbqkb1r/pppp1ppp/4pn2/8/2PP4/8/PP2PPPP/RNBQKBNR w KQkq - 0 3", "nested main line exposes e6 position");
    expect_true(complex_navigator.next_sibling() && complex_navigator.current()->san == "g6", "sibling switch is repeatable");
    expect_true(complex_navigator.back() && complex_navigator.current()->san == "c4", "back returns from deep variation");
    expect_true(complex_navigator.forward() && complex_navigator.current()->san == "e6", "Left out of a nested side line resumes its parent continuation");
    expect_true(complex_navigator.next_sibling() && complex_navigator.current()->san == "g6", "the nested side line can be selected again");
    expect_true(complex_navigator.back() && complex_navigator.back() && complex_navigator.current()->san == "Nf6", "back returns from subvariation to nested variation");
    expect_true(complex_navigator.forward() && complex_navigator.current()->san == "c4", "forward preserves subvariation context");
    expect_true(complex_navigator.back() && complex_navigator.back() && complex_navigator.current() == d4 &&
                complex_navigator.selected_child_index() == 0,
                "Left out of a root variation returns to the branch point and resets only that choice");
    expect_true(complex_navigator.back() && complex_navigator.at_root(),
                "Left from the first root alternative move returns to the game start");
    expect_true(complex_navigator.forward() && complex_navigator.current() == e4,
                "Right after leaving a root variation follows the played game");
    {
        MoveNavigator sibling_switch(*game);
        expect_true(sibling_switch.enter_variation(1) && sibling_switch.current() == d4 &&
                    sibling_switch.previous_sibling() && sibling_switch.current() == e4 &&
                    sibling_switch.next_sibling() && sibling_switch.current() == d4,
                    "explicit sibling switching remains available after branch-exit navigation");
    }
}


// Child-index strings identify exact nodes even when SAN and FEN repeat.
const MoveNode* node_on_path(const PgnGame& game, const std::string& path)
{
    const MoveNode* node = game.root;
    for (std::size_t i = 0; i < path.size(); ++i) {
        std::size_t index = static_cast<std::size_t>(path[i] - '0');
        if (index >= node->children.size()) return 0;
        node = node->children[index];
    }
    return node;
}

void expect_cursor(MoveNavigator& cursor, const MoveNode* target,
                   const char* fen, const char* label)
{
    expect_true(target && cursor.current() == target &&
                cursor.position().to_fen() == fen, label);
    if (!target || cursor.current() != target) return;
    // Every edge of the active context must remember this exact continuation.
    const MoveNode* node = target;
    while (node->parent) {
        const MoveNode* parent = node->parent;
        std::size_t index = 0;
        while (index < parent->children.size() && parent->children[index] != node) ++index;
        expect_true(cursor.select(parent) && cursor.selected_child_index() == index,
                    "Down target retains the correct branch preferences along its path");
        node = parent;
    }
    cursor.select(target);
}

// Node paths below belong to the original f892702 snapshot. The independently
// updated example promotes RAVs and therefore has a different navigation tree.
const char* const kCarlsenNavigationFixture = "tests/data/carlsen-pelletier-2007-navigation.pgn";

void test_carlsen_pelletier_navigation()
{
    // Reuse the existing read-only SHA-256 capture; this never deletes a file.
    const gambitree::PgnDeleteBaseline snapshot = gambitree::capture_pgn_delete_baseline(kCarlsenNavigationFixture);
    const char* hex = "0123456789abcdef";
    std::string digest;
    for (std::size_t i = 0; i < sizeof(snapshot.sha256); ++i) {
        digest += hex[snapshot.sha256[i] >> 4];
        digest += hex[snapshot.sha256[i] & 15];
    }
    expect_true(snapshot.eligible && digest == "f204e53e9b29fd318b55f0f5b9dbccb1b54aa73cbd7ad2f52f55d562a4d52195",
                "immutable Carlsen-Pelletier navigation fixture matches its pinned SHA-256 (including RAV order)");
    std::ifstream in(kCarlsenNavigationFixture);
    std::ostringstream pgn;
    pgn << in.rdbuf();
    PgnDocument document;
    expect_true(in.good() && gambitree::PgnParser::parse(pgn.str(), document) &&
                document.games.size() == 1 && document.warnings.empty() &&
                document.skipped_variation_count == 0,
                "Carlsen-Pelletier snapshot parses without lost analysis");
    if (document.games.empty()) return;
    PgnGame& game = *document.games[0];
    NotationLayout layout;
    layout.build(game, 532, 8, 18);
    std::vector<const MoveNode*> order = displayed_move_order(layout);
    struct Jump { const char* from; const char* to; const char* fen; };
    const Jump jumps[] = {
        {"0000000000000000000010000000", "0000000000000000000010000001", "rn1r2k1/1b3p1p/3ppp2/p1p5/2P5/P1N1P3/1P3PPP/2KR1B1R w - - 0 15"}, // 14... Kf8 -> d6
        {"0000000000000000000010000001", "00000000000000000000100000000", "rn1r1k2/1b1p1p1p/4pp2/p1p5/N1P5/P3P3/1P3PPP/2KR1B1R b - - 5 15"}, // 14... d6 -> Na4
        {"00000000000000000000100000000", "0000000000000000000000", "rn1q1rk1/1b3ppp/3ppn2/p1p3B1/2P5/P1Q1PP2/1P2N1PP/R3KB1R w KQ - 0 12"}, // 15. Na4 -> d6
        {"0000000000000000000010000001000", "0000000000000000000010000001001", "rn1r1k2/1b3p1B/3ppp2/p1p5/2P5/P1N1P3/1P3PPP/2KR3R b - - 0 16"}, // 16. Rd2 -> Bxh7
        {"0000000000000000000010000001001", "0000000000000000000010000001002", "rn1r1k2/1b3p1p/3ppp2/p1p5/2P1B3/P1N1P3/1P3PPP/2KR3R b - - 3 16"}, // 16. Bxh7 -> Be4
        {"0000000000000000000010000001001000000000100000000000100010000000000010000", "0000000000000000000010000001001000000000100000000000100010000000000010001", "7k/8/8/R1p5/3n1P2/1P6/7r/1K6 b - - 2 37"}, // 37. Ra3 -> Kb1
        {"0000000000000000000010000001001000000000100000000000100010000000000010001", "0000000000000000000010000001001000000000100000000000100010000000000010002", "R6k/8/8/2p5/3n1P2/1P6/7r/2K5 b - - 2 37"}, // 37. Kb1 -> Ra8+
        {"000000000000000000001000000100100000000010000000000010001000000000001000100", "0000000000000000000010000001001000000000100000000000100010000000000010002", "R6k/8/8/2p5/3n1P2/1P6/7r/2K5 b - - 2 37"}, // 38. Rb5 -> Ra8+
        {"0000000000000000000010000001001000000000100000000000100010000000000010001000001", "00000000000000000000100000010010000000001000000000001000100000000000100010000000", "8/6k1/1R6/5P2/2p5/1n6/5r2/1K6 w - - 3 41"}, // 40. f6+ -> Rf2
        {"000000000000000000001000000100100000000010000000000010001000000000001000100000100000000000000000", "00000000000000000000100000010010000000001000000000001000100000000000100010000000", "8/6k1/1R6/5P2/2p5/1n6/5r2/1K6 w - - 3 41"}, // 48... Kd2 -> Rf2
    };
    const int widths[] = {192, 532, 1701};
    for (std::size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        layout.build(game, widths[w], 8, 18);
        expect_true(displayed_move_order(layout) == order,
                    "resizing deep analysis preserves every selectable MoveNode in reading order");
        for (std::size_t i = 0; i < sizeof(jumps) / sizeof(jumps[0]); ++i) {
            MoveNavigator cursor(game);
            const MoveNode* from = node_on_path(game, jumps[i].from);
            const MoveNode* to = node_on_path(game, jumps[i].to);
            expect_true(from && to && cursor.select(from) &&
                        cursor.next_after_variation(order), "real deep-analysis Down jump succeeds");
            expect_cursor(cursor, to, jumps[i].fen,
                          "real Down target has exact node identity, FEN, and variant context");
            int scroll = layout.ensure_visible(to, layout.max_scroll(180), 180);
            expect_true(layout.ensure_visible(to, scroll, 180) == scroll,
                        "selection-aware scrolling settles without repeated jumps");
            bool visible = false;
            for (std::size_t j = 0; j < layout.items().size(); ++j) {
                const gambitree::NotationItem& item = layout.items()[j];
                if (item.kind == gambitree::NOTATION_MOVE && item.node == to &&
                    item.y >= scroll && item.y + item.height <= scroll + 180)
                    visible = true;
            }
            expect_true(visible, "Down highlight remains wholly visible at narrow and wide pane sizes");
        }
    }
    const MoveNode* kf8 = node_on_path(game, jumps[0].from);
    const MoveNode* d6 = node_on_path(game, jumps[0].to);
    const MoveNode* unrelated = node_on_path(game, jumps[6].to);
    if (!kf8 || !d6 || !unrelated) return;
    MoveNavigator cursor(game);
    cursor.select(unrelated); // Remember a different, much deeper branch.
    cursor.select(kf8);
    expect_true(cursor.next_after_variation(order) && cursor.current() == d6 &&
                cursor.forward() && cursor.current() == d6->children[0] &&
                cursor.position().to_fen() ==
                    "rn1r2k1/1b3p1p/3ppp2/p1p5/2P5/P1NBP3/1P3PPP/2KR3R b - - 1 15",
                "Kf8 Down Right enters the actual 15.Bd3 continuation");
    expect_true(cursor.leave_variation() && cursor.current() == kf8->parent &&
                cursor.selected_child_index() == 0 && cursor.forward() && cursor.current() == kf8,
                "Up after Down Right leaves exactly the inner alternative");
    expect_true(cursor.next_after_variation(order) && cursor.back() &&
                cursor.current() == kf8->parent && cursor.selected_child_index() == 0 &&
                cursor.forward() && cursor.current() == kf8,
                "Left out of Down-selected d6 clears only that alternative preference");
    cursor.select(unrelated->parent);
    expect_true(cursor.selected_child_index() == 2 && cursor.forward() &&
                cursor.current() == unrelated,
                "Down and boundary exits preserve an unrelated deep sibling choice");
    MoveNavigator end_cursor(game);
    end_cursor.select(d6);
    end_cursor.to_end();
    const MoveNode* leaf = end_cursor.current();
    expect_true(leaf && leaf->san == "h6" && leaf->children.empty(),
                "End still follows the selected analysis to its legal leaf");
    int scroll = layout.ensure_visible(leaf, 0, 180);
    expect_true(layout.ensure_visible(leaf, scroll, 180) == scroll,
                "End selection remains stable in the widened notation");

    // All three numeric $10 annotations stay in the model and get spaced verdicts.
    int equal_count = 0;
    std::string comments;
    for (std::size_t i = 0; i < layout.items().size(); ++i) {
        const gambitree::NotationItem& item = layout.items()[i];
        if (item.kind == gambitree::NOTATION_NAG && item.node &&
            item.node->nags.size() == 1 && item.node->nags[0] == 10) {
            ++equal_count;
            expect_true(item.text == "=" && i > 0 &&
                        layout.items()[i-1].node == item.node &&
                        item.x == layout.items()[i-1].x + layout.items()[i-1].width + 8,
                        "every real $10 displays a spaced equals sign and remains numeric internally");
        }
        if (item.kind == gambitree::NOTATION_COMMENT) {
            if (!comments.empty()) comments += ' ';
            comments += item.text;
        }
    }
    expect_true(equal_count == 3, "all three Carlsen-Pelletier $10 verdicts are tested");
    expect_true(comments.find("16. Be4 Bxe4 17. Nxe4") != std::string::npos &&
                comments.find("Rxe5") != std::string::npos &&
                comments.find("23.Rg1, 24.Rh4") != std::string::npos &&
                comments.find("Rxc4 kann wegen Nd2+") != std::string::npos &&
                comments.find("DIAGRAM GEVONDEN") != std::string::npos &&
                comments.find("[%cal") == std::string::npos &&
                comments.find("König") != std::string::npos,
                "real comments retain German prose, diagram text, order, and readable legacy piece references");
    const MoveNode* second_kf8 = d6->children[0]->children[0];
    expect_true(second_kf8->comments.size() == 2 &&
                second_kf8->comments[0].find("16. ¥e4 ¥xe4 17. ¤xe4") != std::string::npos &&
                second_kf8->comments[1].find("DIAGRAM GEVONDEN") != std::string::npos &&
                second_kf8->following_items[0].comment == second_kf8->comments[0],
                "real original comment strings and ordered PGN items remain byte-for-byte intact");
}



class FigurineTestMeasurer : public gambitree::NotationTextMeasurer {
public:
    int text_width(const std::string& text) const {
        int width = 0;
        for (std::size_t i = 0; i < text.size();) {
            unsigned char lead = static_cast<unsigned char>(text[i]);
            std::size_t end = i + 1;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) ++end;
            width += lead == 0xe2U && text.substr(i, 2) == "\342\231" ? 14 : 8;
            i = end;
        }
        return width;
    }
};

void test_notation_preferences()
{
    gambitree::SessionState state = gambitree::default_session_state(), restored;
    expect_true(state.board_share == 0 && !state.figurines &&
                state.board_theme_id == "classic" &&
                SidebarLayout::width_for_share(1920, 0) == 0,
                "first start retains automatic panels and letter notation");
    expect_true(gambitree::parse_session_state("version=1\nwidth=900\npieces=gambitree\n", restored) &&
                restored.board_share == 0 && !restored.figurines &&
                gambitree::parse_session_state("version=2\nwidth=1200\n", restored) &&
                restored.board_share == 0 && !restored.figurines,
                "both older settings formats retain safe defaults for absent preferences");
    state.board_share = SidebarLayout::board_share(1920, 1440);
    state.figurines = true;
    state.board_theme_id = "wood";
    expect_true(state.board_share == 2500 &&
                gambitree::parse_session_state(gambitree::serialize_session_state(state), restored) &&
                restored.board_share == 2500 && restored.figurines &&
                restored.board_theme_id == "wood",
                "normalized panel ratio, figurine style and board theme survive serialization");
    gambitree::SessionState corrupt;
    expect_true(gambitree::parse_session_state("version=2\nboard_theme=broken\n", corrupt) == false &&
                corrupt.board_theme_id == "classic",
                "unknown saved board themes safely fall back to the classic board");
    const int widths[] = {1920, 1200, 640, 300, 1, 1920, 900, 1920};
    for (std::size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        int preferred = SidebarLayout::width_for_share(widths[i], restored.board_share);
        SidebarGeometry side = SidebarLayout::calculate(widths[i], preferred);
        expect_true(side.left >= 0 && side.width >= 0 &&
                    side.width <= SidebarLayout::maximum_width(widths[i]) &&
                    restored.board_share == 2500,
                    "automatic resize/fullscreen clamping never overwrites the deliberate ratio");
        if (widths[i] >= 900)
            expect_true(side.left == widths[i] / 4,
                        "saved board share scales proportionally across large and normal windows");
        if (widths[i] == 640)
            expect_true(side.left == 191 && side.width == 449,
                        "narrow resize retains the minimum square board while keeping its unclamped preference");
    }
    restored.board_share = SidebarLayout::board_share(1200, 600);
    expect_true(restored.board_share == 5000 && SidebarLayout::width_for_share(1920, restored.board_share) == 960,
                "a fresh manual drag replaces the saved ratio at the current width");
    const char* bad[] = {"-1", "10000", "9999999999999999999999", "0.25", "broken", ""};
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        expect_true(!gambitree::parse_session_state(std::string("board_share=") + bad[i] +
                    "\nfigurines=unknown\nwidth=1200\n", restored) &&
                    restored.board_share == 0 && !restored.figurines && restored.width == 1200,
                    "corrupt new preferences fall back per field without losing valid settings");
    expect_true(!gambitree::parse_session_state("board_share=2500\nboard_share=bad\nfigurines=1\nfigurines=2\n", restored) &&
                restored.board_share == 0 && !restored.figurines,
                "invalid duplicate preferences safely override earlier values");
    state.board_share = 10000;
    expect_true(gambitree::parse_session_state(gambitree::serialize_session_state(state), restored) &&
                restored.board_share == 0, "invalid in-memory shares serialize safely as automatic");
    state.board_share = 2500;
    const std::string directory = test_scratch_directory + "/notation-preferences";
    const std::string path = directory + "/state";
    expect_true(gambitree::save_session_state(path, state) &&
                gambitree::load_session_state(path, restored) == gambitree::SESSION_STATE_LOADED &&
                restored.board_share == 2500 && restored.figurines &&
                restored.board_theme_id == "wood",
                "both preferences survive the real atomic settings write and reload");
    std::remove(path.c_str());
    rmdir(directory.c_str());
}


void test_keyboard_help()
{
    using namespace gambitree;
    expect_true(keyboard_help_requested(XK_question,0,0) &&
                keyboard_help_requested(XK_slash,"?",1) &&
                keyboard_help_requested(XK_a,"?",1) &&
                !keyboard_help_requested(XK_slash,"/",1) &&
                !keyboard_help_requested(NoSymbol,0,0),
                "help recognizes the translated question mark without assuming a physical slash key");
    KeyboardHelpState help;
    expect_true(!help.key(XK_Escape,false,false,26,260,520) && !help.visible,
                "Escape without help passes through to the existing quit handler");
    expect_true(help.key(XK_question,true,false,26,260,520) && help.visible && help.scroll==0,
                "question opens keyboard help");
    const KeySym blocked[]={XK_Right,XK_Left,XK_Up,XK_Down,XK_Home,XK_End,
        XK_Page_Up,XK_Page_Down,XK_g,XK_G,XK_plus,XK_equal,XK_minus,XK_KP_Add,XK_KP_Subtract,
        XK_a,XK_f,XK_p,XK_q,XK_Q,XK_Delete,XK_Return,XK_y,XK_Tab};
    for(std::size_t i=0;i<sizeof(blocked)/sizeof(blocked[0]);++i)
        expect_true(help.key(blocked[i],false,false,26,260,520) && help.visible,
                    "visible help consumes every ordinary command, including quit and removal");
    expect_true(help.key(XK_question,true,false,26,260,520) && !help.visible,
                "second question closes help");
    help.key(XK_question,true,false,26,260,520);
    expect_true(help.key(XK_Escape,false,false,26,260,520) && !help.visible,
                "Escape closes help and is consumed instead of quitting");
    expect_true(!help.key(XK_question,true,true,26,260,520) && !help.visible,
                "an existing confirmation keeps priority and cannot be replaced by help");
    help.key(XK_question,true,false,26,260,520);
    help.key(XK_Down,false,false,26,260,520);
    expect_true(help.scroll==26,"Down scrolls only the help by one full text line");
    help.key(XK_Page_Down,false,false,26,260,520);
    expect_true(help.scroll==260,"Page Down keeps one line of overlap");
    help.key(XK_End,false,false,26,260,520);
    help.scroll_by(1000,520);
    expect_true(help.scroll==520,"help scroll is bounded at the last line");
    help.key(XK_Home,false,false,26,260,520);
    help.scroll_by(-1000,520);
    expect_true(help.scroll==0,"Home and scrolling upward are bounded at the top");
    std::size_t count; const KeyboardHelpEntry* entries=keyboard_help_entries(count);
    expect_true(count==31,"help includes chooser, search, appearance, main-window, scrolling and confirmation controls");
    std::string all;
    for(std::size_t i=0;i<count;++i) {
        expect_true(entries[i].group[0] && entries[i].keys[0] && entries[i].description[0],
                    "all help entries have a group, a key and a readable explanation");
        all+=std::string(entries[i].keys)+" "+entries[i].description+"\n";
    }
    expect_true(all.find("Next alternative here; if none and inside an alternative, skip past it.")!=std::string::npos &&
                all.find("Cycle Classic, Wood and Wood Veneer boards.")!=std::string::npos &&
                all.find("Ctrl + =")!=std::string::npos && all.find("Shift + Tab")!=std::string::npos &&
                all.find("Shift + Page Up / Down")!=std::string::npos &&
                all.find("Ctrl + F")!=std::string::npos &&
                all.find("Shift goes to the previous match")!=std::string::npos &&
                all.find("MoveNode")==std::string::npos && all.find("RAV")==std::string::npos,
                "help describes corrected Down behavior and existing key aliases without internal jargon");
    NotationFontMetrics body(8,26), keys(9,26), title(12,32);
    const int sizes[][2]={{320,240},{640,480},{900,700},{1600,1000},{1920,1200}};
    for(std::size_t n=0;n<sizeof(sizes)/sizeof(sizes[0]);++n) {
        KeyboardHelpLayout layout=KeyboardHelpLayout::build(sizes[n][0],sizes[n][1],body,keys,title);
        expect_true(layout.x>=0 && layout.y>=0 && layout.x+layout.width<=sizes[n][0] &&
                    layout.y+layout.height<=sizes[n][1] && layout.viewport_height()>=layout.line_height,
                    "help panel and usable scrolling viewport stay inside small and large clients");
        expect_true(layout.columns==(sizes[n][0]>=1600?2:1),"help switches between one and two columns");
        if (layout.columns==2) {
            int left_description=-1, right_description=-1;
            for(std::size_t i=0;i<layout.content.size();++i)
                if(layout.content[i].kind==HELP_DESCRIPTION) {
                    if(layout.content[i].x<layout.x+layout.width/2) {
                        if(left_description<0)left_description=layout.content[i].x;
                        expect_true(layout.content[i].x==left_description,
                                    "left help descriptions share one aligned column");
                    } else {
                        if(right_description<0)right_description=layout.content[i].x;
                        expect_true(layout.content[i].x==right_description,
                                    "right help descriptions share one aligned column");
                    }
                }
        }
        for(std::size_t i=0;i<layout.fixed.size();++i) {
            const KeyboardHelpText& line=layout.fixed[i];
            expect_true(line.x>=layout.x && line.x+line.width<=layout.x+layout.width &&
                        line.y>=layout.y && line.y+line.height<=layout.y+layout.height,
                        "title, close instruction and footer fit inside the panel");
        }
        for(std::size_t i=0;i<layout.content.size();++i) {
            const KeyboardHelpText& line=layout.content[i];
            bool reachable=false;
            for(int scroll=0;scroll<=layout.max_scroll();scroll+=layout.line_height)
                if(line.y>=scroll && line.y+line.height<=scroll+layout.viewport_height())reachable=true;
            if(line.y>=layout.max_scroll() && line.y+line.height<=
               layout.max_scroll()+layout.viewport_height())reachable=true;
            expect_true(reachable && line.x>=layout.x && line.x+line.width<=layout.x+layout.width,
                        "every complete help line is reachable without horizontal clipping");
        }
    }
}

void test_navigation_lists()
{
    using namespace gambitree;
    std::vector<std::string> items;
    NavigationListState list;
    std::vector<PgnTag> tags;
    items.push_back("1. Alpha – Beta · Event Open");
    items.push_back("2. Carlsen – Anand · Event Candidates");
    items.push_back("3. TAL, Mihail – Fischer · Event Open");
    items.push_back("4. CBM 64 — Opening survey · 11.♗e3 · E99");
    list.open(NAVIGATION_LIST_GAMES, items, 2);
    list.set_visible_rows(2);
    expect_true(list.matches().size() == 4 &&
                list.selected_source_index() == 2,
                "game chooser retains original ordering and starts at current item");
    list.set_query("cArLsEn");
    expect_true(list.matches().size() == 1 &&
                list.selected_source_index() == 1,
                "navigation chooser filters labels case-insensitively");
    list.set_query("event open");
    expect_true(list.matches().size() == 2 &&
                list.selected_source_index() == 0,
                "navigation chooser filter includes event metadata in source order");
    list.set_query("");
    list.move(1);
    list.page(1);
    expect_true(list.selected_source_index() == 2 &&
                list.first_match() == 1,
                "chooser arrow and page navigation keep selected row visible");
    expect_true(list.select_match_at(0) &&
                list.selected_source_index() == 1,
                "chooser mouse-row selection maps through filtered results");
    list.end();
    expect_true(list.selected_source_index() == 3,
                "chooser End selects the final filtered item");
    list.set_query("no such item");
    expect_true(list.matches().empty() && !list.has_selection(),
                "chooser handles an empty filtered result without a phantom selection");
    {
        NavigationListState incremental;
        incremental.open(NAVIGATION_LIST_GAMES, items, 1);
        incremental.set_query("anand");
        std::vector<std::string> appended;
        appended.push_back("5. Anand – Carlsen");
        incremental.append_items(appended, 1);
        expect_true(incremental.matches().size() == 2 &&
                    incremental.selected_source_index() == 1,
                    "incremental index updates preserve the active filter and selection");
    }
    list.close();
    expect_true(!list.visible(), "chooser Escape close leaves no active selection overlay");

    tags.push_back(PgnTag("White", "CBM 64"));
    tags.push_back(PgnTag("Black", "11.♗e3 13.---"));
    tags.push_back(PgnTag("Event", "Survey"));
    tags.push_back(PgnTag("Site", "Opening"));
    tags.push_back(PgnTag("ECO", "E99"));
    expect_true(navigation_game_label(4, tags).find("Opening survey") !=
                    std::string::npos &&
                navigation_game_label(4, tags).find("E99") !=
                    std::string::npos &&
                navigation_game_label(4, tags).find("CBM 64") !=
                    std::string::npos,
                "ChessBase survey tags produce a readable survey list row");
    tags.clear();
    expect_true(navigation_game_label(9, tags) == "9. Game 9",
                "missing PGN tags have a stable numbered fallback");

    items.clear();
    for (std::size_t i = 0; i < 1000; ++i) {
        std::ostringstream label;
        label << "file " << i << ".pgn";
        items.push_back(label.str());
    }
    list.open(NAVIGATION_LIST_FILES, items, 0);
    list.set_visible_rows(8);
    list.end();
    expect_true(list.selected_source_index() == 999 &&
                list.first_match() == 992,
                "large file chooser scrolls to the last visible page");
    list.set_query("FILE 99");
    expect_true(list.matches().size() == 11 &&
                list.matches().front() == 99,
                "file chooser filters hundreds of launch arguments immediately");
    items.clear();
    items.push_back("José Raúl Capablanca");
    list.open(NAVIGATION_LIST_GAMES, items, 0);
    list.set_query("JOSÉ");
    expect_true(list.matches().size() == 1,
                "chooser case folding handles common accented Latin names");
}

void test_move_selection_geometry()
{
    const char* quality[] = {"!", "?", "!!", "??", "!?", "?!"};
    for (int style=0; style<2; ++style) for (int suffix=0; suffix<2; ++suffix)
        for (int nag=1; nag<=6; ++nag) {
            std::ostringstream input;
            input << "1. e4";
            if (suffix) input << quality[nag-1]; else input << " $" << nag;
            input << " $15 e5 *";
            PgnDocument doc;
            expect_true(gambitree::PgnParser::parse(input.str(),doc), "selection quality fixture parses");
            if(doc.games.empty()) continue;
            const MoveNode* node=doc.games[0]->root->children[0];
            expect_true(node->san=="e4" && node->nags.size()==2 && node->nags[0]==nag && node->nags[1]==15,
                        "suffix and numeric quality retain the original PGN representation");
            for (int width=40; width<=200; width+=160) {
                NotationLayout layout; layout.set_figurines(style!=0); layout.build(*doc.games[0],width,8,22);
                std::vector<gambitree::NotationSelectionRect> rects=layout.selection_rects(node);
                expect_true(!rects.empty(), "annotated move has measured selection rectangles");
                for (std::size_t i=0;i<layout.items().size();++i) {
                    const gambitree::NotationItem& item=layout.items()[i];
                    if(item.node!=node) continue;
                    bool selected=item.kind==gambitree::NOTATION_MOVE || item.move_quality;
                    expect_true(item.selects_move()==selected, "only SAN and quality select their move");
                    if(selected) {
                        expect_true(layout.hit_test(item.x+item.width-1,item.y+1,0)==node &&
                                    layout.hit_test_visible(item.x+item.width-1,1,item.y,22)==node,
                                    "rightmost quality character selects exactly the same MoveNode");
                        bool covered=false;
                        for(std::size_t r=0;r<rects.size();++r)
                            if(rects[r].y==item.y && rects[r].x<=item.x &&
                               rects[r].x+rects[r].width>=item.x+item.width) covered=true;
                        expect_true(covered, "quality annotation is wholly inside the measured selection");
                    } else {
                        expect_true(layout.hit_test(item.x+item.width-1,item.y+1,0)==0,
                                    "position verdict does not become a move hit target");
                        for(std::size_t r=0;r<rects.size();++r)
                            expect_true(rects[r].y!=item.y || rects[r].x+rects[r].width<=item.x ||
                                        rects[r].x>=item.x+item.width,
                                        "selection excludes the position verdict and its separating space");
                    }
                }
                int scroll=layout.ensure_visible(node,layout.max_scroll(88),88);
                for(std::size_t r=0;r<rects.size();++r)
                    expect_true(rects[r].y>=scroll && rects[r].y+rects[r].height<=scroll+88,
                                "scrolling includes wrapped quality annotations");
            }
        }
    const int verdicts[]={0,10,11,13,14,15,16,17,18,19,36,99};
    for(std::size_t v=0;v<sizeof(verdicts)/sizeof(verdicts[0]);++v) {
        std::ostringstream input;input<<"1. e4";
        if(verdicts[v])input<<" $"<<verdicts[v];
        input<<" *";
        PgnDocument verdict_doc;
        expect_true(gambitree::PgnParser::parse(input.str(),verdict_doc),"verdict-only fixture parses");
        if(verdict_doc.games.empty())continue;
        NotationLayout verdict_layout;verdict_layout.build(*verdict_doc.games[0],400,8,22);
        const MoveNode* node=verdict_doc.games[0]->root->children[0];
        std::vector<gambitree::NotationSelectionRect> rectangles=verdict_layout.selection_rects(node);
        expect_true(rectangles.size()==1 && rectangles[0].width==16,
                    "plain moves and every supported verdict select only SAN without added background padding");
    }
    PgnDocument doc;
    expect_true(gambitree::PgnParser::parse(
        "1. e4 $1 $6 $15 $3 e5 $10 (1... c5 $2 (1... e6 $4)) 2. Nf3 $5 Nc6 *",doc),
        "multiple and nested annotation fixture parses");
    if(doc.games.empty()) return;
    NotationLayout layout; layout.build(*doc.games[0],190,8,22);
    const std::vector<gambitree::NotationItem>& items=layout.items();
    for(std::size_t i=0;i<items.size();++i) {
        const gambitree::NotationItem& item=items[i];
        if(item.selects_move())
            expect_true(layout.hit_test(item.x+item.width-1,item.y+1,0)==item.node,
                        "nested and repeated quality annotations have their own exact hit geometry");
    }
    const MoveNode* first=doc.games[0]->root->children[0];
    expect_true(layout.selection_rects(first).size()==2,
                "a verdict between quality annotations is excluded rather than bridged by a wide highlight");
}

void test_notation_readability_settings()
{
    gambitree::SessionState state = gambitree::default_session_state(), loaded;
    expect_true(state.notation_points == 12 && state.engine_points == 12 && !state.figurines,
                "appearance defaults retain letters and independent twelve-point text sizes");
    expect_true(gambitree::parse_session_state("version=2\nboard_share=2500\nfigurines=1\n", loaded) &&
                loaded.notation_points == 12 && loaded.engine_points == 12 && loaded.board_share == 2500 && loaded.figurines,
                "older settings retain paneel and glyph preferences with default independent sizes");
    const int sizes[][2] = {{8,24},{16,10},{24,8},{12,12}};
    const std::string path = test_scratch_directory + "/readability-state";
    for (std::size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        state.notation_points=sizes[i][0]; state.engine_points=sizes[i][1];
        state.board_share=2500; state.figurines=true;
        expect_true(gambitree::parse_session_state(gambitree::serialize_session_state(state),loaded) &&
                    loaded.notation_points==sizes[i][0] && loaded.engine_points==sizes[i][1] &&
                    loaded.board_share==2500 && loaded.figurines,
                    "independent notation and analysis sizes round-trip without overwriting existing preferences");
        expect_true(gambitree::save_session_state(path,state) &&
                    gambitree::load_session_state(path,loaded)==gambitree::SESSION_STATE_LOADED &&
                    loaded.notation_points==state.notation_points && loaded.engine_points==state.engine_points,
                    "font size preferences survive the real settings write and reload");
    }
    std::remove(path.c_str());
    expect_true(!gambitree::parse_session_state("notation_points=7\nengine_points=25\nboard_share=2500\n",loaded) &&
                loaded.notation_points==12 && loaded.engine_points==12 && loaded.board_share==2500 &&
                !gambitree::parse_session_state("notation_points=huge\nengine_points=99999999999999999\n",loaded) &&
                loaded.notation_points==12 && loaded.engine_points==12,
                "corrupt and out-of-range sizes fall back per field");
    state.notation_points=-1; state.engine_points=999;
    expect_true(gambitree::parse_session_state(gambitree::serialize_session_state(state),loaded) &&
                loaded.notation_points==12 && loaded.engine_points==12,
                "serialization validates font sizes before saving");
    const std::string pv="Nf3 d5 Nbd2 R1e2 Bxe6+ e8=Q# O-O O-O-O axb8=N+ e2e4 [pending] King Black Qgarbage";
    const std::string expected="♘f3 d5 ♘bd2 ♖1e2 ♗xe6+ e8=♕# O-O O-O-O axb8=♘+ e2e4 [pending] King Black Qgarbage";
    expect_true(NotationLayout::format_san_line(pv,true)==expected &&
                NotationLayout::format_san_line(pv,false)==pv &&
                NotationLayout::format_san_line(pv,true,0)==pv &&
                NotationLayout::format_san_line("Nf3\tBxe6+\nR1e2",true)=="♘f3\t♗xe6+\n♖1e2",
                "analysis SAN display transforms pieces and promotions while preserving raw tokens, labels and whitespace");
    expect_true(NotationLayout::format_san_line("Q Qx K1 =Q e8=K axb9=Q Nxxe4",true)==
                "Q Qx K1 =Q e8=K axb9=Q Nxxe4",
                "unrecognized analysis tokens are not blindly converted");
    PgnDocument document;
    expect_true(gambitree::PgnParser::parse("1. e4 e5 2. Nf3 Nc6 *",document),"move-pair spacing fixture parses");
    if (document.games.empty()) return;
    NotationLayout layout;
    for (int style=0; style<2; ++style) {
        layout.set_figurines(style!=0); layout.build(*document.games[0],1000,8,22);
        const std::vector<gambitree::NotationItem>& items=layout.items();
        expect_true(items.size()==7 && items[0].text=="1." && items[3].text=="2." &&
                    items[2].text=="e5" && items[5].text==(style ? "♘c6" : "Nc6"),
                    "pair spacing keeps numbering and item order without redundant black move numbers");
        if (items.size()==7) {
            expect_true(items[2].x-(items[1].x+items[1].width)==12 &&
                        items[3].x-(items[2].x+items[2].width)==16 &&
                        items[4].x-(items[3].x+items[3].width)==8,
                        "white-black gap is one and a half spaces; new move pairs get two spaces");
            for (std::size_t i=0;i<items.size();++i)
                if (items[i].node && items[i].kind==gambitree::NOTATION_MOVE)
                    expect_true(layout.hit_test(items[i].x+1,items[i].y+1,0)==items[i].node,
                                "added whitespace leaves each move's own hitbox and identity intact");
        }
    }
}

void test_carlsen_pelletier_promoted_navigation()
{
    std::ifstream in("tests/data/carlsen-pelletier-2007.pgn");
    std::ostringstream pgn;
    pgn << in.rdbuf();
    PgnDocument document;
    expect_true(in.good() && gambitree::PgnParser::parse(pgn.str(), document) &&
                document.games.size() == 1 && document.warnings.empty() &&
                document.skipped_variation_count == 0, "updated Carlsen-Pelletier example retains every promoted line");
    if (document.games.empty()) return;
    PgnGame& game = *document.games[0];
    const MoveNode* d6 = node_on_path(game, std::string(28, '0'));
    const MoveNode* kf8 = node_on_path(game, std::string(27, '0') + "1");
    const MoveNode* bd3 = node_on_path(game, std::string(29, '0'));
    expect_true(d6 && d6->san == "d6" && kf8 && kf8->san == "Kf8" && bd3 && bd3->san == "Bd3" &&
                d6->parent == kf8->parent && bd3->parent == d6,
                "updated example promotes d6 ahead of Kf8 and continues with Bd3");
    if (!d6 || !kf8 || !bd3) return;
    const int widths[] = {192, 532, 1701};
    for (int figurines = 0; figurines < 2; ++figurines) for (std::size_t w = 0; w < 3; ++w) {
        NotationLayout layout;
        layout.set_figurines(figurines != 0);
        layout.build(game, widths[w], 8, 18);
        MoveNavigator cursor(game);
        expect_true(cursor.select(d6) && cursor.next_after_variation(displayed_move_order(layout)) &&
                    cursor.current() == kf8 && cursor.sibling_index() == 1,
                    "promoted example Down selects the following Kf8 alternative without wrapping");
        expect_true(cursor.next_after_variation(displayed_move_order(layout)),
                    "Down leaves the complete Kf8 alternative for the following Bd3 move");
        expect_cursor(cursor, bd3, "rn1r2k1/1b3p1p/3ppp2/p1p5/2P5/P1NBP3/1P3PPP/2KR3R b - - 1 15",
                      "promoted example Down retains exact Bd3 identity, FEN and branch preferences");
        expect_true(cursor.position().variant() == gambitree::STANDARD_CHESS && cursor.back() &&
                    cursor.current() == d6 && cursor.selected_child_index() == 0 &&
                    cursor.forward() && cursor.current() == bd3,
                    "promoted example keeps variant context and its actual main-line continuation");
    }
}

void test_figurine_notation()
{
    const char* source[] = {"Kf1", "Qh5+", "R1e2", "Bxb6", "Nbd2", "e4", "exd5", "O-O", "O-O-O",
                            "Nf7#", "e8=Q+", "axb8=N#", "c1=R", "h8=B"};
    const char* expected[] = {"♔f1", "♕h5+", "♖1e2", "♗xb6", "♘bd2", "e4", "exd5", "O-O", "O-O-O",
                              "♘f7#", "e8=♕+", "axb8=♘#", "c1=♖", "h8=♗"};
    for (std::size_t i = 0; i < sizeof(source) / sizeof(source[0]); ++i) {
        expect_true(NotationLayout::format_san(source[i], true) == expected[i] &&
                    NotationLayout::format_san(source[i], false) == source[i] &&
                    NotationLayout::format_san(source[i], true, 0) == source[i],
                    "piece SAN, pawn SAN, both castlings, disambiguation, captures, check, mate and promotions format without mutation");
    }
    expect_true(NotationLayout::format_san("e8=Q+", true, 1) == "e8=Q+" &&
                NotationLayout::format_san("Kf1", true, 1) == "♔f1",
                "individual missing glyphs fall back to letters without disabling supported figurines");
    FigurineTestMeasurer measurer;
    NotationFontMetrics metrics(8, 22, &measurer);
    const char* fixtures[] = {kCarlsenNavigationFixture, "tests/data/byrne-fischer-1956.pgn",
                              "tests/data/robert-byrne-fischer-1963.pgn"};
    const int widths[] = {192, 532, 1400};
    for (std::size_t f = 0; f < 3; ++f) {
        std::ifstream in(fixtures[f]); std::ostringstream pgn; pgn << in.rdbuf();
        PgnDocument document;
        expect_true(in.good() && gambitree::PgnParser::parse(pgn.str(), document) && document.games.size() == 1,
                    "all three real figurine regression snapshots parse");
        if (document.games.empty()) continue;
        PgnGame& game = *document.games[0];
        NotationLayout layout;
        layout.build(game, 532, metrics);
        const std::vector<const MoveNode*> order = displayed_move_order(layout);
        std::vector<std::string> original_san;
        std::string original_comments;
        for (std::size_t i = 0; i < order.size(); ++i) original_san.push_back(order[i]->san);
        for (std::size_t i = 0; i < layout.items().size(); ++i)
            if (layout.items()[i].kind == gambitree::NOTATION_COMMENT)
                original_comments += layout.items()[i].text + " ";
        MoveNavigator cursor(game);
        const MoveNode* selected = f == 0 ? node_on_path(game, "0000000000000000000010000000") : order[order.size()/2];
        cursor.select(selected);
        const std::string fen = cursor.position().to_fen();
        const std::vector<std::size_t> path = cursor.path();
        std::size_t child_preference = cursor.selected_child_index();
        for (std::size_t w = 0; w < 3; ++w) {
            layout.set_figurines(true);
            layout.build(game, widths[w], metrics);
            expect_true(displayed_move_order(layout) == order && cursor.current() == selected &&
                        cursor.position().to_fen() == fen && cursor.path() == path &&
                        cursor.selected_child_index() == child_preference,
                        "reflow and style toggling preserve exact move order, selected node, FEN and branch preference");
            std::string comments;
            std::map<const MoveNode*, std::string> rendered_moves;
            for (std::size_t i = 0; i < layout.items().size(); ++i) {
                const gambitree::NotationItem& item = layout.items()[i];
                if (item.kind == gambitree::NOTATION_MOVE && item.node)
                    rendered_moves[item.node] += item.text;
                expect_true(item.width == metrics.text_width(item.text) && item.x >= 0 &&
                            item.x + item.width <= widths[w] && item.height == 22,
                            "mixed glyph advances drive line wrapping and item rectangles");
                if (item.kind == gambitree::NOTATION_MOVE && item.node)
                    expect_true(layout.hit_test(item.x + item.width/2, item.y + 1, 0) == item.node,
                                "figurine moves hit their exact MoveNode after wrapping");
                if (item.kind == gambitree::NOTATION_COMMENT)
                    comments += item.text + " ";
                if (item.kind == gambitree::NOTATION_NAG && item.node && item.node->san == "Rf5")
                    expect_true(item.node->nags.size() == 1 && item.node->nags[0] == 15 && item.text == "=/+",
                                "real Carlsen-Pelletier Rf5 verdict renders NAG 15 without changing its stored code");
                expect_true(NotationLayout::normalize_display_text(item.text) == item.text,
                            "line wrapping never splits UTF-8 figurines or prose inside a code point");
            }
            if (f == 0)
                expect_true(comments != original_comments &&
                            comments.find("16. ♗e4 ♗xe4 17. ♘xe4") != std::string::npos,
                            "real ChessBase comments switch their six piece-code displays with figurine mode");
            else
                expect_true(comments == original_comments,
                            "ordinary German and English comments without legacy piece codes remain unchanged");
            int scroll = layout.ensure_visible(selected, layout.max_scroll(110), 110);
            bool visible = false;
            for (std::size_t i = 0; i < layout.items().size(); ++i) {
                const gambitree::NotationItem& item = layout.items()[i];
                if (item.kind == gambitree::NOTATION_MOVE && item.node == selected &&
                    item.y >= scroll && item.y + item.height <= scroll + 110) {
                    visible = layout.hit_test_visible(item.x + 1, item.y - scroll + 1, scroll, 110) == selected;
                }
            }
            expect_true(visible, "selected figurine move stays visible and clickable after style reflow");
            for (std::size_t i = 0; i < order.size(); ++i) {
                expect_true(order[i]->san == original_san[i], "original SAN strings remain unchanged");
                expect_true(rendered_moves[order[i]] == NotationLayout::format_san(order[i]->san, true),
                            "every main-line and alternative move displays exactly its formatted SAN, even across item splits");
            }
            MoveNavigator ending(game);
            ending.select(selected);
            ending.to_end();
            const MoveNode* last = ending.current();
            int end_scroll = layout.ensure_visible(last, layout.max_scroll(110), 110);
            bool end_visible = false;
            for (std::size_t i = 0; i < layout.items().size(); ++i) {
                const gambitree::NotationItem& item = layout.items()[i];
                if (item.kind == gambitree::NOTATION_MOVE && item.node == last &&
                    item.y >= end_scroll && item.y + item.height <= end_scroll + 110)
                    end_visible = true;
            }
            expect_true(last->children.empty() && ending.position().equals(last->position_after) && end_visible,
                        "End still selects a legal terminal node and keeps its figurine notation visible");
            if (f == 0) {
                expect_true(cursor.next_after_variation(displayed_move_order(layout)) &&
                            cursor.current() == node_on_path(game, "0000000000000000000010000001") &&
                            cursor.position().to_fen() == "rn1r2k1/1b3p1p/3ppp2/p1p5/2P5/P1N1P3/1P3PPP/2KR1B1R w - - 0 15",
                            "Kf8 Down d6 retains exact identity and board position in figurine mode");
                cursor.select(selected);
            }
        }
        layout.set_figurines(false); layout.build(game, 532, metrics);
        expect_true(displayed_move_order(layout) == order, "switching back to letters preserves navigation identities");
    }
    PgnDocument document;
    expect_true(gambitree::PgnParser::parse(
        "[White \"King Black Knight\"]\n[Black \"König Bauer\"]\n\n"
        "1. Nf3 $14 {King Black Knight König ¥e4 [%cal Ga7g7]} Nf6 $15 *", document),
        "targeted verdict and comment fixture parses");
    if (document.games.empty()) return;
    NotationLayout layout; layout.set_figurines(true); layout.build(*document.games[0], 1000, metrics);
    const MoveNode* white = document.games[0]->root->children[0];
    const MoveNode* black = white->children[0];
    int nags = 0; std::string comments;
    for (std::size_t i = 0; i < layout.items().size(); ++i) {
        const gambitree::NotationItem& item = layout.items()[i];
        if (item.kind == gambitree::NOTATION_NAG) {
            ++nags;
            expect_true(item.node && item.text == (item.node == white ? "+/=" : "=/+") && i > 0 &&
                        item.x == layout.items()[i-1].x + layout.items()[i-1].width + 8,
                        "NAG 14/15 retain verdict spacing beside white and black figurine SAN");
        }
        if (item.kind == gambitree::NOTATION_COMMENT) comments += item.text + " ";
    }
    expect_true(nags == 2 && white->nags[0] == 14 && black->nags[0] == 15 &&
                white->san == "Nf3" && black->san == "Nf6" &&
                comments == "King Black Knight König ♗e4 " &&
                *document.games[0]->tag_value("White") == "King Black Knight",
                "verdicts stay numeric, names and prose retain letters, directives stay hidden");
    expect_true(NotationLayout::nag_text(14) == "+/=" && NotationLayout::nag_text(15) == "=/+" &&
                NotationLayout::nag_text(123) == "$123", "the existing verdict mappings and raw fallback remain supported");
    NotationFontMetrics unavailable(8, 22, &measurer, 0);
    layout.build(*document.games[0], 1000, unavailable);
    bool plain_white = false, plain_black = false;
    for (std::size_t i = 0; i < layout.items().size(); ++i) {
        const gambitree::NotationItem& item = layout.items()[i];
        if (item.kind == gambitree::NOTATION_MOVE && item.node == white)
            plain_white = item.text == "Nf3" && item.width == measurer.text_width("Nf3");
        if (item.kind == gambitree::NOTATION_MOVE && item.node == black)
            plain_black = item.text == "Nf6" && item.width == measurer.text_width("Nf6");
    }
    expect_true(plain_white && plain_black && layout.figurines(),
                "missing-font fallback uses complete measured letter moves without losing the saved style choice");
    PgnDocument promotion;
    expect_true(gambitree::PgnParser::parse(
        "[SetUp \"1\"]\n[FEN \"7k/4P3/8/8/8/8/8/4K3 w - - 0 1\"]\n\n1. e8=Q+ Kh7 *", promotion),
        "legal promotion figurine fixture parses");
    if (!promotion.games.empty()) {
        layout.build(*promotion.games[0], 500, metrics);
        bool found = false;
        for (std::size_t i=0; i<layout.items().size(); ++i) if (layout.items()[i].text == "e8=♕+") found = true;
        expect_true(found && promotion.games[0]->root->children[0]->san == "e8=Q+",
                    "promotion appears as a measured queen figurine while model SAN is preserved");
    }
}

void test_figurine_toggle_preserves_notation_scroll()
{
    std::ostringstream pgn;
    pgn << "[Source \"ChessBase\"]\n"
        << "[Mega2PGNRecordId \"5190645\"]\n"
        << "[Mega2PGNEcoIndex \"4455\"]\n\n"
        << "1. Nf3 (1. d4 Nf6 (1... d5 {nested analysis with "
        << "a long comment that remains available after changing the display} "
        << "2. c4 e6) 2. c4 e6) Nf6 ";
    for (int round = 0; round < 36; ++round) {
        const int move_number = 2 + round * 2;
        pgn << move_number << ". Ng1 {";
        for (int phrase = 0; phrase < 12; ++phrase)
            pgn << "The analysis remains at this comment while the notation "
                << "style changes. ";
        pgn << "} Ng8 " << (move_number + 1) << ". Nf3 {A second long comment preserves the "
            << "reader's place independently of the selected chess move.} "
            << "Nf6 ";
    }
    pgn << "*";

    PgnDocument document;
    const bool parsed = gambitree::PgnParser::parse(pgn.str(), document);
    expect_true(parsed && document.games.size() == 1,
                "long annotated line with deeply nested alternatives parses for style-toggle scroll regression");
    if (document.games.empty()) return;

    PgnGame& game = *document.games[0];
    const MoveNode* deep_alternative = node_on_path(game, "11");
    MoveNavigator navigator(game);
    FigurineTestMeasurer measurer;
    NotationFontMetrics metrics(8, 20, &measurer);
    NotationLayout layout;
    const int viewport = 120;
    expect_true(deep_alternative && navigator.select(deep_alternative),
                "regression selects a unique move inside a nested alternative");
    if (!deep_alternative) return;

    const std::string selected_fen = navigator.position().to_fen();
    const std::vector<std::size_t> selected_path = navigator.path();
    const std::size_t selected_child = navigator.selected_child_index();
    layout.build(game, 260, metrics);
    const int original_max = layout.max_scroll(viewport);
    const int original_scroll = original_max / 2;
    const int selection_snap = layout.ensure_visible(deep_alternative,
                                                      original_scroll,
                                                      viewport);
    expect_true(original_max > viewport && original_scroll > 0 &&
                selection_snap != original_scroll,
                "fixture distinguishes the scrolled reading position from selected-move visibility");

    int scroll = original_scroll;
    for (int toggle = 0; toggle < 6; ++toggle) {
        layout.set_figurines((toggle % 2) == 0);
        layout.build(game, 260, metrics);
        const int maximum = layout.max_scroll(viewport);
        if (scroll < 0) scroll = 0;
        if (scroll > maximum) scroll = maximum;
        expect_true(scroll == original_scroll &&
                    navigator.current() == deep_alternative &&
                    navigator.position().to_fen() == selected_fen &&
                    navigator.path() == selected_path &&
                    navigator.selected_child_index() == selected_child,
                    "repeated figurine toggles preserve deep selection, FEN, branch path and reading offset");
        bool comment_at_view = false;
        for (std::size_t i = 0; i < layout.items().size(); ++i) {
            const gambitree::NotationItem& item = layout.items()[i];
            if (item.kind == gambitree::NOTATION_COMMENT && item.y >= scroll &&
                item.y < scroll + viewport) {
                comment_at_view = true;
                break;
            }
        }
        expect_true(comment_at_view,
                    "the comment at the scrolled viewport remains visible after notation reflow");
    }

    int top_scroll = 0;
    layout.set_figurines(true);
    layout.build(game, 260, metrics);
    if (top_scroll > layout.max_scroll(viewport))
        top_scroll = layout.max_scroll(viewport);
    expect_true(top_scroll == 0 && navigator.current() == deep_alternative &&
                navigator.position().to_fen() == selected_fen,
                "a style change at the top preserves both the top reading position and selected board position");

    int resized_scroll = scroll;
    layout.set_figurines(false);
    layout.build(game, 440, metrics);
    if (resized_scroll < 0) resized_scroll = 0;
    if (resized_scroll > layout.max_scroll(viewport))
        resized_scroll = layout.max_scroll(viewport);
    expect_true(resized_scroll >= 0 &&
                resized_scroll <= layout.max_scroll(viewport) &&
                navigator.current() == deep_alternative &&
                navigator.position().to_fen() == selected_fen,
                "style reflow remains safely bounded when the notation width changes");

    PgnDocument short_document;
    expect_true(gambitree::PgnParser::parse("1. e4 {short note} *",
                                           short_document) &&
                short_document.games.size() == 1,
                "short notation fixture parses");
    if (!short_document.games.empty()) {
        NotationLayout short_layout;
        short_layout.build(*short_document.games[0], 500, metrics);
        short_layout.set_figurines(true);
        short_layout.build(*short_document.games[0], 500, metrics);
        int short_scroll = 400;
        const int maximum = short_layout.max_scroll(viewport);
        if (short_scroll > maximum) short_scroll = maximum;
        expect_true(short_scroll == 0 && short_layout.items().size() > 0,
                    "short layouts safely bound a stale scroll offset after a display change");
    }
}

void test_notation_search_index()
{
    const std::string source =
        "[Source \"ChessBase\"]\n[Mega2PGNRecordId \"5190645\"]\n"
        "[Mega2PGNEcoIndex \"4455\"]\n\n"
        "1. Nf3 {The resulting ¬. Kramnik meets the line 29...Bc4. "
        "A long phrase crosses the visible wrap here and remains searchable.} "
        "Nf6 (1... d5 {Deep nested Kramnik note.} 2. d4) "
        "2. Ng1 {Kramnik again.} *";
    PgnDocument document;
    expect_true(gambitree::PgnParser::parse(source, document) &&
                document.games.size() == 1,
                "search fixture with ChessBase display text and nested RAV parses");
    if (document.games.empty()) return;

    FigurineTestMeasurer measurer;
    NotationFontMetrics metrics(8, 20, &measurer);
    NotationLayout layout;
    layout.build(*document.games[0], 132, metrics);
    MoveNavigator navigator(*document.games[0]);
    navigator.forward();
    const MoveNode* selected = navigator.current();
    const std::string fen = navigator.position().to_fen();
    const std::vector<std::size_t> path = navigator.path();
    gambitree::NotationSearch search;
    search.open();

    int viewport = 80;
    search.set_query("THE RESULTING ENDGAME", layout, 0, viewport);
    expect_true(search.result_count() == 1 && search.has_current(),
                "case-insensitive search matches ChessBase endgame display text, not the source mark");
    std::vector<gambitree::NotationSearchHighlight> highlights =
        search.highlights(layout, 0, layout.content_height());
    expect_true(!highlights.empty(),
                "a search hit maps back to precise notation item coordinates");

    search.set_query("kRaMnIk", layout, 0, viewport);
    expect_true(search.result_count() == 3 && search.current_result() == 0,
                "multiple case-folded hits are sorted in displayed mainline and RAV order");
    const std::size_t first = search.current_result();
    expect_true(search.move(1) && search.current_result() == (first + 1) % 3 &&
                search.move(-1) && search.current_result() == first &&
                search.move(-1) && search.current_result() == 2 &&
                search.move(1) && search.current_result() == first,
                "previous and next results wrap in both directions");
    search.close();
    search.open();
    expect_true(search.query() == "kRaMnIk" && search.select_all() &&
                search.has_current() && search.current_result() == first,
                "closing and reopening retains the query and selects it for replacement");

    search.set_query("29...Bc4", layout, 0, viewport);
    expect_true(search.result_count() == 1,
                "search joins a rendered move number and SAN while ignoring figurine spelling");
    search.set_query("No such phrase", layout, 0, viewport);
    expect_true(search.result_count() == 0 && !search.has_current(),
                "unmatched searches show the empty 0/0 state");
    search.set_query("", layout, 0, viewport);
    expect_true(search.result_count() == 0 && !search.has_current(),
                "an empty query has no accidental match-all results");

    const MoveNode* knight = document.games[0]->root->children[0];
    for (int style = 0; style < 2; ++style) {
        layout.set_figurines(style != 0);
        layout.build(*document.games[0], 132, metrics);
        search.set_query("nf3", layout, 0, viewport);
        expect_true(search.result_count() >= 1,
                    "SAN letter searches find the corresponding move in both figurine modes");
        highlights = search.highlights(layout, 0, layout.content_height());
        bool exact_knight_span = false;
        for (std::size_t i = 0; i < highlights.size(); ++i)
            if (highlights[i].current && highlights[i].rect.width > 0)
                exact_knight_span = true;
        expect_true(exact_knight_span && navigator.current() == selected &&
                    navigator.position().to_fen() == fen &&
                    navigator.path() == path,
                    "figurine normalization highlights the move without changing active node, FEN or branch path");
        if (style) {
            bool actual_figurine = false;
            for (std::size_t i = 0; i < layout.items().size(); ++i)
                if (layout.items()[i].node == knight &&
                    layout.items()[i].kind == gambitree::NOTATION_MOVE &&
                    layout.items()[i].text == "♘f3") actual_figurine = true;
            expect_true(actual_figurine,
                        "the search match is anchored to the displayed knight figurine");
        }
    }

    layout.set_figurines(false);
    layout.build(*document.games[0], 132, metrics);
    search.set_query("A long phrase crosses the visible wrap", layout, 0,
                     viewport);
    highlights = search.highlights(layout, 0, layout.content_height());
    bool wraps_rows = false;
    for (std::size_t i = 0; i < highlights.size(); ++i)
        for (std::size_t j = i + 1; j < highlights.size(); ++j)
            if (highlights[i].rect.y != highlights[j].rect.y) wraps_rows = true;
    expect_true(search.result_count() == 1 && wraps_rows,
                "a phrase spanning wrapped comment lines retains per-line highlight geometry");

    const int old_generation = static_cast<int>(layout.layout_generation());
    const int old_scroll = search.scroll_to_current(layout, 0, viewport);
    expect_true(old_scroll >= 0 &&
                old_scroll <= layout.max_scroll(viewport) &&
                layout.layout_generation() >= static_cast<uint64_t>(old_generation),
                "result scrolling is clamped to the active notation viewport");
    const std::size_t old_result = search.current_result();
    layout.set_figurines(true);
    layout.build(*document.games[0], 132, metrics);
    search.refresh(layout);
    expect_true(search.query() == "A long phrase crosses the visible wrap" &&
                search.result_count() == 1 &&
                search.current_result() == old_result &&
                navigator.current() == selected &&
                navigator.position().to_fen() == fen,
                "layout refresh after G preserves query, active match and chess selection");

    PgnDocument reference_document;
    expect_true(gambitree::PgnParser::parse(
        "1. Nf3 {Kramnik endgame 29...Bc4} d5 *", reference_document) &&
        reference_document.errors.empty() && reference_document.games.size() == 1,
        "self-contained annotated search fixture parses");
    if (!reference_document.games.empty()) {
        NotationLayout reference_layout;
        gambitree::NotationSearch reference_search;
        reference_layout.build(*reference_document.games[0], 760, metrics);
        const char* queries[] = {"Kramnik", "endgame", "29...Bc4"};
        for (std::size_t i = 0; i < 3; ++i) {
            reference_search.set_query(queries[i], reference_layout, 0, 240);
            expect_true(reference_search.result_count() > 0,
                        "annotated search finds names, endgame text and unspaced move references");
        }
        reference_layout.set_figurines(true);
        reference_layout.build(*reference_document.games[0], 760, metrics);
        reference_search.set_query("Nf3", reference_layout, 0, 240);
        expect_true(reference_search.result_count() > 0,
                    "annotated search finds SAN letters when figurines are displayed");
    }
}

void test_byrne_fischer_navigation()
{
    // Fixture SHA-256: 721722f76f1fe24d413d88ce528558a5b4ea706463e91e551f0ca76171a5cf6d
    std::ifstream in("tests/data/byrne-fischer-1956.pgn");
    std::ostringstream pgn;
    pgn << in.rdbuf();
    PgnDocument document;
    expect_true(in.good() && gambitree::PgnParser::parse(pgn.str(), document) &&
                document.games.size() == 1 && document.warnings.empty(),
                "original Byrne-Fischer file retains every annotated variation");
    if (document.games.empty()) return;
    PgnGame& game = *document.games[0];
    NotationLayout layout;
    layout.build(game, 532, 8, 18);
    std::vector<const MoveNode*> order = displayed_move_order(layout);
    struct Jump { const char* from; const char* to; const char* fen; };
    const Jump jumps[] = {
        {"000000000000000000000000000000100000", "000000000000000000000000000000100001", "r3r1k1/pp2B1bp/1qp3p1/6N1/3P2b1/2Q5/P4PPP/3RK2R w K - 2 19"},
        {"000000000000000000000000000000100001", "0000000000000000000000000000001000000", "r3r3/pp2k1bp/1qp3p1/6N1/3P2b1/2Q5/P4PPP/3R1RK1 b - - 1 19"},
        {"0000000000000000000000000000000001", "00000000000000000000000000000000000", "r3r1k1/pp3pbp/1Bp1b1p1/8/2BP4/Q1n2N2/P4PPP/3R1K1R b - - 0 18"},
        {"000000000000000000000000000000000101", "0000000000000000000000000000000001000", "r3r3/pp3kbp/1qp3p1/1nB5/3P2b1/1Q3N2/P4PPP/3R1K1R b - - 1 19"},
        {"00000000000000000000000000000000001", "00000000000000000000000000000000002", "r3r1k1/pp3pbp/1qp1b1p1/2B5/2BP4/2Q2N2/P4PPP/3R1K1R b - - 0 18"},
        {"00000000000000000000000000000000002", "00000000000000000000000000000000003", "r3r1k1/pp3pbp/1qp1b1p1/2B5/3P4/Q1nB1N2/P4PPP/3R1K1R b - - 5 18"},
        {"00000000000000000000000000000000003", "000000000000000000000000000000000000", "r3r1k1/pp3pbp/1Bp3p1/8/2bP4/Q1n2N2/P4PPP/3R1K1R w - - 0 19"},
    };
    for (int style = 0; style < 2; ++style) {
      layout.set_figurines(style != 0);
      layout.build(game, 532, 8, 18);
      expect_true(displayed_move_order(layout) == order,
                  "Byrne-Fischer notation style preserves every navigation node");
      for (std::size_t i = 0; i < sizeof(jumps) / sizeof(jumps[0]); ++i) {
        MoveNavigator cursor(game);
        const MoveNode* from = node_on_path(game, jumps[i].from);
        const MoveNode* to = node_on_path(game, jumps[i].to);
        expect_true(from && to && cursor.select(from) &&
                    cursor.next_after_variation(order), "Byrne-Fischer Down regression jump succeeds");
        expect_cursor(cursor, to, jumps[i].fen,
                      "Byrne-Fischer siblings and nested exits keep exact nodes, FENs, and preferences");
      }
    }
}

void test_legacy_comment_presentation()
{
    const std::string original =
        "16. ¥e4 ¥xe4 17. ¤xe4 21. ¦xe5 ¢f1 £h5+ §e4 "
        "(… 23.¦g1, 24.¦h4) König, café — €20 ¥100 £50 ¤Währung ¦Text §12 5¢. "
        "abc¥e4 ¥e9 ¤xe4word DIAGRAM GEVONDEN [%cal Ga7g7] Ende";
    const std::string expected =
        "16. Be4 Bxe4 17. Nxe4 21. Rxe5 Kf1 Qh5+ Pe4 "
        "(… 23.Rg1, 24.Rh4) König, café — €20 B100 Q50 NWährung RText P12 5K. "
        "abcBe4 Be9 Nxe4word DIAGRAM GEVONDEN Ende";
    PgnDocument marked, plain;
    expect_true(gambitree::PgnParser::parse(
        "1. e4 {" + original + "} e5 (1... c5 {zweiter Kommentar}) 2. Nf3 *", marked) &&
        gambitree::PgnParser::parse(
        "1. e4 {" + expected + "} e5 (1... c5 {zweiter Kommentar}) 2. Nf3 *", plain),
        "six legacy piece-reference comments parse independently of SAN");
    if (marked.games.empty() || plain.games.empty()) return;
    const int widths[] = {192, 532, 1701};
    for (std::size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        NotationLayout layout, reference;
        layout.build(*marked.games[0], widths[w], 8, 18);
        reference.build(*plain.games[0], widths[w], 8, 18);
        std::string comments;
        expect_true(layout.items().size() == reference.items().size(),
                    "legacy conversions produce the same readable layout as ordinary piece letters");
        for (std::size_t i = 0; i < layout.items().size(); ++i) {
            const gambitree::NotationItem& item = layout.items()[i];
            expect_true(!item.text.empty() && item.width > 0,
                        "converted comment layout contains no empty or zero-width glyph items");
            if (i < reference.items().size())
                expect_true(item.kind == reference.items()[i].kind &&
                            item.text == reference.items()[i].text &&
                            item.x == reference.items()[i].x && item.y == reference.items()[i].y,
                            "comment conversion preserves text order and NOTATION_MOVE order and positions");
            if (item.kind == gambitree::NOTATION_COMMENT) {
                if (!comments.empty()) comments += ' ';
                comments += item.text;
            }
        }
        expect_true(comments == expected + " zweiter Kommentar",
                    "all six pieces, Unicode prose, currency boundaries, directives, and comment order display correctly");
        expect_true(marked.games[0]->root->children[0]->comments[0] == original &&
                    marked.games[0]->root->children[0]->following_items[0].comment == original,
                    "legacy display filtering never changes original PGN comment storage");
    }
}

void test_chessbase_comment_modes_and_context()
{
    const std::string codes = "Prose: ¢ £ ¦ ¥ ¤ §; German: König, Grüße, café, €20.";
    const std::string letters = "Prose: K Q R B N P; German: König, Grüße, café, €20.";
    const std::string figurines = "Prose: ♔ ♕ ♖ ♗ ♘ ♙; German: König, Grüße, café, €20.";
    expect_true(NotationLayout::format_comment_text(codes, false) == letters &&
                NotationLayout::format_comment_text(codes, true) == figurines,
                "all six legacy piece codes map in prose to letters or enabled figurines");
    expect_true(NotationLayout::format_comment_text(codes, true, false, 0) == letters &&
                NotationLayout::format_comment_text(codes, true, false, 1U << 3) ==
                    "Prose: K Q R ♗ N P; German: König, Grüße, café, €20.",
                "comment figurine fallback honors the installed glyph mask per piece");

    const std::string tagged_pgn =
        "[Source \"ChessBase\"]\n[Mega2PGNRecordId \"5190645\"]\n"
        "[Mega2PGNEcoIndex \"4455\"]\n\n"
        "1. e4 {Symbols: ­; the ¬ after.} *\n"
        "[Source \"ChessBase\"]\n[Mega2PGNRecordId \"5190646\"]\n\n"
        "1. d4 {Symbols: ­; the ¬ after.} *\n"
        "[Source \"Other\"]\n[Mega2PGNRecordId \"5190647\"]\n"
        "[Mega2PGNEcoIndex \"4455\"]\n\n"
        "1. c4 {Symbols: ­; the ¬ after.} *\n"
        "[Source \"ChessBase\"]\n[Mega2PGNRecordId \"\"]\n"
        "[Mega2PGNEcoIndex \"4455\"]\n\n"
        "1. Nf3 {Symbols: ­; the ¬ after.} *";
    PgnDocument tagged;
    expect_true(gambitree::PgnParser::parse(tagged_pgn, tagged) &&
                tagged.games.size() == 4,
                "tagged, incomplete, and non-ChessBase records parse together");
    if (tagged.games.size() == 4) {
        const std::string ordinary = "Symbols: ­; the ¬ after.";
        for (int style = 0; style < 2; ++style) {
            for (std::size_t game_index = 0; game_index < tagged.games.size();
                 ++game_index) {
                NotationLayout layout;
                layout.set_figurines(style != 0);
                layout.build(*tagged.games[game_index], 900, 8, 20);
                std::string shown;
                for (std::size_t item = 0; item < layout.items().size(); ++item)
                    if (layout.items()[item].kind == gambitree::NOTATION_COMMENT) {
                        if (!shown.empty()) shown += ' ';
                        shown += layout.items()[item].text;
                    }
                if (game_index == 0) {
                    expect_true(shown == (style ?
                        "Symbols: ♗♗; the endgame after." :
                        "Symbols: bishop pair; the endgame after."),
                        "only a fully identified ChessBase Mega record gets bishop-pair and endgame display");
                } else {
                    expect_true(shown == ordinary,
                        "missing/empty Mega tags and non-ChessBase source preserve ordinary soft-hyphen and NOT meanings");
                }
            }
        }
        expect_true(tagged.games[0]->root->children[0]->comments[0] ==
                        "Symbols: ­; the ¬ after.",
                    "ChessBase-specific display leaves the original PGN comment unchanged");
    }

    const std::string svidler_source =
        "Black's active pieces compensate for his ׻. ע, Potkin; ->עe8, "
        "ץc1 and the ץe7 line; hold his ׻P on the long run.";
    const std::string svidler_expected =
        "Black's active pieces compensate for his [U+05FB]. [U+05E2], Potkin; "
        "->[U+05E2] e8, [U+05E5] c1 and the [U+05E5] e7 line; hold his "
        "[U+05FB] P on the long run.";
    PgnDocument svidler;
    expect_true(gambitree::PgnParser::parse(
                    "1. e4 {" + svidler_source + "} e5 *", svidler) &&
                svidler.games.size() == 1,
                "Svidler-style annotated comments parse without interpreting unknown marks as chess");
    if (!svidler.games.empty()) {
        NotationLayout layout;
        layout.set_figurines(true);
        layout.build(*svidler.games[0], 1200, 8, 20);
        std::string shown;
        for (std::size_t item = 0; item < layout.items().size(); ++item)
            if (layout.items()[item].kind == gambitree::NOTATION_COMMENT)
                shown += layout.items()[item].text + " ";
        if (!shown.empty()) shown.erase(shown.size() - 1);
        expect_true(shown == svidler_expected,
                    "Svidler's four unresolved Hebrew code points have explicit readable U+ fallbacks");
        expect_true(svidler.games[0]->root->children[0]->comments[0] ==
                        svidler_source,
                    "Svidler fallback text is presentation-only and leaves the source comment intact");
    }
}

void test_visited_game_navigation_state()
{
    gambitree::PgnDocument document;
    std::ifstream input("tests/data/adversarial.pgn");
    std::ostringstream pgn;
    PgnGame* game;
    gambitree::VisitedGameStates states;
    std::vector<std::size_t> first_path;
    bool exact = false;
    uint64_t file_a = 1001;
    uint64_t file_b = 1002;

    pgn << input.rdbuf();
    expect_true(gambitree::PgnParser::parse(pgn.str(), document) &&
                !document.games.empty(),
                "nested-RAV PGN parses for navigation snapshots");
    game = document.games.empty() ? 0 : document.games[0];
    if (!game)
        return;
    MoveNavigator source(*game);
    expect_true(source.enter_variation(1) && source.current()->san == "d4" &&
                source.forward() && source.current()->san == "d5" &&
                source.next_sibling() && source.current()->san == "Nf6" &&
                source.forward() && source.current()->san == "c4" &&
                source.forward() && source.current()->san == "e6" &&
                source.next_sibling() && source.current()->san == "g6",
                "deeply nested RAV path can be selected");
    first_path = source.path();
    expect_true(first_path.size() >= 4 && first_path[0] == 1,
                "navigator stores each nested branch choice in its path");
    expect_true(states.save(file_a, 2, *game, source) && states.size() == 1,
                "visited state stores one sparse game entry");
    MoveNavigator restored(*game);
    expect_true(states.restore(file_a, 2, *game, restored, exact) && exact &&
                restored.path() == source.path() &&
                restored.position().to_fen() == source.position().to_fen() &&
                restored.current()->san == "g6",
                "snapshot restores exact nested variant path and position");
    expect_true(restored.forward() == source.forward(),
                "restored current-node continuation follows its saved child");

    MoveNavigator root(*game);
    expect_true(root.enter_variation(1) && root.back() && root.at_root(),
                "root can retain a selected first move branch");
    expect_true(states.save(file_b, 0, *game, root),
                "same game in a different source has independent state");
    MoveNavigator root_restored(*game);
    expect_true(states.restore(file_b, 0, *game, root_restored, exact) &&
                root_restored.at_root() && root_restored.forward() &&
                root_restored.current() == game->root->children[0],
                "Left out of a root alternative restores the played continuation");

    {
        MoveNavigator branch_point(*game);
        MoveNavigator branch_restored(*game);
        expect_true(branch_point.enter_variation(1) &&
                    branch_point.forward() &&
                    branch_point.next_sibling() &&
                    branch_point.current()->san == "Nf6" &&
                    branch_point.back() &&
                    branch_point.current()->san == "d4" &&
                    states.save(file_b, 1, *game, branch_point) &&
                    states.restore(file_b, 1, *game, branch_restored, exact) &&
                    exact && branch_restored.current()->san == "d4" &&
                    branch_restored.forward() &&
                    branch_restored.current()->san == "d5",
                    "saved selection at a branch point restores the parent continuation after Left exits a side line");
    }

    states.erase_deleted_game_and_shift(file_a, 1);
    MoveNavigator shifted(*game);
    expect_true(states.restore(file_a, 1, *game, shifted, exact) && exact &&
                shifted.path() == source.path(),
                "saved state follows its game when earlier index is deleted");
    MoveNavigator deleted_slot(*game);
    expect_true(!states.restore(file_a, 2, *game, deleted_slot, exact),
                "deleted game state is not assigned to the next game");
    states.erase_file(file_a);
    expect_true(states.size() == 2,
                "removing a source purges only that source's sparse states");

    {
        PgnDocument duplicate_document;
        std::vector<std::size_t> main_path;
        std::vector<std::size_t> duplicate_path;
        expect_true(gambitree::PgnParser::parse(
                        "1. e4 (1. e4) *", duplicate_document) &&
                        !duplicate_document.games.empty(),
                    "identical-transposition fixture parses");
        if (!duplicate_document.games.empty()) {
            PgnGame& duplicate_game = *duplicate_document.games[0];
            expect_true(duplicate_game.root->children.size() == 2 &&
                        duplicate_game.root->children[0]->position_after.equals(
                            duplicate_game.root->children[1]->position_after),
                        "two distinct PGN branches can reach the same FEN");
            MoveNavigator main_line(duplicate_game);
            MoveNavigator other_line(duplicate_game);
            main_line.enter_variation(0);
            other_line.enter_variation(1);
            main_path = main_line.path();
            duplicate_path = other_line.path();
            expect_true(main_path != duplicate_path,
                        "same-FEN variant routes remain distinct by tree path");
        }
    }

    {
        std::vector<std::size_t> stale;
        stale.push_back(1);
        stale.push_back(999);
        MoveNavigator partial(*game);
        expect_true(!partial.restore_path(stale) &&
                    partial.current() == game->root->children[1],
                    "invalid saved path falls back to its longest valid prefix");
    }
    {
        PgnDocument changed_document;
        expect_true(gambitree::PgnParser::parse(
                        "[Event \"Different game\"]\n[Result \"*\"]\n\n1. d4 *",
                        changed_document),
                    "changed-game fingerprint fixture parses");
        if (!changed_document.games.empty()) {
            MoveNavigator changed(*changed_document.games[0]);
            expect_true(!states.restore(file_b, 0,
                            *changed_document.games[0], changed, exact) &&
                        changed.at_root(),
                        "stale state is invalidated when game identity changes");
        }
    }
}

void test_board_layout_and_assets()
{
    BoardGeometry geometry = BoardLayout::calculate(1200, 800);
    BoardGeometry small = BoardLayout::calculate(20, 20);
    PngImage image;
    std::string error;
    PieceTheme theme("assets/pieces/classic");
    int x;
    int y;

    expect_true(geometry.square_size > 0 && geometry.board_size == geometry.square_size * 8,
                "board layout is exactly eight squares wide");
    BoardLayout::square_rect(geometry, Position::square_from_string("a1"), false, x, y);
    expect_true(BoardLayout::square_at(geometry, x + 1, y + 1, false) == Position::square_from_string("a1"),
                "normal mapping keeps a1 at lower left");
    BoardLayout::square_rect(geometry, Position::square_from_string("h8"), false, x, y);
    expect_true(BoardLayout::square_at(geometry, x + 1, y + 1, false) == Position::square_from_string("h8"),
                "normal mapping keeps h8 at upper right");
    BoardLayout::square_rect(geometry, Position::square_from_string("a1"), true, x, y);
    expect_true(BoardLayout::square_at(geometry, x + 1, y + 1, true) == Position::square_from_string("a1"),
                "flipped mapping keeps a1 on its rotated visual square");
    expect_true(BoardLayout::square_at(geometry, geometry.x - 1, geometry.y, false) == -1,
                "mapping outside board is rejected");
    expect_true(geometry.x <= 45 && geometry.x + geometry.board_size <= SidebarLayout::calculate(1200).left,
                "board uses a compact left margin while remaining clear of the sidebar");
    expect_true(SidebarLayout::calculate(1200).left - (geometry.x + geometry.board_size + BoardLayout::frame_width()) >= BoardLayout::sidebar_gutter(),
                "board/sidebar separation is at least the central gutter value");
    {
        BoardGeometry narrow_geometry = BoardLayout::calculate(420, 500);
        expect_true(narrow_geometry.board_size == narrow_geometry.square_size * 8 && narrow_geometry.x >= 0 &&
                    narrow_geometry.x + narrow_geometry.board_size <= SidebarLayout::calculate(420).left,
                    "narrow layout retains a square board without sidebar overlap");
    }
    {
        SidebarGeometry wide_sidebar = SidebarLayout::calculate(2200);
        BoardGeometry wide_geometry = BoardLayout::calculate(2200, 650);
        int region_left = BoardLayout::outer_margin() + BoardLayout::coordinate_space();
        int region_right = wide_sidebar.left - BoardLayout::sidebar_gutter() - BoardLayout::frame_width();
        int left_space = wide_geometry.x - region_left;
        int right_space = region_right - (wide_geometry.x + wide_geometry.board_size);
        expect_true(wide_geometry.board_size == wide_geometry.square_size * 8 && left_space >= 0 && right_space >= 0 &&
                    left_space - right_space <= 1 && right_space - left_space <= 1,
                    "height-limited board centres inside the left board region only");
    }
    {
        SidebarGeometry automatic = SidebarLayout::calculate(900);
        SidebarGeometry wider = SidebarLayout::calculate(900, 520);
        SidebarGeometry narrower = SidebarLayout::calculate(900, 220);
        SidebarGeometry constrained = SidebarLayout::calculate(420, 520);
        BoardGeometry dragged = BoardLayout::calculate(900, 650, 520);
        expect_true(wider.width > automatic.width && narrower.width < automatic.width,
                    "sidebar preference supports drag wider and narrower than automatic width");
        expect_true(constrained.left >= 0 && constrained.width <= SidebarLayout::maximum_width(420) &&
                    dragged.x + dragged.board_size + BoardLayout::frame_width() + BoardLayout::sidebar_gutter() <= wider.left,
                    "sidebar drag clamps to preserve a board region and gutter");
    }
    {
        const int sizes[][2] = {{411, 300}, {640, 480}, {900, 650},
                                {1200, 800}, {1920, 1100}, {2200, 1200}};
        for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            const int width = sizes[i][0], height = sizes[i][1];
            SidebarGeometry sidebar = SidebarLayout::calculate(width, 10000);
            BoardGeometry board = BoardLayout::calculate(width, height, sidebar.width);
            expect_true(sidebar.left == BoardLayout::minimum_region_width() &&
                        sidebar.width == width - 191 && sidebar.content_width == width - 219,
                        "maximum notation width reserves only the practical 191px board region");
            expect_true(board.square_size == 16 && board.board_size == 128 &&
                        board.x >= BoardLayout::coordinate_space() &&
                        board.x + board.board_size + BoardLayout::frame_width() +
                            BoardLayout::sidebar_gutter() <= sidebar.left &&
                        board.y >= BoardLayout::outer_margin() &&
                        board.y + board.board_size + BoardLayout::coordinate_space() <= height,
                        "maximum splitter travel keeps all 64 square cells and coordinates inside the board panel");
            for (int sq = 0; sq < 64; ++sq) {
                int sx, sy;
                BoardLayout::square_rect(board, sq, false, sx, sy);
                expect_true(BoardLayout::square_at(board, sx + 8, sy + 8, false) == sq,
                            "every square remains clickable after extreme splitter travel");
            }
        }
        expect_true(SidebarLayout::calculate(1920, 1729).width == 1729 &&
                    SidebarLayout::calculate(640, 1729).width == 449 &&
                    SidebarLayout::calculate(1920, 1729).width == 1729,
                    "the per-window pixel preference survives a narrow resize and return");
    }
    expect_true(!BoardLayout::is_light_square(Position::square_from_string("a1")) &&
                BoardLayout::is_light_square(Position::square_from_string("b1")) &&
                BoardLayout::is_light_square(Position::square_from_string("a2")) &&
                BoardLayout::is_light_square(Position::square_from_string("h1")) &&
                !BoardLayout::is_light_square(Position::square_from_string("h8")),
                "logical board colours use conventional a1-dark parity");
    expect_true(BoardLayout::is_light_square(Position::square_from_string("a1")) !=
                BoardLayout::is_light_square(Position::square_from_string("b1")) &&
                BoardLayout::is_light_square(Position::square_from_string("a1")) !=
                BoardLayout::is_light_square(Position::square_from_string("a2")),
                "adjacent logical squares have opposite colours");
    expect_true(BoardLayout::is_light_square(BoardLayout::square_at(geometry, geometry.x + 1, geometry.y + geometry.board_size - 1, false)) ==
                BoardLayout::is_light_square(BoardLayout::square_at(geometry, geometry.x + geometry.board_size - 1, geometry.y + 1, true)),
                "a logical square keeps its colour after board flip");
    expect_true(small.board_size == 0, "tiny window produces no malformed board geometry");
    expect_true(theme.path_for(gambitree::WHITE_KING) == "assets/pieces/classic/white-king.png",
                "piece theme maps white king path");
    expect_true(theme.path_for(gambitree::BLACK_PAWN) == "assets/pieces/classic/black-pawn.png",
                "piece theme maps black pawn path");
    expect_true(gambitree::load_png_rgba(theme.path_for(gambitree::WHITE_KING), image, error) && image.valid(),
                "temporary RGBA piece PNG loads");
    expect_true(!gambitree::load_png_rgba("tests/data/adversarial.pgn", image, error),
                "corrupt PNG input fails without crashing");
    {
        gambitree::AssetManager manager;
        expect_true(manager.load_piece_theme(theme, error),
                    "piece theme crops only transparent canvas margins");
        {
            int piece;
            bool all_cropped = true;
            for (piece = gambitree::BLACK_KING;
                 piece <= gambitree::WHITE_KING; ++piece) {
                const PngImage* loaded;
                bool edge_below_threshold = true;
                int coordinate;
                if (piece == gambitree::EMPTY)
                    continue;
                loaded = manager.piece_image(piece);
                if (!loaded || !loaded->valid() || loaded->width >= 1254 ||
                    loaded->height >= 1254) {
                    all_cropped = false;
                    continue;
                }
                for (coordinate = 0; coordinate < loaded->width; ++coordinate) {
                    if (loaded->rgba[static_cast<std::size_t>(coordinate) * 4 + 3] >= 4 ||
                        loaded->rgba[static_cast<std::size_t>(loaded->height - 1) *
                                     loaded->stride +
                                     static_cast<std::size_t>(coordinate) * 4 + 3] >= 4)
                        edge_below_threshold = false;
                }
                for (coordinate = 0; coordinate < loaded->height; ++coordinate) {
                    if (loaded->rgba[static_cast<std::size_t>(coordinate) *
                                     loaded->stride + 3] >= 4 ||
                        loaded->rgba[static_cast<std::size_t>(coordinate) *
                                     loaded->stride +
                                     static_cast<std::size_t>(loaded->width - 1) * 4 + 3] >= 4)
                        edge_below_threshold = false;
                }
                if (!edge_below_threshold)
                    all_cropped = false;
            }
            expect_true(all_cropped,
                        "all twelve assets crop faint alpha fringes with a small edge pad");
        }
        if (manager.piece_image(gambitree::WHITE_KING)) {
            const PngImage* king = manager.piece_image(gambitree::WHITE_KING);
            expect_true(king->width == 435 && king->height == 989,
                        "white king uses its threshold-bounded silhouette crop");
        }
        {
            gambitree::AssetManager meridian;
            PieceTheme meridian_theme("assets/pieces/meridian-shaded");
            int piece;
            bool complete = meridian.load_piece_theme(meridian_theme, error);
            for (piece = gambitree::BLACK_KING;
                 piece <= gambitree::WHITE_KING; ++piece) {
                if (piece != gambitree::EMPTY &&
                    (!meridian.piece_image(piece) ||
                     !meridian.piece_image(piece)->valid()))
                    complete = false;
            }
            expect_true(complete,
                        "Meridian Shaded loads all twelve generated PNG masters");
            if (complete) {
                int previous_width = meridian.piece_image(
                    gambitree::WHITE_KING)->width;
                PieceTheme invalid_theme("assets/pieces/missing-set");
                expect_true(!meridian.load_piece_theme(invalid_theme, error) &&
                            meridian.piece_image(gambitree::WHITE_KING) &&
                            meridian.piece_image(gambitree::WHITE_KING)->width ==
                            previous_width,
                            "failed piece-set load leaves the previous complete set intact");
                {
                    static const char* const names[] = {
                        "white-king", "white-queen", "white-rook",
                        "white-bishop", "white-knight", "white-pawn",
                        "black-king", "black-queen", "black-rook",
                        "black-bishop", "black-knight", "black-pawn"
                    };
                    char cwd[4096];
                    std::string directory;
                    bool created = getcwd(cwd, sizeof(cwd)) != 0;
                    std::size_t index;
                    if (created)
                        directory = test_paths::make_directory("meridian-corrupt", test_scratch_directory);
                    created = created && !directory.empty();
                    if (created) {
                        for (index = 0;
                             index < sizeof(names) / sizeof(names[0]);
                             ++index) {
                            std::string target = index == 1
                                ? std::string(cwd) + "/tests/data/adversarial.pgn"
                                : std::string(cwd) + "/assets/pieces/meridian-shaded/" +
                                  names[index] + ".png";
                            if (symlink(target.c_str(),
                                        (directory + "/" + names[index] +
                                         ".png").c_str()) != 0)
                                created = false;
                        }
                    }
                    expect_true(created &&
                                !meridian.load_piece_set(
                                    PieceTheme(directory), error) &&
                                meridian.piece_image(gambitree::WHITE_KING) &&
                                meridian.piece_image(gambitree::WHITE_KING)->width ==
                                    previous_width,
                                "corrupt member rejects a whole new set without mixing assets");
                    if (!directory.empty()) {
                        for (index = 0;
                             index < sizeof(names) / sizeof(names[0]); ++index)
                            std::remove((directory + "/" + names[index] +
                                         ".png").c_str());
                        rmdir(directory.c_str());
                    }
                }
            }
        }
    }
}

void test_piece_set_registry()
{
    const gambitree::PieceSetDescriptor* first =
        gambitree::piece_set_at(0);
    const gambitree::PieceSetDescriptor* second =
        gambitree::piece_set_at(1);
    const gambitree::PieceSetDescriptor* third = gambitree::piece_set_at(2);
    expect_true(gambitree::piece_set_count() == 3 && first && second && third &&
                std::string(first->id) == "gambitree" &&
                std::string(second->id) == "meridian-shaded" &&
                std::string(third->id) == "cburnett" &&
                gambitree::piece_set_at(3) == 0,
                "registry preserves the existing sets and appends Cburnett");
    expect_true(gambitree::default_piece_set().id == std::string("gambitree") &&
                gambitree::find_piece_set("meridian-shaded") == second &&
                gambitree::find_piece_set("cburnett") == third &&
                gambitree::find_piece_set("not-a-set") == 0,
                "Gambitree is default and registry lookup rejects unknown IDs");
    expect_true(gambitree::next_piece_set_id("gambitree") == "meridian-shaded" &&
                gambitree::next_piece_set_id("meridian-shaded") == "cburnett" &&
                gambitree::next_piece_set_id("cburnett") == "gambitree" &&
                gambitree::next_piece_set_id("unknown") == "gambitree",
                "piece-set cycling follows registry order and wraps");
    expect_true(gambitree::initial_piece_set_id(
                    "meridian-shaded", true, "gambitree") == "meridian-shaded" &&
                gambitree::initial_piece_set_id(
                    "classic", true, "meridian-shaded") == "gambitree" &&
                gambitree::initial_piece_set_id(
                    "classic", false, "meridian-shaded") == "meridian-shaded" &&
                gambitree::initial_piece_set_id(
                    "classic", false, "unknown") == "gambitree",
                "explicit CLI selection wins; invalid persisted IDs default safely");
}

void test_png_resampling()
{
    PngImage source;
    PngImage result;
    PngImage cropped;
    bool no_halo = true;
    bool saw_partial_alpha = false;
    int x;
    source.width = 4;
    source.height = 1;
    source.stride = 16;
    source.rgba.resize(16);
    {
        const unsigned char pixels[16] = {
            255, 0, 0, 0, 255, 255, 255, 255,
            255, 255, 255, 255, 0, 255, 0, 0
        };
        std::memcpy(&source.rgba[0], pixels, sizeof(pixels));
    }
    expect_true(crop_png_to_alpha_bounds(source, cropped) &&
                cropped.width == 2 && cropped.height == 1 &&
                cropped.rgba[0] == 255 && cropped.rgba[1] == 255 &&
                cropped.rgba[2] == 255 && cropped.rgba[3] == 255,
                "alpha crop removes transparent PNG canvas margins");
    expect_true(resize_png_lanczos3(source, source.width, source.height, result) &&
                result.rgba == source.rgba,
                "native-size scaling preserves source bytes exactly");
    expect_true(resize_png_lanczos3(source, 9, 3, result) &&
                result.width == 9 && result.height == 3,
                "Lanczos resampler supports enlargement with exact output bounds");
    {
        gambitree::PngLanczos3Job incremental;
        PngImage incremental_result;
        PngImage monolithic_result;
        unsigned long row_steps = 0;
        expect_true(incremental.start(source, 9, 3),
                    "incremental Lanczos job starts with a private candidate");
        while (!incremental.ready()) {
            incremental.step(1);
            ++row_steps;
        }
        incremental.take_result(incremental_result);
        expect_true(resize_png_lanczos3(source, 9, 3, monolithic_result) &&
                    incremental_result.rgba == monolithic_result.rgba &&
                    row_steps > 1,
                    "row-stepped Lanczos output matches settled high-quality output");
        incremental.cancel();
        expect_true(!incremental.ready(),
                    "cancelled incremental Lanczos candidate is not publishable");
    }
    expect_true(resize_png_bilinear(source, source.width, source.height,
                                    cropped) && cropped.rgba == source.rgba,
                "temporary bilinear native-size path preserves exact pixels");
    for (x = 0; x < result.width * result.height; ++x) {
        const unsigned char* pixel = &result.rgba[static_cast<std::size_t>(x) * 4];
        if (pixel[3] > 0) {
            if (pixel[0] != pixel[1] || pixel[1] != pixel[2])
                no_halo = false;
            if (pixel[3] < 255)
                saw_partial_alpha = true;
        }
    }
    expect_true(no_halo && saw_partial_alpha,
                "premultiplied-alpha filtering keeps a smooth edge without transparent-RGB halos");
    {
        PngImage edge;
        edge.width = 2;
        edge.height = 1;
        edge.stride = 8;
        edge.rgba.assign(8, 0);
        edge.rgba[0] = 255;
        edge.rgba[3] = 0;
        edge.rgba[4] = 0;
        edge.rgba[5] = 0;
        edge.rgba[6] = 255;
        edge.rgba[7] = 255;
        expect_true(resize_png_bilinear(edge, 3, 1, result) &&
                    result.rgba[4] == 0 && result.rgba[5] == 0 &&
                    result.rgba[6] == 255 && result.rgba[7] >= 127 &&
                    result.rgba[7] <= 128,
                    "temporary bilinear filtering uses premultiplied alpha without color halos");
    }
    {
        PngImage opaque;
        opaque.width = 1;
        opaque.height = 1;
        opaque.stride = 4;
        opaque.rgba.push_back(32);
        opaque.rgba.push_back(128);
        opaque.rgba.push_back(240);
        opaque.rgba.push_back(255);
        expect_true(resize_png_lanczos3(opaque, 5, 5, result) &&
                    result.rgba[0] == 32 && result.rgba[1] == 128 &&
                    result.rgba[2] == 240 && result.rgba[3] == 255 &&
                    result.rgba[(24U * 4U) + 3U] == 255,
                    "single opaque pixels scale cleanly with preserved alpha and color");
    }
    {
        PngImage transparent;
        transparent.width = 2;
        transparent.height = 2;
        transparent.stride = 8;
        transparent.rgba.assign(16, 0);
        transparent.rgba[0] = 255;
        transparent.rgba[4] = 0;
        transparent.rgba[5] = 255;
        transparent.rgba[6] = 0;
        expect_true(resize_png_lanczos3(transparent, 3, 3, result),
                    "fully transparent images can be resized");
        {
            bool transparent_rgb_zero = true;
            for (x = 0; x < result.width * result.height; ++x) {
                const unsigned char* pixel = &result.rgba[
                    static_cast<std::size_t>(x) * 4];
                if (pixel[3] != 0 || pixel[0] != 0 || pixel[1] != 0 ||
                    pixel[2] != 0)
                    transparent_rgb_zero = false;
            }
            expect_true(transparent_rgb_zero,
                        "fully transparent resampled pixels have zero alpha and RGB");
        }
    }
    {
        PngImage checker;
        checker.width = 8;
        checker.height = 1;
        checker.stride = 32;
        checker.rgba.resize(32);
        for (x = 0; x < 8; ++x) {
            unsigned char value = x % 2 ? 255 : 0;
            checker.rgba[static_cast<std::size_t>(x) * 4] = value;
            checker.rgba[static_cast<std::size_t>(x) * 4 + 1] = value;
            checker.rgba[static_cast<std::size_t>(x) * 4 + 2] = value;
            checker.rgba[static_cast<std::size_t>(x) * 4 + 3] = 255;
        }
        expect_true(resize_png_lanczos3(checker, 2, 1, result) &&
                    result.rgba[0] > 90 && result.rgba[0] < 165 &&
                    result.rgba[4] > 90 && result.rgba[4] < 165,
                    "Lanczos downscaling filters high-frequency detail instead of aliasing");
    }
}

void test_batched_piece_pixel_composition()
{
    const int surface_width = 100;
    const int surface_height = 60;
    const int image_width = 4;
    const int image_height = 4;
    const unsigned long masks[][3] = {
        { 0x00ff0000UL, 0x0000ff00UL, 0x000000ffUL },
        { 0x0000f800UL, 0x000007e0UL, 0x0000001fUL }
    };
    const unsigned char image[] = {
        255, 0, 255, 0,   245, 230, 220, 1,
        255, 255, 255, 127, 255, 255, 255, 255,
        20, 40, 220, 254,  180, 35, 70, 128,
        255, 80, 0, 0,     10, 90, 160, 220,
        255, 100, 20, 40,  0, 255, 0, 200,
        70, 80, 90, 255,   1, 2, 3, 64,
        240, 130, 20, 192, 5, 220, 150, 0,
        160, 170, 180, 80,  20, 220, 40, 255,
        0, 0, 0, 255,      255, 255, 255, 80,
        30, 80, 210, 180,  255, 0, 0, 0,
        255, 255, 0, 240,  100, 110, 120, 128,
        255, 80, 160, 32,  80, 90, 100, 255,
        190, 120, 40, 224,  255, 0, 255, 0,
        4, 5, 6, 7,          220, 210, 200, 240,
        44, 88, 132, 176,   200, 100, 50, 25,
        255, 255, 255, 255
    };
    const int piece_counts[] = { 0, 2, 16, 32 };
    std::size_t count_index;
    std::size_t mask_index;
    for (mask_index = 0; mask_index < 2; ++mask_index) {
        for (count_index = 0;
             count_index < sizeof(piece_counts) / sizeof(piece_counts[0]);
             ++count_index) {
            std::size_t piece_count =
                static_cast<std::size_t>(piece_counts[count_index]);
            unsigned long background = mask_index == 0
                ? 0x004b6852UL : 0x00004a15UL;
            TestPixelSurface candidate(surface_width, surface_height,
                                       background);
            TestPixelSurface reference(surface_width, surface_height,
                                       background);
            std::size_t expected_blended = 0;
            std::size_t actual_blended = 0;
            int piece;
            for (piece = 0; piece < piece_counts[count_index]; ++piece) {
                int origin_x = 3 + (piece % 8) * 11;
                int origin_y = 3 + (piece / 8) * 13;
                int y;
                actual_blended += gambitree::composite_rgba_image(
                    candidate, 0, 0, origin_x, origin_y, image_width,
                    image_height, image, image_width * 4,
                    masks[mask_index][0], masks[mask_index][1],
                    masks[mask_index][2]);
                for (y = 0; y < image_height; ++y) {
                    int x;
                    for (x = 0; x < image_width; ++x) {
                        const unsigned char* source = image +
                            (y * image_width + x) * 4;
                        unsigned long old_pixel = reference.pixel(
                            origin_x + x, origin_y + y);
                        if (source[3]) {
                            reference.set_pixel(
                                origin_x + x, origin_y + y,
                                reference_composite_pixel(
                                    old_pixel, source, masks[mask_index][0],
                                    masks[mask_index][1],
                                    masks[mask_index][2]));
                            ++expected_blended;
                        }
                    }
                }
            }
            expect_true(actual_blended == expected_blended &&
                        candidate.pixels() == reference.pixels(),
                        "batched local piece composition matches legacy X pixel math");
            expect_true(
                gambitree::board_snapshot_readbacks_for_piece_batch(
                    piece_count) == (piece_count == 0 ? 0U : 1U) &&
                gambitree::per_piece_readbacks_for_piece_batch(piece_count) ==
                    0U,
                "piece batch uses at most one shared server snapshot, never per-piece readback");
        }
    }
    {
        TestPixelSurface clipped(5, 5, 0x004b6852UL);
        std::size_t blended = gambitree::composite_rgba_image(
            clipped, 0, 0, -2, -1, image_width, image_height, image,
            image_width * 4, masks[0][0], masks[0][1], masks[0][2]);
        expect_true(blended > 0 && blended < image_width * image_height,
                    "batched piece composition clips at raster edges");
    }
}

void test_asset_locator()
{
    static const char* const names[] = {
        "white-king", "white-queen", "white-rook", "white-bishop", "white-knight", "white-pawn",
        "black-king", "black-queen", "black-rook", "black-bishop", "black-knight", "black-pawn"
    };
    const std::string base_storage = test_scratch_directory + "/gambitree-asset locator";
    const char* base = base_storage.c_str();
    const std::string installed = std::string(base) + "/installed";
    const std::string executable = std::string(base) + "/bin/gambitree";
    std::vector<std::string> roots;
    std::vector<std::string> board_textures;
    std::ofstream file;
    std::size_t index;
    mkdir(base, 0700); mkdir(installed.c_str(), 0700); mkdir((installed + "/pieces").c_str(), 0700); mkdir((installed + "/pieces/classic").c_str(), 0700);
    mkdir((std::string(base) + "/bin").c_str(), 0700); mkdir((std::string(base) + "/bin/assets").c_str(), 0700);
    mkdir((std::string(base) + "/bin/assets/pieces").c_str(), 0700); mkdir((std::string(base) + "/bin/assets/pieces/classic").c_str(), 0700);
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        file.open((installed + "/pieces/classic/" + names[index] + ".png").c_str()); file.close();
        file.open((std::string(base) + "/bin/assets/pieces/classic/" + names[index] + ".png").c_str()); file.close();
    }
    roots = AssetLocator::piece_roots(installed, "classic", executable);
    expect_true(roots.size() >= 2 && roots[0] == installed + "/pieces/classic" &&
                roots[1] == std::string(base) + "/bin/assets/pieces/classic",
                "asset lookup prefers installed root then executable-relative development assets");
    board_textures = AssetLocator::board_texture_candidates(
        installed, "wood-light.png", executable);
    expect_true(board_textures.size() >= 3 &&
                board_textures[0] == installed + "/boards/wood-light.png" &&
                board_textures[1] == std::string(base) +
                    "/bin/assets/boards/wood-light.png" &&
                board_textures[2] == "assets/boards/wood-light.png" &&
                AssetLocator::board_texture_candidates(
                    installed, "../wood-light.png", executable).empty(),
                "board texture lookup checks installed and executable-relative assets and rejects path traversal");
    expect_true(AssetLocator::contains_piece_set(roots[0]) && AssetLocator::contains_piece_set(roots[1]),
                "a complete twelve-piece asset root works independently of cwd and preserves spaces in paths");
    {
        std::string source_root;
        std::string source_executable;
        std::vector<std::string> meridian_roots = AssetLocator::piece_roots(
            "/usr/local/share/gambitree", "meridian-shaded",
            source_directory + "/gambitree");
        expect_true(meridian_roots.size() >= 2 &&
                    meridian_roots[0] ==
                        "/usr/local/share/gambitree/pieces/meridian-shaded" &&
                    meridian_roots[1] ==
                        source_directory + "/assets/pieces/meridian-shaded" &&
                    AssetLocator::contains_piece_set(meridian_roots[1]),
                    "source-tree executable-relative lookup finds Meridian independent of cwd");
        if (std::getenv("GAMBITREE_TEST_INSTALLED_PIECES")) {
            const std::string installed_pieces =
                std::getenv("GAMBITREE_TEST_INSTALLED_PIECES");
            const std::string installed_data = installed_pieces.substr(
                0, installed_pieces.find_last_of('/'));
            expect_true(AssetLocator::find_piece_root(
                            installed_data,
                            "meridian-shaded", source_root,
                            source_executable) &&
                        AssetLocator::contains_piece_set(source_root),
                        "staged install discovers all installed Meridian runtime assets");
            expect_true(AssetLocator::find_piece_root(
                            installed_data,
                            "cburnett", source_root, source_executable) &&
                        AssetLocator::contains_piece_set(source_root) &&
                        source_root == installed_pieces + "/cburnett",
                        "staged install discovers Cburnett without a development fallback");
        }
    }
    std::remove((installed + "/pieces/classic/white-pawn.png").c_str());
    expect_true(!AssetLocator::contains_piece_set(roots[0]),
                "a theme missing any required piece is safely rejected");
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        std::remove((installed + "/pieces/classic/" + names[index] + ".png").c_str());
        std::remove((std::string(base) + "/bin/assets/pieces/classic/" + names[index] + ".png").c_str());
    }
    rmdir((std::string(base) + "/bin/assets/pieces/classic").c_str()); rmdir((std::string(base) + "/bin/assets/pieces").c_str());
    rmdir((std::string(base) + "/bin/assets").c_str()); rmdir((std::string(base) + "/bin").c_str());
    rmdir((installed + "/pieces/classic").c_str()); rmdir((installed + "/pieces").c_str()); rmdir(installed.c_str()); rmdir(base);
}

void test_application_icon()
{
    PngImage image;
    std::vector<unsigned long> pixels;
    std::vector<unsigned long> property;
    std::vector<std::vector<std::string> > candidates;
    std::vector<int> sizes;
    std::string error;
    image.width = 2; image.height = 1; image.stride = 8;
    image.rgba.push_back(0x12); image.rgba.push_back(0x34); image.rgba.push_back(0x56); image.rgba.push_back(0x78);
    image.rgba.push_back(0xff); image.rgba.push_back(0x00); image.rgba.push_back(0x80); image.rgba.push_back(0x00);
    expect_true(gambitree::rgba_to_ewmh_argb(image, pixels, error) && pixels.size() == 2 &&
                pixels[0] == 0x78123456UL && pixels[1] == 0x00ff0080UL,
                "RGBA icon pixels convert to EWMH ARGB with alpha and channel order preserved");
    expect_true(gambitree::append_ewmh_icon_image(image, property, error) && property.size() == 4 &&
                property[0] == 2 && property[1] == 1 && property[2] == 0x78123456UL,
                "an EWMH image record begins with width and height followed by pixels");
    image.width = 1; image.height = 1; image.stride = 4; image.rgba.assign(4, 255);
    expect_true(gambitree::append_ewmh_icon_image(image, property, error) && property.size() == 7 &&
                property[4] == 1 && property[5] == 1 && property[6] == 0xffffffffUL,
                "multiple EWMH icon image records concatenate in one property payload");
    image.width = 513; image.height = 1; image.stride = 513U * 4U; image.rgba.resize(image.stride);
    expect_true(!gambitree::rgba_to_ewmh_argb(image, pixels, error),
                "oversized icon dimensions are rejected before allocation");
    image.width = 2; image.height = 2; image.stride = 8; image.rgba.resize(8);
    expect_true(!gambitree::rgba_to_ewmh_argb(image, pixels, error),
                "truncated RGBA storage is rejected safely");
    image.width = 1; image.height = 2; image.stride = std::numeric_limits<std::size_t>::max(); image.rgba.clear();
    expect_true(!gambitree::rgba_to_ewmh_argb(image, pixels, error),
                "overflowing icon stride and height are rejected");

    {
        const std::string base_storage = test_scratch_directory + "/gambitree-icon-test";
        const char* base = base_storage.c_str();
        const std::string corrupt_path_storage = test_scratch_directory + "/gambitree-icon-test-corrupt.png";
        const char* corrupt_path = corrupt_path_storage.c_str();
        const std::string fake_executable = std::string(base) + "/bin/gambitree";
        const std::string installed_root = std::string(base) + "/stage/usr/share/gambitree";
        const std::string installed_icon = std::string(base) + "/stage/usr/share/icons/hicolor/16x16/apps/gambitree.png";
        const std::string executable_icon = std::string(base) + "/bin/assets/icons/gambitree-16.png";
        std::vector<std::string> paths;
        std::ifstream input;
        std::ofstream output;
        mkdir(base, 0700);
        mkdir((std::string(base) + "/bin").c_str(), 0700);
        mkdir((std::string(base) + "/bin/assets").c_str(), 0700);
        mkdir((std::string(base) + "/bin/assets/icons").c_str(), 0700);
        mkdir((std::string(base) + "/stage").c_str(), 0700);
        mkdir((std::string(base) + "/stage/usr").c_str(), 0700);
        mkdir((std::string(base) + "/stage/usr/share").c_str(), 0700);
        mkdir((installed_root).c_str(), 0700);
        mkdir((std::string(base) + "/stage/usr/share/icons").c_str(), 0700);
        mkdir((std::string(base) + "/stage/usr/share/icons/hicolor").c_str(), 0700);
        mkdir((std::string(base) + "/stage/usr/share/icons/hicolor/16x16").c_str(), 0700);
        mkdir((std::string(base) + "/stage/usr/share/icons/hicolor/16x16/apps").c_str(), 0700);
        input.open("assets/icons/gambitree-16.png", std::ios::binary);
        output.open(installed_icon.c_str(), std::ios::binary);
        if (input && output) output << input.rdbuf();
        input.close(); output.close();
        input.open("assets/icons/gambitree-16.png", std::ios::binary);
        output.open(executable_icon.c_str(), std::ios::binary);
        if (input && output) output << input.rdbuf();
        input.close(); output.close();
        paths = AssetLocator::icon_path_candidates(installed_root, 16, fake_executable);
        expect_true(paths.size() == 3 && paths[0] == installed_root + "/../icons/hicolor/16x16/apps/gambitree.png" &&
                    paths[1] == executable_icon && paths[2] == "assets/icons/gambitree-16.png",
                    "icon lookup orders installed hicolor, executable-relative development, then cwd fallback");
        {
            char original_directory[4096];
            bool have_original_directory = getcwd(original_directory, sizeof(original_directory)) != 0;
            bool changed_directory = have_original_directory && chdir(test_scratch_directory.c_str()) == 0;
            bool loaded_from_executable = changed_directory &&
                gambitree::load_application_icon("", source_directory + "/gambitree", property, error);
            if (changed_directory) chdir(original_directory);
            expect_true(loaded_from_executable,
                        "application icons load from beside the executable when cwd is unrelated");
        }
        sizes = gambitree::application_icon_sizes();
        {
            std::size_t offset = 0;
            bool records_match = property.size() > 0 && sizes.size() == 8;
            std::size_t i;
            for (i = 0; records_match && i < sizes.size(); ++i) {
                std::size_t count;
                if (offset + 2 > property.size() || property[offset] != static_cast<unsigned long>(sizes[i]) ||
                    property[offset + 1] != static_cast<unsigned long>(sizes[i])) { records_match = false; break; }
                count = static_cast<std::size_t>(sizes[i]) * static_cast<std::size_t>(sizes[i]);
                if (count > property.size() - offset - 2) { records_match = false; break; }
                offset += count + 2;
            }
            expect_true(records_match && offset == property.size(),
                        "the combined icon property contains all eight exact-size image records");
        }
        expect_true(gambitree::load_application_icon(installed_root, fake_executable, property, error),
                    "installed hicolor icon lookup works with other sizes supplied by development fallback");
        output.open(corrupt_path, std::ios::binary); output << "not a png"; output.close();
        candidates.clear(); sizes.clear();
        candidates.push_back(std::vector<std::string>(1, std::string(test_scratch_directory + "/no-such-gambitree-icon.png")));
        candidates.push_back(std::vector<std::string>(1, corrupt_path));
        sizes.push_back(16); sizes.push_back(24);
        property.clear();
        expect_true(!gambitree::load_ewmh_icon_candidates(candidates, sizes, property, error) && property.empty(),
                    "missing and corrupt icon assets are skipped without crashing and report no usable icon");
        std::remove(corrupt_path); std::remove(installed_icon.c_str()); std::remove(executable_icon.c_str());
        rmdir((std::string(base) + "/stage/usr/share/icons/hicolor/16x16/apps").c_str());
        rmdir((std::string(base) + "/stage/usr/share/icons/hicolor/16x16").c_str());
        rmdir((std::string(base) + "/stage/usr/share/icons/hicolor").c_str());
        rmdir((std::string(base) + "/stage/usr/share/icons").c_str());
        rmdir(installed_root.c_str()); rmdir((std::string(base) + "/stage/usr/share").c_str());
        rmdir((std::string(base) + "/stage/usr").c_str()); rmdir((std::string(base) + "/stage").c_str());
        rmdir((std::string(base) + "/bin/assets/icons").c_str()); rmdir((std::string(base) + "/bin/assets").c_str());
        rmdir((std::string(base) + "/bin").c_str()); rmdir(base);
    }
}

void test_session_state()
{
    using gambitree::SessionState;
    SessionState state = gambitree::default_session_state();
    SessionState parsed;
    std::string xdg = gambitree::session_state_path(
        "/state/home", "/user/home");
    std::ostringstream root_name;
    std::string base;
    struct stat info;
    expect_true(state.width == 900 && state.height == 650 &&
                state.analysis_enabled && !state.has_position &&
                state.piece_set_id == "gambitree",
                "session state defaults match the existing startup behavior");
    expect_true(xdg == "/state/home/gambitree/state" &&
                gambitree::session_state_path("/", 0) ==
                    "/gambitree/state" &&
                gambitree::session_state_path("relative", "/user/home") ==
                    "/user/home/.local/state/gambitree/state" &&
                gambitree::session_state_path(0, 0).empty(),
                "session state prefers absolute XDG state home and falls back to HOME");
    expect_true(gambitree::parse_session_state(
                    "version=1\nwidth=1200\nheight=800\nx=-180\ny=45\n"
                    "analysis=0\nunknown=ok\n", parsed) &&
                parsed.width == 1200 && parsed.height == 800 &&
                parsed.x == -180 && parsed.y == 45 && parsed.has_position &&
                !parsed.analysis_enabled && !parsed.position_is_outer,
                "legacy client-position state parses and ignores unknown keys");
    expect_true(gambitree::parse_session_state(
                    "version=1\nwidth=1024\nheight=700\n", parsed) &&
                parsed.width == 1024 && parsed.height == 700 &&
                !parsed.has_position && parsed.analysis_enabled,
                "size-only state preserves default analysis preference");
    expect_true(gambitree::parse_session_state(
                    "version=1\nanalysis=0\n", parsed) &&
                parsed.width == 900 && parsed.height == 650 &&
                !parsed.has_position && !parsed.analysis_enabled &&
                parsed.piece_set_id == "gambitree",
                "analysis-only state uses default window size");
    expect_true(gambitree::parse_session_state(
                    "pieces=meridian-shaded\nunknown=value\n", parsed) &&
                parsed.piece_set_id == "meridian-shaded",
                "persisted stable piece-set ID is recognized while unknown keys are ignored");
    expect_true(gambitree::parse_session_state(
                    "pieces=cburnett\nboard_theme=wood\n", parsed) &&
                parsed.piece_set_id == "cburnett" &&
                parsed.board_theme_id == "wood" &&
                gambitree::initial_piece_set_id("classic", false,
                    parsed.piece_set_id) == "cburnett" &&
                gambitree::initial_piece_set_id("gambitree", true,
                    parsed.piece_set_id) == "gambitree",
                "Cburnett persists independently of wood and explicit CLI selection wins");
    {
        SessionState restored;
        expect_true(gambitree::parse_session_state(
                        gambitree::serialize_session_state(parsed), restored) &&
                    restored.piece_set_id == "cburnett" &&
                    restored.board_theme_id == "wood",
                    "Cburnett and wood round-trip through existing session format");
    }
    expect_true(!gambitree::parse_session_state(
                    "pieces=meridian-shaded\npieces=missing-set\n", parsed) &&
                parsed.piece_set_id == "gambitree",
                "duplicate pieces keys use last-wins and unknown IDs fall back to default");
    expect_true(gambitree::parse_session_state(
                    "pieces=meridian-shaded\npieces=gambitree\n", parsed) &&
                parsed.piece_set_id == "gambitree",
                "valid duplicate pieces IDs use the final stable identifier");
    expect_true(!gambitree::parse_session_state(
                    "version=1\nwidth=999999999\nheight=700\n"
                    "analysis=maybe\nx=-4\ny=999999999\n", parsed) &&
                parsed.width == 900 && parsed.height == 700 &&
                parsed.analysis_enabled && !parsed.has_position,
                "invalid values use per-field defaults without rejecting valid fields");
    expect_true(!gambitree::parse_session_state(
                    "version=1\nwidth=-900\nheight=-2\nanalysis=2\n", parsed) &&
                parsed.width == 900 && parsed.height == 650 &&
                parsed.analysis_enabled,
                "negative dimensions and invalid analysis value are ignored safely");
    expect_true(gambitree::parse_session_state(
                    "version=1\nwidth=640\nwidth=1000\nanalysis=1\n"
                    "analysis=0\n", parsed) &&
                parsed.width == 1000 && !parsed.analysis_enabled,
                "duplicate keys use the last occurrence");
    expect_true(!gambitree::parse_session_state(
                    "version=99\nwidth=1200\n", parsed) &&
                parsed.width == 900 && parsed.height == 650,
                "unsupported state version falls back to defaults");
    state.width = 1366;
    state.height = 768;
    state.x = -300;
    state.y = 70;
    state.has_position = true;
    state.analysis_enabled = false;
    state.piece_set_id = "meridian-shaded";
    expect_true(gambitree::parse_session_state(
                    gambitree::serialize_session_state(state), parsed) &&
                parsed.width == state.width && parsed.height == state.height &&
                parsed.x == state.x && parsed.y == state.y &&
                parsed.has_position &&
                parsed.analysis_enabled == state.analysis_enabled &&
                parsed.piece_set_id == state.piece_set_id &&
                parsed.position_is_outer,
                "serialized session state round-trips");
    state.width = 90000;
    state.height = 0;
    expect_true(gambitree::parse_session_state(
                    gambitree::serialize_session_state(state), parsed) &&
                parsed.width == 8192 && parsed.height == 1,
                "serialization clamps dimensions to safe X11 bounds");
    state.width = 1366;
    state.height = 768;

    root_name << test_scratch_directory + "/gambitree-session-" << getpid();
    base = root_name.str();
    if (mkdir(base.c_str(), 0700) != 0) {
        expect_true(false, "session state test directory is created under scratch");
        return;
    }
    {
        const std::string path = base + "/.local/state/gambitree/state";
        SessionState loaded;
        expect_true(gambitree::load_session_state(path, loaded) ==
                        gambitree::SESSION_STATE_ABSENT,
                    "missing state file reports absent and uses defaults");
        expect_true(gambitree::save_session_state(path, state),
                    "state save creates its private directory tree");
        {
            std::ofstream broken(path.c_str());
            broken << "version=1\nwidth=bad\nheight=720\nanalysis=0\n";
        }
        expect_true(gambitree::load_session_state(path, loaded) ==
                        gambitree::SESSION_STATE_LOADED_WITH_INVALID_FIELDS &&
                    loaded.width == 900 && loaded.height == 720 &&
                    !loaded.analysis_enabled,
                    "corrupt field is defaulted while valid saved fields load");
        expect_true(gambitree::save_session_state(path, state),
                    "state save creates parent directories and publishes atomically");
        expect_true(gambitree::load_session_state(path, loaded) ==
                        gambitree::SESSION_STATE_LOADED &&
                    loaded.width == state.width && loaded.height == state.height &&
                    loaded.x == state.x && loaded.y == state.y &&
                    loaded.has_position && !loaded.analysis_enabled,
                    "saved state reloads with all session preferences");
        expect_true(stat(path.c_str(), &info) == 0 &&
                    (info.st_mode & 0777) == 0600,
                    "session state file is private to its owner");
        {
            DIR* directory = opendir((base + "/.local/state/gambitree").c_str());
            struct dirent* entry;
            bool temporary_remains = false;
            if (directory) {
                while ((entry = readdir(directory)) != 0) {
                    if (std::string(entry->d_name).find("state.tmp.") == 0)
                        temporary_remains = true;
                }
                closedir(directory);
            }
            expect_true(directory != 0 && !temporary_remains,
                        "atomic state publication leaves no temporary file");
        }
        {
            std::ofstream blocker((base + "/blocker").c_str());
            blocker << "file";
        }
        expect_true(!gambitree::save_session_state(
                        base + "/blocker/state", state),
                    "state write failure is reported without throwing");
        std::remove(path.c_str());
        rmdir((base + "/.local/state/gambitree").c_str());
        rmdir((base + "/.local/state").c_str());
        rmdir((base + "/.local").c_str());
        std::remove((base + "/blocker").c_str());
    }
    rmdir(base.c_str());

    {
        gambitree::WindowFrameExtents none = { 0, 0, 0, 0 };
        gambitree::WindowFrameExtents decorations = { 7, 5, 26, 4 };
        gambitree::WindowFrameExtents other = { 11, 9, 31, 8 };
        int outer_x;
        int outer_y;
        int client_x;
        int client_y;
        expect_true(gambitree::client_to_outer_position(100, 100, none,
                                                        outer_x, outer_y) &&
                    outer_x == 100 && outer_y == 100,
                    "zero frame extents preserve the window origin");
        expect_true(gambitree::client_to_outer_position(107, 126,
                                                        decorations,
                                                        outer_x, outer_y) &&
                    outer_x == 100 && outer_y == 100,
                    "client origin converts to outer origin using actual extents");
        expect_true(gambitree::outer_to_client_position(100, 100,
                                                        decorations,
                                                        client_x, client_y) &&
                    client_x == 107 && client_y == 126 &&
                    gambitree::client_to_outer_position(client_x, client_y,
                                                        decorations,
                                                        outer_x, outer_y) &&
                    outer_x == 100 && outer_y == 100,
                    "client/outer conversion round-trips with decorations");
        expect_true(gambitree::outer_to_client_position(-1920, -1080, other,
                                                        client_x, client_y) &&
                    gambitree::client_to_outer_position(client_x, client_y,
                                                        other, outer_x, outer_y) &&
                    outer_x == -1920 && outer_y == -1080,
                    "coordinate conversion supports negative monitor origins and other extents");
        expect_true(gambitree::parse_session_state(
                        "version=1\nwidth=900\nheight=650\nx=107\ny=126\n",
                        parsed) && !parsed.position_is_outer &&
                    !gambitree::can_restore_session_position(parsed, false) &&
                    gambitree::can_restore_session_position(parsed, true) &&
                    gambitree::client_to_outer_position(parsed.x, parsed.y,
                                                        decorations,
                                                        outer_x, outer_y) &&
                    outer_x == 100 && outer_y == 100,
                    "version 1 position migrates from client to outer coordinates");
        expect_true(gambitree::parse_session_state(
                        "version=2\nwidth=900\nheight=650\nx=100\ny=100\n",
                        parsed) && parsed.position_is_outer &&
                    parsed.x == 100 && parsed.y == 100 &&
                    !gambitree::can_restore_session_position(parsed, false) &&
                    gambitree::can_restore_session_position(parsed, true),
                    "version 2 stores outer origin and requires known frame extents to restore");
        {
            int saved_x = 100;
            int saved_y = 100;
            int cycle;
            bool stable = true;
            for (cycle = 0; cycle < 10; ++cycle) {
                int requested_outer_x = saved_x;
                int requested_outer_y = saved_y;
                /* PPosition is submitted as the outer placement origin;
                   the WM then places the decorated client inside it. */
                int actual_client_x = requested_outer_x + decorations.left;
                int actual_client_y = requested_outer_y + decorations.top;
                if (!gambitree::client_to_outer_position(
                        actual_client_x, actual_client_y, decorations,
                        saved_x, saved_y) || saved_x != 100 || saved_y != 100)
                    stable = false;
            }
            expect_true(stable,
                        "ten capture/restore cycles do not accumulate frame-offset drift");
        }
    }

    {
        int x = 100, y = 80;
        expect_true(gambitree::keep_window_visible(x, y, 900, 650,
                                                   0, 0, 1920, 1080) &&
                    x == 100 && y == 80,
                    "visible saved position is preserved");
        x = -1800; y = -900;
        expect_true(gambitree::keep_window_visible(x, y, 900, 650,
                                                   -1920, -1080, 1920, 1080) &&
                    x == -1800 && y == -900,
                    "negative monitor coordinates are supported");
        x = 20; y = -1000;
        expect_true(gambitree::keep_window_visible(x, y, 900, 650,
                                                   0, -1080, 1920, 1080) &&
                    x == 20 && y == -1000,
                    "monitor positioned above the primary remains usable");
        x = 9000; y = 9000;
        expect_true(gambitree::keep_window_visible(x, y, 900, 650,
                                                   0, 0, 1920, 1080) &&
                    x == 1824 && y == 984,
                    "fully off-screen position is clamped onto the screen");
        x = -850; y = 20;
        expect_true(gambitree::keep_window_visible(x, y, 900, 650,
                                                   0, 0, 1920, 1080) &&
                    x == -804 && y == 20,
                    "mostly off-screen position is adjusted to keep a visible strip");
        x = -3000; y = -2000;
        expect_true(gambitree::keep_window_visible(x, y, 2400, 1200,
                                                   0, 0, 1920, 1080) &&
                    x == -2304 && y == -1104,
                    "window larger than screen retains a visible region");
        x = 900; y = 700;
        expect_true(gambitree::keep_window_visible(x, y, 900, 650,
                                                   0, 0, 1280, 720) &&
                    x == 900 && y == 624,
                    "changed screen layout clamps only the off-screen axis");
    }
    expect_true(gambitree::initial_analysis_request_due(true, true, false) &&
                !gambitree::initial_analysis_request_due(true, true, true) &&
                !gambitree::initial_analysis_request_due(true, false, false) &&
                !gambitree::initial_analysis_request_due(false, true, false),
                "restored analysis is requested once after an initial position is ready");
    state = gambitree::default_session_state();
    state.analysis_enabled = !state.analysis_enabled;
    expect_true(gambitree::parse_session_state(
                    gambitree::serialize_session_state(state), parsed) &&
                !parsed.analysis_enabled,
                "A-toggle preference is serialized for the next session");
}
void test_session_autosave()
{
    using namespace gambitree;
    const std::string base = test_paths::make_directory("autosave", test_scratch_directory);
    expect_true(!base.empty(), "autosave fixtures use an explicit scratch directory");
    if (base.empty()) return;
    const std::string path = base + "/state";
    const SessionState initial = default_session_state();
    SessionState state = initial, loaded;
    expect_true(save_session_state(path, initial), "seed the existing atomic state writer");
    SessionStateSaver saver(path, initial);
    state.board_theme_id = "wood-veneer";
    state.piece_set_id = "cburnett";
    state.notation_points = 16; state.engine_points = 14;
    state.figurines = true; state.board_share = 4000; state.analysis_enabled = false;
    saver.observe(state, 100);
    expect_true(saver.pending() && !saver.due(1099) &&
                saver.remaining_milliseconds(100, 5000) == 1000 &&
                saver.save(state, 1099) == SESSION_SAVE_UNCHANGED,
                "one change waits the full 1000 monotonic milliseconds");
    // Unrelated user/engine activity just observes the same persistent values.
    for (uint64_t now = 200; now < 1100; now += 100) saver.observe(state, now);
    expect_true(saver.due(1100) && saver.save(state, 1100) == SESSION_SAVE_SAVED &&
                !saver.pending() && saver.save(state, 5000, true) == SESSION_SAVE_UNCHANGED,
                "ordinary activity does not move the deadline and one save clears dirty state");
    expect_true(load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                loaded.board_theme_id == "wood-veneer" && loaded.piece_set_id == "cburnett" &&
                loaded.notation_points == 16 && loaded.engine_points == 14 &&
                loaded.figurines && loaded.board_share == 4000 && !loaded.analysis_enabled,
                "all existing appearance and analysis preferences survive autosave");
    state.engine_points = 17;
    saver.observe(state, 6000);
    expect_true(!saver.due(6001) && saver.save(state, 6001, true) == SESSION_SAVE_SAVED,
                "normal shutdown immediately flushes an outstanding change");
    state.engine_points = 18; saver.observe(state, 7000);
    state.engine_points = 17; saver.observe(state, 7100);
    expect_true(!saver.pending(), "reverting to the last saved values cancels a pending write");

    const std::string resize_path = base + "/resize";
    SessionState resized = initial;
    save_session_state(resize_path, initial);
    SessionStateSaver resize(resize_path, initial);
    unsigned long writes = 0;
    bool quiet = true;
    const unsigned long events = 100000;
    const std::clock_t started = std::clock();
    for (unsigned long i = 0; i < events; ++i) {
        resized.width = 1000 + i % 500;
        resized.height = 700 + i % 100;
        const uint64_t now = static_cast<uint64_t>(i) * 20;
        resize.observe(resized, now);
        quiet = quiet && resize.save(resized, now) == SESSION_SAVE_UNCHANGED;
    }
    const double elapsed_ms = (std::clock() - started) * 1000.0 / CLOCKS_PER_SEC;
    const uint64_t last = static_cast<uint64_t>(events - 1) * 20;
    struct stat info;
    expect_true(quiet && stat((resize_path + ".lock").c_str(), &info) != 0 &&
                !resize.due(last + 999),
                "100000 resize events perform no writes or lock-file I/O during the sequence");
    // Simulate the idle event loop using its timeout, without sleeping or events.
    uint64_t now = last;
    while (!resize.due(now)) now += resize.remaining_milliseconds(now, 100);
    if (resize.save(resized, now) == SESSION_SAVE_SAVED) ++writes;
    if (resize.save(resized, now + 1000) == SESSION_SAVE_SAVED) ++writes;
    expect_true(now == last + 1000 && writes == 1 &&
                load_session_state(resize_path, loaded) == SESSION_STATE_LOADED &&
                loaded.width == resized.width && loaded.height == resized.height,
                "the idle timeout saves exactly once 1000ms after the final resize");
    std::cout << "Autosave stress: " << events << " resizes in " << elapsed_ms
              << "ms CPU, 0 writes during resize, 1 after idle.\n";

    SessionState mapped = initial;
    SessionStateSaver geometry(base + "/geometry", initial);
    mapped.width = 1100; mapped.has_position = true; mapped.x = 80; mapped.y = 90;
    geometry.adopt_initial_geometry(mapped);
    geometry.observe(mapped, 0);
    expect_true(!geometry.pending(), "initial WM placement is not a local configuration edit");
    geometry.observe(mapped, 20, true);
    expect_true(geometry.save(mapped, 1020) == SESSION_SAVE_UNCHANGED && !geometry.pending(),
                "unchanged settled geometry produces no write");
    geometry.observe(mapped, 2000, true);
    mapped.x += 40;
    expect_true(geometry.save(mapped, 3000) == SESSION_SAVE_SAVED &&
                load_session_state(base + "/geometry", loaded) == SESSION_STATE_LOADED &&
                loaded.has_position && loaded.x == mapped.x,
                "move-only hints persist the settled outer-frame position");
    SessionStateSaver startup(base + "/startup", initial);
    SessionState explicit_preference = initial;
    explicit_preference.piece_set_id = "cburnett";
    startup.observe(explicit_preference, 100);
    explicit_preference.has_position = true; explicit_preference.x = 60;
    startup.adopt_initial_geometry(explicit_preference);
    expect_true(startup.pending() && startup.due(1100) &&
                startup.save(explicit_preference, 1100) == SESSION_SAVE_SAVED,
                "adopting initial WM geometry preserves a pending explicit preference");

    const std::string block = base + "/blocked";
    { std::ofstream file(block.c_str()); file << "block directory creation"; }
    SessionStateSaver failing(block + "/state", initial);
    state = initial; state.notation_points = 18;
    failing.observe(state, 0);
    expect_true(failing.save(state, 1000) == SESSION_SAVE_FAILED && failing.pending() &&
                !failing.due(5999) && failing.save(state, 5999) == SESSION_SAVE_UNCHANGED,
                "write errors retain dirty state and back off for five seconds");
    unlink(block.c_str()); mkdir(block.c_str(), 0700);
    expect_true(failing.save(state, 6000) == SESSION_SAVE_SAVED && !failing.pending(),
                "a later retry saves the still-dirty configuration");

    const std::string shared = base + "/shared";
    save_session_state(shared, initial);
    SessionStateSaver stale(shared, initial), parent(shared, initial);
    int ready[2], release[2];
    const bool pipes = pipe(ready) == 0 && pipe(release) == 0;
    expect_true(pipes, "create process barriers for concurrent session writers");
    if (pipes) {
        const pid_t child = fork();
        if (child == 0) {
            close(ready[0]); close(release[1]);
            SessionState local;
            bool ok = load_session_state(shared, local) == SESSION_STATE_LOADED;
            SessionStateSaver writer(shared, local);
            local.board_theme_id = "wood";
            writer.observe(local, 0);
            const int fd = open((shared + ".lock").c_str(), O_RDWR | O_CREAT, 0600);
            struct flock lock; std::memset(&lock, 0, sizeof(lock));
            lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
            ok = ok && fd >= 0 && fcntl(fd, F_SETLK, &lock) == 0;
            const char result = ok ? 'Y' : 'N';
            if (write(ready[1], &result, 1) != 1) _exit(2);
            char token;
            if (!ok || read(release[0], &token, 1) != 1) _exit(2);
            close(fd);
            _exit(writer.save(local, 1000) == SESSION_SAVE_SAVED ? 0 : 3);
        }
        close(ready[1]); close(release[0]);
        char token = 0;
        const bool locked = child > 0 && read(ready[0], &token, 1) == 1 && token == 'Y';
        expect_true(locked, "second process holds the stable session lock inode");
        if (locked) {
            SessionState local = initial; local.notation_points = 20;
            parent.observe(local, 0);
            expect_true(parent.save(local, 1000) == SESSION_SAVE_BUSY && parent.pending(),
                        "a contended session lock never blocks the event loop");
            expect_true(write(release[1], "Y", 1) == 1, "release the other configuration writer");
            int status = 0; waitpid(child, &status, 0);
            expect_true(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                        parent.save(local, 1999) == SESSION_SAVE_UNCHANGED &&
                        parent.save(local, 2000) == SESSION_SAVE_SAVED &&
                        load_session_state(shared, loaded) == SESSION_STATE_LOADED &&
                        loaded.notation_points == 20 && loaded.board_theme_id == "wood",
                        "stale processes merge their own changed fields without losing the other writer's preferences");
        } else if (child > 0) {
            int status; waitpid(child, &status, 0);
        }
        close(ready[0]); close(release[1]);
    }
    stat(shared.c_str(), &info);
    const ino_t inode = info.st_ino;
    expect_true(stale.save(initial, 3000, true) == SESSION_SAVE_UNCHANGED &&
                stat(shared.c_str(), &info) == 0 && info.st_ino == inode,
                "a clean stale process does not replace newer settings at shutdown");
    SessionStateSaver converged(shared, initial);
    SessionState same_value = initial; same_value.board_theme_id = "wood";
    converged.observe(same_value, 0);
    expect_true(converged.save(same_value, 1000) == SESSION_SAVE_UNCHANGED &&
                !converged.pending() && stat(shared.c_str(), &info) == 0 && info.st_ino == inode,
                "a change already saved by another process needs no duplicate write");
    const pid_t reader = fork();
    if (reader == 0) {
        SessionState recent;
        _exit(load_session_state(shared, recent) == SESSION_STATE_LOADED &&
              recent.board_theme_id == "wood" && recent.notation_points == 20 ? 0 : 1);
    }
    int reader_status = 0;
    if (reader > 0) waitpid(reader, &reader_status, 0);
    expect_true(reader > 0 && WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0,
                "a newly started process reads the recently saved preferences");

    const std::string legacy = base + "/legacy";
    { std::ofstream file(legacy.c_str()); file << "version=1\nwidth=900\nheight=650\nx=50\ny=60\n"; }
    SessionState old;
    load_session_state(legacy, old);
    SessionStateSaver migration(legacy, old);
    old.position_is_outer = true; old.x = 45; old.y = 35;
    migration.adopt_initial_geometry(old);
    old.figurines = true; migration.observe(old, 0);
    expect_true(migration.save(old, 1000) == SESSION_SAVE_SAVED &&
                load_session_state(legacy, loaded) == SESSION_STATE_LOADED &&
                loaded.position_is_outer && loaded.x == 45 && loaded.y == 35 && loaded.figurines,
                "version-1 files migrate without relabelling client coordinates as outer coordinates");
    SessionStateSaver unavailable("", initial);
    unavailable.observe(state, 0);
    expect_true(!unavailable.pending(), "an unavailable state path disables autosave without a retry loop");
    SessionStateSaver extreme(base + "/extreme", initial);
    extreme.observe(state, std::numeric_limits<uint64_t>::max() - 10);
    expect_true(extreme.remaining_milliseconds(std::numeric_limits<uint64_t>::max() - 1, 100) == 1,
                "deadline arithmetic saturates without integer overflow");

    unlink((block + "/state").c_str()); unlink((block + "/state.lock").c_str()); rmdir(block.c_str());
    DIR* entries = opendir(base.c_str());
    if (entries) {
        struct dirent* entry;
        while ((entry = readdir(entries)) != 0) {
            if (entry->d_name[0] != '.') unlink((base + "/" + entry->d_name).c_str());
        }
        closedir(entries);
    }
    rmdir(base.c_str());
}

void test_piece_layout()
{
    PiecePlacement square = PieceLayout::centered(100, 200, 81, 362, 362);
    PiecePlacement portrait = PieceLayout::centered(10, 20, 101, 100, 200);
    PiecePlacement landscape = PieceLayout::centered(10, 20, 101, 200, 100);
    PiecePlacement tiny = PieceLayout::centered(0, 0, 1, 362, 362);

    expect_true(square.valid() && square.width == 72 && square.height == 72,
                "square PNG scales to ninety percent of an odd square");
    expect_true(square.x == 104 && square.y == 204,
                "square PNG is centered with integer rounding");
    expect_true(portrait.width == 45 && portrait.height == 90,
                "portrait PNG preserves aspect ratio");
    expect_true(portrait.x == 38 && portrait.y == 25,
                "portrait PNG canvas is centered in square");
    expect_true(landscape.width == 90 && landscape.height == 45,
                "landscape PNG preserves aspect ratio");
    expect_true(landscape.x == 15 && landscape.y == 48,
                "landscape PNG canvas is centered in square");
    expect_true(!tiny.valid(), "too-small square has no drawable piece placement");
    expect_true(PieceLayout::source_coordinate(0, 73, 362) == 0,
                "first destination row maps to first source row");
    expect_true(PieceLayout::source_coordinate(72, 73, 362) == 361,
                "last destination row maps to last source row without wrapping");
    expect_true(PieceLayout::source_coordinate(0, 7, 3) == 0 &&
                PieceLayout::source_coordinate(6, 7, 3) == 2,
                "small scaling maps first and last source columns exactly once");
    expect_true(PieceLayout::source_coordinate(-1, 73, 362) == -1 &&
                PieceLayout::source_coordinate(73, 73, 362) == -1,
                "out-of-range destination coordinates are rejected");
}

void verify_piece_centering(const PngImage& source, int square_size,
                            const std::string& label,
                            int& old_max_horizontal_margin_difference,
                            int& old_max_vertical_margin_difference,
                            int& maximum_horizontal_margin_difference,
                            int& maximum_vertical_margin_difference)
{
    PngImage scaled;
    gambitree::PngAlphaBounds bounds;
    PiecePlacement canvas;
    PiecePlacement placed;
    const int square_x = 37;
    const int square_y = 53;
    int left;
    int right;
    int top;
    int bottom;
    int old_left;
    int old_right;
    int old_top;
    int old_bottom;
    bool valid = source.valid() &&
        (canvas = PieceLayout::centered(square_x, square_y, square_size,
                                        source.width, source.height)).valid() &&
        gambitree::resize_png_lanczos3(source, canvas.width, canvas.height,
                                       scaled) &&
        gambitree::png_alpha_bounds(scaled, bounds);
    expect_true(valid, (label + " produces scaled alpha bounds").c_str());
    if (!valid)
        return;
    old_left = canvas.x + bounds.x - square_x;
    old_right = square_x + square_size -
                (canvas.x + bounds.x + bounds.width);
    old_top = canvas.y + bounds.y - square_y;
    old_bottom = square_y + square_size -
                 (canvas.y + bounds.y + bounds.height);
    if (std::abs(old_left - old_right) >
        old_max_horizontal_margin_difference)
        old_max_horizontal_margin_difference =
            std::abs(old_left - old_right);
    if (std::abs(old_top - old_bottom) > old_max_vertical_margin_difference)
        old_max_vertical_margin_difference = std::abs(old_top - old_bottom);
    placed = PieceLayout::center_alpha_bounds(square_x, square_y, square_size,
                                               canvas, bounds);
    expect_true(placed.valid() && placed.width == scaled.width &&
                placed.height == scaled.height,
                (label + " keeps cached image dimensions while centering alpha").c_str());
    if (!placed.valid())
        return;
    left = placed.x + bounds.x - square_x;
    right = square_x + square_size -
            (placed.x + bounds.x + bounds.width);
    top = placed.y + bounds.y - square_y;
    bottom = square_y + square_size -
             (placed.y + bounds.y + bounds.height);
    if (std::abs(left - right) > maximum_horizontal_margin_difference)
        maximum_horizontal_margin_difference = std::abs(left - right);
    if (std::abs(top - bottom) > maximum_vertical_margin_difference)
        maximum_vertical_margin_difference = std::abs(top - bottom);
    expect_true(left >= 0 && right >= 0 && top >= 0 && bottom >= 0 &&
                std::abs(left - right) <= 1 && std::abs(top - bottom) <= 1,
                (label + " visible alpha bounds are centered within one pixel").c_str());
}

void normalize_piece_for_test(const PngImage& source, int piece,
                              int square_size, PngImage& scaled,
                              gambitree::PngAlphaBounds& bounds)
{
    int target = PieceLayout::target_height_for_piece(piece, square_size);
    PiecePlacement dimensions = PieceLayout::dimensions_for_target_height(
        source.width, source.height, target);
    if (!dimensions.valid() || dimensions.width > square_size ||
        dimensions.height > square_size ||
        !gambitree::resize_png_lanczos3(source, dimensions.width,
                                       dimensions.height, scaled))
        return;
    gambitree::png_alpha_bounds(scaled, bounds);
}

void test_piece_alpha_centering()
{
    static const int square_sizes[] = { 48, 64, 83, 84, 100, 127 };
    PieceTheme theme("assets/pieces/classic");
    gambitree::AssetManager assets;
    std::string error;
    int maximum_horizontal_margin_difference = 0;
    int pair_rendered_widths[7][2][6] = {{{0}}};
    int pair_rendered_heights[7][2][6] = {{{0}}};
    int pair_rendered_top_margins[7][2][6] = {{{0}}};
    int piece;
    expect_true(assets.load_piece_theme(theme, error),
                "all artwork is loaded and cropped before centering tests");
    if (!assets.piece_image(gambitree::WHITE_KING))
        return;
    for (piece = gambitree::BLACK_KING;
         piece <= gambitree::WHITE_KING; ++piece) {
        const PngImage* cropped;
        std::size_t size_index;
        if (piece == gambitree::EMPTY)
            continue;
        cropped = assets.piece_image(piece);
        if (!cropped)
            continue;
        for (size_index = 0;
             size_index < sizeof(square_sizes) / sizeof(square_sizes[0]);
             ++size_index) {
            PngImage scaled;
            gambitree::PngAlphaBounds bounds;
            PiecePlacement image;
            PiecePlacement placed;
            int square_size = square_sizes[size_index];
            int square_x = 37;
            int square_y = 53;
            int target = PieceLayout::target_height_for_piece(piece,
                                                              square_size);
            int expected_offset = PieceLayout::vertical_offset_for_piece(
                piece, square_size);
            int left;
            int right;
            int top;
            int bottom;
            std::ostringstream label;
            label << "piece " << piece << " at " << square_size
                  << "px";
            normalize_piece_for_test(*cropped, piece, square_size, scaled,
                                     bounds);
            {
                std::ostringstream result;
                result << label.str() << " reaches shared type height "
                       << target << " (actual " << bounds.height << ')';
                expect_true(scaled.valid() && bounds.valid() &&
                            std::abs(bounds.height - target) <= 1,
                            result.str().c_str());
            }
            if (!scaled.valid() || !bounds.valid())
                continue;
            pair_rendered_widths[piece < 0 ? -piece : piece]
                                [piece < 0 ? 1 : 0][size_index] = bounds.width;
            pair_rendered_heights[piece < 0 ? -piece : piece]
                                 [piece < 0 ? 1 : 0][size_index] = bounds.height;
            image.width = scaled.width;
            image.height = scaled.height;
            placed = PieceLayout::place_alpha_bounds_for_piece(
                square_x, square_y, square_size, piece, image, bounds);
            left = placed.x + bounds.x - square_x;
            right = square_x + square_size -
                    (placed.x + bounds.x + bounds.width);
            top = placed.y + bounds.y - square_y;
            bottom = square_y + square_size -
                     (placed.y + bounds.y + bounds.height);
            pair_rendered_top_margins[piece < 0 ? -piece : piece]
                                     [piece < 0 ? 1 : 0][size_index] = top;
            maximum_horizontal_margin_difference = std::max(
                maximum_horizontal_margin_difference, std::abs(left - right));
            expect_true(placed.valid() && left >= 0 && right >= 0 &&
                        top >= 0 && bottom >= 0 &&
                        std::abs(left - right) <= 1 &&
                        bounds.width <= square_size * 9 / 10 &&
                        bounds.height <= square_size * 9 / 10,
                        (label.str() + " stays within safe square margins").c_str());
            expect_true(placed.y == PieceLayout::center_alpha_bounds(
                            square_x, square_y, square_size, image, bounds).y +
                            expected_offset,
                        (label.str() + " uses its reference-scaled type offset").c_str());
            expect_true(std::fabs(
                            scaled.width / static_cast<double>(scaled.height) -
                            cropped->width / static_cast<double>(cropped->height))
                            <= 0.05,
                        (label.str() + " preserves source aspect ratio within rounding").c_str());
        }
    }
    expect_true(maximum_horizontal_margin_difference <= 1,
                "all normalized assets remain horizontally centered");
    std::cout << "Normalized piece assets: max opposing horizontal margins differ by "
              << maximum_horizontal_margin_difference << "px.\n";

    for (std::size_t pair = 1; pair <= 6; ++pair) {
        static const int expected_target_at_84[] = {
            0, 54, 60, 63, 55, 66, 69
        };
        static const int expected_target_factors[] = {
            0, 54, 60, 63, 55, 66, 69
        };
        static const int expected_reference_offsets[] = {
            0, 0, 2, 1, 3, -1, 0
        };
        int white_piece = static_cast<int>(pair);
        int black_piece = -white_piece;
        std::size_t size_index;
        expect_true(PieceLayout::vertical_offset_for_piece(white_piece, 84) ==
                        expected_reference_offsets[pair] &&
                    PieceLayout::vertical_offset_for_piece(black_piece, 84) ==
                        expected_reference_offsets[pair],
                    "white and black counterparts share the exact reference offset");
        expect_true(PieceLayout::target_height_for_piece(white_piece, 84) ==
                        expected_target_at_84[pair] &&
                    PieceLayout::target_height_for_piece(black_piece, 84) ==
                        expected_target_at_84[pair],
                    "reference scale targets remain unchanged");
        for (size_index = 0;
             size_index < sizeof(square_sizes) / sizeof(square_sizes[0]);
             ++size_index) {
            int expected_target =
                (square_sizes[size_index] * expected_target_factors[pair] + 42) /
                84;
            expected_target = std::min(expected_target,
                                       square_sizes[size_index] * 9 / 10);
            expect_true(PieceLayout::target_height_for_piece(
                            white_piece, square_sizes[size_index]) ==
                        PieceLayout::target_height_for_piece(
                            black_piece, square_sizes[size_index]) &&
                        PieceLayout::target_height_for_piece(
                            white_piece, square_sizes[size_index]) ==
                            expected_target,
                        "piece target heights remain unchanged and shared by color");
        }
        for (size_index = 0;
             size_index < sizeof(square_sizes) / sizeof(square_sizes[0]);
             ++size_index) {
            int reference_offset = expected_reference_offsets[pair];
            int magnitude = reference_offset < 0 ? -reference_offset :
                                                   reference_offset;
            int expected_scaled = (magnitude * square_sizes[size_index] + 42) / 84;
            if (reference_offset < 0)
                expected_scaled = -expected_scaled;
            expect_true(PieceLayout::vertical_offset_for_piece(
                            white_piece, square_sizes[size_index]) == expected_scaled &&
                        PieceLayout::vertical_offset_for_piece(
                            black_piece, square_sizes[size_index]) == expected_scaled,
                        "per-type offset scales proportionally with signed rounding");
        }
        for (size_index = 0;
             size_index < sizeof(square_sizes) / sizeof(square_sizes[0]);
             ++size_index)
            expect_true(pair_rendered_heights[pair][0][size_index] > 0 &&
                        pair_rendered_heights[pair][1][size_index] > 0 &&
                        std::abs(pair_rendered_heights[pair][0][size_index] -
                                 pair_rendered_heights[pair][1][size_index]) <= 1,
                        "white and black counterpart silhouettes normalize to equal heights");
        for (size_index = 0;
             size_index < sizeof(square_sizes) / sizeof(square_sizes[0]);
             ++size_index)
            expect_true(pair_rendered_top_margins[pair][0][size_index] ==
                            pair_rendered_top_margins[pair][1][size_index],
                        "white and black counterparts share destination Y per type");
    }
    for (std::size_t size_index = 0;
         size_index < sizeof(square_sizes) / sizeof(square_sizes[0]);
         ++size_index)
        expect_true(PieceLayout::target_height_for_piece(
                        gambitree::WHITE_PAWN, square_sizes[size_index]) ==
                    (square_sizes[size_index] * 54 + 42) / 84 &&
                    PieceLayout::target_height_for_piece(
                        gambitree::BLACK_PAWN, square_sizes[size_index]) ==
                    PieceLayout::target_height_for_piece(
                        gambitree::WHITE_PAWN, square_sizes[size_index]),
                    "both pawn colors use the shared 54px-at-84px target");
    {
        const char* const type_names[] = {
            "", "pawn", "knight", "bishop", "rook", "queen", "king"
        };
        for (std::size_t type = 1; type <= 6; ++type)
            std::cout << "84px " << type_names[type] << " normalized visible bounds: W "
                      << pair_rendered_widths[type][0][3] << 'x'
                      << pair_rendered_heights[type][0][3] << ", B "
                      << pair_rendered_widths[type][1][3] << 'x'
                      << pair_rendered_heights[type][1][3] << ".\n";
    }

    {
        int synthetic_max_horizontal_difference = 0;
        int synthetic_max_vertical_difference = 0;
        const int synthetic_sizes[] = { 48, 64, 83, 100, 127 };
        const int shapes[][4] = {
            { 10, 20, 30, 60 },
            { 7, 11, 31, 59 },
            { 55, 4, 27, 33 }
        };
        std::size_t shape;
        for (shape = 0; shape < 3; ++shape) {
            PngImage original;
            PngImage cropped;
            std::size_t size_index;
            original.width = 100;
            original.height = 100;
            original.stride = 400;
            original.rgba.assign(40000, 0);
            for (int y = shapes[shape][1];
                 y < shapes[shape][1] + shapes[shape][3]; ++y) {
                for (int x = shapes[shape][0];
                     x < shapes[shape][0] + shapes[shape][2]; ++x) {
                    std::size_t offset = static_cast<std::size_t>(y) * 400 +
                                         static_cast<std::size_t>(x) * 4;
                    original.rgba[offset] = 190;
                    original.rgba[offset + 1] = 80;
                    original.rgba[offset + 2] = 30;
                    original.rgba[offset + 3] = 255;
                }
            }
            expect_true(gambitree::crop_png_to_alpha_bounds(original, cropped) &&
                        cropped.width == shapes[shape][2] &&
                        cropped.height == shapes[shape][3],
                        "asymmetric synthetic source crops to exact alpha extents");
            for (size_index = 0;
                 size_index < sizeof(synthetic_sizes) / sizeof(synthetic_sizes[0]);
                 ++size_index) {
                std::ostringstream label;
                label << "synthetic asymmetric crop " << shape << " at "
                      << synthetic_sizes[size_index] << "px";
                int old_horizontal = 0;
                int old_vertical = 0;
                verify_piece_centering(
                    cropped, synthetic_sizes[size_index], label.str(),
                    old_horizontal, old_vertical,
                    synthetic_max_horizontal_difference,
                    synthetic_max_vertical_difference);
            }
        }
        expect_true(synthetic_max_horizontal_difference <= 1 &&
                    synthetic_max_vertical_difference <= 1,
                "synthetic asymmetric crops have at most one margin pixel of rounding");
        std::cout << "Synthetic alpha margins: max old difference "
                  << synthetic_max_horizontal_difference << "px horizontal, "
                  << synthetic_max_vertical_difference << "px vertical.\n";
    }
}

void test_meridian_piece_geometry()
{
    const char* master_names[] = {
        "white-king", "white-queen", "white-rook", "white-bishop",
        "white-knight", "white-pawn", "black-king", "black-queen",
        "black-rook", "black-bishop", "black-knight", "black-pawn"
    };
    static const int square_sizes[] = { 48, 64, 83, 84, 100, 127 };
    static const int expected_targets[] = { 0, 61, 64, 65, 60, 66, 67 };
    static const int expected_offsets[] = { 0, 2, 2, 2, 2, 1, 1 };
    const gambitree::PieceSetDescriptor* set =
        gambitree::find_piece_set("meridian-shaded");
    gambitree::AssetManager assets;
    std::string error;
    int maximum_horizontal_difference = 0;
    int piece;
    expect_true(set != 0 && assets.load_piece_set(
                    PieceTheme("assets/pieces/meridian-shaded",
                               set ? set->alpha_threshold : 4,
                               set ? set->crop_padding : 8), error),
                "Meridian geometry uses its own descriptor and cropped masters");
    if (!set)
        return;
    for (std::size_t master = 0;
         master < sizeof(master_names) / sizeof(master_names[0]); ++master) {
        PngImage png;
        std::string error;
        expect_true(gambitree::load_png_rgba(
                        std::string("assets/pieces/meridian-shaded/") +
                        master_names[master] + ".png", png, error) &&
                    png.width == 512 && png.height == 512,
                    "Meridian runtime PNG masters are direct 512px renders");
    }
    for (std::size_t type = 1; type <= 6; ++type) {
        expect_true(set->target_heights_at_84[type] == expected_targets[type] &&
                    set->vertical_offsets_at_84[type] == expected_offsets[type],
                    "Meridian set-level geometry follows the measured source scale and baseline");
    }
    expect_true(PieceLayout::vertical_offset_for_piece(
                    *set, gambitree::WHITE_PAWN, 84) ==
                PieceLayout::vertical_offset_for_piece(
                    *set, gambitree::WHITE_KNIGHT, 84) &&
                PieceLayout::vertical_offset_for_piece(
                    *set, gambitree::WHITE_ROOK, 84) ==
                PieceLayout::vertical_offset_for_piece(
                    *set, gambitree::WHITE_PAWN, 84),
                "Meridian pawn, knight and rook visible centers share a baseline");
    for (piece = gambitree::BLACK_KING;
         piece <= gambitree::WHITE_KING; ++piece) {
        const PngImage* source;
        std::size_t size_index;
        if (piece == gambitree::EMPTY)
            continue;
        source = assets.piece_image(piece);
        expect_true(source && source->valid(),
                    "every Meridian side/type has a decoded source image");
        expect_true(source && assets.piece_image(-piece) &&
                    source->width == assets.piece_image(-piece)->width &&
                    source->height == assets.piece_image(-piece)->height,
                    "Meridian white/black pairs have matched source geometry");
        if (!source)
            continue;
        for (size_index = 0;
             size_index < sizeof(square_sizes) / sizeof(square_sizes[0]);
             ++size_index) {
            int square_size = square_sizes[size_index];
            int target = PieceLayout::target_height_for_piece(
                *set, piece, square_size);
            PiecePlacement dimensions = PieceLayout::dimensions_for_target_height(
                source->width, source->height, target);
            PngImage scaled;
            gambitree::PngAlphaBounds bounds;
            PiecePlacement canvas;
            PiecePlacement placed;
            int left;
            int right;
            int top;
            int bottom;
            bool valid = dimensions.valid() &&
                gambitree::resize_png_lanczos3(*source, dimensions.width,
                                               dimensions.height, scaled) &&
                gambitree::png_alpha_bounds(scaled, bounds);
            expect_true(valid, "Meridian raster resizes with Lanczos at every tested square size");
            if (!valid)
                continue;
            canvas.width = scaled.width;
            canvas.height = scaled.height;
            placed = PieceLayout::place_alpha_bounds_for_piece(
                *set, 20, 30, square_size, piece, canvas, bounds);
            left = placed.x + bounds.x - 20;
            right = 20 + square_size -
                (placed.x + bounds.x + bounds.width);
            top = placed.y + bounds.y - 30;
            bottom = 30 + square_size -
                (placed.y + bounds.y + bounds.height);
            maximum_horizontal_difference = std::max(
                maximum_horizontal_difference, std::abs(left - right));
            expect_true(left >= 0 && right >= 0 && top >= 0 && bottom >= 0 &&
                        std::abs(left - right) <= 1 &&
                        placed.y == PieceLayout::center_alpha_bounds(
                            20, 30, square_size, canvas, bounds).y +
                            PieceLayout::vertical_offset_for_piece(
                                *set, piece, square_size),
                        "Meridian placement preserves safe bounds and type offset");
            expect_true(PieceLayout::target_height_for_piece(
                            *set, piece, square_size) ==
                        PieceLayout::target_height_for_piece(
                            *set, -piece, square_size) &&
                        PieceLayout::vertical_offset_for_piece(
                            *set, piece, square_size) ==
                        PieceLayout::vertical_offset_for_piece(
                            *set, -piece, square_size),
                        "white and black Meridian counterparts share target and Y policy");
        }
    }
    expect_true(maximum_horizontal_difference <= 1,
                "Meridian keeps horizontal alpha centering within one pixel");
    expect_true(PieceLayout::target_height_for_piece(
                    gambitree::default_piece_set(), gambitree::WHITE_PAWN, 84) == 54 &&
                PieceLayout::vertical_offset_for_piece(
                    gambitree::default_piece_set(), gambitree::WHITE_ROOK, 84) == 3,
                "Gambitree's established pawn scale and rook placement remain unchanged");
}

std::string rgba_fingerprint(const PngImage& image)
{
    // FNV-1a-64: fixed-width unsigned arithmetic, independent of host word size.
    uint64_t hash = (static_cast<uint64_t>(0xcbf29ce4U) << 32) | 0x84222325U;
    const uint64_t prime = (static_cast<uint64_t>(0x100U) << 32) | 0x1b3U;
    for (std::size_t i = 0; i < image.rgba.size(); ++i) {
        hash ^= image.rgba[i];
        hash *= prime;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

void test_board_veneer()
{
    const std::string light_path = "assets/boards/wood-veneer-light.png";
    const std::string dark_path = "assets/boards/wood-veneer-dark.png";
    std::vector<std::string> light_paths(1, light_path), dark_paths(1, dark_path);
    std::vector<std::string> absent(1,
        test_scratch_directory + "/no-such-veneer-texture.png");
    gambitree::BoardVeneer veneer, missing;
    PngImage light, dark, board;
    std::string error;
    expect_true(gambitree::load_png_rgba(light_path, light, error) &&
                gambitree::load_png_rgba(dark_path, dark, error) &&
                light.width == 1024 && light.height == 1024 &&
                dark.width == 1024 && dark.height == 1024,
                "both bundled veneer sheets decode at 1024px");
    expect_true(!missing.load(absent, dark_paths, error) && !missing.loaded() &&
                !missing.render(75, false, board) && !board.valid() &&
                !missing.load(light_paths, absent, error) && !missing.loaded(),
                "either missing sheet leaves no partial background, enabling classic fallback");
    std::vector<std::string> transparent(1, "assets/pieces/cburnett/white-pawn.png");
    expect_true(!missing.load(transparent, dark_paths, error) && !missing.loaded(),
                "transparent artwork is rejected as a veneer background");
    light_paths.insert(light_paths.begin(), absent[0]);
    expect_true(veneer.load(light_paths, dark_paths, error) && veneer.loaded(),
                "veneer asset lookup tries valid fallback paths transactionally");
    if (!veneer.loaded()) return;
    std::set<int> windows[2];
    for (int square = 0; square < 64; ++square) {
        gambitree::PngAlphaBounds crop = gambitree::BoardVeneer::window(square, 1024);
        gambitree::PngAlphaBounds large = gambitree::BoardVeneer::window(square, 2048);
        expect_true(crop.valid() && crop.width == 128 && crop.height == 128 &&
                    crop.x >= 0 && crop.x + crop.width <= 1024 &&
                    crop.y >= 0 && crop.y + crop.height <= 1024 &&
                    large.x == crop.x * 2 && large.y == crop.y * 2 &&
                    large.width == crop.width * 2 && large.height == crop.height * 2,
                    "logical veneer windows use fixed normalized source coordinates");
        windows[BoardLayout::is_light_square(square) ? 1 : 0].insert(crop.y * 1024 + crop.x);
    }
    expect_true(windows[0].size() == 32 && windows[1].size() == 32 &&
                !BoardLayout::is_light_square(0) && BoardLayout::is_light_square(56) &&
                gambitree::BoardVeneer::window(0, 1024).x == 5 * 128 &&
                gambitree::BoardVeneer::window(0, 1024).y == 4 * 128,
                "all 32 squares of each colour receive distinct deterministic source windows");
    const int sizes[] = {40, 75, 125};
    // Captured from approved rendering before simplification, independently
    // hashed from complete row-major RGBA files with Python (no window oracle).
    const char* const expected_rgba[3][2] = {
        {"bcb58c60a2e540f1", "6727e101a462a4a9"}, // 320px: normal, flipped
        {"d076c8541f824349", "8cb45792c7b75ce9"}, // 600px
        {"169777092dea211e", "c523607fb947801e"}  // 1000px
    };
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const int square_size = sizes[i], size = square_size * 8;
        PngImage repeat, rotated;
        expect_true(veneer.render(square_size, false, board) &&
                    veneer.render(square_size, false, repeat) &&
                    veneer.render(square_size, true, rotated) &&
                    board.width == size && board.height == size &&
                    board.rgba == repeat.rgba,
                    "320, 600 and 1000px veneer boards render identically on repeated draws");
        if (!board.valid() || !rotated.valid()) continue;
        const PngImage* outputs[] = {&board, &rotated};
        for (int flip = 0; flip < 2; ++flip) {
            std::ostringstream label;
            label << "approved complete veneer RGBA at " << size << "px, flipped=" << flip;
            expect_true(rgba_fingerprint(*outputs[flip]) == expected_rgba[i][flip],
                        label.str().c_str());
        }
        bool rotates = true;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                for (int c = 0; c < 4; ++c)
                    rotates = rotates && board.rgba[y * board.stride + x * 4 + c] ==
                        rotated.rgba[(size - 1 - y) * rotated.stride + (size - 1 - x) * 4 + c];
        expect_true(rotates,
                    "rotation preserves every logical veneer square and rotates its full grain by 180 degrees");
        std::set<unsigned long> tile_hashes[2];
        bool grain = true;
        for (int square = 0; square < 64; ++square) {
            unsigned long hash = 2166136261UL;
            int low = 255, high = 0;
            for (int y = 0; y < square_size; ++y)
                for (int x = 0; x < square_size; ++x) {
                    const std::size_t at = ((7 - square / 8) * square_size + y) * board.stride +
                                          ((square % 8) * square_size + x) * 4;
                    for (int c = 0; c < 3; ++c) hash = hash * 33UL + board.rgba[at + c];
                    low = std::min(low, static_cast<int>(board.rgba[at]));
                    high = std::max(high, static_cast<int>(board.rgba[at]));
                }
            grain = grain && high > low + 1;
            tile_hashes[BoardLayout::is_light_square(square) ? 1 : 0].insert(hash);
        }
        expect_true(grain && tile_hashes[0].size() == 32 && tile_hashes[1].size() == 32,
                    "all 32 rendered crops per colour remain distinct and retain internal grain at every size");
    }
    PngImage previous;
    expect_true(veneer.render(1, false, previous) &&
                !veneer.load(light_paths, absent, error) &&
                veneer.render(1, false, board) && board.rgba == previous.rgba,
                "a failed reload retains the complete previous veneer pair");
    const int invalid[] = {-1, 0, 512, std::numeric_limits<int>::max()};
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        expect_true(!veneer.render(invalid[i], false, board) && !board.valid(),
                    "invalid and extreme veneer sizes fail before multiplication or allocation");
    const char* themes[] = {"classic", "wood", "wood-veneer"};
    for (int i = 0; i < 3; ++i) {
        gambitree::SessionState state = gambitree::default_session_state(), restored;
        state.board_theme_id = themes[i];
        state.piece_set_id = "cburnett";
        expect_true(gambitree::valid_board_theme_id(themes[i]) &&
                    gambitree::next_board_theme_id(themes[i]) == themes[(i + 1) % 3] &&
                    gambitree::parse_session_state(gambitree::serialize_session_state(state), restored) &&
                    restored.board_theme_id == themes[i] && restored.piece_set_id == "cburnett",
                    "B cycles all three themes and session state keeps board and piece preferences independent");
    }
    expect_true(!gambitree::valid_board_theme_id("broken") &&
                gambitree::next_board_theme_id("broken") == "classic",
                "unknown board themes retain a safe classic default");
}

void test_cburnett_vertical_placement()
{
    const gambitree::PieceSetDescriptor* cburnett =
        gambitree::find_piece_set("cburnett");
    const int reference_offsets[] = { 0, 3, 1, 0, 3, 1, 1 };
    const int offsets_at_75[] = { 0, 2, 0, -1, 2, 0, 0 };
    const int offsets_at_115[] = { 0, 2, -1, -2, 2, -1, -1 };
    const int sizes[] = {
        -1, 0, 1, 24, 28, 29, 40, 57, 58, 75, 84, 86, 87,
        114, 115, 116, 125, 192,
        std::numeric_limits<int>::max() / 2,
        std::numeric_limits<int>::max() - 1,
        std::numeric_limits<int>::max()
    };
    expect_true(cburnett != 0, "Cburnett placement uses the registered set");
    if (!cburnett) return;
    for (int type = 1; type <= 6; ++type) {
        expect_true(cburnett->vertical_offsets_at_84[type] == reference_offsets[type],
                    "Cburnett keeps its individual reference baseline corrections");
        for (int side = -1; side <= 1; side += 2) {
            expect_true(PieceLayout::vertical_offset_for_piece(
                            *cburnett, side * type, 75) == offsets_at_75[type] &&
                        PieceLayout::vertical_offset_for_piece(
                            *cburnett, side * type, 115) == offsets_at_115[type],
                        "variant B is active: all twelve pieces lift 1px at 75 and 2px at 115");
        }
    }
    for (std::size_t s = 0; s < gambitree::piece_set_count(); ++s) {
        const gambitree::PieceSetDescriptor& set = *gambitree::piece_set_at(s);
        for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            const int square = sizes[i];
            for (int piece = gambitree::BLACK_KING;
                 piece <= gambitree::WHITE_KING; ++piece) {
                int expected = 0;
                if (square > 0 && piece != gambitree::EMPTY) {
                    const int type = piece < 0 ? -piece : piece;
                    const int reference = set.vertical_offsets_at_84[type];
                    // Independent floating-point oracle avoids overflowing an
                    // integer numerator, even at INT_MAX; round per type first.
                    expected = static_cast<int>(std::floor(
                        std::abs(reference) * static_cast<double>(square) / 84.0 + 0.5));
                    if (reference < 0) expected = -expected;
                    if (&set == cburnett)
                        expected -= static_cast<int>(std::floor(
                            2.0 * square / 115.0 + 0.5));
                }
                expect_true(PieceLayout::vertical_offset_for_piece(
                                set, piece, square) == expected,
                            "only Cburnett receives the common lift after rounding, up to INT_MAX");
            }
        }
    }
}

void test_cburnett_assets_and_scaling()
{
    const gambitree::PieceSetDescriptor* set = gambitree::find_piece_set("cburnett");
    const int sizes[] = { 24, 40, 75, 84, 115, 125, 192 };
    const unsigned long backgrounds[] = { 0xdeb881UL, 0x805230UL, 0x633027UL };
    const PieceTheme theme("assets/pieces/cburnett");
    gambitree::AssetManager assets;
    std::string error;
    bool loaded = set && assets.load_piece_set(theme, error);
    expect_true(loaded, "all twelve original Cburnett renders load transactionally");
    if (!loaded) return;
    for (int piece = gambitree::BLACK_KING; piece <= gambitree::WHITE_KING; ++piece) {
        if (piece == gambitree::EMPTY) continue;
        PngImage master;
        const PngImage* source = assets.piece_image(piece);
        bool valid = gambitree::load_png_rgba(theme.path_for(piece), master, error) &&
                     master.width == 512 && master.height == 512 && source;
        expect_true(valid, "each Cburnett master is a 512px RGBA image");
        if (!valid) continue;
        bool transparent_border = true, partial_alpha = false, opaque = false;
        for (int y = 0; y < master.height; ++y) {
            for (int x = 0; x < master.width; ++x) {
                unsigned char alpha = master.rgba[y * master.stride + x * 4 + 3];
                if ((x == 0 || y == 0 || x == 511 || y == 511) && alpha)
                    transparent_border = false;
                partial_alpha = partial_alpha || (alpha > 0 && alpha < 255);
                opaque = opaque || alpha == 255;
            }
        }
        expect_true(transparent_border && partial_alpha && opaque,
                    "Cburnett has a transparent canvas and antialiased opaque artwork");
        for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            int square = sizes[i];
            PiecePlacement dimensions = PieceLayout::dimensions_for_target_height(
                source->width, source->height,
                PieceLayout::target_height_for_piece(*set, piece, square));
            PngImage scaled;
            gambitree::PngAlphaBounds bounds;
            valid = dimensions.valid() && dimensions.width <= square &&
                    dimensions.height <= square &&
                    gambitree::resize_png_lanczos3(*source, dimensions.width,
                                                  dimensions.height, scaled) &&
                    gambitree::png_alpha_bounds(scaled, bounds);
            expect_true(valid, "all twelve Cburnett pieces scale at small, odd and large sizes");
            if (!valid) continue;
            expect_true(std::abs(dimensions.width * source->height -
                                 dimensions.height * source->width) <= source->height,
                        "Cburnett scaling preserves the source aspect ratio within pixel rounding");
            PiecePlacement placement = PieceLayout::place_alpha_bounds_for_piece(
                *set, 0, 0, square, piece, dimensions, bounds);
            gambitree::PieceSetDescriptor unshifted = *set;
            unshifted.id = "unshifted-reference";
            PiecePlacement previous = PieceLayout::place_alpha_bounds_for_piece(
                unshifted, 0, 0, square, piece, dimensions, bounds);
            const int lift = static_cast<int>(std::floor(2.0 * square / 115.0 + 0.5));
            expect_true(placement.y == previous.y - lift &&
                        placement.x == previous.x &&
                        placement.width == previous.width &&
                        placement.height == previous.height,
                        "variant B lifts every scaled silhouette equally without changing size or horizontal position");
            expect_true(placement.x + bounds.x >= 0 && placement.y + bounds.y >= 0 &&
                        placement.x + bounds.x + bounds.width <= square &&
                        placement.y + bounds.y + bounds.height <= square,
                        "Cburnett silhouettes stay inside square hitboxes at every size");
            for (std::size_t b = 0; b < sizeof(backgrounds) / sizeof(backgrounds[0]); ++b) {
                TestPixelSurface surface(square, square, backgrounds[b]);
                std::size_t drawn = gambitree::composite_rgba_image(surface, 0, 0,
                    placement.x, placement.y, scaled.width, scaled.height,
                    &scaled.rgba[0], scaled.stride, 0xff0000UL, 0xff00UL, 0xffUL);
                expect_true(drawn > static_cast<std::size_t>(square * square / 20) &&
                            surface.pixel(0, 0) == backgrounds[b] &&
                            surface.pixel(square - 1, square - 1) == backgrounds[b],
                            "Cburnett composites onto light/dark backgrounds without rectangular canvases");
            }
        }
    }
    int previous_width = assets.piece_image(gambitree::WHITE_KING)->width;
    expect_true(!assets.load_piece_set(PieceTheme("assets/pieces/cburnett/missing"), error) &&
                assets.piece_image(gambitree::WHITE_KING)->width == previous_width,
                "a failed replacement keeps the loaded Cburnett artwork intact");
}

void test_configure_event_batch()
{
    gambitree::ConfigureEventBatch batch;
    batch.include(900, 650);
    expect_true(batch.width() == 900 && batch.height() == 650 &&
                batch.event_count() == 1,
                "first ConfigureNotify establishes pending size");
    batch.include(960, 700);
    batch.include(1120, 840);
    batch.include(1120, 840);
    expect_true(batch.width() == 1120 && batch.height() == 840 &&
                batch.event_count() == 4,
                "coalesced ConfigureNotify batch keeps the last geometry");
    expect_true(batch.event_count() - 1 == 3,
                "resize coalescing avoids three obsolete redraws");
}

void test_complete_frame_buffer()
{
    gambitree::CompleteFrameBuffer frame(11, 900, 650);
    expect_true(frame.valid() && !frame.needs_replacement(900, 650),
                "a complete application frame is reusable at its dimensions");
    expect_true(frame.needs_replacement(1120, 840) &&
                frame.drawable() == 11 && frame.width() == 900 &&
                frame.height() == 650,
                "new geometry retains the old complete frame while a candidate is prepared");
    expect_true(!frame.commit_completed(12, 1120, 840, false) &&
                frame.drawable() == 11 && frame.needs_replacement(1120, 840),
                "an incomplete candidate cannot replace the current frame");
    expect_true(frame.commit_completed(12, 1120, 840, true) &&
                frame.drawable() == 12 && frame.width() == 1120 &&
                frame.height() == 840 && !frame.needs_replacement(1120, 840),
                "a fully rendered candidate atomically becomes the current frame");
    expect_true(frame.drawable_for_expose() == 12,
                "Expose restoration continues to use the latest complete frame");
}

void test_piece_cache_key()
{
    gambitree::PieceCacheKey key;
    key.set("meridian-shaded", 84);
    expect_true(key.matches("meridian-shaded", 84),
                "piece cache reuses a raster when set and square size match");
    expect_true(!key.matches("meridian-shaded", 85),
                "piece cache rebuilds when final square size changes");
    expect_true(!key.matches("gambitree", 84),
                "piece cache rebuilds when piece set changes at same size");
    key.invalidate();
    expect_true(!key.matches("meridian-shaded", 84),
                "piece-set switch invalidates the previous scaled cache");
    key.set("gambitree", 84);
    expect_true(key.matches("gambitree", 84),
                "new active set can populate the cache for the same size");
}

void test_deferred_piece_resize()
{
    gambitree::DeferredPieceResize resize(100);
    expect_true(!resize.configure(84, 84, 0) && !resize.pending(),
                "unchanged high-quality square size schedules no resize work");
    resize.configure(86, 84, 20);
    resize.configure(90, 84, 40);
    resize.configure(95, 84, 60);
    resize.configure(100, 84, 80);
    expect_true(resize.waiting_for_settle() &&
                resize.target_square_size() == 100 &&
                !resize.due(179) && resize.due(180),
                "continuous resize burst defers one target until 100ms after the last event");
    expect_true(resize.begin_build(180) && resize.building(),
                "settled resize starts an incremental candidate build");
    {
        unsigned long work;
        for (work = 0; work < 11; ++work) {
            resize.finish_work_unit(false);
            expect_true(resize.building(),
                        "partial twelve-piece candidate stays uncommittable");
        }
        expect_true(resize.work_units_completed() == 11 &&
                    !resize.ready_to_commit(),
                    "eleven work units cannot publish a partial cache");
        resize.finish_work_unit(true);
    }
    expect_true(resize.ready_to_commit() && resize.commit(181) &&
                !resize.pending() && resize.completed_targets() == 1 &&
                resize.maximum_settle_milliseconds() == 100 &&
                resize.cancelled_targets() == 3,
                "only all twelve unique piece work units can commit one cache");

    resize.configure(110, 100, 1000);
    resize.configure(111, 100, 1099);
    expect_true(!resize.due(1198) && resize.due(1199) &&
                resize.target_square_size() == 111,
                "an event just before the deadline moves the settle deadline");
    resize.begin_build(1199);
    resize.finish_work_unit(false);
    resize.finish_work_unit(false);
    {
        unsigned long old_generation = resize.generation();
        resize.configure(103, 100, 1210);
        expect_true(resize.waiting_for_settle() &&
                    resize.target_square_size() == 103 &&
                    resize.generation() != old_generation &&
                    resize.work_units_completed() == 0,
                    "new size cancels two-unit obsolete work and waits for a fresh settle");
    }
    resize.configure(100, 100, 1220);
    expect_true(!resize.pending() && resize.cancelled_targets() == 6,
                "return to the cached high-quality size cancels pending rebuild");
    resize.configure(120, 100, 1300);
    resize.begin_build(1400, true);
    resize.cancel();
    expect_true(!resize.pending() && resize.cancelled_targets() == 7,
                "piece-set switch can cancel resize settling immediately");
    {
        const unsigned long intervals[] = { 75, 100, 125, 150 };
        for (std::size_t interval = 0;
             interval < sizeof(intervals) / sizeof(intervals[0]); ++interval) {
            gambitree::DeferredPieceResize burst(intervals[interval]);
            uint64_t event_time;
            for (event_time = 0; event_time <= 990; event_time += 15)
                burst.configure(85 + static_cast<int>(event_time / 15), 84,
                                event_time);
            expect_true(!burst.due(990 + intervals[interval] - 1) &&
                        burst.due(990 + intervals[interval]) &&
                        burst.begin_build(990 + intervals[interval]) &&
                        burst.building() &&
                        burst.maximum_settle_milliseconds() ==
                            intervals[interval],
                        "resize burst starts one incremental rebuild at each candidate debounce interval");
        }
    }
}

void test_piece_raster_worklist()
{
    std::size_t count = 0;
    const int* pieces = gambitree::unique_piece_raster_worklist(count);
    std::set<int> unique;
    std::size_t square;
    Position initial = Position::standard();
    unsigned long instances = 0;
    for (square = 0; square < count; ++square)
        unique.insert(pieces[square]);
    for (square = 0; square < 64; ++square) {
        if (initial.piece_at(static_cast<int>(square)) != gambitree::NO_PIECE)
            ++instances;
    }
    expect_true(count == 12 && unique.size() == 12 &&
                count == gambitree::kUniquePieceRasterCount,
                "HQ resizing worklist contains exactly twelve unique color/type rasters");
    expect_true(instances == 32,
                "standard starting position has 32 displayed piece instances");
}


void test_extended_nag_presentation()
{
    struct ExpectedNag { int code; const char* display; const char* side; const char* nuance; };
    const ExpectedNag expected[] = {
        {7,"□","","forced"}, {8,"□","","singular"},
        {22,"⊙W","White","zugzwang"}, {23,"⊙Z","Black","zugzwang"},
        {30,"↻+W","White","slight"}, {31,"↻+Z","Black","slight"},
        {32,"↻++W","White","moderate"}, {33,"↻++Z","Black","moderate"},
        {37,"↓","Black","initiative"}, {40,"→W","White","attack"},
        {41,"→Z","Black","attack"}, {44,"=∞","White","sufficient"},
        {45,"∞=","Black","sufficient"}, {50,"⊕W","White","moderate"},
        {131,"↔+Z","Black","slight"}, {132,"↔W","White","moderate"},
        {133,"↔Z","Black","moderate"}, {136,"⊕+W","White","moderate"},
        {137,"⊕+Z","Black","moderate"}, {138,"⊕++W","White","severe"},
        {140,"∆","","continuation"}, {141,"∇","","counter"},
        {142,"≥","","alternative"}, {143,"≤","","alternative"},
        {144,"≈","","alternative"}, {145,"RR","","editorial"}, {146,"N","","novelty"}
    };
    const int existing_codes[] = {1,2,3,4,5,6,10,11,13,14,15,16,17,18,19,36};
    const char* existing_text[] = {"!","?","!!","??","!?","?!","=","=","∞","+/=","=/+","±","-/+","+−","−+","↑"};
    for (std::size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        const gambitree::NagPresentation* info = gambitree::NotationLayout::nag_presentation(expected[i].code);
        expect_true(info && info->code==expected[i].code &&
                    gambitree::NotationLayout::nag_text(expected[i].code)==expected[i].display &&
                    info->side==std::string(expected[i].side) &&
                    info->nuance==std::string(expected[i].nuance) &&
                    !std::string(info->meaning).empty(),
                    "each additional database NAG has a clear central display and meaning");
    }
    for (std::size_t i=0;i<sizeof(existing_codes)/sizeof(existing_codes[0]);++i)
        expect_true(gambitree::NotationLayout::nag_text(existing_codes[i])==existing_text[i],
                    "all earlier NAG displays remain unchanged");

    std::ostringstream pgn;
    pgn << "1.e4";
    for (std::size_t i=0;i<sizeof(existing_codes)/sizeof(existing_codes[0]);++i)
        pgn << " $" << existing_codes[i];
    for (std::size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i)
        pgn << " $" << expected[i].code;
    pgn << " {annotation stays before the reply} e5 $148 "
           "(1...c5 $22 $149 2.Nf3 d6 $138 (2...e6 $133)) *";
    gambitree::PgnDocument all_document;
    expect_true(gambitree::PgnParser::parse(pgn.str(),all_document) && all_document.games.size()==1,
                "all 43 known NAGs, fallback and nested-side fixtures parse");
    if (all_document.games.size()==1) {
        gambitree::MoveNode* e4=child(all_document.games[0]->root,0,"annotated e4 exists");
        expect_true(e4 && e4->nags.size()==43,
                    "each original known NAG code stays on its source move in the PGN model");
        if(e4) {
            gambitree::NotationLayout layout; layout.build(*all_document.games[0],10000,8,18);
            std::size_t expected_items=0, nag_items=0, quality_items=0;
            for(std::size_t i=0;i<43;++i) {
                const std::string display=gambitree::NotationLayout::nag_text(
                    i<sizeof(existing_codes)/sizeof(existing_codes[0]) ? existing_codes[i] : expected[i-sizeof(existing_codes)/sizeof(existing_codes[0])].code);
                bool in_word=false;
                for(std::size_t j=0;j<display.size();++j) {
                    if(display[j]==' ') in_word=false;
                    else if(!in_word) { ++expected_items; in_word=true; }
                }
            }
            bool saw_white_zug=false, saw_black_zug=false, saw_slight_dev=false, saw_moderate_dev=false;
            bool saw_slight_cp=false, saw_moderate_cp=false, saw_severe_time=false;
            for(std::size_t i=0;i<layout.items().size();++i) {
                const gambitree::NotationItem& item=layout.items()[i];
                if(item.kind!=gambitree::NOTATION_NAG || item.node!=e4) continue;
                ++nag_items;
                expect_true(!item.text.empty(),
                            "visible annotations stay attached to the annotated move with no empty item");
                if(item.move_quality) ++quality_items;
                if(item.text=="⊙W") saw_white_zug=true;
                if(item.text=="⊙Z") saw_black_zug=true;
                if(item.text=="↻+W") saw_slight_dev=true;
                if(item.text=="↻++W") saw_moderate_dev=true;
                if(item.text=="↔+Z") saw_slight_cp=true;
                if(item.text=="↔W") saw_moderate_cp=true;
                if(item.text=="⊕++W") saw_severe_time=true;
            }
            expect_true(nag_items==expected_items,
                        "all displayed mapping words are measured as real layout items");
            expect_true(quality_items==6,
                        "only NAG 1-6 keep the special move-quality selection behavior");
            expect_true(saw_white_zug && saw_black_zug,
                        "both side-specific zugzwang displays survive layout");
            expect_true(saw_slight_dev && saw_moderate_dev,
                        "slight and moderate development displays survive layout");
            expect_true(saw_slight_cp && saw_moderate_cp && saw_severe_time,
                        "counterplay grades and severe time-pressure display survive layout");
            gambitree::NotationSearch annotation_search;
            annotation_search.set_query("=∞", layout, 0, 1000);
            expect_true(annotation_search.result_count() == 1,
                        "search indexes the same compact compensation symbol shown in notation");
            gambitree::MoveNode* c5=e4 && e4->children.size()>1 ? e4->children[1] : 0;
            expect_true(c5 && c5->san=="c5" && c5->nags.size()==2 && c5->nags[0]==22 && c5->nags[1]==149,
                        "a black move starting a RAV retains visible and suppressed codes internally");
            bool nested_133=false,nested_138=false;
            gambitree::MoveNode* nf3 = c5 && !c5->children.empty() ? c5->children[0] : 0;
            gambitree::MoveNode* d6 = nf3 && !nf3->children.empty() ? nf3->children[0] : 0;
            gambitree::MoveNode* e6 = nf3 && nf3->children.size()>1 ? nf3->children[1] : 0;
            for(std::size_t i=0;i<layout.items().size();++i) {
                if(layout.items()[i].kind!=gambitree::NOTATION_NAG) continue;
                if(layout.items()[i].text=="↔Z" && layout.items()[i].node==e6) nested_133=true;
                if(layout.items()[i].text=="⊕++W" && layout.items()[i].node==d6) nested_138=true;
            }
            expect_true(nested_133 && nested_138,
                        "graded mappings render at nested variation depth without changing their tree nodes");
        }
    }

    {
        const int hidden_values[]={149,150,220,221,222,255,4000,std::numeric_limits<int>::max()};
        std::ostringstream hidden_pgn; hidden_pgn << "1.e4 $148";
        for(std::size_t i=0;i<sizeof(hidden_values)/sizeof(hidden_values[0]);++i)
            hidden_pgn << " $" << hidden_values[i];
        hidden_pgn << " e5 *";
        const std::string hidden_text=hidden_pgn.str();
        std::ostringstream high_only_pgn; high_only_pgn << "1.e4";
        for(std::size_t i=1;i<sizeof(hidden_values)/sizeof(hidden_values[0]);++i)
            high_only_pgn << " $" << hidden_values[i];
        high_only_pgn << " e5 *";
        gambitree::PgnDocument hidden_doc,high_only_doc,plain_doc;
        expect_true(gambitree::PgnParser::parse(hidden_text,hidden_doc) && hidden_doc.games.size()==1 &&
                    gambitree::PgnParser::parse(high_only_pgn.str(),high_only_doc) && high_only_doc.games.size()==1 &&
                    gambitree::PgnParser::parse("1.e4 e5 *",plain_doc) && plain_doc.games.size()==1,
                    "all requested large valid NAG integers parse without overflow");
        if(hidden_doc.games.size()==1 && high_only_doc.games.size()==1 && plain_doc.games.size()==1) {
            gambitree::MoveNode* e4=child(hidden_doc.games[0]->root,0,"hidden-code e4 exists");
            expect_true(e4 && e4->nags.size()==9 && e4->nags[0]==148 &&
                        e4->nags[8]==std::numeric_limits<int>::max(),
                        "hidden values including INT_MAX remain in the numeric PGN model");
            expect_true(gambitree::NotationLayout::nag_text(148)=="$148",
                        "$148 remains under the normal unknown-code fallback");
            for(std::size_t i=0;i<sizeof(hidden_values)/sizeof(hidden_values[0]);++i)
                expect_true(gambitree::NotationLayout::nag_text(hidden_values[i]).empty(),
                            "every NAG at or above 149 has an empty visible representation");
            gambitree::NotationLayout hidden_layout,high_layout,plain_layout;
            hidden_layout.build(*hidden_doc.games[0],500,8,18);
            high_layout.build(*high_only_doc.games[0],500,8,18);
            plain_layout.build(*plain_doc.games[0],500,8,18);
            expect_true(high_layout.content_height()==plain_layout.content_height() &&
                        high_layout.items().size()==plain_layout.items().size(),
                        "suppressed annotations add no layout rows or placeholder items");
            bool only_148=false;
            for(std::size_t i=0;i<hidden_layout.items().size();++i)
                if(hidden_layout.items()[i].kind==gambitree::NOTATION_NAG) {
                    expect_true(hidden_layout.items()[i].text=="$148",
                                "the only visible unknown is the below-boundary NAG");
                    only_148=true;
                }
            for(std::size_t i=0;i<high_layout.items().size();++i) {
                const gambitree::NotationItem& a=high_layout.items()[i];
                const gambitree::NotationItem& b=plain_layout.items()[i];
                const bool same_node = (!a.node && !b.node) ||
                    (a.node && b.node && a.node->san==b.node->san);
                expect_true(a.kind==b.kind && same_node &&
                            a.text==b.text && a.x==b.x && a.y==b.y && a.width==b.width,
                            "hidden NAGs add no ghost width or alter move hit geometry");
            }
            gambitree::MoveNode* high_e4=child(high_only_doc.games[0]->root,0,"high-only e4 exists");
            gambitree::MoveNode* plain_e4=child(plain_doc.games[0]->root,0,"plain e4 exists");
            const std::vector<gambitree::NotationSelectionRect> high_rects=high_layout.selection_rects(high_e4);
            const std::vector<gambitree::NotationSelectionRect> plain_rects=plain_layout.selection_rects(plain_e4);
            bool same_selection=high_rects.size()==plain_rects.size();
            for(std::size_t i=0;same_selection && i<high_rects.size();++i)
                same_selection=high_rects[i].x==plain_rects[i].x &&
                    high_rects[i].y==plain_rects[i].y &&
                    high_rects[i].width==plain_rects[i].width &&
                    high_rects[i].height==plain_rects[i].height;
            expect_true(same_selection,
                        "suppressed annotations add no selection rectangle or move hit area");
            expect_true(only_148,"$148 still appears as a numeric fallback");
        }
        gambitree::PgnDocument overflow;
        expect_true(!gambitree::PgnParser::parse("1.e4 $2147483648 *",overflow),
                    "parser safely rejects a NAG integer beyond its existing int storage range");
    }
}

void test_notation_layout()
{
    PgnDocument document;
    NotationLayout layout;
    MoveNavigator* navigator;
    MoveNode* main;
    MoveNode* variation;
    MoveNode* nested;
    MoveNode* foreign;
    std::size_t index;
    bool saw_comment = false;
    bool saw_nag = false;
    bool saw_variation = false;
    int e5_index = -1;
    int c5_index = -1;
    int nf3_index = -1;
    int nc6_index = -1;
    int d6_index = -1;
    int d4_index = -1;
    expect_true(gambitree::PgnParser::parse("[White \"Ada\"]\n[Black \"Turing\"]\n[Result \"*\"]\n\n1. e4 e5 $1 (1... c5 $5 {variation} 2. Nf3 Nc6 (2... d6 {nested}) 3. d4) (1... e6) 2. Nf3 *", document), "notation demo PGN parses");
    layout.build(*document.games[0], 96, 8, 18);
    expect_true(!layout.items().empty() && layout.content_height() >= 18, "notation builds visible items");
    expect_true(NotationLayout::nag_text(1) == "!" && NotationLayout::nag_text(2) == "?" &&
                NotationLayout::nag_text(3) == "!!" && NotationLayout::nag_text(4) == "??" &&
                NotationLayout::nag_text(5) == "!?" && NotationLayout::nag_text(6) == "?!" &&
                NotationLayout::nag_text(11) == "=" &&
                NotationLayout::nag_text(13) == "∞" &&
                NotationLayout::nag_text(16) == "±" &&
                NotationLayout::nag_text(17) == "-/+" &&
                NotationLayout::nag_text(18) == "+−" &&
                NotationLayout::nag_text(19) == "−+" &&
                NotationLayout::nag_text(36) == "↑" &&
                NotationLayout::nag_text(0) == "$0" && NotationLayout::nag_text(123) == "$123",
                "existing NAG mappings stay readable while unknown values under 149 stay raw");
    for (index = 0; index < layout.items().size(); ++index) {
        if (layout.items()[index].kind == gambitree::NOTATION_COMMENT) saw_comment = true;
        if (layout.items()[index].kind == gambitree::NOTATION_NAG) saw_nag = true;
        if (layout.items()[index].kind == gambitree::NOTATION_VARIATION) saw_variation = true;
    }
    expect_true(saw_comment && saw_nag && saw_variation, "notation retains comments NAGs and variations");
    main = document.games[0]->root->children[0];
    variation = document.games[0]->root->children[0]->children[1];
    nested = variation->children[0]->children[1];
    for (index = 0; index < layout.items().size(); ++index) {
        if (layout.items()[index].node == main->children[0]) e5_index = static_cast<int>(index);
        if (layout.items()[index].node == variation) c5_index = static_cast<int>(index);
        if (layout.items()[index].node == main->children[0]->children[0]) nf3_index = static_cast<int>(index);
        if (layout.items()[index].node == variation->children[0]->children[0]) nc6_index = static_cast<int>(index);
        if (layout.items()[index].node == nested) d6_index = static_cast<int>(index);
        if (layout.items()[index].node == variation->children[0]->children[0]->children[0]) d4_index = static_cast<int>(index);
        expect_true(layout.items()[index].x >= 0 && layout.items()[index].width >= 0 &&
                    layout.items()[index].x + layout.items()[index].width <= 96,
                    "notation item stays within its content width");
    }
    expect_true(e5_index >= 0 && c5_index > e5_index && nf3_index > c5_index,
                "a sibling RAV is rendered after its anchor move and before the main-line continuation");
    expect_true(nc6_index >= 0 && d6_index > nc6_index && d4_index > d6_index,
                "a nested RAV is rendered at its own anchor before its continuation");
    expect_true(layout.ensure_visible(nested, 0, 18) > 0 &&
                layout.ensure_visible(nested, layout.max_scroll(18), 18) <= layout.max_scroll(18),
                "ensure-visible scrolls a selected nested move into bounded view");
    expect_true(layout.hit_test(layout.items()[static_cast<std::size_t>(nf3_index)].x + 1,
                                layout.items()[static_cast<std::size_t>(nf3_index)].y - 36 + 1, 36) ==
                main->children[0]->children[0],
                "move hit testing applies the scroll offset exactly once");
    expect_true(layout.hit_test_visible(layout.items()[static_cast<std::size_t>(nf3_index)].x + 1,
                                        1, layout.items()[static_cast<std::size_t>(nf3_index)].y, 18) ==
                main->children[0]->children[0],
                "a fully visible notation row remains clickable");
    expect_true(layout.hit_test_visible(layout.items()[static_cast<std::size_t>(nf3_index)].x + 1,
                                        0, layout.items()[static_cast<std::size_t>(nf3_index)].y + 1, 18) == 0,
                "a partially hidden notation row has no active hit region");
    expect_true(layout.hit_test(layout.items()[1].x + 1, layout.items()[1].y + 1, 0) != 0, "move hit region maps to a node");
    navigator = new MoveNavigator(*document.games[0]);
    expect_true(navigator->select(variation) && navigator->current() == variation, "direct selection enters variation");
    expect_true(navigator->position().equals(variation->position_after), "variation selection exposes correct position");
    expect_true(navigator->select(nested) && navigator->current() == nested, "direct selection enters nested variation");
    expect_true(navigator->back() && navigator->forward() &&
                navigator->current() == variation->children[0]->children[0],
                "Left across a nested alternative resumes its parent line");
    foreign = new MoveNode();
    expect_true(!navigator->select(foreign), "foreign node cannot be selected");
    delete foreign;
    delete navigator;
    expect_true(main->san == "e4" && layout.max_scroll(1) > 0, "main-line ordering and scroll bounds are present");

    {
        PgnDocument ordered_document;
        const std::string ordered_pgn =
            "1. e4 e5 {first explanation} (1... c5 {inside c5} (1... e6)) "
            "{second explanation} (1... d5) {third explanation} 2. Nf3 *";
        expect_true(gambitree::PgnParser::parse(ordered_pgn, ordered_document) &&
                    ordered_document.games.size() == 1,
                    "interleaved comments and sibling/nested RAV fixture parses");
        if (!ordered_document.games.empty()) {
            PgnGame& ordered_game = *ordered_document.games[0];
            MoveNode* anchor = child(ordered_game.root->children[0], 0,
                                     "ordered comment/RAV anchor e5 exists");
            expect_true(anchor && anchor->following_items.size() == 5 &&
                        anchor->following_items[0].type == gambitree::PgnMoveItem::COMMENT &&
                        anchor->following_items[1].type == gambitree::PgnMoveItem::VARIATION &&
                        anchor->following_items[2].type == gambitree::PgnMoveItem::COMMENT &&
                        anchor->following_items[3].type == gambitree::PgnMoveItem::VARIATION &&
                        anchor->following_items[4].type == gambitree::PgnMoveItem::COMMENT,
                        "move model retains the exact order of comments and RAV references");
            layout.build(ordered_game, 800, 8, 18);
            {
                int first_comment = -1, c5 = -1, nested_comment = -1;
                int nested_e6 = -1, second_comment = -1, d5 = -1;
                int third_comment = -1, nf3 = -1;
                for (index = 0; index < layout.items().size(); ++index) {
                    const gambitree::NotationItem& item = layout.items()[index];
                    if (item.kind == gambitree::NOTATION_COMMENT && item.text == "first") first_comment = index;
                    if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "c5") c5 = index;
                    if (item.kind == gambitree::NOTATION_COMMENT && item.text == "inside") nested_comment = index;
                    if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "e6") nested_e6 = index;
                    if (item.kind == gambitree::NOTATION_COMMENT && item.text == "second") second_comment = index;
                    if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "d5") d5 = index;
                    if (item.kind == gambitree::NOTATION_COMMENT && item.text == "third") third_comment = index;
                    if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "Nf3") nf3 = index;
                }
                expect_true(first_comment >= 0 && first_comment < c5 && c5 < nested_comment &&
                            nested_comment < nested_e6 && nested_e6 < second_comment &&
                            second_comment < d5 && d5 < third_comment && third_comment < nf3,
                            "GUI notation preserves interleaved and nested source order before main-line resumption");
            }
            {
                bool main_move_clickable = false;
                for (index = 0; index < layout.items().size(); ++index) {
                    const gambitree::NotationItem& item = layout.items()[index];
                    if (item.kind == gambitree::NOTATION_MOVE && item.node &&
                        item.node->san == "e4")
                        main_move_clickable = layout.hit_test(item.x, item.y, 0) == item.node;
                }
                expect_true(main_move_clickable,
                            "ordered rendering retains clickable move items for navigation");
            }
        }
    }

    {
        PgnDocument trailing_document;
        expect_true(gambitree::PgnParser::parse(
                    "1. e4 e5 {pre-result commentary} 0-1 "
                    "{post-result commentary} (2. Nf3 Nc6)",
                    trailing_document) && trailing_document.games.size() == 1,
                    "post-result comments and hypothetical variation parse as one game");
        if (!trailing_document.games.empty()) {
            layout.build(*trailing_document.games[0], 800, 8, 18);
            int pre_comment = -1, result_index = -1, tail_comment = -1;
            int hypothetical_move = -1;
            for (index = 0; index < layout.items().size(); ++index) {
                const gambitree::NotationItem& item = layout.items()[index];
                if (item.kind == gambitree::NOTATION_RESULT && item.text == "0-1") result_index = index;
                if (item.kind == gambitree::NOTATION_COMMENT && item.text == "pre-result") pre_comment = index;
                if (item.kind == gambitree::NOTATION_COMMENT && item.text == "post-result") tail_comment = index;
                if (item.kind == gambitree::NOTATION_MOVE && item.text == "Nf3" && !item.node)
                    hypothetical_move = index;
            }
            expect_true(pre_comment >= 0 && pre_comment < result_index &&
                        result_index > pre_comment && tail_comment > result_index &&
                        hypothetical_move > tail_comment,
                        "pre-result comment, result, trailing comment, and hypothetical continuation appear in source order");
            expect_true(layout.hit_test(layout.items()[static_cast<std::size_t>(hypothetical_move)].x,
                                        layout.items()[static_cast<std::size_t>(hypothetical_move)].y, 0) == 0,
                        "hypothetical post-result moves are deliberately not clickable");
            expect_true(layout.hit_test(layout.items()[1].x, layout.items()[1].y, 0) ==
                        trailing_document.games[0]->root->children[0],
                        "played move remains linked to its original navigation node");
        }
    }

    {
        PgnDocument marked_document, plain_document;
        NotationLayout marked_layout, plain_layout;
        const std::string marked_pgn =
            "{[%mdl 512] root [%eval 0.1] lead} 1. e4 {[%diagram]} e5 {A [%diagram] B} "
            "{second [%clk 0:03:12]} (1... c5 {side [%mdl 512]} (1... e6 {nested [%eval 0.65]})) "
            "2. Nf3 * {tail [%clk] comment} (2... Nc6 {hypothetical [%mdl 1]})";
        const std::string plain_pgn =
            "{root lead} 1. e4 {} e5 {A B} {second} "
            "(1... c5 {side} (1... e6 {nested})) 2. Nf3 * {tail comment} (2... Nc6 {hypothetical})";
        expect_true(gambitree::PgnParser::parse(marked_pgn, marked_document) &&
                    marked_document.games.size() == 1 &&
                    gambitree::PgnParser::parse(plain_pgn, plain_document) &&
                    plain_document.games.size() == 1,
                    "diagram-marker presentation fixture parses across main line, nested RAVs, and post-result items");
        if (!marked_document.games.empty() && !plain_document.games.empty()) {
            std::vector<std::string> marked_moves, plain_moves, visible_comments;
            bool found_raw_marker = false;
            const PgnGame& marked_game = *marked_document.games[0];
            expect_true(!marked_game.root->comments.empty() &&
                        marked_game.root->comments[0] == "[%mdl 512] root [%eval 0.1] lead" &&
                        !marked_game.root->children[0]->comments.empty() &&
                        marked_game.root->children[0]->comments[0] == "[%diagram]",
                        "diagram instructions remain intact in original PGN comment data");
            marked_layout.build(marked_game, 1200, 8, 18);
            plain_layout.build(*plain_document.games[0], 1200, 8, 18);
            for (index = 0; index < marked_layout.items().size(); ++index) {
                const gambitree::NotationItem& item = marked_layout.items()[index];
                if (item.kind == gambitree::NOTATION_MOVE) marked_moves.push_back(item.text);
                if (item.kind == gambitree::NOTATION_COMMENT) {
                    visible_comments.push_back(item.text);
                    if (item.text.find("[%") != std::string::npos) found_raw_marker = true;
                }
            }
            for (index = 0; index < plain_layout.items().size(); ++index)
                if (plain_layout.items()[index].kind == gambitree::NOTATION_MOVE)
                    plain_moves.push_back(plain_layout.items()[index].text);
            expect_true(marked_moves == plain_moves,
                        "hiding comment markers does not change any main-line, RAV, or hypothetical move layout item");
            expect_true(!found_raw_marker,
                        "no diagram instruction is present in visible comments throughout nested and trailing items");
            expect_true(visible_comments.size() == 10 &&
                        visible_comments[0] == "root" && visible_comments[1] == "lead" &&
                        visible_comments[2] == "A" && visible_comments[3] == "B" &&
                        visible_comments[4] == "second" && visible_comments[5] == "side" &&
                        visible_comments[6] == "nested" && visible_comments[7] == "tail" &&
                        visible_comments[8] == "comment" && visible_comments[9] == "hypothetical",
                        "marker-only comments add no text items and remaining comment words keep source order");
            expect_true(marked_game.root->children[0]->following_items.size() == 1 &&
                        marked_game.root->children[0]->following_items[0].comment == "[%diagram]" &&
                        marked_game.root->children[0]->children[0]->following_items.size() == 3 &&
                        marked_game.root->children[0]->children[0]->following_items[0].comment == "A [%diagram] B" &&
                        marked_game.root->children[0]->children[0]->following_items[1].comment == "second [%clk 0:03:12]" &&
                        marked_game.trailing_items.size() == 2 &&
                        marked_game.trailing_items[0].comment == "tail [%clk] comment",
                        "ordered comments and post-result data keep their original marker-bearing values");
        }
    }

    {
        const char* results[] = { "1-0", "0-1", "1/2-1/2", "*" };
        std::size_t result_index;
        for (result_index = 0; result_index < 4; ++result_index) {
            PgnDocument result_document;
            std::string pgn = "1. e4 e5 " + std::string(results[result_index]) +
                              " {tail for result}";
            expect_true(gambitree::PgnParser::parse(pgn, result_document) &&
                        result_document.games.size() == 1,
                        "every PGN result keeps following commentary within its game");
            if (!result_document.games.empty()) {
                layout.build(*result_document.games[0], 800, 8, 18);
                int shown_result = -1, shown_comment = -1;
                for (index = 0; index < layout.items().size(); ++index) {
                    const gambitree::NotationItem& item = layout.items()[index];
                    if (item.kind == gambitree::NOTATION_RESULT && item.text == results[result_index])
                        shown_result = index;
                    if (item.kind == gambitree::NOTATION_COMMENT && item.text == "tail")
                        shown_comment = index;
                }
                expect_true(shown_result >= 0 && shown_comment > shown_result,
                            "all four result markers precede their post-result comments in the notation layout");
            }
        }
    }

    {
        PgnDocument scored_document;
        expect_true(gambitree::PgnParser::parse(
                    "1. e4+/- e5-/+ 2. Nf3+- Nc6-+ *",
                    scored_document) && scored_document.games.size() == 1,
                    "attached evaluation suffixes parse for GUI presentation");
        if (!scored_document.games.empty()) {
            layout.build(*scored_document.games[0], 800, 8, 18);
            const char* expected[] = { "±", "-/+", "+−", "−+" };
            const int nags[] = { 16, 17, 18, 19 };
            std::size_t seen = 0;
            for (index = 0; index < layout.items().size(); ++index) {
                const gambitree::NotationItem& item = layout.items()[index];
                if (item.kind != gambitree::NOTATION_NAG || seen >= 4) continue;
                expect_true(item.text == expected[seen] && item.node &&
                            item.node->nags.size() == 1 &&
                            item.node->nags[0] == nags[seen],
                            "displayed evaluation glyph stays attached to its exact NAG-bearing move");
                ++seen;
            }
            expect_true(seen == 4,
                        "all four evaluation annotations appear as display symbols rather than dollar codes");
        }

        scored_document.clear();
        expect_true(gambitree::PgnParser::parse(
                    "1. e4 $16 e5 $17 2. Nf3 $18 Nc6 $19 *",
                    scored_document) && scored_document.games.size() == 1,
                    "numeric evaluation NAGs parse for GUI presentation");
        if (!scored_document.games.empty()) {
            layout.build(*scored_document.games[0], 800, 8, 18);
            const char* expected[] = { "±", "-/+", "+−", "−+" };
            std::size_t seen = 0;
            for (index = 0; index < layout.items().size(); ++index) {
                const gambitree::NotationItem& item = layout.items()[index];
                if (item.kind != gambitree::NOTATION_NAG || seen >= 4) continue;
                expect_true(item.text == expected[seen],
                            "numeric NAG receives the same GUI evaluation glyph as an attached suffix");
                ++seen;
            }
            expect_true(seen == 4,
                        "numeric NAGs 16 through 19 are rendered symbolically in notation");
        }
    }

    {
        PgnDocument annotated_document;
        const std::string annotated_pgn =
            "1. e4! e5? 2. Nf3!! Nc6!? 3. Bb5?! a6 $11 "
            "4. Ba4 $13 Nf6 $16 5. O-O $17 Be7 $18 "
            "6. Re1 $19 b5 $36 7. Bb3 d6 *";
        const int expected_nags[] = { 1, 2, 3, 5, 6, 11, 13, 16, 17, 18, 19, 36 };
        const char* expected_symbols[] = {
            "!", "?", "!!", "!?", "?!", "=", "∞", "±", "-/+", "+−", "−+", "↑"
        };
        std::vector<MoveNode*> annotated_moves;
        MoveNode* node;
        bool annotation_test_parsed =
            gambitree::PgnParser::parse(annotated_pgn, annotated_document) &&
            annotated_document.games.size() == 1;
        expect_true(annotation_test_parsed,
                    "all twelve requested move NAG fixtures parse in a legal main line");
        if (annotation_test_parsed) {
            node = annotated_document.games[0]->root;
            while (!node->children.empty()) {
                node = node->children[0];
                annotated_moves.push_back(node);
            }
            layout.build(*annotated_document.games[0], 1200, 8, 18);
            std::size_t nag_index = 0;
            for (index = 0; index < annotated_moves.size(); ++index) {
                MoveNode* move = annotated_moves[index];
                int san_item = -1, nag_item = -1;
                std::size_t item_index;
                if (move->nags.empty()) continue;
                expect_true(nag_index < 12 && move->nags.size() == 1 &&
                            move->nags[0] == expected_nags[nag_index],
                            "each move retains exactly its original numeric NAG");
                for (item_index = 0; item_index < layout.items().size(); ++item_index) {
                    const gambitree::NotationItem& item = layout.items()[item_index];
                    if (item.node != move) continue;
                    if (item.kind == gambitree::NOTATION_MOVE && item.text == move->san)
                        san_item = static_cast<int>(item_index);
                    if (item.kind == gambitree::NOTATION_NAG) {
                        nag_item = static_cast<int>(item_index);
                        expect_true(item.text == expected_symbols[nag_index],
                                    "each of the twelve NAG codes has its requested readable display");
                    }
                }
                expect_true(san_item >= 0 && nag_item > san_item,
                            "NAG display item remains attached to its annotated SAN move");
                if (san_item >= 0 && nag_item > san_item) {
                    const gambitree::NotationItem& san = layout.items()[static_cast<std::size_t>(san_item)];
                    const gambitree::NotationItem& nag = layout.items()[static_cast<std::size_t>(nag_item)];
                    const bool move_quality = expected_nags[nag_index] == 1 ||
                        expected_nags[nag_index] == 2 || expected_nags[nag_index] == 3 ||
                        expected_nags[nag_index] == 5 || expected_nags[nag_index] == 6;
                    expect_true(nag.x == san.x + san.width + (move_quality ? 0 : 8),
                                "move-quality symbols touch SAN while position judgements keep a readable gap");
                }
                ++nag_index;
            }
            expect_true(nag_index == 12,
                        "all twelve mapped NAGs appear once and remain attached to their source moves");
        }
    }

    {
        PgnDocument unknown_document;
        expect_true(gambitree::PgnParser::parse("1. e4 $123 *", unknown_document) &&
                    unknown_document.games.size() == 1,
                    "an unlisted numeric NAG remains accepted as unknown data");
        if (!unknown_document.games.empty()) {
            layout.build(*unknown_document.games[0], 300, 8, 18);
            bool raw_unknown = false;
            for (index = 0; index < layout.items().size(); ++index)
                if (layout.items()[index].kind == gambitree::NOTATION_NAG &&
                    layout.items()[index].text == "$123") raw_unknown = true;
            expect_true(raw_unknown && unknown_document.games[0]->root->children[0]->nags[0] == 123,
                        "unlisted NAG stays as its raw code in both model and visible notation");
        }
    }

    layout.build(*document.games[0], 1, 8, 18);
    for (index = 0; index < layout.items().size(); ++index)
        expect_true(layout.items()[index].x >= 0 && layout.items()[index].width >= 0 &&
                    layout.items()[index].x + layout.items()[index].width <= 1,
                    "extremely narrow notation layout has no horizontal overflow");
    expect_true(layout.ensure_visible(document.games[0]->root, 7, 18) == 7,
                "root has no move rectangle and leaves manual scroll unchanged");

    {
        PgnDocument long_document;
        std::ostringstream long_pgn;
        int move_number;
        long_pgn << "[Result \"*\"]\n\n1. Nf3 Nf6 {scrolling comment} ";
        for (move_number = 2; move_number <= 52; ++move_number)
            long_pgn << move_number << ". " << (move_number % 2 ? "Nf3 Nf6 " : "Ng1 Ng8 ");
        long_pgn << "*";
        expect_true(gambitree::PgnParser::parse(long_pgn.str(), long_document), "long notation scrolling demo parses");
        layout.build(*long_document.games[0], 96, 8, 18);
        expect_true(layout.max_scroll(200) > 0, "long notation exceeds the visible scroll viewport");
        {
            int end_scroll = layout.ensure_end_visible(0, 36);
            bool result_fully_visible = false;
            std::size_t item_index;
            for (item_index = 0; item_index < layout.items().size(); ++item_index) {
                const gambitree::NotationItem& item = layout.items()[item_index];
                if (item.kind == gambitree::NOTATION_RESULT && item.y >= end_scroll &&
                    item.y + item.height <= end_scroll + 36) result_fully_visible = true;
            }
            expect_true(end_scroll == layout.max_scroll(36) && result_fully_visible,
                        "scroll-to-end makes the final result line fully visible");
        }
    }
}

void test_odds_games_presentation()
{
    std::ifstream input("tests/data/odds_games.pgn");
    std::string pgn((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    PgnDocument document;
    std::set<int> nag_values;
    std::vector<const MoveNode*> pending;
    std::vector<std::string> paths;
    std::vector<std::string> errors;
    std::string error;
    const MoveNode* mate = 0;
    const MoveNode* annotated = 0;
    NotationLayout layout;
    PgnLibrary library;
    std::size_t index;
    expect_true(input.good() || input.eof(), "odds_games.pgn integration fixture is readable");
    expect_true(gambitree::PgnParser::parse(pgn, document) && document.errors.empty() && document.games.size() >= 2,
                "odds_games.pgn games parse completely, including the two odds fixtures");
    if (document.games.size() < 2) return;
    expect_true(document.games[0]->initial_position.variant() == gambitree::STANDARD_CHESS &&
                document.games[0]->initial_position.piece_at(Position::square_from_string("a1")) == gambitree::NO_PIECE,
                "odds_games.pgn odds FEN remains standard chess with the missing white a1 rook");
    paths.push_back("tests/data/odds_games.pgn");
    expect_true(library.load(paths, errors), "odds_games.pgn loads through the lazy game library");
    while (!library.indexing_complete()) library.step_indexing(100, errors);
    expect_true(library.game_count() == document.games.size() && library.materialize_current(error),
                "lazy indexing/materialization finds every game in odds_games.pgn and opens the first");
    expect_true(library.current_game() != 0 && library.current_game()->initial_position.piece_at(
                    Position::square_from_string("a1")) == gambitree::NO_PIECE,
                "lazy first game retains its handicap start position");
    expect_true(library.next_game() && library.materialize_current(error) && library.current_game() != 0 &&
                library.current_game()->initial_position.variant() == gambitree::STANDARD_CHESS,
                "lazy game switching opens the second orthodox-rules odds game");
    pending.push_back(document.games[0]->root);
    while (!pending.empty()) {
        const MoveNode* node = pending.back();
        std::size_t child;
        pending.pop_back();
        for (index = 0; index < node->nags.size(); ++index) nag_values.insert(node->nags[index]);
        if (node->san == "O-O#") mate = node;
        if (node->san == "Na5" && node->nags.size() == 1 && node->nags[0] == 1) annotated = node;
        for (child = 0; child < node->children.size(); ++child) pending.push_back(node->children[child]);
    }
    expect_true(nag_values.size() == 4 && nag_values.count(1) && nag_values.count(2) &&
                nag_values.count(4) && nag_values.count(5),
                "odds_games.pgn keeps its four structural NAG values: $1, $2, $4, and $5");
    expect_true(mate && mate->nags.empty() && mate->comments.size() == 1,
                "terminal O-O# keeps the prose annotation as a comment, not as a structural NAG");
    if (mate) {
        std::vector<const MoveNode*> path;
        const MoveNode* node = mate;
        Position terminal;
        int move_index = -1, comment_index = -1, comment_symbol_tokens = 0;
        std::string presented_comment;
        MoveNavigator navigator(*document.games[0]);
        while (node && node->has_move) { path.push_back(node); node = node->parent; }
        while (!path.empty()) { navigator.forward(); path.pop_back(); }
        terminal = navigator.position();
        expect_true(terminal.is_checkmate() && terminal.to_fen() ==
                    "r4b1r/ppp3pp/8/4p3/2Pq4/3P4/PP2QPPP/2k2RK1 b - - 1 18",
                    "odds_games.pgn terminal O-O# reaches the expected checkmate position and FEN");
        expect_true(mate->comments[0] == "Checkmate by castling $1 A rare and special\nopportunity $1",
                    "odds_games.pgn comment storage retains its original literal $1 codes and newline");
        layout.build(*document.games[0], 900, 8, 18);
        for (index = 0; index < layout.items().size(); ++index) {
            const gambitree::NotationItem& item = layout.items()[index];
            if (item.node == mate && item.kind == gambitree::NOTATION_MOVE) move_index = static_cast<int>(index);
            if (item.kind == gambitree::NOTATION_COMMENT && move_index >= 0 &&
                static_cast<int>(index) > move_index) {
                if (comment_index < 0) comment_index = static_cast<int>(index);
                if (!presented_comment.empty()) presented_comment += ' ';
                presented_comment += item.text;
                if (item.text == "!") ++comment_symbol_tokens;
            }
        }
        expect_true(move_index >= 0 && comment_index > move_index,
                    "terminal move and prose comment retain their structural order");
        expect_true(comment_index >= 0 && comment_symbol_tokens == 2 && presented_comment ==
                    "Checkmate by castling ! A rare and special opportunity !",
                    "odds_games.pgn terminal comment formats both NAG-like codes in presentation only");
    }
    expect_true(annotated != 0, "odds_games.pgn has a structural $1 attached to the 5...Na5 variation move");
    if (annotated) {
        layout.build(*document.games[0], 900, 8, 18);
        {
            int move_index = -1, nag_index = -1;
            int nag_width = -1;
            for (index = 0; index < layout.items().size(); ++index) {
                const gambitree::NotationItem& item = layout.items()[index];
                if (item.node == annotated && item.kind == gambitree::NOTATION_MOVE) move_index = static_cast<int>(index);
                if (item.node == annotated && item.kind == gambitree::NOTATION_NAG) {
                    nag_index = static_cast<int>(index);
                    if (item.text == "!") nag_width = item.width;
                }
            }
            expect_true(move_index >= 0 && nag_index > move_index && nag_width == 8,
                        "structural $1 renders as measured ASCII ! immediately after its annotated move");
        }
    }
    {
        PgnDocument symbolic;
        NotationLayout symbolic_layout;
        bool found = false;
        expect_true(gambitree::PgnParser::parse("[Result \"*\"]\n\n1. e4 ! e5 ? *\n", symbolic) &&
                    symbolic.games.size() == 1 && symbolic.games[0]->root->children[0]->nags.size() == 1 &&
                    symbolic.games[0]->root->children[0]->nags[0] == 1,
                    "symbolic PGN ! remains normalized to numeric NAG 1 in the tree");
        if (!symbolic.games.empty()) {
            symbolic_layout.build(*symbolic.games[0], 200, 8, 18);
            for (index = 0; index < symbolic_layout.items().size(); ++index)
                if (symbolic_layout.items()[index].kind == gambitree::NOTATION_NAG &&
                    symbolic_layout.items()[index].text == "!") found = true;
        }
        expect_true(found, "symbolic and numeric NAG 1 share the same presentation formatter");
    }
}

void test_notation_wrapping()
{
    NotationFontMetrics metrics(5, 18);
    std::vector<std::string> lines;
    lines = NotationLayout::wrap_text("abcd", 20, metrics);
    expect_true(lines.size() == 1 && lines[0] == "abcd", "text that exactly fits remains on one line");
    lines = NotationLayout::wrap_text("abcd", 19, metrics);
    expect_true(lines.size() == 2 && lines[0] == "abc" && lines[1] == "d",
                "a token one pixel too wide is split only because it cannot fit a full line");
    lines = NotationLayout::wrap_text("one two", 34, metrics);
    expect_true(lines.size() == 2 && lines[0] == "one" && lines[1] == "two",
                "a final normal word moves intact to the next line");
    lines = NotationLayout::wrap_text("Event: International Championship", 65, metrics);
    expect_true(lines.size() == 3 && lines[0] == "Event:" && lines[1] == "International" && lines[2] == "Championship",
                "a long Event header wraps at whitespace without splitting normal words");
    lines = NotationLayout::wrap_text("unbreakabletoken", 20, metrics);
    expect_true(lines.size() > 1 && lines[0] == "unbr" && lines[1] == "eaka",
                "an actually oversized unbroken token is safely split");
    lines = NotationLayout::wrap_text("a bb", 1, metrics);
    expect_true(lines.size() == 3 && lines[0] == "a" && lines[1] == "b" && lines[2] == "b",
                "a narrow pane still produces bounded text fragments");
    expect_true(NotationLayout::fit_tokens("Nf3 d5 c4", 34, metrics) == "Nf3 d5" &&
                NotationLayout::fit_tokens("Nf3 d5", 29, metrics) == "Nf3",
                "PV fitting retains only complete measured tokens when the next token is one pixel too wide");
    expect_true(NotationLayout::fit_tokens("VeryLongSanToken", 20, metrics).empty(),
                "a PV token wider than the complete available row is omitted rather than clipped");
    expect_true(NotationLayout::normalize_display_text("plain comment") == "plain comment" &&
                NotationLayout::normalize_display_text("caf\303\251") == "caf\303\251" &&
                NotationLayout::normalize_display_text("one\ntwo\tthree") == "one two three",
                "comment display normalisation preserves UTF-8 and normalises whitespace");
    expect_true(NotationLayout::format_comment_text("$1 $2 $3 $4 $5 $6") == "! ? !! ?? !? ?!",
                "comment presentation formats all six conventional NAG codes");
    expect_true(NotationLayout::format_comment_text("first $1 then $6") == "first ! then ?!",
                "multiple conventional codes in one comment are formatted");
    expect_true(NotationLayout::format_comment_text("($1) $1. price$1 abc$12xyz $17") ==
                "(!) !. price$1 abc$12xyz $17",
                "comment NAG recognition respects token boundaries and preserves punctuation/unknown codes");
    expect_true(NotationLayout::format_comment_text("cost $5, but price$1 and $17 remain") ==
                "cost !?, but price$1 and $17 remain" &&
                NotationLayout::format_comment_text("$ is money; $01 is not a conventional code") ==
                "$ is money; $01 is not a conventional code",
                "ordinary dollar signs and noncanonical/unknown numbers are preserved");
    expect_true(NotationLayout::format_comment_text("caf\303\251\n$5\tprice: $2!") == "caf\303\251 !? price: ?!",
                "comment NAG formatting preserves UTF-8 and normalizes whitespace");
    expect_true(NotationLayout::format_comment_text("[%diagram]").empty() &&
                NotationLayout::format_comment_text("[%mdl 512] This loses, but") == "This loses, but" &&
                NotationLayout::format_comment_text("[%eval 0.65] White is better.") == "White is better." &&
                NotationLayout::format_comment_text("[%clk 0:03:12]").empty() &&
                NotationLayout::format_comment_text("A brilliant move. [%diagram] Fischer sacrifices his queen.") ==
                    "A brilliant move. Fischer sacrifices his queen." &&
                NotationLayout::format_comment_text("Fischer sacrifices his queen. [%diagram]") ==
                    "Fischer sacrifices his queen." &&
                NotationLayout::format_comment_text("first[%diagram]second [%mdl 512] [%eval 0.2]third") ==
                    "first second third",
                "well-formed PGN directives are hidden and surrounding prose is spaced cleanly");
    expect_true(NotationLayout::format_comment_text("[This is an editorial note.] [%eval 0.65] [%clk 0:03:12]") ==
                    "[This is an editorial note.]",
                "ordinary bracketed text stays visible while PGN instructions disappear");
    expect_true(NotationLayout::format_comment_text("unfinished [%eval 0.65") ==
                    "unfinished [%eval 0.65" &&
                NotationLayout::format_comment_text("empty [%] bracket [editorial]") ==
                    "empty [%] bracket [editorial]",
                "malformed or unterminated bracket segments remain intact");
    {
        std::string invalid("bad\377\001text", 9);
        expect_true(NotationLayout::normalize_display_text(invalid) == "bad?text",
                    "invalid UTF-8 and controls become safe display text without replacement glyph bytes");
    }
    expect_true(NotationLayout::full_line_viewport_height(54, 18) == 54 &&
                NotationLayout::full_line_viewport_height(55, 18) == 54 &&
                NotationLayout::full_line_viewport_height(71, 18) == 54,
                "notation viewport exposes only an integral number of line boxes");
}

void test_pgn_library()
{
    const std::string first_storage = test_scratch_directory + "/gambitree-library-first.pgn";
    const char* first = first_storage.c_str();
    const std::string second_storage = test_scratch_directory + "/gambitree-library-second.pgn";
    const char* second = second_storage.c_str();
    const std::string empty_storage = test_scratch_directory + "/gambitree-library-empty.pgn";
    const char* empty = empty_storage.c_str();
    const std::string malformed_storage = test_scratch_directory + "/gambitree-library-malformed.pgn";
    const char* malformed = malformed_storage.c_str();
    std::vector<std::string> paths;
    std::vector<std::string> errors;
    std::string error;
    PgnLibrary library;
    std::ofstream output(first);
    output << "[White \"One\"]\n[Black \"Two\"]\n[Result \"*\"]\n\n1. e4 *\n\n"
           << "[White \"Three\"]\n[Black \"Four\"]\n[Result \"1-0\"]\n\n1. d4 1-0\n";
    output.close();
    output.open(second);
    output << "[SetUp \"1\"]\n[FEN \"8/8/8/8/8/8/4K3/7k w - - 0 1\"]\n[White \"FEN White\"]\n[Result \"*\"]\n\n*\n";
    output.close();
    output.open(empty); output.close();
    output.open(malformed); output << "[White \"unterminated\"\n1. e4"; output.close();
    paths.push_back(first); paths.push_back(test_scratch_directory + "/gambitree-library-missing.pgn"); paths.push_back(empty); paths.push_back("."); paths.push_back(malformed); paths.push_back(second);
    expect_true(library.load(paths, errors), "a valid PGN file loads despite invalid sibling files");
    expect_true(library.requested_paths() == paths,
                "file chooser retains all original command-line paths and their order");
    while (!library.indexing_complete()) library.step_indexing(100, errors);
    expect_true(library.file_count() == 4 && library.game_count() == 2 && !errors.empty(), "indexing retains readable files while reporting empty or malformed content");
    {
        std::vector<gambitree::PgnTag> tags;
        expect_true(library.game_tags(1, tags, error) &&
                    tags.size() >= 3 &&
                    tags[0].name == "White" && tags[0].value == "Three" &&
                    library.current_game() == 0,
                    "party chooser reads tags from the indexed header without materializing its move tree");
    }
    expect_true(library.materialize_current(error), "first indexed game materializes on demand");
    expect_true(library.current_game()->tag_value("White") && *library.current_game()->tag_value("White") == "One", "first game metadata is active");
    expect_true(!library.previous_file() && !library.previous_game(), "first file and game boundaries are safe");
    expect_true(library.next_game() && library.game_index() == 1 && library.materialize_current(error), "next game selects and materializes a second game in its file");
    expect_true(library.current_game()->tag_value("White") && *library.current_game()->tag_value("White") == "Three", "game switch updates metadata source");
    expect_true(library.select_game(0) && library.game_index() == 0 &&
                library.materialize_current(error) &&
                library.current_game()->tag_value("White") &&
                *library.current_game()->tag_value("White") == "One" &&
                !library.current_game()->root->children.empty() &&
                library.current_game()->root->children[0]->position_after.to_fen() ==
                    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
                "arbitrary party selection uses the existing indexed document");
    expect_true(library.select_file_path(second) && library.file_index() == 3 &&
                library.game_index() == 0 &&
                library.materialize_current(error),
                "selecting a listed file resets to its first game without stale game state");
    expect_true(library.select_file_path(first) &&
                library.select_game(1) &&
                library.materialize_current(error),
                "library chooser can return to a selected game after file switching");
    expect_true(library.next_file() && library.file_index() == 2 && !library.materialize_current(error),
                "an indexed malformed game reports a lazy parse failure without a crash");
    expect_true(library.next_file() && library.file_index() == 3 && library.game_index() == 0 && library.materialize_current(error),
                "file switch reaches a valid game after a malformed lazy game");
    expect_true(library.current_game()->initial_position.to_fen() == "8/8/8/8/8/8/4K3/7k w - - 0 1", "FEN-start game is retained");
    expect_true(!library.next_file() && !library.next_game(), "last file and game boundaries are safe");
    expect_true(library.previous_file() && !library.materialize_current(error) && library.previous_file() && library.materialize_current(error) && library.current_game()->tag_value("White") &&
                *library.current_game()->tag_value("White") == "One", "lazy game switching never leaves a dangling active game");
    std::remove(first); std::remove(second); std::remove(empty); std::remove(malformed);
}

std::string read_file_bytes(const std::string& path)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool write_file_bytes(const std::string& path, const std::string& bytes)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary |
                         std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

void verify_partial_tree(const MoveNode* parent, const Position& before,
                         std::set<const MoveNode*>& owned)
{
    for (std::size_t i = 0; i < parent->children.size(); ++i) {
        const MoveNode* node = parent->children[i];
        const bool fresh = owned.insert(node).second;
        expect_true(fresh && node->has_move && node->parent == parent,
                    "every retained node has one owner and its exact parent");
        if (!fresh) continue;
        Position next = before;
        Undo undo;
        const bool legal = node->san == "--" ? next.make_null_move(undo) : next.make_move(node->move, undo);
        expect_true(legal && next.to_fen() == node->position_after.to_fen(),
                    "every retained edge is legal and matches its stored full position");
        verify_partial_tree(node, next, owned);
    }
}

void test_partial_games()
{
    using namespace gambitree;
    const std::string damaged = read_file_bytes("tests/data/guarin-malagon-2024-illegal-last-move.pgn");
    const std::string original = read_file_bytes("tests/data/guarin-malagon-2024-mate-suffix.pgn");
    std::string expected = original;
    const std::size_t at = expected.rfind("Rg1+");
    expect_true(at != std::string::npos, "original mate fixture retains its legitimate final move");
    if (at == std::string::npos) return;
    expected.replace(at, 4, "Rh8");
    expect_true(damaged == expected, "the partial regression changes only the last move to Rh8");
    const char* before_error = "8/8/8/4B3/2b3p1/6R1/5K2/7k w - - 7 78";
    PgnDocument strict, view;
    expect_true(!PgnParser::parse(damaged, strict) && strict.games.empty() && strict.errors.size() == 1,
                "strict parsing still rejects and discards the illegal game");
    expect_true(!PgnParser::parse_for_viewing(damaged, view) && view.games.size() == 1 && view.errors.size() == 1,
                "viewer parsing returns an explicitly partial game while reporting the original error");
    if (view.games.size() != 1) return;
    PgnGame& game = *view.games[0];
    expect_true(game.parse_status == PGN_PARTIAL && game.available_plies == 154 &&
                game.tag_value("White") && *game.tag_value("White") == "Guarin Fonseca, Nobel Alfredo" &&
                game.tag_value("PlyCount") && *game.tag_value("PlyCount") == "155" &&
                game.parse_error.line == 22 && game.parse_error.column == 933,
                "the partial game retains its headers, 154 available plies and exact source coordinates");
    const std::string warning = game.parse_warning();
    expect_true(warning.find("Partially loaded") == 0 && warning.find("78.White") != std::string::npos &&
                warning.find("\"Rh8\"") != std::string::npos && warning.find("no unique legal move") != std::string::npos &&
                warning.find("154 preceding plies available") != std::string::npos,
                "partial warning explains the offending source move and usable prefix");
    std::set<const MoveNode*> owned;
    owned.insert(game.root);
    verify_partial_tree(game.root, game.initial_position, owned);
    expect_true(owned.size() == 155 && owned.count(game.error_position) == 1 &&
                game.error_position->children.empty() && game.error_position->san == "Bc4",
                "only 154 validated moves and the root are owned and navigable");
    MoveNavigator cursor(game);
    expect_true(cursor.current() == game.error_position && cursor.position().to_fen() == before_error && !cursor.forward(),
                "new viewer navigation automatically selects the exact position before 78.Rh8");
    int backwards = 0, forwards = 0;
    while (cursor.back()) ++backwards;
    expect_true(cursor.at_root() && cursor.position().to_fen() == game.initial_position.to_fen(),
                "normal backwards navigation reaches the original starting position");
    while (cursor.forward()) ++forwards;
    expect_true(backwards == 154 && forwards == 154 && cursor.position().to_fen() == before_error,
                "all 77 legal move pairs can be replayed in both directions");
    for (int figurines = 0; figurines < 2; ++figurines) for (int width = 240; width <= 500; width += 260) {
        NotationLayout layout;
        layout.set_figurines(figurines != 0);
        layout.build(game, width, NotationFontMetrics());
        std::string notice;
        int moves = 0, results = 0;
        for (std::size_t i = 0; i < layout.items().size(); ++i) {
            const NotationItem& item = layout.items()[i];
            if (item.kind == NOTATION_MOVE && item.node) {
                ++moves;
                expect_true(owned.count(item.node) == 1 && item.node->san != "Rh8",
                            "notation hitboxes refer only to owned legal moves");
            }
            if (item.kind == NOTATION_RESULT) ++results;
            if (item.kind == NOTATION_COMMENT) { notice += item.text; notice += ' '; }
        }
        expect_true(moves == 154 && results == 0 && notice.find("Rh8") != std::string::npos &&
                    notice.find("154") != std::string::npos && !layout.selection_rects(game.error_position).empty(),
                    "notation marks the truncation without an illegal move or misleading terminal result");
        const int scroll = layout.ensure_visible(cursor.current(), 0, 160);
        bool visible = false;
        for (std::size_t i = 0; i < layout.items().size(); ++i) {
            const NotationItem& item = layout.items()[i];
            if (item.node == cursor.current() && item.kind == NOTATION_MOVE)
                visible = item.y >= scroll && item.y + item.height <= scroll + 160;
        }
        expect_true(visible, "initial truncation selection can be revealed after reflow and figurine changes");
    }

    struct Case { const char* text; PgnParseStatus status; std::size_t plies; const char* fen; bool complete; };
    const Case cases[] = {
        {"[Event \"First move\"]\n\n1. Rh8 *", PGN_PARTIAL, 0,
         "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false},
        {"1. e4 e5 2. Nf3 Nc6 3. Bh9 Bb5 *", PGN_PARTIAL, 4,
         "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3", false},
        {"1. e4 e5 (1... c5 2. Nf3 Kb8) (1... d5) 2. Nf3 *", PGN_RECOVERED_VARIATION, 3,
         "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2", true},
        {"1. e4 e5 (1... c5 2. Nf3 Kb8 2. d4 *", PGN_PARTIAL, 3,
         "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2", false},
        {"1. e4 e5 (1... c5 2. Nf3", PGN_PARTIAL, 3,
         "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2", false},
        {"1. e4 e5 ) 2. Nf3 *", PGN_PARTIAL, 2,
         "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2", false},
        {"1. e4 e5 {unclosed comment", PGN_PARTIAL, 2,
         "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2", false},
        {"1. e4 1-0 (1... e5 2. Rh8)", PGN_PARTIAL, 1,
         "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", false},
        {"[SetUp \"1\"]\n[FEN \"4k3/8/8/8/8/8/8/4R1K1 b - - 0 31\"]\n31... -- *", PGN_PARTIAL, 0,
         "4k3/8/8/8/8/8/8/4R1K1 b - - 0 31", false}
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        PgnDocument doc;
        expect_true(PgnParser::parse_for_viewing(cases[i].text, doc) == cases[i].complete && doc.games.size() == 1,
                    "viewer recovery distinguishes complete mainlines from aborted parsing");
        if (doc.games.size() != 1) continue;
        PgnGame& recovered = *doc.games[0];
        MoveNavigator navigation(recovered);
        expect_true(recovered.parse_status == cases[i].status && recovered.available_plies == cases[i].plies &&
                    navigation.current() == recovered.error_position && navigation.position().to_fen() == cases[i].fen,
                    "each recovery selects its owned last valid node with exact ply count and FEN");
        std::set<const MoveNode*> tree;
        tree.insert(recovered.root);
        verify_partial_tree(recovered.root, recovered.initial_position, tree);
        expect_true(tree.count(recovered.error_position) == 1, "partial error positions belong to the navigable tree");
        if (cases[i].status == PGN_RECOVERED_VARIATION) {
            navigation.to_begin(); navigation.to_end();
            expect_true(recovered.root->children[0]->children.size() == 3 &&
                        navigation.current()->san == "Nf3" && doc.warnings.size() == 1 && doc.errors.empty(),
                        "a bounded RAV error retains the mainline and subsequent sibling RAV");
        }
    }
    const char* fatal[] = {
        "[Event \"unterminated", "[Event \"ok\"]\n[FEN \"unterminated",
        "[SetUp \"1\"]\n[FEN \"invalid\"]\n1. e4 *", "[Variant \"Unsupported\"]\n1. e4 *"
    };
    for (std::size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); ++i) {
        PgnDocument doc;
        expect_true(!PgnParser::parse_for_viewing(fatal[i], doc) && doc.games.empty() && !doc.errors.empty(),
                    "unreadable headers and unknown starting positions remain fatal without invented nodes");
    }
    const std::string following = "\n\n[Event \"Following valid game\"]\n\n1. d2d4 Ng8f6 *\n";
    const std::string multiple = damaged + following;
    PgnDocument combined;
    expect_true(!PgnParser::parse_for_viewing(multiple, combined) && combined.games.size() == 2 &&
                combined.games[0]->parse_status == PGN_PARTIAL && combined.games[1]->parse_status == PGN_COMPLETE,
                "a following valid LAN game is recovered without absorbing it into the partial game");
    PgnDocument broken_rav;
    expect_true(!PgnParser::parse_for_viewing(std::string(cases[4].text) + following, broken_rav) &&
                broken_rav.games.size() == 2 && broken_rav.games[1]->parse_status == PGN_COMPLETE,
                "even an unclosed RAV stops at a reliable next-game tag boundary");
    PgnDocument swallowed;
    expect_true(!PgnParser::parse_for_viewing(std::string("1. e4 {unclosed") + following, swallowed) &&
                swallowed.games.size() == 1 && swallowed.games[0]->parse_status == PGN_PARTIAL,
                "an unclosed comment cannot invent a reliable boundary for a later apparent game");
    PgnDocument lan_view;
    expect_true(PgnParser::parse_for_viewing(read_file_bytes("tests/data/spice-cup-2025-lan.pgn"), lan_view) &&
                lan_view.games.size() == 2 && lan_view.games[0]->parse_status == PGN_COMPLETE &&
                lan_view.games[1]->parse_status == PGN_COMPLETE,
                "both original LAN regression games remain complete in the viewer parser");
    PgnDocument valid;
    expect_true(PgnParser::parse_for_viewing(original, valid) && valid.games.size() == 1 &&
                valid.games[0]->parse_status == PGN_COMPLETE && valid.games[0]->parse_warning().empty(),
                "the legal 78.Rg1+ game remains complete in viewer mode");

    if (valid.games.size() == 1) {
        MoveNavigator normal(*valid.games[0]);
        expect_true(normal.at_root(), "a completely valid game retains the original starting selection");
    }
    const std::string path = test_scratch_directory + "/partial-then-valid.pgn";
    expect_true(write_file_bytes(path, multiple), "write isolated partial/valid selector fixture");
    PgnLibrary library;
    std::vector<std::string> paths(1, path), errors;
    std::string error;
    std::ostringstream log;
    std::streambuf* previous = std::cerr.rdbuf(log.rdbuf());
    const bool opened = library.load(paths, errors);
    while (!library.indexing_complete()) library.step_indexing(100, errors);
    const bool first = library.materialize_current(error);
    bool cached = false, switched = false, returned = false;
    if (first) {
        cached = library.materialize_current(error) && library.current_game()->parse_status == PGN_PARTIAL;
        MoveNavigator navigation(*library.current_game());
        navigation.back(); navigation.forward();
        std::string reason;
        expect_true(library.current_game_deletion_eligible(reason), "a partial game with reliable boundaries remains eligible for deletion");
        switched = library.select_game(1) && library.materialize_current(error) &&
                   library.current_game()->parse_status == PGN_COMPLETE;
        if (switched) {
            MoveNavigator normal(*library.current_game());
            expect_true(normal.at_root(), "selecting a valid party keeps ordinary initial navigation");
        }
        returned = library.select_game(0) && library.materialize_current(error) &&
                   library.current_game()->parse_status == PGN_PARTIAL;
        if (returned) {
            MoveNavigator again(*library.current_game());
            expect_true(again.position().to_fen() == before_error, "returning through the selector shows the error position again");
        }
    }
    std::cerr.rdbuf(previous);
    expect_true(opened && library.game_count() == 2 && first && cached && switched && returned && error.empty(),
                "indexed loading, cache reuse and arbitrary selection work for partial and valid games");
    const std::string logged = log.str();
    const std::size_t occurrence = logged.find("\"Rh8\"");
    expect_true(occurrence != std::string::npos && logged.find("\"Rh8\"", occurrence + 1) == std::string::npos &&
                logged.find("partial-then-valid.pgn: game 1:") != std::string::npos,
                "a partial-game warning is logged once even after navigation, caching and reselection");
    std::remove(path.c_str());
    const std::string rav_path = test_scratch_directory + "/broken-rav-then-valid.pgn";
    expect_true(write_file_bytes(rav_path, std::string("[Event \"Broken RAV\"]\n\n") + cases[4].text + following),
                "write an unclosed-RAV index regression");
    PgnLibrary rav_library;
    paths[0] = rav_path;
    expect_true(rav_library.load(paths, errors), "unclosed-RAV file opens for indexing");
    while (!rav_library.indexing_complete()) rav_library.step_indexing(100, errors);
    expect_true(rav_library.game_count() == 2 && rav_library.materialize_current(error) &&
                rav_library.current_game()->parse_status == PGN_PARTIAL && rav_library.select_game(1) &&
                rav_library.materialize_current(error) && rav_library.current_game()->parse_status == PGN_COMPLETE,
                "the real index and lazy loader separate a valid game after an unclosed RAV");
    std::remove(rav_path.c_str());
}

void finish_index(gambitree::PgnIndex& index, const std::string& path)
{
    std::string error;
    expect_true(index.open(path, error), "deletion fixture index opens");
    while (!index.complete() && !index.failed())
        expect_true(index.step(256, error), "deletion fixture indexing completes");
}

void test_pgn_delete_filesystem()
{
    std::ostringstream suffix;
    std::string base;
    std::string path;
    std::string first;
    std::string middle;
    std::string last;
    std::string original;
    std::string expected;
    std::string error;
    gambitree::PgnIndex index;
    struct stat replaced_status;
    gambitree::PgnDeleteBaseline baseline;
    gambitree::PgnDeleteResult result;
    static const unsigned char sha256_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    int fd;
    suffix << getpid();
    base = std::string(test_scratch_directory + "/gambitree deletion suite ") +
        suffix.str();
    mkdir(base.c_str(), 0700);
    path = base + "/three games.pgn";
    first = "[Event  \"A\"]\r\n[White \"One\"]\r\n[Black \"Two\"]\r\n\r\n"
        "1. e4 {keep this comment} e5 $1 (1... c5) *\r\n";
    middle = "\r\n[Event \"B\"]\r\n[White \"Three\"]\r\n[Black \"Four\"]\r\n\r\n"
        "1. d4 {middle comment} d5 (1... Nf6 $5) 1/2-1/2\r\n";
    last = "\r\n[Event \"C\"]\r\n[White \"Five\"]\r\n[Black \"Six\"]\r\n\r\n"
        "1. c4 ; semicolon comment\r\n1... e5 *";
    original = first + middle + last;
    expect_true(write_file_bytes(path, original),
                "CRLF and no-final-newline deletion fixture is written");
    chmod(path.c_str(), 0640);
    finish_index(index, path);
    expect_true(index.complete_entry_count() == 3,
                "byte-range deletion fixture has three exact games");
    baseline = gambitree::capture_pgn_delete_baseline(path);
    expect_true(baseline.eligible &&
                gambitree::pgn_delete_baseline_matches(path, baseline, error),
                "small regular single-link file receives a SHA-256 load baseline");
    expect_true(!gambitree::pgn_delete_range(path, baseline, 1, 1, false,
                                             result) &&
                read_file_bytes(path) == original,
                "invalid byte range fails before changing the source");
    expected = first + "\r\n" + last.substr(2);
    expect_true(gambitree::pgn_delete_range(
                    path, baseline, index.entries()[1].begin,
                    index.entries()[1].end, false, result) &&
                result.committed && !result.file_removed,
                "middle game is replaced through a same-directory staged file");
    expect_true(read_file_bytes(path) == expected,
                "deleting a CRLF game removes its separator without adding blank lines");
    expect_true(stat(path.c_str(), &replaced_status) == 0 &&
                (replaced_status.st_mode & 0777) == 0640,
                "staged replacement retains source permission bits");
    {
        DIR* directory = opendir(base.c_str());
        struct dirent* entry;
        bool staged_left = false;
        if (directory) {
            while ((entry = readdir(directory)) != 0)
                if (std::strstr(entry->d_name, ".gambitree-delete.") != 0)
                    staged_left = true;
            closedir(directory);
        }
        expect_true(directory != 0 && !staged_left,
                    "successful replacement leaves no staging file behind");
    }
    expect_true(!gambitree::pgn_delete_baseline_matches(path, baseline, error),
                "old fingerprint rejects the file after a successful mutation");
    baseline = gambitree::capture_pgn_delete_baseline(path);
    expect_true(baseline.eligible,
                "successful replacement can establish the next baseline");

    path = base + "/bom-delete.pgn";
    original = std::string("\xef\xbb\xbf") +
        "[Event \"First\"]\n[Result \"*\"]\n\n1. e4 *\n\n"
        "[Event \"Second\"]\n[Result \"*\"]\n\n1. d4 *";
    expect_true(write_file_bytes(path, original),
                "BOM deletion fixture is written");
    finish_index(index, path);
    expect_true(index.entries().size() == 2 &&
                index.entries()[0].begin == 0 &&
                index.entries()[0].end == index.entries()[1].begin,
                "BOM remains part of the first absolute byte range");
    baseline = gambitree::capture_pgn_delete_baseline(path);
    expect_true(baseline.eligible && gambitree::pgn_delete_range(
                    path, baseline, index.entries()[0].begin,
                    index.entries()[0].end, false, result) &&
                result.committed,
                "first game can be deleted from a BOM-prefixed PGN");
    expected = std::string("\xef\xbb\xbf") +
        "[Event \"Second\"]\n[Result \"*\"]\n\n1. d4 *";
    expect_true(read_file_bytes(path) == expected,
                "deleting the first game preserves exactly one leading BOM");

    write_file_bytes(base + "/sha256.pgn", "abc");
    baseline = gambitree::capture_pgn_delete_baseline(base + "/sha256.pgn");
    expect_true(baseline.eligible &&
                std::memcmp(baseline.sha256, sha256_abc,
                            sizeof(sha256_abc)) == 0,
                "deletion baseline uses standard SHA-256 content hashing");

    path = base + "/external change.pgn";
    write_file_bytes(path, first + last);
    baseline = gambitree::capture_pgn_delete_baseline(path);
    original = read_file_bytes(path);
    expected = original;
    expected[expected.find("One")] = 'X';
    write_file_bytes(path, expected);
    expect_true(!gambitree::pgn_delete_baseline_matches(path, baseline, error) &&
                read_file_bytes(path) == expected,
                "same-size external content change is rejected and preserved");

    path = base + "/replaced inode.pgn";
    write_file_bytes(path, first + last);
    baseline = gambitree::capture_pgn_delete_baseline(path);
    write_file_bytes(base + "/replacement.pgn", read_file_bytes(path));
    rename((base + "/replacement.pgn").c_str(), path.c_str());
    expect_true(!gambitree::pgn_delete_baseline_matches(path, baseline, error),
                "same-size external replacement is detected by inode identity");

    path = base + "/linked.pgn";
    write_file_bytes(path, first);
    link(path.c_str(), (base + "/linked-alias.pgn").c_str());
    baseline = gambitree::capture_pgn_delete_baseline(path);
    expect_true(!baseline.eligible,
                "files with multiple hard links are not deletion eligible");
    unlink((base + "/linked-alias.pgn").c_str());
    unlink(path.c_str());

    path = base + "/target.pgn";
    write_file_bytes(path, first);
    symlink(path.c_str(), (base + "/symlink.pgn").c_str());
    baseline = gambitree::capture_pgn_delete_baseline(base + "/symlink.pgn");
    expect_true(!baseline.eligible,
                "symbolic-link sources are not deletion eligible");

    baseline = gambitree::capture_pgn_delete_baseline(path);
    unlink(path.c_str());
    expect_true(!gambitree::pgn_delete_baseline_matches(path, baseline, error),
                "a source removed after load is rejected");
    baseline = gambitree::capture_pgn_delete_baseline(base);
    expect_true(!baseline.eligible,
                "non-regular filesystem objects are not deletion eligible");

    path = base + "/at-limit.pgn";
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ftruncate(fd, static_cast<off_t>(32) * 1024 * 1024);
        close(fd);
    }
    baseline = gambitree::capture_pgn_delete_baseline(path);
    expect_true(baseline.eligible && baseline.size ==
                static_cast<uint64_t>(32) * 1024U * 1024U,
                "a file exactly at the 32 MiB boundary is eligible");
    fd = open(path.c_str(), O_WRONLY);
    if (fd >= 0) {
        ftruncate(fd, static_cast<off_t>(32) * 1024 * 1024 + 1);
        close(fd);
    }
    baseline = gambitree::capture_pgn_delete_baseline(path);
    expect_true(!baseline.eligible && baseline.size == 0,
                "files above 32 MiB are refused without retaining a hash");

    unlink((base + "/three games.pgn").c_str());
    unlink((base + "/external change.pgn").c_str());
    unlink((base + "/replaced inode.pgn").c_str());
    unlink((base + "/symlink.pgn").c_str());
    unlink((base + "/target.pgn").c_str());
    unlink((base + "/at-limit.pgn").c_str());
    unlink((base + "/sha256.pgn").c_str());
    unlink((base + "/bom-delete.pgn").c_str());
    rmdir(base.c_str());
}

void test_pgn_delete_library()
{
    std::ostringstream suffix;
    std::string base;
    std::string path;
    std::string text;
    std::vector<std::string> paths;
    std::vector<std::string> errors;
    std::string error;
    PgnLibrary library;
    bool committed;
    bool removed;
    bool sync_warning;
    suffix << getpid();
    base = std::string(test_scratch_directory + "/gambitree-delete-library-") +
        suffix.str();
    mkdir(base.c_str(), 0700);
    path = base + "/games.pgn";
    text = "[Event \"A\"]\n[White \"One\"]\n[Result \"*\"]\n\n1. e4 *\n\n"
        "[Event \"B\"]\n[White \"Two\"]\n[Result \"*\"]\n\n1. d4 *\n\n"
        "[Event \"C\"]\n[White \"Three\"]\n[Result \"*\"]\n\n1. c4 *";
    write_file_bytes(path, text);
    paths.push_back(path);
    expect_true(library.load(paths, errors),
                "deletion library fixture loads");
    while (!library.indexing_complete())
        library.step_indexing(100, errors);
    expect_true(library.game_count() == 3 && library.materialize_current(error),
                "deletion library indexes and materializes first game");
    expect_true(library.delete_current_game(committed, removed,
                    sync_warning, error) && committed && !removed &&
                library.game_count() == 2 && library.game_index() == 0 &&
                library.materialize_current(error) &&
                library.current_game()->tag_value("White") &&
                *library.current_game()->tag_value("White") == "Two",
                "first deletion selects the former next game and reindexes");
    expect_true(library.delete_current_game(committed, removed,
                    sync_warning, error) && committed && !removed &&
                library.game_count() == 1 && library.game_index() == 0 &&
                library.materialize_current(error) &&
                library.current_game()->tag_value("White") &&
                *library.current_game()->tag_value("White") == "Three",
                "successive deletion refreshes the baseline and selects next");
    expect_true(library.delete_current_game(committed, removed,
                    sync_warning, error) && committed && removed &&
                library.file_count() == 0 && access(path.c_str(), F_OK) != 0,
                "deleting the sole remaining game removes its PGN file");

    path = base + "/last-selected.pgn";
    write_file_bytes(path,
        "[White \"Previous\"]\n[Result \"*\"]\n\n1. e4 *\n\n"
        "[White \"Selected last\"]\n[Result \"*\"]\n\n1. d4 *\n");
    paths.clear();
    paths.push_back(path);
    library.load(paths, errors);
    while (!library.indexing_complete())
        library.step_indexing(100, errors);
    library.next_game();
    library.materialize_current(error);
    expect_true(library.delete_current_game(committed, removed,
                    sync_warning, error) && committed && !removed &&
                library.game_count() == 1 && library.game_index() == 0 &&
                library.materialize_current(error) &&
                library.current_game()->tag_value("White") &&
                *library.current_game()->tag_value("White") == "Previous",
                "deleting the last ordered game selects the previous game");
    unlink(path.c_str());

    path = base + "/bom-library.pgn";
    write_file_bytes(path, std::string("\xef\xbb\xbf") +
        "[White \"BOM first\"]\n[Result \"*\"]\n\n1. e4 *\n\n"
        "[White \"BOM second\"]\n[Result \"*\"]\n\n1. d4 *\n");
    paths.clear();
    paths.push_back(path);
    library.load(paths, errors);
    while (!library.indexing_complete())
        library.step_indexing(100, errors);
    library.materialize_current(error);
    expect_true(library.delete_current_game(committed, removed,
                    sync_warning, error) && committed && !removed &&
                read_file_bytes(path).compare(0, 3, "\xef\xbb\xbf", 3) == 0 &&
                library.game_count() == 1 &&
                library.materialize_current(error) &&
                library.current_game()->tag_value("White") &&
                *library.current_game()->tag_value("White") == "BOM second",
                "library deletion preserves the BOM and reindexes remaining games");
    expect_true(library.delete_current_game(committed, removed,
                    sync_warning, error) && committed && removed &&
                access(path.c_str(), F_OK) != 0,
                "deleting the sole BOM-prefixed game still removes the whole file");

    {
        std::string first_path = base + "/first source.pgn";
        std::string next_path = base + "/next source.pgn";
        write_file_bytes(first_path,
            "[White \"First source\"]\n[Result \"*\"]\n\n1. e4 *\n");
        write_file_bytes(next_path,
            "[White \"Next source\"]\n[Result \"*\"]\n\n1. d4 *\n");
        paths.clear();
        paths.push_back(first_path);
        paths.push_back(next_path);
        library.load(paths, errors);
        while (!library.indexing_complete())
            library.step_indexing(100, errors);
        library.materialize_current(error);
        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && removed &&
                    library.file_count() == 1 &&
                    library.current_file()->path == next_path &&
                    library.game_count() == 1 &&
                    library.materialize_current(error) &&
                    library.current_game()->tag_value("White") &&
                    *library.current_game()->tag_value("White") ==
                        "Next source",
                    "removing a source selects the adjacent loaded PGN file");
        unlink(next_path.c_str());
    }

    path = base + "/external.pgn";
    write_file_bytes(path,
        "[White \"Before\"]\n[Result \"*\"]\n\n1. e4 *\n\n"
        "[White \"After\"]\n[Result \"*\"]\n\n1. d4 *\n");
    paths.clear();
    paths.push_back(path);
    library.load(paths, errors);
    while (!library.indexing_complete())
        library.step_indexing(100, errors);
    library.materialize_current(error);
    text = read_file_bytes(path);
    text[ text.find("Before") ] = 'X';
    write_file_bytes(path, text);
    expect_true(!library.delete_current_game(committed, removed,
                    sync_warning, error) && !committed &&
                library.game_count() == 2 && read_file_bytes(path) == text,
                "external modification leaves disk and loaded games untouched");
    unlink(path.c_str());
    unlink((base + "/last-selected.pgn").c_str());
    unlink((base + "/first source.pgn").c_str());
    unlink((base + "/next source.pgn").c_str());
    rmdir(base.c_str());
}

void test_pgn_delete_partial_boundaries()
{
    using namespace gambitree;
    const std::string path = test_scratch_directory + "/delete-partial-boundaries.pgn";
    const std::string first = "[Event \"Before\"]\n\n1. d4 d5 *";
    const std::string last = "[Event \"After\"]\n\n1. c4 e5 *";
    std::string damaged = read_file_bytes("tests/data/guarin-malagon-2024-illegal-last-move.pgn");
    while (!damaged.empty() && std::isspace(static_cast<unsigned char>(damaged[damaged.size() - 1])))
        damaged.erase(damaged.size() - 1);
    const std::string rav = "[Event \"Bad alternative\"]\n\n1. e4 e5 (1... c5 2. Rh8) 2. Nf3 *";
    const std::string bad_games[] = { damaged, rav };
    std::vector<std::string> paths(1, path), errors;
    std::string error, reason;
    bool committed, removed, sync_warning;
    // Cover both kinds of move error at every position, including a sole game.
    for (std::size_t kind = 0; kind < 2; ++kind) {
        for (int location = 0; location < 4; ++location) {
            const std::string source = location == 0 ? bad_games[kind] + "\n\n" + first + "\n\n" + last :
                location == 1 ? first + "\n\n" + bad_games[kind] + "\n\n" + last :
                location == 2 ? first + "\n\n" + last + "\n\n" + bad_games[kind] : bad_games[kind];
            expect_true(write_file_bytes(path, source), "write an isolated partial-game deletion copy");
            PgnLibrary library;
            expect_true(library.load(paths, errors), "partial deletion source loads");
            while (!library.indexing_complete()) library.step_indexing(100, errors);
            const std::size_t selected = location == 3 ? 0 : location;
            expect_true(library.select_game(selected) && library.materialize_current(error) &&
                        library.current_game()->parse_status == (kind ? PGN_RECOVERED_VARIATION : PGN_PARTIAL),
                        "deletion selects the actual mainline or RAV error without changing parser status");
            const PgnIndex& index = library.current_file()->index;
            expect_true(index.boundaries_reliable(selected) &&
                        index.whole_file_is_single_game() == (location == 3) &&
                        library.current_game_deletion_eligible(reason),
                        "move validity is independent of confirmed game and whole-file boundaries");
            if (!kind) {
                MoveNavigator navigation(*library.current_game());
                expect_true(library.current_game()->available_plies == 154 && navigation.position().to_fen() ==
                            "8/8/8/4B3/2b3p1/6R1/5K2/7k w - - 7 78",
                            "the long partial deletion fixture still has the exact 154-ply prefix and pre-error FEN");
            }
            expect_true(library.delete_current_game(committed, removed, sync_warning, error) && committed &&
                        removed == (location == 3), "reliably bounded mainline and RAV errors can be removed");
            if (location == 3) {
                expect_true(library.file_count() == 0 && access(path.c_str(), F_OK) != 0,
                            "a proven sole partial game safely removes its source file");
            } else {
                expect_true(read_file_bytes(path) == first + "\n\n" + last + (location == 2 ? "\n" : ""),
                            "damaged-game deletion preserves other game bytes and the existing final-newline policy");
                expect_true(library.game_count() == 2 && library.game_index() == (location == 0 ? 0U : 1U) &&
                            library.materialize_current(error) && library.current_game()->parse_status == PGN_COMPLETE &&
                            *library.current_game()->tag_value("Event") == (location == 0 ? "Before" : "After"),
                            "partial deletion reindexes and selects the correct next or previous complete game");
            }
        }
    }
    const std::string uncertain[] = {
        "[Event \"Open comment\"]\n\n1. Rh8 {not closed",
        "[Event \"Open RAV\"]\n\n1. e4 (1. Rh8 *",
        "[Event \"Stray end\"]\n\n1. e4 ) *",
        "[Event \"Open tag\"]\n\n1. Rh8 [Event \"unfinished",
        "[Event \"Hidden second game\"]\n\n1. e4 * 1. d4 *",
        "[Event \"Hidden tag boundary\"] 1. Rh8 * [Event \"Second\"] 1. d4 *",
        "[Event \"First headers\"]\n\n[Event \"Second headers\"]",
        "[Event \"Post-result RAV\"]\n\n1. e4 *\n(1... e5)",
        "[Event \"Hidden NAG/result\"]\n\n1. Rh8 *$1 1. d4 *",
        "{only a closed comment}",
        std::string("[Event \"Embedded NUL\"]\n\n1. Rh8 *") + '\0' + last
    };
    for (std::size_t i = 0; i < sizeof(uncertain) / sizeof(uncertain[0]); ++i) {
        const std::string source = uncertain[i];
        expect_true(write_file_bytes(path, source), "write an isolated uncertain-boundary deletion copy");
        PgnLibrary library;
        expect_true(library.load(paths, errors), "structurally damaged deletion source can be indexed");
        while (!library.indexing_complete()) library.step_indexing(100, errors);
        const PgnIndex& index = library.current_file()->index;
        expect_true(index.complete() && !index.whole_file_is_single_game(),
                    "a finished index, even with one entry, does not prove safe whole-file removal");
        for (std::size_t g = 0; g < library.game_count(); ++g) {
            expect_true(!index.boundaries_reliable(g), "ambiguous game ranges remain explicitly untrusted");
            if (library.select_game(g) && library.materialize_current(error))
                expect_true(!library.current_game_deletion_eligible(reason) &&
                            reason.find("cannot safely tell where this game starts and ends") != std::string::npos,
                            "uncertain game boundaries produce a clear removal refusal even for a viewable game");
            expect_true(!library.delete_current_game(committed, removed, sync_warning, error) && !committed &&
                        !removed && read_file_bytes(path) == source,
                        "uncertain framing never authorizes destructive replacement or whole-file removal");
        }
    }
    // A comment may contain apparent headers; neither side of that split is proven.
    const std::string open_comment = "[Event \"Broken\"]\n\n1. e4 {not closed\n\n" + last;
    write_file_bytes(path, first + "\n\n" + open_comment);
    PgnLibrary mixed;
    expect_true(mixed.load(paths, errors), "mixed reliable and uncertain source loads");
    while (!mixed.indexing_complete()) mixed.step_indexing(100, errors);
    const PgnIndex& mixed_index = mixed.current_file()->index;
    expect_true(mixed.game_count() == 3 && mixed_index.boundaries_reliable(0) &&
                !mixed_index.boundaries_reliable(1) && !mixed_index.boundaries_reliable(2),
                "apparent headers inside an unclosed comment invalidate both adjacent ranges, not earlier safe games");
    expect_true(mixed.materialize_current(error) && mixed.delete_current_game(committed, removed, sync_warning, error) &&
                committed && !removed && read_file_bytes(path) == open_comment,
                "a reliable game can still be removed from a file containing later uncertain boundaries");

    const std::string trivia = " \n{closed file preamble}\n; another comment\n";
    // Quotes and delimiters inside tag values do not affect source framing.
    write_file_bytes(path, trivia + "[Event \"Quoted \\\" { ( text\"]\n\n1. Rh8 *");
    PgnLibrary sole;
    sole.load(paths, errors);
    while (!sole.indexing_complete()) sole.step_indexing(100, errors);
    expect_true(sole.current_file()->index.whole_file_is_single_game() && sole.materialize_current(error) &&
                sole.delete_current_game(committed, removed, sync_warning, error) && committed && removed &&
                access(path.c_str(), F_OK) != 0,
                "whole-file proof permits harmless preamble trivia and quoted structural characters");

    write_file_bytes(path, damaged);
    PgnLibrary changed;
    changed.load(paths, errors);
    while (!changed.indexing_complete()) changed.step_indexing(100, errors);
    expect_true(changed.materialize_current(error), "load a partial game before an external modification");
    std::string replacement = damaged;
    replacement.replace(replacement.find("Rh8"), 3, "Rh7");
    write_file_bytes(path, replacement);
    expect_true(!changed.delete_current_game(committed, removed, sync_warning, error) && !committed &&
                read_file_bytes(path) == replacement,
                "the existing identity and content checks still reject an externally changed partial source");
    PgnDeleteBaseline baseline = capture_pgn_delete_baseline(path);
    PgnDeleteResult result;
    expect_true(!pgn_delete_range(path, baseline, 1, baseline.size, true, result) && !result.committed &&
                read_file_bytes(path) == replacement,
                "whole-file unlink rejects a subrange even with a valid source fingerprint");
    unlink(path.c_str());
}

void test_pgn_index_headers_and_escape_lines()
{
    const std::string path = test_scratch_directory + "/header-escape-deletion.pgn";
    const char* ambiguous[] = {
        "[White \"Player A\"]\n[Black \"Player B\"]\n\n[White \"Player C\"]\n[Black \"Player D\"]\n\n1. d4 d5 *\n",
        "[Annotator \"A\"]\n\n[Annotator \"B\"]\n\n1. d4 d5 *\n",
        "[White \"A\"] [Black \"B\"] [White \"C\"]\n\n1. d4 d5 *\n",
        "[White \"A\"]\n\n[White C]\n[Black \"D\"]\n\n1. d4 d5 *\n",
        "[White \"A\"]\n\n[Black \"not closed\n[White \"C\"]\n\n1. d4 d5 *\n"
    };
    std::vector<std::string> paths(1, path), errors;
    std::string error, reason;
    bool committed, removed, sync_warning;
    for (std::size_t i = 0; i < sizeof(ambiguous) / sizeof(ambiguous[0]); ++i) {
        expect_true(write_file_bytes(path, ambiguous[i]), "write an isolated ambiguous-header source");
        PgnLibrary library;
        expect_true(library.load(paths, errors), "ambiguous header source opens");
        while (!library.indexing_complete()) library.step_indexing(100, errors);
        expect_true(library.game_count() != 0 && !library.current_file()->index.whole_file_is_single_game(),
                    "repeated or damaged headers cannot prove safe whole-file removal");
        for (std::size_t g = 0; g < library.game_count(); ++g) {
            expect_true(!library.current_file()->index.boundaries_reliable(g),
                        "all ranges affected by an ambiguous header block remain untrusted");
            library.select_game(g);
            library.materialize_current(error);
            expect_true(!library.current_game_deletion_eligible(reason) &&
                        !library.delete_current_game(committed, removed, sync_warning, error) && !committed &&
                        !removed && read_file_bytes(path) == ambiguous[i],
                        "ambiguous headers never authorize replacement or unlink of their source");
        }
    }
    // Escape text deliberately contains misleading tags, quotes and delimiters.
    const std::string escape = "% ignored [Event \"fake\"] { ( ] \"";
    for (int crlf = 0; crlf < 2; ++crlf) for (int where = 0; where < 6; ++where) {
        std::string prefix = where == 0 ? escape + "\n" : where == 5 ? "\xef\xbb\xbf" + escape + "\n" : "";
        std::string first = "[Event \"First\"]\n";
        if (where == 2) first += escape + "\n";
        first += "[White \"A\"]\n[Black \"B\"]\n\n";
        first += where == 3 ? "1. e4\n" + escape + "\ne5 *" :
            where == 4 ? "1. e4 e5 (1... c5\n" + escape + "\n) *" : "1. e4 e5 *";
        first += "\n\n";
        if (where == 1) first += escape + "\n\n";
        std::string second = "[Event \"Second\"]\n[White \"C\"]\n[Black \"D\"]\n\n1. d4 d5 *\n";
        if (crlf) {
            std::string* parts[] = { &prefix, &first, &second };
            for (std::size_t part = 0; part < 3; ++part) {
                std::string converted;
                for (std::size_t j = 0; j < parts[part]->size(); ++j) {
                    if ((*parts[part])[j] == '\n') converted += '\r';
                    converted += (*parts[part])[j];
                }
                parts[part]->swap(converted);
            }
        }
        gambitree::PgnDocument parsed;
        expect_true(gambitree::PgnParser::parse(prefix + first + second, parsed) && parsed.games.size() == 2 &&
                    parsed.games[0]->root->children.size() == 1 &&
                    !parsed.games[0]->root->children[0]->children.empty() &&
                    parsed.games[0]->root->children[0]->children[0]->san == "e5",
                    "strict parsing also ignores escape lines before and between headers without inventing games");
        expect_true(write_file_bytes(path, prefix + first + second), "write an isolated escape-line deletion source");
        PgnLibrary library;
        expect_true(library.load(paths, errors), "escape-line source opens");
        while (!library.indexing_complete()) library.step_indexing(1, errors);
        const PgnIndex& index = library.current_file()->index;
        expect_true(library.game_count() == 2 && index.boundaries_reliable(0) && index.boundaries_reliable(1),
                    "LF/CRLF escape lines before, between and inside games introduce no fictitious or uncertain ranges");
        if (library.game_count() != 2) continue;
        expect_true(index.entries()[0].begin == prefix.size() &&
                    index.entries()[0].end == prefix.size() + first.size() &&
                    index.entries()[1].begin == index.entries()[0].end && index.entries()[1].end == index.file_size(),
                    "escape lines preserve exact source boundaries including a BOM-prefixed preamble");
        for (std::size_t g = 0; g < 2; ++g) {
            expect_true(library.select_game(g) && library.materialize_current(error) &&
                        library.current_game()->parse_status == gambitree::PGN_COMPLETE &&
                        library.current_game_deletion_eligible(reason),
                        "escape lines neither change parsed games nor block their deletion");
            MoveNavigator navigation(*library.current_game());
            navigation.to_end();
            std::ostringstream context;
            context << "escape contents preserve the mainline (CRLF=" << crlf << ", location=" << where
                    << ", game=" << g + 1 << ", final=" << navigation.current()->san << ")";
            expect_true(navigation.current()->san == (g ? "d5" : "e5"), context.str().c_str());
        }
        library.select_game(0); library.materialize_current(error);
        expect_true(library.delete_current_game(committed, removed, sync_warning, error) && committed && !removed &&
                    library.game_count() == 1 && library.game_index() == 0 && read_file_bytes(path) == prefix + second &&
                    library.materialize_current(error) && *library.current_game()->tag_value("Event") == "Second",
                    "deleting an escape-delimited game preserves and selects the following game byte for byte");
        expect_true(library.current_file()->index.whole_file_is_single_game() &&
                    library.delete_current_game(committed, removed, sync_warning, error) && committed && removed &&
                    access(path.c_str(), F_OK) != 0,
                    "a sole game with a harmless escape preamble may safely remove its whole file");
    }
    // Missing Event tags and a missing result do not hide a following header boundary.
    const std::string first = "[White \"A\"]\n[Black \"B\"]\n\n1. Rh8\n\n";
    const std::string second = "[White \"C\"]\n[Black \"D\"]\n\n1. d4 d5 *";
    write_file_bytes(path, first + second);
    PgnLibrary missing;
    missing.load(paths, errors);
    while (!missing.indexing_complete()) missing.step_indexing(1, errors);
    expect_true(missing.game_count() == 2 && missing.current_file()->index.boundaries_reliable(0) &&
                missing.current_file()->index.boundaries_reliable(1) && missing.materialize_current(error) &&
                missing.current_game()->parse_status == gambitree::PGN_PARTIAL &&
                missing.delete_current_game(committed, removed, sync_warning, error) && committed && !removed &&
                read_file_bytes(path) == second,
                "repeated names reset at proven boundaries even with missing Event/result and an illegal move");
    unlink(path.c_str());
}

void test_pgn_index_numbering_restarts()
{
    const std::string path = test_scratch_directory + "/numbering-restart.pgn";
    const std::string headers = "[Event \"First\"]\n[White \"Player A\"]\n[Black \"Player B\"]\n\n";
    const char* after_two = "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2";
    const char* after_three = "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2";
    const char* after_four = "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3";
    const char* after_five = "r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3";
    const char* white_fen = "[SetUp \"1\"]\n[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 31\"]\n";
    const char* black_fen = "[SetUp \"1\"]\n[FEN \"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 31\"]\n";
    const std::string oversized = std::string(96, '9') + ". e4 e5 *";
    struct Case { const char* setup; const char* moves; bool uncertain; bool partial; std::size_t plies; const char* fen; };
    const Case cases[] = {
        { "", "1. e4 e5\n\n1. e4 e5 *\n", true, true, 2, after_two },
        { "", "1.e4 e5\n\n1.e4 e5 *\n", true, true, 2, after_two },
        { "", "1. e4 e5 2. Nf3 Nc6\n\n1. Bb5 *", true, false, 5, after_five },
        { "", "1. e4 1... e5 1... Nf3 *", true, false, 3, after_three },
        { "", "7. e4 e5 6. Nf3 *", true, false, 3, after_three },
        { "", oversized.c_str(), true, false, 2, after_two },
        { "", "1. e4 1... e5 2. Nf3 2... Nc6 *", false, false, 4, after_four },
        { "", "1. $1 ! 1. e4 1... e5 *", false, false, 2, after_two },
        { "", "e4 e5 Nf3 Nc6 *", false, false, 4, after_four },
        { "", "e4 1... e5 Nf3 Nc6 3. Bb5 *", false, false, 5, after_five },
        { "", "41. e4 e5 99. Nf3 *", false, false, 3, after_three },
        { "", "1. e4 e5 2. Nf3 (1. Nf3 Nc6 (1... Nf6)) Nc6 3. Bb5 *", false, false, 5, after_five },
        { "", "1. e4 $1 {999. 1. ignored}\n% 1. ignored\n1... e5 2. Nf3 *", false, false, 3, after_three },
        { white_fen, "31. e4 31... e5 32. Nf3 *", false, false, 3,
          "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 32" },
        { black_fen, "31... e5 32. Nf3 32... Nc6 *", false, false, 3,
          "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 33" },
        { white_fen, "1. e4 e5 2. Nf3 *", false, false, 3,
          "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 32" }
    };
    std::vector<std::string> paths(1, path), errors;
    std::string error, reason;
    bool committed, removed, sync_warning;
    for (int crlf = 0; crlf < 2; ++crlf) for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        std::string source = headers + cases[i].setup + cases[i].moves;
        if (crlf) for (std::size_t j = 0; j < source.size(); ++j)
            if (source[j] == '\n') { source.insert(j, 1, '\r'); ++j; }
        expect_true(write_file_bytes(path, source), "write an isolated move-number deletion source");
        PgnLibrary library;
        expect_true(library.load(paths, errors), "numbered source opens");
        while (!library.indexing_complete()) library.step_indexing(1, errors);
        const PgnIndex& index = library.current_file()->index;
        expect_true(library.game_count() == 1 && index.entries()[0].begin == 0 &&
                    index.entries()[0].end == source.size() &&
                    index.boundaries_reliable(0) == !cases[i].uncertain &&
                    index.whole_file_is_single_game() == !cases[i].uncertain,
                    "numbering hints affect boundary confidence without inventing games or changing ranges");
        const bool loaded = library.materialize_current(error);
        expect_true(loaded, "numbering changes do not prevent the viewer from loading its existing game tree");
        if (!loaded) continue;
        PgnGame& game = *library.current_game();
        MoveNavigator navigation(game);
        expect_true(game.parse_status == (cases[i].partial ? gambitree::PGN_PARTIAL : gambitree::PGN_COMPLETE),
                    "the viewer retains its tolerance for incorrect numbers and its existing partial-load status");
        if (cases[i].partial)
            expect_true(game.available_plies == 2 && navigation.position().to_fen() == after_two &&
                        game.parse_error.message.find("\"e4\"") != std::string::npos &&
                        game.parse_error.line == 7 && game.parse_error.column == (i == 1 ? 3 : 4),
                        "the original damaged game still selects the exact two-ply prefix and reports the same error");
        navigation.to_begin();
        std::size_t plies = 0;
        while (navigation.forward()) ++plies;
        expect_true(plies == cases[i].plies && navigation.position().to_fen() == cases[i].fen,
                    "FEN starts, RAVs, omitted and inaccurate numbers preserve exact playable plies and final FEN");
        expect_true(library.current_game_deletion_eligible(reason) == !cases[i].uncertain,
                    "only uncertain numbering blocks deletion, not ordinary black numbers or a non-one start");
        if (cases[i].uncertain) {
            expect_true(!library.delete_current_game(committed, removed, sync_warning, error) &&
                        !committed && !removed && read_file_bytes(path) == source &&
                        error.find("cannot safely tell where this game starts and ends") != std::string::npos,
                        "a numbering restart never authorizes replacement or whole-file deletion");
        } else {
            expect_true(library.delete_current_game(committed, removed, sync_warning, error) &&
                        committed && removed && access(path.c_str(), F_OK) != 0,
                        "unambiguous numbering still permits safe removal of a sole game");
        }
    }
    const std::string first = "[Event \"Before\"]\n\n99. d4 d5 *\n\n";
    const std::string last = "[Event \"After\"]\n\n1. c4 e5 *\n";
    const std::string source = first + headers + cases[0].moves + "\n" + last;
    write_file_bytes(path, source);
    PgnLibrary library;
    library.load(paths, errors);
    while (!library.indexing_complete()) library.step_indexing(1, errors);
    const PgnIndex& index = library.current_file()->index;
    expect_true(library.game_count() == 3 && index.boundaries_reliable(0) &&
                !index.boundaries_reliable(1) && index.boundaries_reliable(2),
                "number tracking resets at proven boundaries and never makes adjacent valid games untrusted");
    expect_true(library.select_game(1) && library.materialize_current(error) &&
                !library.delete_current_game(committed, removed, sync_warning, error) &&
                !committed && read_file_bytes(path) == source,
                "an uncertain middle range cannot be removed from between two independently bounded games");
    unlink(path.c_str());
}

void test_pgn_delete_repeated_separators()
{
    int newline_mode;
    for (newline_mode = 0; newline_mode < 3; ++newline_mode) {
        std::ostringstream suffix;
        std::string base;
        std::string path;
        std::string newline = newline_mode == 1 ? "\r\n" : "\n";
        std::string separators[4];
        std::string games[5];
        std::string expected;
        std::string source;
        std::vector<std::string> paths;
        std::vector<std::string> errors;
        PgnLibrary library;
        bool committed = false;
        bool removed = false;
        bool sync_warning = false;
        std::string error;
        int index;
        suffix << getpid() << "-" << newline_mode;
        base = std::string(test_scratch_directory + "/gambitree-delete-separators-") +
            suffix.str();
        mkdir(base.c_str(), 0700);
        path = base + "/five games.pgn";
        for (index = 0; index < 5; ++index) {
            std::ostringstream event;
            event << "[Event \"Game " << index + 1 << "\"]";
            games[index] = event.str() + newline + "[White \"Player " +
                static_cast<char>('A' + index) + "\"]" + newline +
                "[Result \"*\"]" + newline + newline +
                "1. e4 {comment retained} (1. d4 $5) *";
        }
        for (index = 0; index < 4; ++index)
            separators[index] = newline + newline;
        if (newline_mode == 2)
            separators[1] = newline + " \t" + newline + newline + newline;
        for (index = 0; index < 5; ++index) {
            if (index != 0)
                source += separators[index - 1];
            source += games[index];
        }
        expect_true(write_file_bytes(path, source),
                    "five-game separator regression fixture is written");
        paths.push_back(path);
        expect_true(library.load(paths, errors),
                    "separator regression fixture opens in the PGN library");
        while (!library.indexing_complete())
            library.step_indexing(100, errors);
        library.next_game();
        library.next_game();
        expect_true(library.game_index() == 2 &&
                    library.materialize_current(error),
                    "separator test selects the middle game before deletion");

        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && !removed &&
                    library.game_count() == 4,
                    "middle deletion reindexes four remaining games");
        expected = games[0] + separators[0] + games[1] + separators[1] +
                   games[3] + separators[3] + games[4];
        expect_true(read_file_bytes(path) == expected,
                    "middle deletion preserves party bytes and does not duplicate its trailing separator");
        expect_true(library.materialize_current(error),
                    "reindexed next game materializes before another deletion");

        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && !removed &&
                    library.game_count() == 3,
                    "second immediate deletion reindexes three games");
        expected = games[0] + separators[0] + games[1] + separators[1] +
                   games[4];
        expect_true(read_file_bytes(path) == expected,
                    "repeated deletion does not grow separator whitespace");
        expect_true(library.materialize_current(error),
                    "second reindexed selection materializes for repeated deletion");

        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && !removed &&
                    library.game_count() == 2,
                    "last game deletion leaves the preceding game indexed");
        expected = games[0] + separators[0] + games[1] + newline;
        expect_true(read_file_bytes(path) == expected,
                    "last game deletion collapses its leading separator to one line ending");
        expect_true(library.materialize_current(error),
                    "previous game materializes after deleting the last ordered game");

        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && !removed &&
                    library.game_count() == 1,
                    "one further deletion leaves exactly one game");
        expected = games[0] + newline;
        expect_true(read_file_bytes(path) == expected,
                    "repeated last-game deletion leaves no growing EOF whitespace");
        expect_true(library.materialize_current(error),
                    "sole game materializes before final file deletion");

        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && removed &&
                    library.file_count() == 0 && access(path.c_str(), F_OK) != 0,
                    "deleting the sole remaining game removes the PGN file");
        rmdir(base.c_str());
    }
    {
        std::ostringstream suffix;
        std::string path;
        std::string first = "[Event \"First\"]\n[Result \"*\"]\n\n1. e4 *";
        std::string second = "[Event \"Second\"]\n[Result \"*\"]\n\n1. d4 *";
        std::string text = "\r\n \t\n" + first + "\n\n" + second;
        std::vector<std::string> paths;
        std::vector<std::string> errors;
        PgnLibrary library;
        bool committed = false;
        bool removed = false;
        bool sync_warning = false;
        std::string error;
        suffix << getpid();
        path = std::string(test_scratch_directory + "/gambitree-delete-leading-space-") +
               suffix.str() + ".pgn";
        write_file_bytes(path, text);
        paths.push_back(path);
        library.load(paths, errors);
        while (!library.indexing_complete())
            library.step_indexing(100, errors);
        library.materialize_current(error);
        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && !removed &&
                    read_file_bytes(path) == second,
                    "deleting the first game removes only leading separator whitespace");
        library.materialize_current(error);
        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && removed,
                    "leading-space fixture still permits final game removal");
        unlink(path.c_str());
    }
    {
        std::ostringstream suffix;
        std::string path;
        std::string first = "[Event \"Previous\"]\n[Result \"*\"]\n\n1. e4 *";
        std::string comment = "\n{boundary comment stays}\n";
        std::string last = "[Event \"Last\"]\n[Result \"*\"]\n\n1. d4 *";
        std::vector<std::string> paths;
        std::vector<std::string> errors;
        PgnLibrary library;
        bool committed = false;
        bool removed = false;
        bool sync_warning = false;
        std::string error;
        suffix << getpid();
        path = std::string(test_scratch_directory + "/gambitree-delete-comment-") +
               suffix.str() + ".pgn";
        write_file_bytes(path, first + comment + "\n" + last);
        paths.push_back(path);
        library.load(paths, errors);
        while (!library.indexing_complete())
            library.step_indexing(100, errors);
        library.next_game();
        library.materialize_current(error);
        expect_true(library.delete_current_game(committed, removed,
                        sync_warning, error) && committed && !removed &&
                    read_file_bytes(path) == first + comment,
                    "last-game separator cleanup preserves an inter-game comment");
        unlink(path.c_str());
    }
}

void test_pgn_index()
{
    const std::string path_storage = test_scratch_directory + "/gambitree-index.pgn";
    const char* path = path_storage.c_str();
    std::ofstream output(path);
    PgnIndex index;
    std::string error;
    int game;
    output.close();
    {
        const std::string without_bom =
            "[Event \"First\"]\n[Result \"*\"]\n\n1. e4 (1. d4) *\n\n"
            "[Event \"Second\"]\n[Result \"*\"]\n\n1. d4 *";
        const std::string with_bom = std::string("\xef\xbb\xbf") + without_bom;
        gambitree::PgnDocument parsed;
        expect_true(gambitree::PgnParser::parse(
                        std::string("\xef\xbb\xbf") +
                        without_bom.substr(0, without_bom.find("[Event \"Second\"]")),
                        parsed) &&
                    parsed.games.size() == 1 &&
                    parsed.games[0]->tag_value("Event") != 0 &&
                    *parsed.games[0]->tag_value("Event") == "First",
                    "PGN parser skips only the initial UTF-8 BOM");
        expect_true(!gambitree::PgnParser::parse(
                        std::string("\xef\xbb\xbf") +
                        without_bom.substr(0, without_bom.find("[Event \"Second\"]")),
                        parsed, false),
                    "parser does not treat a later-game BOM as file metadata");
        expect_true(write_file_bytes(path, with_bom),
                    "BOM-prefixed index fixture is written");
        expect_true(index.open(path, error), "BOM-prefixed PGN index opens");
        while (!index.complete() && !index.failed())
            expect_true(index.step(1, error), "BOM index progresses incrementally");
        expect_true(index.entries().size() == 2 &&
                    index.entries()[0].begin == 0 &&
                    index.entries()[0].end == index.entries()[1].begin &&
                    index.entries()[1].end == index.file_size(),
                    "BOM is ignored for first-tag recognition without shifting absolute offsets");
        if (index.entries().size() == 2) {
            std::string first_game = with_bom.substr(
                static_cast<std::size_t>(index.entries()[0].begin),
                static_cast<std::size_t>(index.entries()[0].end -
                                         index.entries()[0].begin));
            gambitree::PgnDocument first_document;
            expect_true(gambitree::PgnParser::parse(first_game,
                                                    first_document) &&
                        first_document.games.size() == 1 &&
                        first_document.games[0]->tag_value("Event") != 0 &&
                        *first_document.games[0]->tag_value("Event") == "First",
                        "lazy game range parses identically with a leading BOM");
        }
        std::remove(path);
    }
    output.open(path);
    output << "[Event \"First\"]\n[White \"A\"]\n\n1. e4 { [Event \"not a tag\"] } e5 *\n\n"
           << "[Event \"Second\"]\n[SetUp \"1\"]\n[FEN \"8/8/8/8/8/8/4K3/7k w - - 0 1\"]\n\n*\n";
    output.close();
    expect_true(index.open(path, error), "PGN index opens a file without parsing moves");
    while (!index.complete()) expect_true(index.step(1, error), "incremental index step succeeds");
    expect_true(index.entries().size() == 2 && index.entries()[0].begin < index.entries()[0].end &&
                index.entries()[0].end == index.entries()[1].begin && index.entries()[1].end == index.file_size(),
                "index stores exact adjacent byte ranges for tagged games");
    expect_true(index.entry_storage_bytes() >= index.entries().size() * sizeof(gambitree::PgnIndexEntry),
                "index retains compact offsets rather than per-game tag strings");
    expect_true(index.lines_scanned() > 0 && index.chunks() > 0,
                "index reports physical-line and chunk diagnostics without materializing games");
    std::remove(path);

    output.open(path);
    for (game = 0; game < 1000; ++game)
        output << "[Event \"Bulk\"]\n[Round \"" << game << "\"]\n\n1. e4 e5 *\n\n";
    output.close();
    expect_true(index.open(path, error), "bulk index reopens file");
    while (!index.complete()) expect_true(index.step_for_milliseconds(1, error), "time-budgeted index slice succeeds");
    expect_true(index.entries().size() == 1000, "thousands of games are indexed without MoveNode materialization");
    std::remove(path);

    output.open(path);
    output << "1. e4 e5 1-0\n\n1. d4 d5 0-1\n";
    output.close();
    expect_true(index.open(path, error), "tagless PGN index opens");
    while (!index.complete()) index.step(1, error);
    expect_true(index.entries().size() == 2, "top-level result tokens produce two tagless game ranges");
    expect_true(index.complete_entry_count() == 2, "both tagless game ranges are complete");
    if (index.entries().size() == 2)
        expect_true(index.entries()[0].end == index.entries()[1].begin,
                    "tagless game ranges are adjacent");
    std::remove(path);

    /* A sparse conventional database exercises the provisional byte scanner without a huge fixture. */
    output.open(path, std::ios::out | std::ios::binary);
    output << "[Event \"not-a-tag-block\"]\r\nthis is not a tag pair\r\n\r\n"
           << "[Event \"First\"]\r\n[Site \"A\"]\r\n\r\n1. e4 *\r\n\r\n"
           << "[Event \"Second\"]\r\n[Site \"B\"]\r\n\r\n1. d4 *\r\n";
    output.seekp(static_cast<std::streamoff>(PgnIndex::provisional_threshold() + 17));
    output << '\n';
    output.close();
    expect_true(index.open(path, error) && index.provisional(),
                "large conventional CRLF database selects the provisional block scanner");
    while (!index.complete()) expect_true(index.step_for_milliseconds(5, error), "provisional scanner progresses incrementally");
    expect_true(index.entries().size() == 2 && index.complete_entry_count() == 2 &&
                index.entries()[0].end == index.entries()[1].begin &&
                index.entries()[1].end == index.file_size(),
                "provisional scanner finds adjacent ranges through block boundaries and final EOF");
    expect_true(index.scanner_buffer_bytes() <= 2U * 1024U * 1024U &&
                index.entry_storage_bytes() <= index.entries().capacity() * sizeof(gambitree::PgnIndexEntry),
                "provisional scanner keeps a fixed buffer and compact offset-only entries");
    expect_true(static_cast<PgnOffset>(static_cast<PgnOffset>(5) * 1024 * 1024 * 1024) >
                static_cast<PgnOffset>(std::numeric_limits<unsigned int>::max()), "PGN offsets represent positions above 4 GiB");
    {
        PgnLibrary fast_library;
        std::vector<std::string> fast_paths;
        std::vector<std::string> fast_errors;
        fast_paths.push_back(path);
        expect_true(fast_library.load(fast_paths, fast_errors), "provisional database opens through the lazy library");
        while (!fast_library.indexing_complete()) fast_library.step_indexing_for_milliseconds(5, fast_errors);
        expect_true(fast_library.game_count() == 2 && fast_library.materialize_current(error),
                    "provisional range lazy-materializes only its selected game");
    }
    std::remove(path);

    /* The large-file scanner must recognize a BOM only at absolute offset zero. */
    output.open(path, std::ios::out | std::ios::binary);
    output << "\xef\xbb\xbf[Event \"First\"]\r\n[Site \"A\"]\r\n\r\n1. e4 *\r\n\r\n"
           << "[Event \"Second\"]\r\n[Site \"B\"]\r\n\r\n1. d4 *\r\n";
    output.seekp(static_cast<std::streamoff>(PgnIndex::provisional_threshold() + 17));
    output << '\n';
    output.close();
    expect_true(index.open(path, error) && index.provisional(),
                "BOM-prefixed large PGN selects provisional indexing");
    while (!index.complete() && !index.failed())
        expect_true(index.step_for_milliseconds(5, error),
                    "BOM provisional scanner progresses");
    expect_true(index.entries().size() == 2 &&
                index.entries()[0].begin == 0 &&
                index.entries()[0].end == index.entries()[1].begin,
                "provisional scanner keeps BOM in absolute first-game range");
    std::remove(path);

    /* Escape lines preserve both the fast-path probe and pending tag blocks. */
    output.open(path, std::ios::out | std::ios::binary);
    const std::string fast_first = "\xef\xbb\xbf% export preamble\r\n[Event \"First\"]\r\n"
        "% ignored { [Event \"fake\"]\r\n[Site \"A\"]\r\n\r\n1. e4 *\r\n\r\n";
    const std::string fast_second = "% between games\r\n[Event \"Second\"]\r\n[Site \"B\"]\r\n\r\n1. d4 *\r\n\r\n";
    const std::string fast_third = "[Event \"Third\"]\r\n% between tags\r\n[Site \"C\"]\r\n\r\n1. c4 *\r\n";
    output << fast_first << fast_second << fast_third;
    output.seekp(static_cast<std::streamoff>(PgnIndex::provisional_threshold() + 17));
    output << '\n';
    output.close();
    expect_true(index.open(path, error) && index.provisional(),
                "escape lines do not prevent selection of the fast indexer");
    while (!index.complete() && !index.failed())
        expect_true(index.step_for_milliseconds(5, error), "fast escape-line indexing progresses");
    expect_true(index.entries().size() == 3 &&
                index.entries()[0].begin == fast_first.find("[Event \"First\"]") &&
                index.entries()[1].begin == fast_first.size() + fast_second.find("[Event \"Second\"]") &&
                index.entries()[2].begin == fast_first.size() + fast_second.size() &&
                index.entries()[0].end == index.entries()[1].begin &&
                index.entries()[1].end == index.entries()[2].begin && index.entries()[2].end == index.file_size(),
                "fast indexing ignores escape lines while preserving all three exact ranges");
    expect_true(index.scanner_buffer_bytes() <= 2U * 1024U * 1024U &&
                index.entry_storage_bytes() <= index.entries().capacity() * sizeof(gambitree::PgnIndexEntry),
                "escape handling preserves the fast scanner's fixed buffer and compact entries");
    std::remove(path);

    output.open(path);
    output << "[Event \"Only one structural tag\"]\n\n1. e4 { [Event \"inside comment\"] } *\n";
    output.close();
    expect_true(index.open(path, error) && !index.provisional(),
                "non-conventional input rejects the provisional path and retains the general indexer");
    while (!index.complete()) expect_true(index.step(1, error), "general fallback indexes rejected fast-path input");
    expect_true(index.entries().size() == 1, "general fallback preserves comment-aware indexing");
    std::remove(path);

    output.open(path, std::ios::out | std::ios::binary);
    output << std::string(1024U * 1024U, 'x') << '\n';
    output.close();
    expect_true(index.open(path, error), "bounded general index opens a pathological physical line");
    expect_true(!index.step(1, error) && error.find("physical line") != std::string::npos,
                "bounded general index rejects an oversized physical line without unbounded allocation");
    std::remove(path);
}

void test_notation_semantics()
{
    PgnDocument document;
    NotationLayout layout;
    std::size_t index;
    int e4 = -1, e5 = -1, c5 = -1, nf3 = -1;
    expect_true(gambitree::PgnParser::parse("1. e4 e5 2. Nf3 Nc6 *", document), "plain main-line numbering PGN parses");
    if (!document.games.empty()) {
        layout.build(*document.games[0], 500, 8, 18);
        expect_true(layout.items().size() >= 6 && layout.items()[0].text == "1." && layout.items()[1].text == "e4" &&
                    layout.items()[2].text == "e5" && layout.items()[3].text == "2." && layout.items()[4].text == "Nf3" &&
                    layout.items()[5].text == "Nc6", "normal main line uses conventional white/black numbering");
    }
    document.clear();
    expect_true(gambitree::PgnParser::parse("1. e4 (1. d4 d5) {between} e5 $1 (1... c5) 2. Nf3 Nc6 (2... d6) 3. Bb5 *", document),
                "numbering regression PGN parses");
    if (document.games.empty()) return;
    layout.build(*document.games[0], 500, 8, 18);
    for (index = 0; index < layout.items().size(); ++index) {
        const gambitree::NotationItem& item = layout.items()[index];
        if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "e4") e4 = static_cast<int>(index);
        if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "e5") e5 = static_cast<int>(index);
        if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "Nf3") nf3 = static_cast<int>(index);
        if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "d6") c5 = static_cast<int>(index);
    }
    expect_true(e4 >= 0 && e5 > e4 && layout.items()[static_cast<std::size_t>(e5 - 1)].text != "1...",
                "main-line black reply follows comments/NAGs without a redundant ellipsis number");
    expect_true(nf3 >= 0 && c5 > nf3 && layout.items()[static_cast<std::size_t>(c5 - 1)].text == "2...",
                "black-start variation has an explicit ellipsis number");

    document.clear();
    expect_true(gambitree::PgnParser::parse("[SetUp \"1\"]\n[FEN \"6k1/7p/5N2/4n3/8/3B4/5P2/4K3 w - - 0 13\"]\n\n13. Nxh7 ({The critical move seems to be} 13. Bxh7+ Kh8 14. f4 $5 {which certainly deserves further investigation.}) Ng4 *", document),
                "RAV leading-comment regression fragment parses");
    layout.build(*document.games[0], 800, 8, 18);
    {
        int main_move = -1, open = -1, leading = -1, bishop = -1, king = -1, pawn = -1, nag = -1, trailing = -1, close = -1, reply = -1;
        for (index = 0; index < layout.items().size(); ++index) {
            const gambitree::NotationItem& item = layout.items()[index];
            if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "Nxh7") main_move = static_cast<int>(index);
            if (item.text == "(") open = static_cast<int>(index);
            if (item.text.find("critical") != std::string::npos) leading = static_cast<int>(index);
            if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "Bxh7+") bishop = static_cast<int>(index);
            if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "Kh8") king = static_cast<int>(index);
            if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "f4") pawn = static_cast<int>(index);
            if (item.kind == gambitree::NOTATION_NAG && item.text == "!?") nag = static_cast<int>(index);
            if (item.text.find("certainly") != std::string::npos) trailing = static_cast<int>(index);
            if (item.text == ")") close = static_cast<int>(index);
            if (item.kind == gambitree::NOTATION_MOVE && item.node && item.node->san == "Ng4") reply = static_cast<int>(index);
        }
        expect_true(main_move < open && open < leading && leading < bishop && bishop < king && king < pawn && pawn < nag && nag < trailing && trailing < close && close < reply,
                    "RAV leading and trailing comments retain their structural rendering order");
        expect_true(layout.items()[static_cast<std::size_t>(reply)].y > layout.items()[static_cast<std::size_t>(close)].y,
                    "main-line continuation starts on a new visual row after a closed RAV");
        expect_true(layout.items()[static_cast<std::size_t>(leading)].text.find('{') == std::string::npos &&
                    layout.items()[static_cast<std::size_t>(leading)].text.find("critical") != std::string::npos,
                    "structural leading comments render without synthetic PGN braces");
    }

    document.clear();
    expect_true(gambitree::PgnParser::parse("[SetUp \"1\"]\n[FEN \"8/8/8/8/8/8/4K3/7k b - - 0 13\"]\n\n13... Kg1 *", document), "black-to-move FEN parses");
    layout.build(*document.games[0], 200, 8, 18);
    expect_true(!layout.items().empty() && layout.items()[0].text == "13...", "black-to-move FEN starts with an explicit ellipsis number");

    document.clear();
    expect_true(gambitree::PgnParser::parse("1. e4 {literal { brace} e5 *", document),
                "comment containing a literal opening brace parses");
    layout.build(*document.games[0], 200, 8, 18);
    {
        bool literal_brace = false;
        for (index = 0; index < layout.items().size(); ++index)
            if (layout.items()[index].kind == gambitree::NOTATION_COMMENT &&
                layout.items()[index].text == "{") literal_brace = true;
        expect_true(literal_brace, "presentation removes only structural delimiters and preserves literal comment braces");
    }
}

void test_pgn_size_limit()
{
    std::size_t maximum = PgnLibrary::maximum_file_size();
    const std::string huge_storage = test_scratch_directory + "/gambitree-library-huge.pgn";
    const char* huge = huge_storage.c_str();
    const std::string valid_storage = test_scratch_directory + "/gambitree-library-small.pgn";
    const char* valid = valid_storage.c_str();
    std::ofstream output(huge, std::ios::out | std::ios::binary);
    std::vector<std::string> paths;
    std::vector<std::string> errors;
    std::string error;
    PgnLibrary library;
    {
        std::string harmless_line(1023, ' ');
        std::size_t written = 0;
        output << "[Event \"Huge\"]\n[Site \"Test\"]\n\n";
        written = 29;
        while (written <= maximum) {
            output << harmless_line << '\n';
            written += harmless_line.size() + 1;
        }
    }
    output.close();
    output.open(valid); output << "1. e4 *"; output.close();
    expect_true(PgnLibrary::acceptable_file_size(maximum - 1) && PgnLibrary::acceptable_file_size(maximum) &&
                !PgnLibrary::acceptable_file_size(maximum + 1), "per-game materialization limit accepts below and at limit, rejects above it");
    paths.push_back(huge); paths.push_back(valid);
    expect_true(library.load(paths, errors), "large files may be streaming-indexed without whole-file allocation");
    while (!library.indexing_complete()) library.step_indexing(100, errors);
    expect_true(library.game_count() == 1 && !library.materialize_current(error),
                "an indexed game range above 64 MiB is rejected only at materialization");
    expect_true(library.next_file() && library.materialize_current(error),
                "a valid sibling file remains materializable after oversized range rejection");
    std::remove(huge); std::remove(valid);
}

void test_uci_protocol()
{
    UciInfo info;
    std::vector<std::string> pv;
    Position position = Position::standard();
    UciAnalysis analysis;
    expect_true(UciEngine::protocol_state_after(UCI_STARTING, "uciok") == UCI_WAITING_READY &&
                UciEngine::protocol_state_after(UCI_WAITING_READY, "readyok") == UCI_READY &&
                UciEngine::protocol_state_after(UCI_ANALYZING, "bestmove e2e4") == UCI_READY,
                "UCI handshake and bestmove state transitions are deterministic");
    expect_true(!UciEngine::accepts_info(UCI_STOPPING, 1, 2) &&
                UciEngine::protocol_state_after(UCI_ANALYZING, "bestmove e2e4", 1, 2) == UCI_ANALYZING &&
                UciEngine::protocol_state_after(UCI_STOPPING, "bestmove e2e4", 1, 2) == UCI_READY,
                "old-generation info is ignored and its stop bestmove only releases the queued new search");
    expect_true(UciEngine::parse_info("info depth 18 seldepth 25 score cp 35 pv g1f3 d7d5 c2c4", info) &&
                info.depth == 18 && info.seldepth == 25 && info.score_type == UCI_SCORE_CENTIPAWNS &&
                info.depth_valid && info.seldepth_valid && info.score_valid && info.pv_valid &&
                info.score == 35 && info.pv.size() == 3,
                "UCI info parser retains depth, seldepth, centipawn score, and PV");
    expect_true(UciEngine::pv_to_san(position, info.pv) == "Nf3 d5 c4", "UCI PV converts legal moves to SAN");
    {
        Position chess960;
        std::string fen_error;
        std::vector<std::string> castle_pv;
        expect_true(Position::from_fen("4k3/8/8/8/8/8/8/2K4R w H - 0 1", chess960,
                                       &fen_error, gambitree::CHESS960),
                    "Chess960 UCI PV start parses");
        castle_pv.push_back("c1h1");
        expect_true(UciEngine::pv_to_san(chess960, castle_pv) == "O-O",
                    "Stockfish Chess960 king-to-rook PV converts to castling SAN");
    }
    info = UciInfo();
    expect_true(UciEngine::parse_info("info string ignored nodes 99 nps 100 depth 34 seldepth 50 score cp 16 pv g1f3 d7d5 c2c4 e7e6 b1c3 g8f6", info) &&
                info.pv_valid && info.pv.size() == 6 && info.pv[5] == "g8f6",
                "unknown UCI fields before pv do not truncate a long principal variation");
    info = UciInfo();
    expect_true(UciEngine::parse_info("info string ignored depth 22 score mate -3 lowerbound upperbound pv e2e4", info) &&
                info.depth == 22 && info.score_type == UCI_SCORE_MATE && info.score == -3 &&
                info.lowerbound && info.upperbound, "UCI parser ignores unknown fields and retains mate/bounds");
    expect_true(UciEngine::parse_info("info depth ??? score cp", info), "incomplete UCI info is harmless");
    {
        UciInfo accumulated;
        UciInfo partial;
        UciEngine::parse_info("info depth 12 score cp 34 pv g1f3 d7d5", accumulated);
        UciEngine::parse_info("info depth 13 nodes 1000", partial);
        UciEngine::merge_info(accumulated, partial);
        expect_true(accumulated.depth == 13 && accumulated.score_valid && accumulated.score == 34 &&
                    accumulated.pv_valid && accumulated.pv.size() == 2,
                    "partial UCI info merges without clearing an earlier score or PV");
        partial = UciInfo();
        UciEngine::parse_info("info score cp 52", partial);
        UciEngine::merge_info(accumulated, partial);
        expect_true(accumulated.score == 52 && accumulated.pv_valid && accumulated.pv.size() == 2,
                    "new UCI score without PV preserves the current-generation PV");
        partial = UciInfo();
        UciEngine::parse_info("info depth 14 score cp 17 pv g1f3", partial);
        UciEngine::merge_info(accumulated, partial);
        expect_true(accumulated.depth == 14 && accumulated.pv.size() == 1 && accumulated.pv[0] == "g1f3",
                    "a newer complete UCI PV may correctly replace an older longer PV");
        {
            UciInfo exact;
            UciInfo bound;
            UciEngine::parse_info("info depth 35 seldepth 65 multipv 1 score cp -65 pv b8d7 c1d2", exact);
            UciEngine::parse_info("info depth 36 seldepth 59 multipv 1 score cp -72 lowerbound pv b8d7", bound);
            UciEngine::merge_info(exact, bound);
            expect_true(exact.depth == 36 && exact.pv.size() == 2 && exact.pv[0] == "b8d7" && exact.pv[1] == "c1d2" &&
                        exact.score == -65 && !exact.lowerbound,
                        "bound-only Stockfish PV progress cannot replace an earlier exact principal variation");
            {
                UciInfo san_exact;
                UciInfo san_bound;
                UciEngine::parse_info("info depth 35 score cp 16 pv g1f3 d7d5", san_exact);
                UciEngine::parse_info("info depth 36 score cp 17 upperbound pv g1f3", san_bound);
                UciEngine::merge_info(san_exact, san_bound);
                expect_true(UciEngine::pv_to_san(position, san_exact.pv) == "Nf3 d5",
                            "SAN conversion uses the retained exact PV after q.pgn-style bound progress");
            }
        }
    }
    pv.push_back("e2e4"); pv.push_back("notamove");
    expect_true(UciEngine::pv_to_san(position, pv) == "e4 notamove", "invalid PV tail falls back to raw UCI text");
    analysis.valid = true; analysis.generation = 7; analysis.side_to_move = WHITE;
    analysis.info.score_type = UCI_SCORE_CENTIPAWNS; analysis.info.score = 35;
    expect_true(UciEngine::analysis_is_current(analysis, 7) && !UciEngine::analysis_is_current(analysis, 8) &&
                UciEngine::format_score(analysis) == "+0.35", "generation gate rejects stale evaluation output");
    analysis.side_to_move = BLACK; analysis.info.score = 120;
    expect_true(UciEngine::format_score(analysis) == "-1.20", "UCI score is converted to White perspective for Black to move");
    analysis.info.score_type = UCI_SCORE_MATE; analysis.info.score = -2;
    expect_true(UciEngine::format_score(analysis) == "M2", "mate score is converted to White perspective");
    {
        UciEngine missing("/definitely/not/a/gambitree-engine");
        std::string error;
        int attempt;
        expect_true(missing.start(error), "engine fork succeeds even when exec will fail asynchronously");
        for (attempt = 0; attempt < 20 && missing.state() != UCI_UNAVAILABLE; ++attempt) { usleep(10000); missing.poll(); }
        expect_true(missing.state() == UCI_UNAVAILABLE, "unexpected engine exit becomes a harmless unavailable state");
    }
    {
        const std::string fake_path_storage = test_scratch_directory + "/gambitree-fake-uci-stale.sh";
        const char* fake_path = fake_path_storage.c_str();
        std::ofstream fake(fake_path);
        std::string error;
        Position first = Position::standard();
        Position second = Position::standard();
        Move e4;
        Undo undo;
        UciEngine engine(fake_path);
        int attempt;
        fake << "#!/bin/sh\n"
             << "position=''\n"
             << "while IFS= read -r command; do\n"
             << " case \"$command\" in\n"
             << "  uci) printf 'id name fake-uci\\nuciok\\n' ;;\n"
             << "  isready) printf 'readyok\\n' ;;\n"
             << "  position\\ *) position=\"$command\" ;;\n"
             << "  go\\ *) case \"$position\" in *' b KQkq e3 0 1') printf 'info depth 2 score cp 22 pv e7e5\\n' ;; *) printf 'info depth 2 score cp 11 pv e2e4\\n' ;; esac ;;\n"
             << "  stop) printf 'bestmove e2e4\\ninfo depth 99 score cp 99 pv d2d4\\n' ;;\n"
             << "  quit) exit 0 ;;\n"
             << " esac\n"
             << "done\n";
        fake.close();
        expect_true(chmod(fake_path, 0755) == 0, "fake UCI stale-generation helper is executable");
        expect_true(second.parse_san("e4", e4) && second.make_move(e4, undo),
                    "stale-generation fixture constructs a distinct next position");
        if (engine.start(error)) {
            for (attempt = 0; attempt < 200 && engine.state() != UCI_READY; ++attempt) { engine.poll(); usleep(10000); }
            engine.request_analysis(first);
            for (attempt = 0; attempt < 200 &&
                 !(engine.analysis().generation == 1 && engine.analysis().info.score_valid); ++attempt) { engine.poll(); usleep(10000); }
            expect_true(engine.analysis().generation == 1 && engine.analysis().info.score == 11,
                        "first fake UCI search produces generation A analysis");
            engine.request_analysis(second);
            usleep(20000);
            engine.poll();
            expect_true(engine.generation() == 2 && engine.analysis().generation == 2 &&
                        (!engine.analysis().info.score_valid || engine.analysis().info.score != 99),
                        "late info batched after generation A bestmove cannot contaminate generation B");
            for (attempt = 0; attempt < 200 &&
                 !(engine.analysis().generation == 2 && engine.analysis().info.score_valid); ++attempt) { engine.poll(); usleep(10000); }
            expect_true(engine.analysis().generation == 2 && engine.analysis().info.score == 22 &&
                        engine.analysis().san_pv == "e5",
                        "generation B accepts its own UCI info after the stop barrier");
            engine.shutdown();
            expect_true(engine.child_pid() < 0, "fake UCI child exits after shutdown");
        } else expect_true(false, "fake UCI helper starts for stale-generation protocol test");
        std::remove(fake_path);
    }
}

UciAnalysis analysis_snapshot(unsigned long generation, int depth, int score, const char* pv)
{
    UciAnalysis result;
    result.generation = generation;
    result.valid = true;
    result.side_to_move = WHITE;
    result.info.depth = depth;
    result.info.depth_valid = true;
    result.info.score_type = UCI_SCORE_CENTIPAWNS;
    result.info.score = score;
    result.info.score_valid = true;
    result.info.pv_valid = true;
    result.san_pv = pv;
    return result;
}

void test_engine_display_snapshots()
{
    EngineDisplay display;
    UciAnalysis a = analysis_snapshot(1, 18, 42, "Nf3 d5 c4");
    UciAnalysis none;
    UciAnalysis b;
    display.update_snapshot(1, UCI_ANALYZING, "Stockfish", a, true);
    expect_true(display.score_is_current() && display.pv_is_current(), "current analysis is shown as current");
    display.update_snapshot(2, UCI_STOPPING, "Stockfish", none, true);
    expect_true(display.score_analysis() && display.pv_analysis() && !display.score_is_current() && !display.pv_is_current() &&
                display.score_analysis()->info.score == 42 && display.pv_analysis()->san_pv == "Nf3 d5 c4",
                "generation change keeps the last complete analysis as a pending snapshot");
    {
        UciAnalysis depth_only;
        depth_only.generation = 2;
        depth_only.info.depth = 7;
        depth_only.info.depth_valid = true;
        display.update_snapshot(2, UCI_ANALYZING, "Stockfish", depth_only, true);
        expect_true(display.score_analysis() && display.pv_analysis() && !display.score_is_current() && !display.pv_is_current(),
                    "depth-only current-generation info cannot clear a pending score or PV");
    }
    display.update_snapshot(2, UCI_ANALYZING, "Stockfish", a, true);
    expect_true(display.score_analysis() && !display.score_is_current() && display.score_analysis()->info.score == 42,
                "stale info cannot replace or damage the pending snapshot");
    display.update_snapshot(2, UCI_ANALYZING, "Stockfish", none, true);
    expect_true(display.score_analysis() && display.score_analysis()->info.score == 42,
                "old bestmove or empty current state does not erase the pending snapshot");
    b = analysis_snapshot(2, 7, 31, "");
    b.info.pv_valid = false;
    display.update_snapshot(2, UCI_ANALYZING, "Stockfish", b, true);
    expect_true(display.score_is_current() && display.score_analysis()->info.score == 31 &&
                !display.pv_is_current() && display.pv_analysis() && display.pv_analysis()->san_pv == "Nf3 d5 c4",
                "a new score without PV updates only that field and retains pending PV");
    b.info.pv_valid = true;
    b.san_pv = "d4 Nf6 c4";
    display.update_snapshot(2, UCI_ANALYZING, "Stockfish", b, true);
    expect_true(display.score_is_current() && display.pv_is_current() && display.pv_analysis()->san_pv == "d4 Nf6 c4",
                "first complete current-generation analysis replaces pending data");
    display.update_snapshot(3, UCI_STOPPING, "Stockfish", none, true);
    display.update_snapshot(4, UCI_STOPPING, "Stockfish", none, true);
    expect_true(display.score_analysis() && display.pv_analysis(), "rapid generation changes retain a nonempty pending display");
    display.clear();
    display.update_snapshot(4, UCI_ANALYZING, "Stockfish", none, false);
    expect_true(!display.score_analysis() && !display.pv_analysis(), "analysis off hides display snapshots");
    b = analysis_snapshot(4, 8, 18, "c4 e5 Nc3");
    display.update_snapshot(4, UCI_ANALYZING, "Stockfish", b, true);
    expect_true(display.score_is_current() && display.pv_is_current(),
                "re-enabling analysis accepts only the new current-generation result");
    {
        UciAnalysis terminal;
        UciAnalysis next;
        terminal.generation = 5;
        terminal.valid = true;
        terminal.terminal = true;
        terminal.checkmate = true;
        terminal.info.depth_valid = true;
        terminal.info.score_type = UCI_SCORE_MATE;
        terminal.info.score = 0;
        terminal.info.score_valid = true;
        display.update_snapshot(5, UCI_ANALYZING, "Stockfish 19", terminal, true);
        expect_true(display.state() == gambitree::ENGINE_DISPLAY_TERMINAL && display.status() == "Checkmate" &&
                    display.score_is_current() && display.score_analysis()->info.score_type == UCI_SCORE_MATE &&
                    display.score_analysis()->info.score == 0 && UciEngine::format_score(*display.score_analysis()) == "M0" &&
                    !display.pv_analysis(),
                    "terminal checkmate generation shows its current M0 result without an older pending PV");
        terminal.finished = true; /* Stockfish 19 reports bestmove (none) after mate 0. */
        bool completion_redraw = display.update_snapshot(5, UCI_READY, "Stockfish 19", terminal, true);
        expect_true(display.state() == gambitree::ENGINE_DISPLAY_TERMINAL && display.status() == "Checkmate" &&
                    !display.pv_analysis() && completion_redraw,
                    "bestmove (none) completes terminal generation without resurrecting the prior PV");
        next.generation = 6;
        display.update_snapshot(6, UCI_ANALYZING, "Stockfish 19", next, true);
        expect_true(display.state() == gambitree::ENGINE_DISPLAY_PENDING && !display.score_analysis() &&
                    !display.pv_analysis(),
                    "navigating away from a terminal node starts a clean search with no terminal snapshot pending");
        next = analysis_snapshot(6, 1, 8, "Nf3");
        display.update_snapshot(6, UCI_ANALYZING, "Stockfish 19", next, true);
        expect_true(display.state() == gambitree::ENGINE_DISPLAY_ANALYZING && display.pv_is_current() &&
                    display.pv_analysis()->san_pv == "Nf3",
                    "the new non-terminal generation accepts normal current analysis");
        display.update_snapshot(6, UCI_ANALYZING, "Stockfish 19", a, true);
        expect_true(display.pv_is_current() && display.pv_analysis()->san_pv == "Nf3",
                    "stale info snapshot from generation A cannot change generation B");
        terminal.checkmate = false;
        terminal.generation = 7;
        terminal.info.score_type = UCI_SCORE_CENTIPAWNS;
        terminal.info.score = 0;
        display.update_snapshot(7, UCI_ANALYZING, "Stockfish 19", terminal, true);
        expect_true(display.state() == gambitree::ENGINE_DISPLAY_TERMINAL && display.status() == "Stalemate" &&
                    !display.pv_analysis(),
                    "terminal no-legal-move generation without check is presented as stalemate");
    }
}

void test_engine_pv_preview()
{
    PgnDocument document;
    expect_true(gambitree::PgnParser::parse("1. d4 d5 (1... Nf6) 2. c4 *", document), "preview PGN parses");
    if (document.games.empty()) return;
    PgnGame& game = *document.games[0];
    MoveNavigator navigator(game);
    const MoveNode* root = navigator.current();
    const Position origin = navigator.position();
    const uint64_t fingerprint = gambitree::game_navigation_fingerprint(game);
    UciInfo info;
    UciEngine::parse_info("info depth 12 seldepth 20 score cp 25 pv e2e4 e7e5 g1f3 b8c6 f1b5 a7a6", info);
    UciAnalysis shown = analysis_snapshot(7, 12, 25, "e4 e5 Nf3 Nc6 Bb5 a6");
    shown.info = info;
    EngineDisplay display;
    display.update_snapshot(7, UCI_ANALYZING, "Stockfish", shown, true);
    const std::string paired = "e4 e5. Nf3 Nc6. Bb5 a6.";
    const int sizes[] = {8, 12, 24};
    for (std::size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        gambitree::NotationFontMetrics metrics(sizes[s], sizes[s] + 10);
        for (int figurines = 0; figurines != 2; ++figurines) {
            const gambitree::EnginePvLayout row = gambitree::layout_engine_pv(shown, false, 2000, metrics, figurines != 0);
            expect_true(row.text == (figurines ? "e4 e5. ♘f3 ♘c6. ♗b5 a6." : paired),
                        "PV has periods only after Black moves, with or without figurines");
            expect_true(row.moves.size() == 6, "each visible PV move has a measured hitbox");
            for (std::size_t i = 0; i < row.moves.size(); ++i) {
                const gambitree::NotationItem& item = row.moves[i];
                expect_true(row.hit_test(item.x + item.width / 2, item.height / 2) == static_cast<int>(i) &&
                            row.hit_test(item.x, item.height) == -1,
                            "PV hitboxes follow text size and exclude the next row");
            }
            const int first_width = metrics.text_width("e4", true);
            const gambitree::EnginePvLayout narrow = gambitree::layout_engine_pv(shown, false, first_width, metrics, figurines != 0);
            expect_true(narrow.text == "e4" && narrow.moves.size() == 1 &&
                        narrow.hit_test(first_width, 1) == -1,
                        "clipped PV moves have no click targets and tokens stay whole");
            const gambitree::EnginePvLayout pending = gambitree::layout_engine_pv(shown, true, 2000, metrics, figurines != 0);
            expect_true(pending.text.find("[pending] e4 e5.") == 0 && pending.hit_test(0, 1) == -1 &&
                        pending.moves.size() == row.moves.size(), "pending marker is never a PV move hitbox");
        }
    }
    VariableTextMeasurer variable;
    gambitree::NotationFontMetrics variable_metrics(8, 24, &variable);
    const gambitree::EnginePvLayout variable_row = gambitree::layout_engine_pv(shown, false, 2000, variable_metrics, false);
    expect_true(variable_row.moves[2].x == variable.text_width("e4 e5. "), "PV hitboxes use actual proportional text measurement");

    expect_true(display.select_preview(shown, 3, origin, root, 7) && display.preview_index() == 3 &&
                display.preview_position()->to_fen() == "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3" &&
                display.preview_move()->from == Position::square_from_string("b8") &&
                display.preview_move()->to == Position::square_from_string("c6"),
                "arbitrary clicked UCI prefix reconstructs the expected board and last move");
    expect_true(navigator.current() == root && navigator.position().equals(origin) &&
                gambitree::game_navigation_fingerprint(game) == fingerprint &&
                game.root->children[0]->children.size() == 2 && shown.info.pv == info.pv,
                "preview preserves selected PGN node, tree, origin and engine PV");

    UciAnalysis latest = analysis_snapshot(7, 30, 81, "d4 d5");
    UciEngine::parse_info("info depth 30 seldepth 54 score cp 81 pv d2d4 d7d5", latest.info);
    expect_true(!display.update_snapshot(7, UCI_ANALYZING, "Stockfish", latest, true) &&
                display.detail_analysis()->info.depth == 12 && display.score_analysis()->info.score == 25 &&
                display.pv_analysis()->san_pv == shown.san_pv && display.preview_index() == 3,
                "new output updates hidden analysis without replacing the selected visible snapshot");
    const UciAnalysis frozen = *display.pv_analysis();
    expect_true(display.select_preview(frozen, 0, origin, root, 7) && display.preview_index() == 0 &&
                display.preview_position()->to_fen() == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
                "another click uses the frozen UCI line, not newly received output");
    display.end_preview();
    expect_true(!display.preview_position() && !display.preview_move() && display.preview_index() == -1 &&
                display.pv_analysis()->san_pv == "d4 d5" && display.detail_analysis()->info.seldepth == 54,
                "ending preview immediately exposes the latest complete analysis");
    expect_true(display.select_preview(shown, 1, origin, root, 7) && display.preview_index() == 1,
                "a click on the last actually painted snapshot survives a throttled newer update");
    display.end_preview();

    const KeySym navigation_keys[] = {XK_Left, XK_Right, XK_Up, XK_Down, XK_Home, XK_End, XK_Page_Up, XK_Page_Down};
    for (std::size_t i = 0; i < sizeof(navigation_keys) / sizeof(navigation_keys[0]); ++i) {
        display.select_preview(shown, 2, origin, root, 7);
        if (gambitree::pgn_navigation_key(navigation_keys[i], 0)) display.end_preview();
        expect_true(display.preview_index() == -1 && navigator.current() == root,
                    "every PGN navigation key ends preview before navigating from the original node");
    }
    expect_true(gambitree::pgn_navigation_key(XK_Page_Up, ControlMask) &&
                gambitree::pgn_navigation_key(XK_Page_Down, ControlMask) &&
                !gambitree::pgn_navigation_key(XK_Page_Up, ShiftMask) &&
                !gambitree::pgn_navigation_key(XK_Escape, 0) && !gambitree::pgn_navigation_key(XK_f, 0),
                "file navigation ends preview; scrolling, Escape and board rotation keep their existing meanings");
    display.select_preview(shown, 2, origin, root, 7);
    display.end_preview();
    expect_true(!navigator.back() && !display.preview_position(), "unsuccessful PGN navigation also dismisses preview");
    navigator.to_end();
    display.select_preview(shown, 0, origin, root, 7);
    display.sync_position(navigator.position(), navigator.current());
    expect_true(!display.preview_position(), "changing the selected PGN node automatically ends preview");
    navigator.to_begin();
    display.select_preview(shown, 2, origin, root, 7);
    display.sync_position(origin, root);
    expect_true(display.preview_index() == 2, "presentation-only redraws preserve preview");
    display.update_snapshot(7, UCI_ANALYZING, "Stockfish", latest, false);
    expect_true(!display.preview_position() && display.state() == gambitree::ENGINE_DISPLAY_OFF &&
                !display.select_preview(shown, 0, origin, root, 7), "disabling analysis ends preview and rejects hidden clicks");
    display.update_snapshot(7, UCI_ANALYZING, "Stockfish", shown, true);
    display.select_preview(shown, 2, origin, root, 7);
    display.update_snapshot(8, UCI_STOPPING, "Stockfish", UciAnalysis(), true);
    expect_true(!display.preview_position() && !display.select_preview(shown, 0, origin, root, 8),
                "generation changes end preview and reject stale displayed UCI lines");

    display.update_snapshot(7, UCI_ANALYZING, "Stockfish", shown, true);
    UciAnalysis malformed = shown;
    malformed.info.pv[1] = "e7e5garbage";
    expect_true(display.select_preview(malformed, 0, origin, root, 7) &&
                !display.select_preview(malformed, 1, origin, root, 7) && display.preview_index() == 0,
                "valid prefix can be previewed but malformed tail cannot alter a selected board");
    const char* bad[] = {"", "e7", "e7e5q", "e7e5x", "a9a1", "e2e4", "0000"};
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        malformed.info.pv[1] = bad[i];
        expect_true(!display.select_preview(malformed, 1, origin, root, 7) && display.preview_index() == 0,
                    "illegal, incomplete and malformed UCI prefixes fail without changing the preview");
    }
    malformed = shown; malformed.info.pv_valid = false;
    expect_true(!display.select_preview(malformed, 0, origin, root, 7) &&
                !display.select_preview(shown, shown.info.pv.size(), origin, root, 7),
                "missing PV and out-of-range selection are rejected");
    display.clear();
    expect_true(!display.preview_position() && !display.select_preview(shown, 0, origin, root, 7),
                "clearing analysis clears its preview and rejects cached click targets");

    Position black_origin;
    std::string error;
    Position::from_fen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1", black_origin, &error);
    UciAnalysis black = analysis_snapshot(9, 10, 0, "e5 Nf3 Nc6");
    black.side_to_move = BLACK;
    UciEngine::parse_info("info depth 10 score cp 0 pv e7e5 g1f3 b8c6", black.info);
    expect_true(gambitree::layout_engine_pv(black, false, 2000, variable_metrics, false).text == "e5. Nf3 Nc6.",
                "black-first PV uses actual starting side instead of even/odd move numbering");
    display.update_snapshot(9, UCI_ANALYZING, "Stockfish", black, true);
    expect_true(display.select_preview(black, 2, black_origin, root, 9) &&
                display.preview_position()->to_fen() == "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3" &&
                !display.select_preview(black, 0, origin, root, 9),
                "black-first preview reconstructs correctly and rejects a mismatched starting side");
}

void test_engine_display_throttle()
{
    EngineUiThrottle throttle;
    const unsigned long base = 1000UL;
    expect_true(throttle.engine_update_due(base), "first engine display update is immediate");
    throttle.engine_presented(base);
    expect_true(!throttle.engine_update_due(base + 100UL) && throttle.pending(),
                "engine updates within 200 ms are coalesced");
    expect_true(throttle.engine_update_due(base + 200UL),
                "coalesced engine update becomes due at five Hz");
    throttle.engine_presented(base + 200UL);
    throttle.user_presented(base + 201UL);
    expect_true(!throttle.pending(), "immediate user redraw clears pending engine-only repaint");
    {
        EngineDisplayGeometry geometry = EngineDisplay::geometry(20);
        expect_true(geometry.depth_label_x == 20 && geometry.depth_right_x == 84 && geometry.score_x == 98 &&
                    geometry.seldepth_x == 260 && geometry.pv_x == 20 && geometry.status_x == 140,
                    "engine depth, score, and PV columns have fixed pixel geometry");
        {
            NotationFontMetrics pending_metrics(8, 18);
            expect_true(geometry.seldepth_x - geometry.score_x >=
                        pending_metrics.text_width("-12.45 [pending]"),
                        "pending score field cannot overlap the seldepth column after an indexing redraw");
        }
    }
}

void test_command_line()
{
    char program[] = "gambitree";
    char print[] = "--print-window-id";
    char engine[] = "--engine";
    char engine_path[] = "custom engine";
    char pieces_option[] = "--pieces";
    char theme_name[] = "classic";
    char game[] = "game.pgn";
    char unknown[] = "--unknown";
    char unsafe_theme[] = "../classic";
    char meridian_id[] = "meridian-shaded";
    char cburnett_id[] = "cburnett";
    char local_id[] = "local_set-2";
    char removed_option[] = "--piece-theme";
    char* arguments[] = { program, print, engine, engine_path, pieces_option, theme_name, game, 0 };
    char* invalid[] = { program, unknown, 0 };
    char* invalid_theme[] = { program, pieces_option, unsafe_theme, 0 };
    char* pieces[] = { program, pieces_option, meridian_id, 0 };
    char* local_set[] = { program, pieces_option, local_id, 0 };
    char* classic[] = { program, pieces_option, theme_name, 0 };
    char* missing_pieces[] = { program, pieces_option, 0 };
    char* cburnett[] = { program, pieces_option, cburnett_id, 0 };
    char* removed[] = { program, removed_option, cburnett_id, 0 };
    CommandLineOptions options;
    char title_option[] = "--title";
    char title[] = "Study";
    char spaced_title[] = "Carlsen - Anand 2013";
    char utf8_title[] = "Schaakpartij \342\200\223 Z\303\274rich";
    char empty_title[] = "";
    char* empty_pieces[] = { program, pieces_option, empty_title, 0 };
    char* no_title[] = { program, game, 0 };
    char* explicit_title[] = { program, title_option, title, 0 };
    char* before_path[] = { program, title_option, spaced_title, game, 0 };
    char* after_path[] = { program, game, title_option, utf8_title, 0 };
    char* missing_title[] = { program, title_option, 0 };
    char* empty[] = { program, title_option, empty_title, 0 };
    char* repeated[] = { program, title_option, title, game,
                         title_option, spaced_title, 0 };
    char* empty_with_path[] = { program, title_option, empty_title, game, 0 };
    char* last_empty[] = { program, title_option, title, game,
                           title_option, empty_title, 0 };
    char* last_nonempty[] = { program, title_option, empty_title,
                              title_option, title, 0 };
    char help_option[] = "--help";
    char version_option[] = "--version";
    char* trailing_title[] = { program, game, title_option, 0 };
    char* last_missing[] = { program, title_option, title, title_option, 0 };
    char* consecutive_titles[] = { program, title_option, title_option, title, 0 };
    char* filename_title[] = { program, title_option, game, 0 };
    char* title_help[] = { program, title_option, help_option, 0 };
    char* title_version[] = { program, title_option, version_option, 0 };
    char* title_unknown[] = { program, title_option, unknown, 0 };
    char* title_missing_engine[] = { program, title_option, engine, 0 };
    char* title_options[] = { program, title_option, engine, engine_path,
        title_option, pieces_option, cburnett_id, title_option, pieces_option, theme_name,
        title_option, print, 0 };
    expect_true(gambitree::parse_command_line(2, no_title, options) == COMMAND_LINE_RUN &&
                !options.window_title_explicit && options.window_title.empty(),
                "no --title leaves the standard window title enabled");
    expect_true(gambitree::parse_command_line(3, explicit_title, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title == title && options.paths.empty(),
                "--title accepts an explicit title without a PGN file");
    expect_true(gambitree::parse_command_line(4, before_path, options) == COMMAND_LINE_RUN &&
                options.window_title == spaced_title && options.paths.size() == 1 &&
                options.paths[0] == game,
                "--title preserves spaces before a PGN file argument");
    expect_true(gambitree::parse_command_line(4, after_path, options) == COMMAND_LINE_RUN &&
                options.window_title == utf8_title && options.paths.size() == 1 &&
                options.paths[0] == game,
                "--title preserves UTF-8 after a PGN file argument");
    expect_true(gambitree::parse_command_line(2, missing_title, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty() && options.error.empty(),
                "--title without an argument selects an empty title");
    expect_true(gambitree::parse_command_line(3, trailing_title, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty() &&
                options.paths.size() == 1 && options.paths[0] == game,
                "a trailing --title opens the PGN file with an empty title");
    expect_true(gambitree::parse_command_line(4, last_missing, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty(),
                "a final --title without text clears an earlier title");
    expect_true(gambitree::parse_command_line(4, consecutive_titles, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title == title,
                "consecutive --title options leave the last title in effect");
    expect_true(gambitree::parse_command_line(3, filename_title, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title == game && options.paths.empty(),
                "an ordinary argument after --title is title text even when it looks like a PGN path");
    expect_true(gambitree::parse_command_line(3, title_help, options) == gambitree::COMMAND_LINE_HELP &&
                options.window_title_explicit && options.window_title.empty(),
                "--title leaves a following --help option available");
    expect_true(gambitree::parse_command_line(3, title_version, options) == gambitree::COMMAND_LINE_VERSION &&
                options.window_title_explicit && options.window_title.empty(),
                "--title leaves a following --version option available");
    expect_true(gambitree::parse_command_line(12, title_options, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty() && options.paths.empty() &&
                options.engine_path == engine_path && options.piece_theme == theme_name &&
                options.piece_theme_explicit && options.print_window_id,
                "--title preserves following engine, repeated piece-set and window-ID options");
    expect_true(gambitree::parse_command_line(3, title_unknown, options) == COMMAND_LINE_ERROR &&
                options.error == "unrecognized option '--unknown'",
                "an unknown option after --title retains its original error");
    expect_true(gambitree::parse_command_line(3, title_missing_engine, options) == COMMAND_LINE_ERROR &&
                options.error == "--engine requires a path",
                "a missing engine argument after --title retains its original error");
    expect_true(gambitree::parse_command_line(3, empty, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty(),
                "--title accepts an explicitly empty title");
    expect_true(gambitree::parse_command_line(4, empty_with_path, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty() &&
                options.paths.size() == 1 && options.paths[0] == game,
                "an empty title preserves the PGN file argument");
    expect_true(gambitree::parse_command_line(6, last_empty, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty(),
                "a final empty --title overrides an earlier non-empty title");
    expect_true(gambitree::parse_command_line(5, last_nonempty, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title == title,
                "a final non-empty --title overrides an earlier empty title");
    expect_true(gambitree::parse_command_line(6, repeated, options) == COMMAND_LINE_RUN &&
                options.window_title == spaced_title && options.paths.size() == 1,
                "the last --title wins without affecting PGN file arguments");
    expect_true(gambitree::parse_command_line(7, arguments, options) == COMMAND_LINE_RUN &&
                options.print_window_id && options.engine_path == "custom engine" &&
                options.piece_theme == "classic" &&
                options.piece_theme_explicit &&
                !options.window_title_explicit &&
                options.paths.size() == 1 && options.paths[0] == "game.pgn",
                "command-line parser accepts --print-window-id alongside engine and PGN options");
    expect_true(gambitree::parse_command_line(2, invalid, options) == COMMAND_LINE_ERROR,
                "command-line parser rejects unknown options");
    expect_true(gambitree::parse_command_line(3, invalid_theme, options) == COMMAND_LINE_ERROR,
                "command-line parser rejects unsafe piece theme names");
    expect_true(gambitree::parse_command_line(3, pieces, options) == COMMAND_LINE_RUN &&
                options.piece_theme == "meridian-shaded" &&
                options.piece_theme_explicit,
                "--pieces selects a registered stable piece-set ID explicitly");
    expect_true(gambitree::parse_command_line(3, cburnett, options) == COMMAND_LINE_RUN &&
                options.piece_theme == "cburnett" && options.piece_theme_explicit,
                "--pieces cburnett selects the bundled artwork");
    expect_true(gambitree::parse_command_line(3, classic, options) == COMMAND_LINE_RUN &&
                options.piece_theme == "classic" && options.piece_theme_explicit &&
                gambitree::initial_piece_set_id(options.piece_theme,
                    options.piece_theme_explicit, "cburnett") == "gambitree",
                "--pieces classic selects Gambitree through the existing alias mapping");
    expect_true(gambitree::parse_command_line(3, local_set, options) == COMMAND_LINE_RUN &&
                options.piece_theme == local_id && options.piece_theme_explicit &&
                gambitree::initial_piece_set_id(options.piece_theme,
                    options.piece_theme_explicit, "cburnett") == local_id,
                "--pieces passes a local piece-set name unchanged to the existing loader");
    expect_true(gambitree::parse_command_line(2, missing_pieces, options) == COMMAND_LINE_ERROR &&
                options.error == "--pieces requires a name",
                "--pieces rejects a missing name with the correct option name");
    expect_true(gambitree::parse_command_line(3, empty_pieces, options) == COMMAND_LINE_ERROR &&
                options.error == "--pieces requires a name",
                "--pieces rejects an empty name");
    expect_true(gambitree::parse_command_line(3, removed, options) == COMMAND_LINE_ERROR &&
                options.error == "unrecognized option '--piece-theme'",
                "the removed --piece-theme option is rejected as unknown");
    expect_true(gambitree::should_print_window_id(true, false) &&
                gambitree::should_print_window_id(false, true) &&
                !gambitree::should_print_window_id(false, false),
                "window-id output is enabled by either the flag or diagnostics only");
}

void test_board_command_line()
{
    char program[] = "gambitree", board[] = "--board", game[] = "game.pgn";
    char classic[] = "classic", wood[] = "wood", veneer[] = "wood-veneer";
    char invalid[] = "unknown", empty[] = "";
    char pieces[] = "--pieces", cburnett[] = "cburnett";
    char engine[] = "--engine", engine_path[] = "custom engine";
    char title[] = "--title", title_text[] = "Study board";
    char* themes[] = {classic, wood, veneer};
    CommandLineOptions options;
    char* no_board[] = {program, game, 0};
    expect_true(gambitree::parse_command_line(2, no_board, options) == COMMAND_LINE_RUN &&
                options.board_theme.empty(),
                "without --board the saved board preference remains in effect");
    for (std::size_t i = 0; i < sizeof(themes) / sizeof(themes[0]); ++i) {
        char* arguments[] = {program, board, themes[i], game, 0};
        expect_true(gambitree::parse_command_line(4, arguments, options) == COMMAND_LINE_RUN &&
                    options.board_theme == themes[i] && options.paths.size() == 1 && options.paths[0] == game,
                    "--board accepts each existing board theme before a PGN path");
    }
    char* combined[] = {program, game, pieces, cburnett, board, veneer,
                        engine, engine_path, title, title_text, 0};
    expect_true(gambitree::parse_command_line(10, combined, options) == COMMAND_LINE_RUN &&
                options.board_theme == veneer && options.paths.size() == 1 && options.paths[0] == game &&
                options.piece_theme == cburnett && options.piece_theme_explicit &&
                options.engine_path == engine_path && options.window_title_explicit &&
                options.window_title == title_text,
                "--board works after a PGN path alongside pieces, engine and title options");
    char* repeated[] = {program, board, wood, board, classic, 0};
    expect_true(gambitree::parse_command_line(5, repeated, options) == COMMAND_LINE_RUN &&
                options.board_theme == classic,
                "the last --board wins, including an explicit Classic board");
    char* after_title[] = {program, title, board, wood, game, 0};
    expect_true(gambitree::parse_command_line(5, after_title, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty() &&
                options.board_theme == wood && options.paths.size() == 1 && options.paths[0] == game,
                "an empty optional title leaves a following --board available");
    char* missing[] = {program, board, 0};
    expect_true(gambitree::parse_command_line(2, missing, options) == COMMAND_LINE_ERROR &&
                options.error == "--board requires a name",
                "--board rejects a missing argument");
    char* empty_board[] = {program, board, empty, 0};
    expect_true(gambitree::parse_command_line(3, empty_board, options) == COMMAND_LINE_ERROR &&
                options.error == "--board requires a name",
                "--board rejects an empty argument");
    char* invalid_board[] = {program, board, invalid, 0};
    expect_true(gambitree::parse_command_line(3, invalid_board, options) == COMMAND_LINE_ERROR &&
                options.error == "--board accepts only classic, wood or wood-veneer",
                "--board rejects unknown names and lists the supported themes");
    expect_true(gambitree::parse_command_line(2, no_board, options) == COMMAND_LINE_RUN &&
                options.board_theme.empty(),
                "parsing a fresh command line clears an earlier explicit board choice");
}

void test_window_geometry()
{
    gambitree::WindowGeometry g;
    expect_true(gambitree::parse_window_geometry("1200x800+40+60", g) &&
                g.mask == (WidthValue | HeightValue | XValue | YValue) &&
                g.width == 1200 && g.height == 800 && g.x == 40 && g.y == 60 && !g.adjusted,
                "X11 geometry parses client size and positive offsets without a display");
    expect_true(gambitree::parse_window_geometry("=1200x800-0-0", g) &&
                (g.mask & (XNegative | YNegative)) == (XNegative | YNegative) &&
                gambitree::window_geometry_spec(g, 900, 650) == "900x650-0-0",
                "negative zero preserves right/bottom anchoring after size adjustment");
    expect_true(gambitree::parse_window_geometry("+40+60", g) &&
                g.mask == (XValue | YValue) &&
                gambitree::window_geometry_spec(g, 800, 600) == "800x600+40+60",
                "position-only geometry keeps omitted dimensions available from the session");
    expect_true(gambitree::parse_window_geometry(" 1200 \t", g) && g.mask == WidthValue && g.width == 1200,
                "width-only geometry and surrounding whitespace are accepted");
    expect_true(gambitree::parse_window_geometry("x700", g) && g.mask == HeightValue && g.height == 700,
                "height-only geometry retains the saved width");
    expect_true(gambitree::parse_window_geometry("0x1", g) &&
                g.width == 480 && g.height == 320 && g.adjusted,
                "zero and tiny requested dimensions recover to a useful minimum");
    const std::string huge(4000, '9');
    expect_true(gambitree::parse_window_geometry(huge + "x" + huge + "+" + huge + "-" + huge, g) &&
                g.width == 8192 && g.height == 8192 && g.x == 32767 && g.y == -32767 && g.adjusted,
                "arbitrarily long numeric components are bounded before Xlib can overflow");
    const char* invalid[] = {"", " \t", "nonsense", "640x", "640x480+", "640x480+20garbage"};
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        expect_true(!gambitree::parse_window_geometry(invalid[i], g) && g.mask == NoValue,
                    "invalid geometry does not leak partially parsed fields");

    char program[] = "gambitree", option[] = "--geometry", value[] = "1000x700-20-30";
    char file[] = "game.pgn", board[] = "--board", wood[] = "wood";
    char bad[] = "bad", empty[] = "", title[] = "--title";
    CommandLineOptions options;
    char* normal[] = {program, file, option, value, board, wood, 0};
    expect_true(gambitree::parse_command_line(6, normal, options) == COMMAND_LINE_RUN &&
                options.geometry.width == 1000 && options.geometry.height == 700 &&
                options.geometry.x == -20 && options.geometry.y == -30 &&
                options.board_theme == wood && options.paths.size() == 1 && options.paths[0] == file,
                "--geometry works after a file and alongside other preferences");
    char* missing[] = {program, option, board, wood, file, 0};
    expect_true(gambitree::parse_command_line(5, missing, options) == COMMAND_LINE_RUN &&
                !options.geometry.mask && !options.warning.empty() && options.board_theme == wood &&
                options.paths.size() == 1,
                "missing geometry warns and leaves the following option intact");
    char* trailing[] = {program, file, option, 0};
    expect_true(gambitree::parse_command_line(3, trailing, options) == COMMAND_LINE_RUN &&
                !options.geometry.mask && !options.warning.empty() && options.paths.size() == 1,
                "a trailing --geometry does not stop startup or lose the PGN argument");
    char* invalid_value[] = {program, option, bad, file, 0};
    expect_true(gambitree::parse_command_line(4, invalid_value, options) == COMMAND_LINE_RUN &&
                !options.geometry.mask && !options.warning.empty() && options.paths.size() == 1,
                "invalid geometry is ignored without preventing startup");
    char* empty_value[] = {program, option, empty, 0};
    expect_true(gambitree::parse_command_line(3, empty_value, options) == COMMAND_LINE_RUN &&
                !options.geometry.mask && !options.warning.empty(),
                "empty geometry falls back rather than becoming a command-line error");
    char* repeated[] = {program, option, value, option, bad, 0};
    expect_true(gambitree::parse_command_line(5, repeated, options) == COMMAND_LINE_RUN &&
                options.geometry.width == 1000 && !options.warning.empty(),
                "an invalid later request preserves the previous valid geometry");
    char smaller[] = "640x480";
    char* replaced[] = {program, option, value, option, smaller, 0};
    expect_true(gambitree::parse_command_line(5, replaced, options) == COMMAND_LINE_RUN &&
                options.geometry.width == 640 && options.geometry.height == 480 &&
                !(options.geometry.mask & (XValue | YValue)) && options.warning.empty(),
                "the last valid geometry replaces earlier CLI components");
    char* after_title[] = {program, title, option, value, 0};
    expect_true(gambitree::parse_command_line(4, after_title, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty() && options.geometry.width == 1000,
                "an omitted title leaves --geometry available to the parser");
    char* absent[] = {program, file, 0};
    expect_true(gambitree::parse_command_line(2, absent, options) == COMMAND_LINE_RUN &&
                !options.geometry.mask && options.warning.empty(),
                "without --geometry the original session restoration remains enabled");
}

void test_role_command_line()
{
    char program[] = "gambitree", role[] = "--role", name[] = "analysis";
    char spaced[] = "Study window", file[] = "game.pgn", title[] = "--title";
    char empty[] = "", nonascii[] = "Z\303\274rich", control[] = "bad\nrole";
    CommandLineOptions options;
    char* absent[] = {program, file, 0};
    expect_true(gambitree::parse_command_line(2, absent, options) == COMMAND_LINE_RUN &&
                options.window_role.empty(), "without --role no window role is requested");
    char* before[] = {program, role, name, file, 0};
    expect_true(gambitree::parse_command_line(4, before, options) == COMMAND_LINE_RUN &&
                options.window_role == name && options.paths.size() == 1 && options.paths[0] == file,
                "--role accepts an identifier before a PGN path");
    char* after[] = {program, file, role, spaced, 0};
    expect_true(gambitree::parse_command_line(4, after, options) == COMMAND_LINE_RUN &&
                options.window_role == spaced && options.paths.size() == 1 && options.paths[0] == file,
                "--role preserves a quoted name with spaces after a PGN path");
    char* repeated[] = {program, role, name, role, spaced, 0};
    expect_true(gambitree::parse_command_line(5, repeated, options) == COMMAND_LINE_RUN &&
                options.window_role == spaced, "the last --role wins");
    char* after_title[] = {program, title, role, name, 0};
    expect_true(gambitree::parse_command_line(4, after_title, options) == COMMAND_LINE_RUN &&
                options.window_title_explicit && options.window_title.empty() && options.window_role == name,
                "an empty optional title leaves a following --role intact");
    char* missing[] = {program, role, 0};
    expect_true(gambitree::parse_command_line(2, missing, options) == COMMAND_LINE_ERROR &&
                options.error == "--role requires a non-empty name", "--role rejects a missing name");
    char* empty_name[] = {program, role, empty, 0};
    expect_true(gambitree::parse_command_line(3, empty_name, options) == COMMAND_LINE_ERROR &&
                options.error == "--role requires a non-empty name", "--role rejects an empty name");
    char* invalid[] = {nonascii, control};
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char* arguments[] = {program, role, invalid[i], 0};
        expect_true(gambitree::parse_command_line(3, arguments, options) == COMMAND_LINE_ERROR &&
                    options.error == "--role accepts only printable ASCII characters",
                    "--role rejects bytes outside the supported STRING character range");
    }
    expect_true(gambitree::parse_command_line(2, absent, options) == COMMAND_LINE_RUN &&
                options.window_role.empty(), "a fresh command line clears an earlier role");
}

void test_window_modes()
{
    using namespace gambitree;
    char program[] = "gambitree", file[] = "game.pgn";
    char fullscreen[] = "--fullscreen", iconic[] = "--iconic", title[] = "--title";
    char* plain[] = {program, file, 0};
    CommandLineOptions options;
    expect_true(gambitree::parse_command_line(2, plain, options) == COMMAND_LINE_RUN &&
                !options.fullscreen && !options.iconic, "window modes default to normal startup");
    char* full[] = {program, fullscreen, file, 0};
    expect_true(gambitree::parse_command_line(3, full, options) == COMMAND_LINE_RUN &&
                options.fullscreen && !options.iconic && options.paths.size() == 1 && options.paths[0] == file,
                "--fullscreen requests fullscreen without consuming a PGN argument");
    char* minimized[] = {program, file, iconic, 0};
    expect_true(gambitree::parse_command_line(3, minimized, options) == COMMAND_LINE_RUN &&
                !options.fullscreen && options.iconic && options.paths.size() == 1,
                "--iconic works after a PGN path without an argument");
    char* both[] = {program, title, fullscreen, iconic, fullscreen, iconic, file, 0};
    expect_true(gambitree::parse_command_line(7, both, options) == COMMAND_LINE_RUN &&
                options.fullscreen && options.iconic && options.window_title_explicit &&
                options.window_title.empty() && options.paths.size() == 1,
                "startup modes combine, repeat harmlessly, and follow an empty optional title");
    expect_true(gambitree::parse_command_line(2, plain, options) == COMMAND_LINE_RUN &&
                !options.fullscreen && !options.iconic, "mode requests do not survive a fresh command line");

    const std::string path = test_scratch_directory + "/window-mode-state";
    SessionState initial = default_session_state(), current, loaded;
    initial.width = 800; initial.height = 600;
    initial.has_position = true; initial.position_is_outer = true;
    initial.x = 30; initial.y = 40;
    expect_true(save_session_state(path, initial), "seed normal window geometry");
    SessionStateSaver saver(path, initial);
    current = initial;
    current.width = 1600; current.height = 900; current.x = current.y = 0;
    current.board_theme_id = "wood";
    saver.retain_saved_geometry(current);
    saver.observe(current, 100);
    expect_true(current.width == 800 && current.height == 600 && current.x == 30 && current.y == 40 &&
                current.board_theme_id == "wood" && !saver.due(1099) && saver.due(1100),
                "transient geometry is excluded while ordinary preferences retain their debounce");
    // Another live process can publish newer normal geometry during fullscreen.
    SessionState remote = initial;
    remote.width = 1000; remote.height = 700;
    expect_true(save_session_state(path, remote), "publish newer geometry from another process");
    expect_true(saver.save(current, 1100) == SESSION_SAVE_SAVED &&
                load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                loaded.width == 1000 && loaded.height == 700 && loaded.board_theme_id == "wood",
                "saving a fullscreen/iconic preference preserves another process's normal geometry");
    current.width = 640; current.height = 480;
    current.x = 50; current.y = 60;
    saver.observe(current, 1200);
    expect_true(saver.save(current, 1201, true) == SESSION_SAVE_SAVED &&
                load_session_state(path, loaded) == SESSION_STATE_LOADED &&
                loaded.width == 640 && loaded.height == 480 && loaded.x == 50 && loaded.y == 60,
                "after returning to normal, resize and position changes save through normal shutdown");
    const std::string serialized = serialize_session_state(loaded);
    expect_true(serialized.find("fullscreen") == std::string::npos && serialized.find("iconic") == std::string::npos,
                "startup modes do not add persistent session fields");
}

void test_keyboard_and_sidebar_layout()
{
    SidebarGeometry normal = SidebarLayout::calculate(1200);
    SidebarGeometry narrow = SidebarLayout::calculate(180);
    expect_true(normal.width >= 320 && normal.width <= 500 && normal.content_width == normal.width - 28,
                "sidebar uses a bounded preferred width and stable padding");
    expect_true(narrow.left >= 0 && narrow.content_left >= narrow.left && narrow.content_width >= 0,
                "narrow sidebar geometry remains inside the window");
    expect_true(gambitree::keyboard_navigation_command(XK_Page_Up, 0) == gambitree::KEYBOARD_PREVIOUS_GAME &&
                gambitree::keyboard_navigation_command(XK_Page_Down, LockMask) == gambitree::KEYBOARD_NEXT_GAME &&
                gambitree::keyboard_navigation_command(XK_Page_Up, ControlMask | Mod2Mask) == gambitree::KEYBOARD_PREVIOUS_FILE &&
                gambitree::keyboard_navigation_command(XK_Page_Down, ControlMask | LockMask) == gambitree::KEYBOARD_NEXT_FILE &&
                gambitree::keyboard_navigation_command(XK_Page_Up, ShiftMask | LockMask) == gambitree::KEYBOARD_SCROLL_PAGE_UP &&
                gambitree::keyboard_navigation_command(XK_Page_Down, ShiftMask) == gambitree::KEYBOARD_SCROLL_PAGE_DOWN &&
                gambitree::keyboard_navigation_command(XK_Page_Down, ShiftMask | ControlMask) == gambitree::KEYBOARD_SCROLL_PAGE_DOWN,
                "Page keys distinguish games from Ctrl file navigation while ignoring lock modifiers");
    expect_true(gambitree::navigation_list_shortcut(XK_g, ControlMask) ==
                    gambitree::NAVIGATION_LIST_SHORTCUT_GAMES &&
                gambitree::navigation_list_shortcut(XK_G,
                    ControlMask | ShiftMask) ==
                    gambitree::NAVIGATION_LIST_SHORTCUT_FILES &&
                gambitree::navigation_list_shortcut(XK_f, ControlMask) ==
                    gambitree::NAVIGATION_LIST_SHORTCUT_NONE &&
                gambitree::navigation_list_shortcut(XK_g, 0) ==
                    gambitree::NAVIGATION_LIST_SHORTCUT_NONE,
                "Ctrl+G list shortcuts are distinct from Ctrl+F and unmodified G");
    expect_true(gambitree::notation_page_scroll_offset(400,500,20,2000,true)==860 &&
                gambitree::notation_page_scroll_offset(860,500,20,2000,false)==400 &&
                gambitree::notation_page_scroll_offset(0,300,12,100,true)==100 &&
                gambitree::notation_page_scroll_offset(90,300,12,100,true)==100 &&
                gambitree::notation_page_scroll_offset(90,300,12,100,false)==0,
                "Shift+Page moves one visible screen with overlap and clamps at both ends");
}

void test_delete_dialog_input_and_label_layout()
{
    gambitree::DeleteDialogInputState state;
    gambitree::DeleteDialogTextExtents extents;
    gambitree::DeleteDialogTextOrigin origin;

    expect_true(state.focused_button == 1,
                "delete dialog initially focuses Delete");
    expect_true(gambitree::handle_delete_dialog_key(
                    state, gambitree::DELETE_DIALOG_KEY_ENTER, false) ==
                    gambitree::DELETE_DIALOG_CONFIRM,
                "Enter immediately confirms the initially focused Delete");
    expect_true(gambitree::handle_delete_dialog_key(
                    state, gambitree::DELETE_DIALOG_KEY_ENTER, false) ==
                    gambitree::DELETE_DIALOG_NO_ACTION,
                "repeated confirmation key cannot activate twice");

    state = gambitree::DeleteDialogInputState();
    expect_true(gambitree::handle_delete_dialog_key(
                    state, gambitree::DELETE_DIALOG_KEY_TAB, false) ==
                    gambitree::DELETE_DIALOG_NO_ACTION &&
                state.focused_button == 0,
                "Tab moves focus from Delete to Cancel");
    expect_true(gambitree::handle_delete_dialog_key(
                    state, gambitree::DELETE_DIALOG_KEY_ENTER, false) ==
                    gambitree::DELETE_DIALOG_CANCEL,
                "Enter on Cancel dismisses without confirming");

    state = gambitree::DeleteDialogInputState();
    expect_true(gambitree::handle_delete_dialog_key(
                    state, gambitree::DELETE_DIALOG_KEY_TAB, true) ==
                    gambitree::DELETE_DIALOG_NO_ACTION &&
                state.focused_button == 0,
                "Shift+Tab moves to the other button in a two-button dialog");
    expect_true(gambitree::handle_delete_dialog_key(
                    state, gambitree::DELETE_DIALOG_KEY_ESCAPE, false) ==
                    gambitree::DELETE_DIALOG_CANCEL,
                "Escape always cancels regardless of focus");

    extents.x = 1;
    extents.y = -12;
    extents.width = 40;
    extents.height = 16;
    extents.advance = 42;
    extents.ascent = 14;
    extents.descent = 4;
    origin = gambitree::center_delete_dialog_label(20, 100, 124, 50,
                                                    extents);
    expect_true(origin.x == 61 && origin.baseline == 130,
                "button label uses centered Xft ink advance and font baseline metrics");
    expect_true(origin.x + extents.x >= 20 &&
                origin.x + extents.x + extents.width <= 144 &&
                origin.baseline + extents.y >= 100 &&
                origin.baseline + extents.y + extents.height <= 150,
                "centered Xft glyph ink remains inside the button bounds");
    expect_true(gambitree::delete_dialog_title_bar_height(25) == 43 &&
                gambitree::delete_dialog_title_bar_height(25, 2) == 70 &&
                gambitree::delete_dialog_button_height(23) == 50 &&
                gambitree::delete_dialog_button_width(68, 63) == 112 &&
                gambitree::delete_dialog_button_y(280, 50) == 216,
                "dialog title and button geometry follows actual font metrics");
    expect_true(gambitree::delete_dialog_height_for_content(
                    43, 7, 23, 50) == 292,
                "dialog height reserves title, body, and larger-button space");
}

void test_delete_dialog_pgn_metadata()
{
    expect_true(gambitree::format_pgn_game_date("1993.11.07") ==
                    "1993.11.07",
                "full PGN game date is displayed unchanged");
    expect_true(gambitree::format_pgn_game_date("1993.??.??") == "1993",
                "unknown month and day are omitted from PGN date");
    expect_true(gambitree::format_pgn_game_date("1993.11.??") ==
                    "1993.11",
                "unknown day is omitted from PGN date");
    expect_true(gambitree::format_pgn_game_date("1993.??.07") ==
                    "1993" &&
                gambitree::format_pgn_game_date("????.11.07").empty(),
                "date formatting never shows a later component after an unknown one");
    expect_true(gambitree::format_pgn_game_date("????.??.??").empty() &&
                gambitree::format_pgn_game_date("not-a-date").empty() &&
                gambitree::format_pgn_game_date("1993.13.32").empty(),
                "fully unknown and invalid dates are not displayed");
    expect_true(gambitree::format_pgn_elo("2230") == "2230" &&
                gambitree::format_pgn_elo(" ? ").empty() &&
                gambitree::format_pgn_elo("").empty() &&
                gambitree::format_pgn_elo("999999999999999999999999").empty() &&
                gambitree::format_pgn_elo("2415.5").empty() &&
                gambitree::format_pgn_elo("99999").empty(),
                "only bounded numeric PGN Elo ratings are displayed");
    expect_true(gambitree::format_pgn_player(
                    "White", "Midjord, Joan Paetur", "2230") ==
                    "White: Midjord, Joan Paetur (2230)" &&
                gambitree::format_pgn_player(
                    "Black", "Scharf, M.", "?") == "Black: Scharf, M.",
                "dialog player lines append a valid Elo without empty parentheses");
    expect_true(gambitree::format_pgn_player(
                    "White", "Björk Guðmundsdóttir", "") ==
                    "White: Björk Guðmundsdóttir",
                "UTF-8 player names remain intact in dialog metadata");
}

void test_proportional_metrics()
{
    VariableTextMeasurer measurer;
    NotationFontMetrics metrics(8, 18, &measurer);
    std::vector<std::string> lines = NotationLayout::wrap_text("Wi Wi", 18, metrics);
    expect_true(metrics.text_width("Wi") == 14 && lines.size() == 2 && lines[0] == "Wi" && lines[1] == "Wi",
                "proportional text metrics drive wrapping without splitting normal words");
}

void test_compact_metadata_and_notation_lines()
{
    PgnDocument document;
    VariableTextMeasurer measurer;
    NotationFontMetrics metrics(8, 24, &measurer);
    std::vector<MetadataLine> lines;
    NotationLayout layout;
    std::set<int> line_tops;
    std::size_t index;
    expect_true(gambitree::PgnParser::parse(
                    "[White \"Vladimir Petkov\"]\n[WhiteElo \"2512\"]\n"
                    "[Black \"Momchil Nikolov\"]\n[BlackElo \"2450\"]\n"
                    "[Event \"80th ch-BUL 2016\"]\n[Round \"9.3\"]\n"
                    "[Site \"Pleven BUL\"]\n[Date \"2016.04.26\"]\n[Result \"0-1\"]\n\n"
                    "1. d4 Nf6 {comment} (1... d5 $5 (1... e6)) 2. c4 e6 0-1", document),
                "compact metadata PGN parses");
    lines = MetadataLayout::build(*document.games[0], 300, metrics);
    expect_true(lines.size() == 3 && lines[0].text.find("Vladimir Petkov (2512) - Momchil Nikolov (2450)") != std::string::npos &&
                lines[1].text.find("80th ch-BUL 2016 - 9.3") != std::string::npos &&
                lines[2].text.find("Pleven BUL - 2016.04.26 - 0-1") != std::string::npos,
                "metadata packs players/Elo, event/round, and site/date/result without labels");
    expect_true(lines[0].text.find("White:") == std::string::npos && lines[1].text.find("Event:") == std::string::npos,
                "compact metadata omits redundant labels");
    expect_true(gambitree::PgnParser::parse("[White \"Ada\"]\n[Black \"Bob\"]\n[Event \"E\"]\n\n1. e4 e5 *", document),
                "metadata without Elo parses");
    lines = MetadataLayout::build(*document.games[0], 300, metrics);
    expect_true(!lines.empty() && lines[0].text == "Ada - Bob" && lines[0].text.find("()") == std::string::npos,
                "missing Elo creates neither empty parentheses nor empty metadata rows");
    lines = MetadataLayout::build(*document.games[0], 18, metrics);
    expect_true(lines.size() > 2, "narrow metadata uses measured wrapping instead of horizontal overflow");

    const std::string legacy[] = {"\xc2\xa2" "e4", "\xc2\xa3" "d5",
        "\xc2\xa6" "xe5", "\xc2\xa5" "bd2", "\xc2\xa4" "1e2",
        "\xc2\xa7" "e8"};
    const char* letters[] = {"Ke4", "Qd5", "Rxe5", "Bbd2", "N1e2", "Pe8"};
    const char* figurines[] = {"♔e4", "♕d5", "♖xe5", "♗bd2", "♘1e2", "♙e8"};
    for (std::size_t i=0;i<6;++i)
        expect_true(NotationLayout::format_chessbase_references(legacy[i],false)==letters[i] &&
                    NotationLayout::format_chessbase_references(legacy[i],true,63)==figurines[i],
                    "all six legacy ChessBase piece references get letter and figurine display forms");
    expect_true(NotationLayout::format_chessbase_references("$100 and \\A §4",true,63)==
                    "$100 and \\A §4" &&
                NotationLayout::format_chessbase_references("Money £100",true,63)=="Money £100",
                "currency and ordinary Unicode text remain untouched by ChessBase reference presentation");
    expect_true(gambitree::PgnParser::parse(
                    "[Event \"Survey\"]\n[Site \"Opening\"]\n[White \"CBM 64\"]\n"
                    "[Black \"11.\xc2\xa5" "e3 13.---\"]\n[Annotator \"Hazai\"]\n[ECO \"E99\"]\n\n1. e4 *",
                    document), "ChessBase opening-survey PGN parses without metadata changes");
    lines=MetadataLayout::build(*document.games[0],500,metrics,false);
    expect_true(lines.size()>=2 && lines[0].text=="CBM 64 — Opening survey E99" &&
                lines[1].text=="11.Be3 13.--- · Hazai" &&
                *document.games[0]->tag_value("Black")=="11.\xc2\xa5" "e3 13.---",
                "survey heading presents ChessBase references without changing original tags");
    lines=MetadataLayout::build(*document.games[0],500,metrics,true);
    expect_true(lines.size()>=2 && lines[1].text=="11.♗e3 13.--- · Hazai",
                "survey opening references honor the selected figurine style");
    expect_true(gambitree::PgnParser::parse(
                    "[Event \"Survey\"]\n[Site \"Opening\"]\n[White \"Money £100\"]\n"
                    "[Black \"No line\"]\n\n1. e4 *",document),
                "incomplete survey metadata parses");
    lines=MetadataLayout::build(*document.games[0],500,metrics,true);
    expect_true(!lines.empty() && lines[0].text=="Money £100 — Opening survey",
                "ordinary currency in survey title is not reinterpreted as a piece reference");
    expect_true(gambitree::PgnParser::parse(
                    "[Event \"Survey\"]\n[Site \"Opening\"]\n[White \"A very long opening collection title\"]\n"
                    "[Black \"11.¥e3\"]\n\n1. e4 *",document),
                "survey without ECO or annotator parses");
    lines=MetadataLayout::build(*document.games[0],80,metrics,true);
    bool no_empty_annotator=true;
    for(std::size_t i=0;i<lines.size();++i)
        if(lines[i].text.find(" · ")!=std::string::npos)no_empty_annotator=false;
    expect_true(lines.size()>2 && no_empty_annotator,
                "long survey headings wrap and missing ECO or annotator adds no empty metadata");
    expect_true(gambitree::PgnParser::parse(
                    "[White \"Money £100\"]\n[Black \"Bob\"]\n\n1. e4 *",document),
                "ordinary player names with currency parse");
    lines=MetadataLayout::build(*document.games[0],500,metrics,true);
    expect_true(!lines.empty() && lines[0].text=="Money £100 - Bob",
                "ordinary party headers keep their established layout and literal currency");

    expect_true(gambitree::PgnParser::parse(
                    "1. d4 Nf6 {comment words} (1... d5 $5 (1... e6 {nested words})) 2. c4 e6 3. Nc3 *", document),
                "nested notation line-box PGN parses");
    layout.build(*document.games[0], 70, metrics);
    for (index = 0; index < layout.items().size(); ++index) line_tops.insert(layout.items()[index].y);
    {
        std::set<int>::const_iterator previous = line_tops.begin();
        std::set<int>::const_iterator current = previous;
        if (current != line_tops.end()) ++current;
        for (; current != line_tops.end(); ++current, ++previous)
            expect_true(*current - *previous >= metrics.line_height(),
                        "successive wrapped notation line boxes never overlap");
    }
}

}

int main()
{
    source_directory = test_paths::working_directory();
    test_scratch_directory = test_paths::make_directory("gambitree-tests");
    if (source_directory.empty() || test_scratch_directory.empty()) {
        std::cerr << "Cannot create test scratch directory: " << std::strerror(errno) << "\n";
        return 1;
    }
    test_scratch_paths();
    expect_true(std::strcmp(GAMBITREE_NAME, "gambitree") == 0,
                "program name is gambitree");
    expect_true(std::strlen(GAMBITREE_VERSION) > 0,
                "version is non-empty");
    test_fen();
    test_nonstandard_start_positions();
    test_chess960_castling();
    test_moves_and_undo();
    test_special_moves();
    test_legality_and_status();
    test_san();
    test_missing_pawn_capture_file();
    test_lan();
    test_san_check_annotations();
    test_san_error_diagnostics();
    test_partial_games();
    test_analytical_null_moves();
    test_pgn();
    test_pgn_symbolic_suffixes();
    test_pgn_evaluations_and_post_result_analysis();
    test_real_chess960_pgn();
    test_adversarial_pgn_file();
    test_navigation();
    test_carlsen_pelletier_navigation();
    test_carlsen_pelletier_promoted_navigation();
    test_byrne_fischer_navigation();
    test_legacy_comment_presentation();
    test_chessbase_comment_modes_and_context();
    test_figurine_toggle_preserves_notation_scroll();
    test_notation_search_index();
    test_notation_preferences();
    test_notation_readability_settings();
    test_move_selection_geometry();
    test_keyboard_help();
    test_navigation_lists();
    test_figurine_notation();
    test_visited_game_navigation_state();
    test_board_layout_and_assets();
    test_piece_set_registry();
    test_png_resampling();
    test_batched_piece_pixel_composition();
    test_asset_locator();
    test_application_icon();
    test_session_state();
    test_session_autosave();
    test_piece_layout();
    test_piece_alpha_centering();
    test_meridian_piece_geometry();
    test_board_veneer();
    test_cburnett_vertical_placement();
    test_cburnett_assets_and_scaling();
    test_configure_event_batch();
    test_complete_frame_buffer();
    test_piece_cache_key();
    test_deferred_piece_resize();
    test_piece_raster_worklist();
    test_notation_layout();
    test_extended_nag_presentation();
    test_odds_games_presentation();
    test_notation_wrapping();
    test_pgn_library();
    test_pgn_delete_filesystem();
    test_pgn_delete_library();
    test_pgn_delete_partial_boundaries();
    test_pgn_index_headers_and_escape_lines();
    test_pgn_index_numbering_restarts();
    test_pgn_delete_repeated_separators();
    test_pgn_index();
    test_notation_semantics();
    test_pgn_size_limit();
    test_uci_protocol();
    test_engine_display_throttle();
    test_engine_display_snapshots();
    test_engine_pv_preview();
    test_command_line();
    test_board_command_line();
    test_window_geometry();
    test_role_command_line();
    test_window_modes();
    test_keyboard_and_sidebar_layout();
    test_proportional_metrics();
    test_delete_dialog_input_and_label_layout();
    test_delete_dialog_pgn_metadata();
    test_compact_metadata_and_notation_lines();

    if (failures != 0) {
        std::cerr << "Test scratch directory: " << test_scratch_directory << "\n";
        return 1;
    }

    std::cout << "Chess core, PGN, navigation, and board tests passed.\n";
    return 0;
}
