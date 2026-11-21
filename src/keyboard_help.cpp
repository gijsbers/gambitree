#include "gambitree/keyboard_help.h"
#include <X11/keysym.h>
#include <algorithm>

namespace gambitree {
namespace {
const KeyboardHelpEntry entries[] = {
    {"Replay a game", "Right Arrow", "Next move."},
    {"Replay a game", "Left Arrow", "Previous move."},
    {"Replay a game", "Home", "Starting position."},
    {"Replay a game", "End", "Last move in the chosen line."},
    {"Explore alternative moves", "Up Arrow", "Return to where the current alternative starts."},
    {"Explore alternative moves", "Down Arrow", "Next alternative here; if none and inside an alternative, skip past it."},
    {"Games and files", "Page Up / Down", "Previous / next game."},
    {"Games and files", "Ctrl + PgUp / PgDn", "Previous / next file."},
    {"Games and files", "Ctrl + G", "Choose a game from this PGN file."},
    {"Games and files", "Ctrl + Shift + G", "Choose a PGN file opened at startup."},
    {"In a game or file list", "Type", "Filter the list as you type."},
    {"In a game or file list", "Up / Down", "Select an item; Enter opens it."},
    {"In a game or file list", "Escape", "Close the list without changing the game."},
    {"Scroll notation", "Shift + Page Up / Down", "Scroll the notation one screen up or down."},
    {"Search notation", "Ctrl + F", "Find text in moves, alternatives and comments."},
    {"Search notation", "Enter / F3", "Find the next match. Shift goes to the previous match."},
    {"Search notation", "Escape", "Close the search bar and keep your place."},
    {"In a removal confirmation", "Tab / Left / Right", "Choose a button. Shift + Tab also works."},
    {"In a removal confirmation", "Enter", "Use the selected button."},
    {"In a removal confirmation", "Escape / N", "Cancel removal."},
    {"In a removal confirmation", "Y", "Confirm permanent removal."},
    {"Appearance", "G", "Switch between letters and chess figurines."},
    {"Appearance", "Plus / Minus", "Resize notation text. = also increases it."},
    {"Appearance", "Ctrl + Plus / Minus", "Resize engine text. Ctrl + = also increases it."},
    {"Appearance", "F", "Flip the chessboard."},
    {"Appearance", "B", "Cycle Classic, Wood and Wood Veneer boards."},
    {"Appearance", "P", "Cycle Gambitree Classic, Meridian Shaded and Cburnett pieces."},
    {"Engine", "A", "Turn engine analysis on or off."},
    {"Other", "Ctrl + Delete", "Remove the selected game after confirmation. This cannot be undone."},
    {"Other", "Q / Escape", "Quit Gambitree when help is closed."},
    {"Other", "?", "Show or hide this guide."}
};
void add(std::vector<KeyboardHelpText>& output, KeyboardHelpTextKind kind,
         const std::string& text, int x, int y, const NotationFontMetrics& metrics)
{
    KeyboardHelpText line;
    line.kind=kind; line.text=text; line.x=x; line.y=y;
    line.width=metrics.text_width(text); line.height=metrics.line_height();
    output.push_back(line);
}
int wrapped(std::vector<KeyboardHelpText>& output, KeyboardHelpTextKind kind,
            const std::string& text, int x, int y, int width,
            const NotationFontMetrics& metrics)
{
    std::vector<std::string> rows=NotationLayout::wrap_text(text,width,metrics);
    for(std::size_t i=0;i<rows.size();++i) {
        add(output,kind,rows[i],x,y,metrics);
        y+=metrics.line_height();
    }
    return y;
}
}
const KeyboardHelpEntry* keyboard_help_entries(std::size_t& count)
{ count=sizeof(entries)/sizeof(entries[0]); return entries; }

bool keyboard_help_requested(KeySym translated, const char* text, int length)
{ return translated==XK_question || (text && length==1 && text[0]=='?'); }

void KeyboardHelpState::scroll_by(int distance, int maximum_scroll)
{
    scroll+=distance;
    if(scroll<0)scroll=0;
    if(scroll>maximum_scroll)scroll=maximum_scroll;
}
bool KeyboardHelpState::key(KeySym translated, bool question, bool confirmation,
                            int line_height, int viewport_height, int maximum_scroll)
{
    if(confirmation)return false;
    if(question) { visible=!visible; scroll=0; return true; }
    if(!visible)return false;
    if(translated==XK_Escape) { visible=false; return true; }
    int page=std::max(line_height,viewport_height-line_height);
    if(translated==XK_Up)scroll_by(-line_height,maximum_scroll);
    if(translated==XK_Down)scroll_by(line_height,maximum_scroll);
    if(translated==XK_Page_Up)scroll_by(-page,maximum_scroll);
    if(translated==XK_Page_Down)scroll_by(page,maximum_scroll);
    if(translated==XK_Home)scroll=0;
    if(translated==XK_End)scroll=maximum_scroll;
    return true;
}
int KeyboardHelpLayout::viewport_height() const { return body_bottom-body_top; }
int KeyboardHelpLayout::max_scroll() const { return std::max(0,content_height-viewport_height()); }

KeyboardHelpLayout KeyboardHelpLayout::build(int window_width, int window_height,
    const NotationFontMetrics& body, const NotationFontMetrics& keys,
    const NotationFontMetrics& title)
{
    KeyboardHelpLayout result;
    int margin=window_width>=160 && window_height>=160 ? 12 : 0;
    result.width=std::max(1,std::min(1360,window_width-2*margin));
    result.height=std::max(1,window_height-2*margin);
    result.x=(window_width-result.width)/2; result.y=margin;
    int padding=result.width>=100 ? 16 : 0;
    int left=result.x+padding, available=std::max(1,result.width-2*padding);
    int top=result.y+padding;
    top=wrapped(result.fixed,HELP_TITLE,"Keyboard help",left,top,available,title);
    top=wrapped(result.fixed,HELP_CLOSE,"Press ? or Esc to close",left,top,available,body);
    result.body_top=top+8;
    result.line_height=body.line_height();
    result.content_height=0;
    result.columns=available>=960 ? 2 : 1;
    int gap=32, column_width=(available-(result.columns-1)*gap)/result.columns;
    std::size_t count;
    const KeyboardHelpEntry* table=keyboard_help_entries(count);
    std::size_t split=count;
    for(std::size_t i=0;i<count;++i)
        if(std::string(table[i].group)=="Appearance") { split=i; break; }
    for(int column=0;column<result.columns;++column) {
        std::size_t begin=column==0?0:split;
        std::size_t end=column==0?split:count;
        int column_left=left+column*(column_width+gap);
        int max_key=0;
        for(std::size_t i=begin;i<end;++i)
            max_key=std::max(max_key,keys.text_width(table[i].keys));
        int key_column=std::min(max_key+12,std::max(72,column_width*42/100));
        if(column_width<180) key_column=std::max(1,column_width/2);
        int description_x=column_left+key_column+12;
        int description_width=std::max(1,column_left+column_width-description_x);
        int row=0;
        std::string previous_group;
        for(std::size_t i=begin;i<end;++i) {
            const KeyboardHelpEntry& entry=table[i];
            std::string group=entry.group;
            if(previous_group!=group) {
                if(!previous_group.empty()) row+=result.line_height/2;
                row=wrapped(result.content,HELP_GROUP,group,column_left,row,column_width,keys);
                previous_group=group;
            }
            int key_bottom=wrapped(result.content,HELP_KEY,entry.keys,
                                   column_left,row,key_column,keys);
            int description_bottom=wrapped(result.content,HELP_DESCRIPTION,
                                           entry.description,description_x,row,
                                           description_width,body);
            row=std::max(key_bottom,description_bottom)+4;
        }
        result.content_height=std::max(result.content_height,row);
    }
    const std::string scrolling="Scroll: Up/Down, Page Up/Down, Home/End or mouse wheel.";
    const std::string tip="Tip: drag the divider to resize the board and notation.";
    std::string footer=tip;
    int bottom=result.y+result.height-padding;
    int footer_lines=static_cast<int>(NotationLayout::wrap_text(footer,available,body).size());
    if(result.content_height>bottom-footer_lines*result.line_height-8-result.body_top)footer=scrolling;
    footer_lines=static_cast<int>(NotationLayout::wrap_text(footer,available,body).size());
    if(bottom-footer_lines*result.line_height-8-result.body_top<result.line_height) {
        footer="Up/Down: scroll";
        footer_lines=static_cast<int>(NotationLayout::wrap_text(footer,available,body).size());
    }
    int footer_top=bottom-footer_lines*result.line_height;
    int viewport=std::max(0,footer_top-8-result.body_top);
    result.body_bottom=result.body_top+(viewport/result.line_height)*result.line_height;
    wrapped(result.fixed,HELP_FOOTER,footer,left,footer_top,available,body);
    return result;
}
}
