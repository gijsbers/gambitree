# Gambitree build file. Xlib/Xft render the native GUI; libpng decodes piece assets.

CXX ?= g++
CPPFLAGS ?= -Iinclude
CPPFLAGS += -DGAMBITREE_DATA_DIR=\"$(PREFIX)/share/gambitree\"
CXXFLAGS ?= -std=c++98 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?=
X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null)
X11_LIBS := $(shell pkg-config --libs x11 2>/dev/null)
XFT_CFLAGS := $(shell pkg-config --cflags xft fontconfig freetype2 2>/dev/null)
XFT_LIBS := $(shell pkg-config --libs xft fontconfig freetype2 2>/dev/null)
PNG_CFLAGS := $(shell pkg-config --cflags libpng 2>/dev/null)
PNG_LIBS := $(shell pkg-config --libs libpng 2>/dev/null)
ifeq ($(strip $(X11_LIBS)),)
X11_LIBS := -lX11
endif
ifeq ($(strip $(XFT_LIBS)),)
XFT_LIBS := -lXft -lfontconfig -lfreetype
endif
ifeq ($(strip $(PNG_LIBS)),)
PNG_LIBS := -lpng
endif
PREFIX ?= /usr/local
DESTDIR ?=

PROGRAM := gambitree
BUILD_DIR := build
SRC_DIR := src
TEST_DIR := tests
ICON_SIZES := 16 24 32 48 64 96 128 256

SOURCES := $(SRC_DIR)/main.cpp $(SRC_DIR)/command_line.cpp $(SRC_DIR)/window_geometry.cpp $(SRC_DIR)/keyboard_navigation.cpp $(SRC_DIR)/keyboard_help.cpp $(SRC_DIR)/navigation_list.cpp $(SRC_DIR)/wheel_event_batch.cpp $(SRC_DIR)/chess.cpp $(SRC_DIR)/pgn.cpp $(SRC_DIR)/pgn_index.cpp $(SRC_DIR)/pgn_delete.cpp $(SRC_DIR)/pgn_library.cpp $(SRC_DIR)/navigation.cpp $(SRC_DIR)/uci_engine.cpp $(SRC_DIR)/engine_display.cpp $(SRC_DIR)/board_layout.cpp $(SRC_DIR)/board_veneer.cpp $(SRC_DIR)/sidebar_layout.cpp $(SRC_DIR)/piece_layout.cpp $(SRC_DIR)/piece_set.cpp $(SRC_DIR)/png_image.cpp $(SRC_DIR)/application_icon.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/asset_locator.cpp $(SRC_DIR)/notation_layout.cpp $(SRC_DIR)/notation_search.cpp $(SRC_DIR)/metadata_layout.cpp $(SRC_DIR)/board_renderer.cpp $(SRC_DIR)/x11_notation_renderer.cpp $(SRC_DIR)/xft_notation_text.cpp $(SRC_DIR)/session_state.cpp $(SRC_DIR)/x11_window.cpp
OBJECTS := $(BUILD_DIR)/main.o $(BUILD_DIR)/command_line.o $(BUILD_DIR)/window_geometry.o $(BUILD_DIR)/keyboard_navigation.o $(BUILD_DIR)/keyboard_help.o $(BUILD_DIR)/navigation_list.o $(BUILD_DIR)/wheel_event_batch.o $(BUILD_DIR)/chess.o $(BUILD_DIR)/pgn.o $(BUILD_DIR)/pgn_index.o $(BUILD_DIR)/pgn_delete.o $(BUILD_DIR)/pgn_library.o $(BUILD_DIR)/navigation.o $(BUILD_DIR)/uci_engine.o $(BUILD_DIR)/engine_display.o $(BUILD_DIR)/board_layout.o $(BUILD_DIR)/board_veneer.o $(BUILD_DIR)/sidebar_layout.o $(BUILD_DIR)/piece_layout.o $(BUILD_DIR)/piece_set.o $(BUILD_DIR)/png_image.o $(BUILD_DIR)/application_icon.o $(BUILD_DIR)/theme.o $(BUILD_DIR)/asset_locator.o $(BUILD_DIR)/notation_layout.o $(BUILD_DIR)/notation_search.o $(BUILD_DIR)/metadata_layout.o $(BUILD_DIR)/board_renderer.o $(BUILD_DIR)/x11_notation_renderer.o $(BUILD_DIR)/xft_notation_text.o $(BUILD_DIR)/session_state.o $(BUILD_DIR)/x11_window.o
TEST_SOURCES := $(TEST_DIR)/test_main.cpp
TEST_OBJECTS := $(BUILD_DIR)/test_main.o $(BUILD_DIR)/command_line.o $(BUILD_DIR)/window_geometry.o $(BUILD_DIR)/keyboard_navigation.o $(BUILD_DIR)/keyboard_help.o $(BUILD_DIR)/navigation_list.o $(BUILD_DIR)/chess.o $(BUILD_DIR)/pgn.o $(BUILD_DIR)/pgn_index.o $(BUILD_DIR)/pgn_delete.o $(BUILD_DIR)/pgn_library.o $(BUILD_DIR)/navigation.o $(BUILD_DIR)/uci_engine.o $(BUILD_DIR)/engine_display.o $(BUILD_DIR)/board_layout.o $(BUILD_DIR)/board_veneer.o $(BUILD_DIR)/sidebar_layout.o $(BUILD_DIR)/piece_layout.o $(BUILD_DIR)/piece_set.o $(BUILD_DIR)/png_image.o $(BUILD_DIR)/application_icon.o $(BUILD_DIR)/theme.o $(BUILD_DIR)/asset_locator.o $(BUILD_DIR)/notation_layout.o $(BUILD_DIR)/notation_search.o $(BUILD_DIR)/metadata_layout.o $(BUILD_DIR)/session_state.o
TEST_PROGRAM := $(BUILD_DIR)/gambitree-tests

.PHONY: all debug release test test-x11 asan ubsan clean install uninstall test-install

all: $(PROGRAM)

debug: clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -g -O0" all

release: clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -O2" all

asan: clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=address" LDFLAGS="$(LDFLAGS) -fsanitize=address" test

ubsan: clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=undefined" LDFLAGS="$(LDFLAGS) -fsanitize=undefined" test

$(PROGRAM): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(X11_LIBS) $(XFT_LIBS) $(PNG_LIBS)

$(TEST_PROGRAM): $(TEST_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(PNG_LIBS) $(X11_LIBS)

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.cpp include/gambitree/command_line.h include/gambitree/window_geometry.h include/gambitree/version.h include/gambitree/x11_window.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/command_line.o: $(SRC_DIR)/command_line.cpp include/gambitree/command_line.h include/gambitree/window_geometry.h include/gambitree/theme.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/window_geometry.o: $(SRC_DIR)/window_geometry.cpp include/gambitree/window_geometry.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/keyboard_navigation.o: $(SRC_DIR)/keyboard_navigation.cpp include/gambitree/keyboard_navigation.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/keyboard_help.o: $(SRC_DIR)/keyboard_help.cpp include/gambitree/keyboard_help.h include/gambitree/notation_layout.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/navigation_list.o: $(SRC_DIR)/navigation_list.cpp include/gambitree/navigation_list.h include/gambitree/pgn.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/wheel_event_batch.o: $(SRC_DIR)/wheel_event_batch.cpp include/gambitree/wheel_event_batch.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/chess.o: $(SRC_DIR)/chess.cpp include/gambitree/chess.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/pgn.o: $(SRC_DIR)/pgn.cpp include/gambitree/pgn.h include/gambitree/chess.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/pgn_library.o: $(SRC_DIR)/pgn_library.cpp include/gambitree/pgn_library.h include/gambitree/pgn_delete.h include/gambitree/pgn.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

# These callers also depend on the embedded PgnIndex/PgnFile layout.
$(BUILD_DIR)/pgn_library.o $(BUILD_DIR)/x11_window.o $(BUILD_DIR)/test_main.o: include/gambitree/pgn_index.h

$(BUILD_DIR)/pgn_index.o: $(SRC_DIR)/pgn_index.cpp include/gambitree/pgn_index.h include/gambitree/pgn.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/pgn_delete.o: $(SRC_DIR)/pgn_delete.cpp include/gambitree/pgn_delete.h include/gambitree/pgn_index.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/navigation.o: $(SRC_DIR)/navigation.cpp include/gambitree/navigation.h include/gambitree/pgn.h include/gambitree/chess.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/uci_engine.o: $(SRC_DIR)/uci_engine.cpp include/gambitree/uci_engine.h include/gambitree/chess.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/engine_display.o: $(SRC_DIR)/engine_display.cpp include/gambitree/engine_display.h include/gambitree/uci_engine.h include/gambitree/notation_layout.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/board_veneer.o: $(SRC_DIR)/board_veneer.cpp include/gambitree/board_veneer.h include/gambitree/board_layout.h include/gambitree/png_image.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/board_layout.o: $(SRC_DIR)/board_layout.cpp include/gambitree/board_layout.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/sidebar_layout.o: $(SRC_DIR)/sidebar_layout.cpp include/gambitree/sidebar_layout.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/piece_layout.o: $(SRC_DIR)/piece_layout.cpp include/gambitree/piece_layout.h include/gambitree/piece_set.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/piece_set.o: $(SRC_DIR)/piece_set.cpp include/gambitree/piece_set.h include/gambitree/chess.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/png_image.o: $(SRC_DIR)/png_image.cpp include/gambitree/png_image.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(PNG_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/application_icon.o: $(SRC_DIR)/application_icon.cpp include/gambitree/application_icon.h include/gambitree/asset_locator.h include/gambitree/png_image.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(PNG_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/theme.o: $(SRC_DIR)/theme.cpp include/gambitree/theme.h include/gambitree/chess.h include/gambitree/png_image.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/asset_locator.o: $(SRC_DIR)/asset_locator.cpp include/gambitree/asset_locator.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/notation_layout.o: $(SRC_DIR)/notation_layout.cpp include/gambitree/notation_layout.h include/gambitree/pgn.h include/gambitree/chess.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/notation_search.o: $(SRC_DIR)/notation_search.cpp include/gambitree/notation_search.h include/gambitree/notation_layout.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/metadata_layout.o: $(SRC_DIR)/metadata_layout.cpp include/gambitree/metadata_layout.h include/gambitree/notation_layout.h include/gambitree/pgn.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/session_state.o: $(SRC_DIR)/session_state.cpp include/gambitree/session_state.h include/gambitree/piece_set.h include/gambitree/theme.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/board_renderer.o: $(SRC_DIR)/board_renderer.cpp include/gambitree/board_renderer.h include/gambitree/board_veneer.h include/gambitree/asset_locator.h include/gambitree/board_layout.h include/gambitree/chess.h include/gambitree/piece_cache_key.h include/gambitree/piece_layout.h include/gambitree/piece_raster_worklist.h include/gambitree/piece_set.h include/gambitree/pixel_composite.h include/gambitree/png_image.h include/gambitree/theme.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(XFT_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/xft_notation_text.o: $(SRC_DIR)/xft_notation_text.cpp include/gambitree/xft_notation_text.h include/gambitree/notation_layout.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(XFT_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/x11_notation_renderer.o: $(SRC_DIR)/x11_notation_renderer.cpp include/gambitree/x11_notation_renderer.h include/gambitree/xft_notation_text.h include/gambitree/notation_layout.h include/gambitree/notation_search.h include/gambitree/pgn.h include/gambitree/keyboard_help.h include/gambitree/navigation_list.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(XFT_CFLAGS) $(CXXFLAGS) -c -o $@ $<


$(BUILD_DIR)/x11_window.o: $(SRC_DIR)/x11_window.cpp include/gambitree/x11_window.h include/gambitree/window_geometry.h include/gambitree/application_icon.h include/gambitree/asset_locator.h include/gambitree/board_renderer.h include/gambitree/chess.h include/gambitree/complete_frame_buffer.h include/gambitree/deferred_piece_resize.h include/gambitree/navigation.h include/gambitree/notation_layout.h include/gambitree/notation_search.h include/gambitree/pgn.h include/gambitree/piece_set.h include/gambitree/session_state.h include/gambitree/x11_notation_renderer.h include/gambitree/keyboard_help.h include/gambitree/navigation_list.h include/gambitree/engine_display.h include/gambitree/uci_engine.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(XFT_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/test_main.o: $(TEST_DIR)/test_main.cpp include/gambitree/command_line.h include/gambitree/window_geometry.h include/gambitree/board_veneer.h include/gambitree/version.h include/gambitree/chess.h include/gambitree/pgn.h include/gambitree/navigation.h include/gambitree/board_layout.h include/gambitree/piece_cache_key.h include/gambitree/piece_layout.h include/gambitree/piece_raster_worklist.h include/gambitree/piece_set.h include/gambitree/pixel_composite.h include/gambitree/png_image.h include/gambitree/theme.h include/gambitree/notation_layout.h include/gambitree/notation_search.h include/gambitree/session_state.h include/gambitree/configure_event_batch.h include/gambitree/complete_frame_buffer.h include/gambitree/deferred_piece_resize.h include/gambitree/pgn_delete.h include/gambitree/keyboard_help.h include/gambitree/navigation_list.h include/gambitree/keyboard_navigation.h include/gambitree/engine_display.h include/gambitree/uci_engine.h $(TEST_DIR)/test_paths.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/test_x11_notation.o: $(TEST_DIR)/test_x11_notation.cpp include/gambitree/session_state.h include/gambitree/board_renderer.h include/gambitree/x11_notation_renderer.h include/gambitree/xft_notation_text.h include/gambitree/notation_layout.h include/gambitree/notation_search.h include/gambitree/keyboard_help.h include/gambitree/engine_display.h include/gambitree/uci_engine.h $(TEST_DIR)/test_paths.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(XFT_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/gambitree-x11-tests: $(BUILD_DIR)/test_x11_notation.o $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/x11_window.o,$(OBJECTS))
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(X11_LIBS) $(XFT_LIBS) $(PNG_LIBS)

test-x11: $(PROGRAM) $(BUILD_DIR)/gambitree-x11-tests
	TMPDIR="$${TMPDIR:-$(BUILD_DIR)}" $(BUILD_DIR)/gambitree-x11-tests

$(BUILD_DIR):
	mkdir -p $@

test: $(PROGRAM) $(TEST_PROGRAM)
	TMPDIR="$${TMPDIR:-$(BUILD_DIR)}" $(TEST_PROGRAM)
	TMPDIR="$${TMPDIR:-$(BUILD_DIR)}" ./tests/test_manpage.sh

clean:
	rm -rf $(BUILD_DIR) $(PROGRAM)

install: $(PROGRAM)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(PROGRAM) "$(DESTDIR)$(PREFIX)/bin/$(PROGRAM)"
	install -d "$(DESTDIR)$(PREFIX)/share/man/man1"
	install -m 644 doc/gambitree.1 "$(DESTDIR)$(PREFIX)/share/man/man1/gambitree.1"
	install -d "$(DESTDIR)$(PREFIX)/share/applications"
	install -m 644 applications/gambitree.desktop "$(DESTDIR)$(PREFIX)/share/applications/gambitree.desktop"
	install -d "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/classic"
	install -m 644 assets/pieces/classic/*.png "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/classic"
	install -d "$(DESTDIR)$(PREFIX)/share/gambitree/boards"
	install -m 644 assets/boards/wood-light.png assets/boards/wood-dark.png assets/boards/wood-veneer-light.png assets/boards/wood-veneer-dark.png "$(DESTDIR)$(PREFIX)/share/gambitree/boards"
	install -d "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/meridian-shaded"
	install -m 644 assets/pieces/meridian-shaded/*.png "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/meridian-shaded"
	install -d "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/cburnett"
	install -m 644 assets/pieces/cburnett/*.png "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/cburnett"
	for size in $(ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 assets/icons/gambitree-$$size.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/gambitree.png"; \
	done
	install -d "$(DESTDIR)$(PREFIX)/share/doc/gambitree"
	install -m 644 LICENSE THIRD-PARTY-NOTICES "$(DESTDIR)$(PREFIX)/share/doc/gambitree"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(PROGRAM)"
	rm -f "$(DESTDIR)$(PREFIX)/share/man/man1/gambitree.1"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/gambitree.desktop"
	rmdir "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/LICENSE" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/THIRD-PARTY-NOTICES"
	# Also clean documentation installed by earlier versions.
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/assets-README.md"
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/wood-veneer/README.md" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/wood-veneer/SOURCES.json" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/wood-veneer/SHA256SUMS" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/wood-veneer/prepare.py"
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/wood-veneer/LICENSE"
	rmdir "$(DESTDIR)$(PREFIX)/share/doc/gambitree/wood-veneer" 2>/dev/null || true
	rm -f "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/classic/"*.png
	rm -f "$(DESTDIR)$(PREFIX)/share/gambitree/boards/wood-light.png" "$(DESTDIR)$(PREFIX)/share/gambitree/boards/wood-dark.png" "$(DESTDIR)$(PREFIX)/share/gambitree/boards/wood-veneer-light.png" "$(DESTDIR)$(PREFIX)/share/gambitree/boards/wood-veneer-dark.png"
	rmdir "$(DESTDIR)$(PREFIX)/share/gambitree/boards" 2>/dev/null || true
	rm -f "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/meridian-shaded/"*.png
	rmdir "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/meridian-shaded" 2>/dev/null || true
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/meridian-shaded/README.md"
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/meridian-shaded/source/LICENSE"
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/meridian-shaded/source/"*.svg
	rmdir "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/meridian-shaded/source" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/meridian-shaded" 2>/dev/null || true
	rm -f "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/cburnett/"*.png
	rmdir "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/cburnett" 2>/dev/null || true
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett/README.md" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett/LICENSE" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett/SOURCES.sha1" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett/SHA256SUMS" "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett/render.sh"
	rm -f "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett/source/"*.svg
	rmdir "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett/source" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party/cburnett" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/doc/gambitree/third-party" 2>/dev/null || true
	for size in $(ICON_SIZES); do \
		rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/gambitree.png"; \
		rmdir "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps" 2>/dev/null || true; \
		rmdir "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}" 2>/dev/null || true; \
	done
	rmdir "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/icons" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/gambitree/pieces/classic" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/gambitree/pieces" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/gambitree" 2>/dev/null || true
	rmdir "$(DESTDIR)$(PREFIX)/share/doc/gambitree" 2>/dev/null || true

test-install: $(PROGRAM) $(TEST_PROGRAM)
	set -e; stage=$$(mktemp -d "$${TMPDIR:-$(BUILD_DIR)}/gambitree-install.XXXXXX"); trap 'rm -rf "$$stage"' EXIT; stage=$$(CDPATH= cd "$$stage" && pwd); \
	$(MAKE) install PREFIX=/usr DESTDIR="$$stage"; \
	test -x "$$stage/usr/bin/$(PROGRAM)"; \
	test -f "$$stage/usr/share/man/man1/gambitree.1"; \
	test -f "$$stage/usr/share/applications/gambitree.desktop"; \
	test -f "$$stage/usr/share/gambitree/pieces/classic/white-king.png"; \
	test -f "$$stage/usr/share/gambitree/boards/wood-veneer-light.png"; \
	test -f "$$stage/usr/share/gambitree/boards/wood-veneer-dark.png"; \
	cmp LICENSE "$$stage/usr/share/doc/gambitree/LICENSE"; \
	cmp THIRD-PARTY-NOTICES "$$stage/usr/share/doc/gambitree/THIRD-PARTY-NOTICES"; \
	test $$(find "$$stage/usr/share/doc/gambitree" -type f | wc -l) -eq 2; \
	test ! -d "$$stage/usr/share/doc/gambitree/wood-veneer"; \
	test ! -d "$$stage/usr/share/doc/gambitree/third-party"; \
	sed -n '/^Copyright /,$$p' assets/pieces/cburnett/LICENSE > "$$stage/cburnett-license"; \
	sed -n '/^Copyright /,$$p' "$$stage/usr/share/doc/gambitree/THIRD-PARTY-NOTICES" | cmp - "$$stage/cburnett-license"; \
	(cd "$$stage/usr/share/gambitree/boards" && sha256sum -c "$(CURDIR)/assets/boards/wood-veneer/SHA256SUMS"); \
	test -f "$$stage/usr/share/gambitree/pieces/meridian-shaded/white-king.png"; \
	test -f "$$stage/usr/share/gambitree/pieces/meridian-shaded/black-pawn.png"; \
	test -f "$$stage/usr/share/gambitree/pieces/cburnett/white-king.png"; \
	test -f "$$stage/usr/share/gambitree/pieces/cburnett/black-pawn.png"; \
	(cd assets/pieces/cburnett/source && sha1sum -c ../SOURCES.sha1); \
	sed '/  source\//d' assets/pieces/cburnett/SHA256SUMS | (cd "$$stage/usr/share/gambitree/pieces/cburnett" && sha256sum -c -); \
	GAMBITREE_TEST_INSTALLED_PIECES="$$stage/usr/share/gambitree/pieces" TMPDIR="$${TMPDIR:-$(BUILD_DIR)}" $(TEST_PROGRAM); \
	for size in $(ICON_SIZES); do test -f "$$stage/usr/share/icons/hicolor/$${size}x$${size}/apps/gambitree.png"; done; \
	$(MAKE) uninstall PREFIX=/usr DESTDIR="$$stage"; \
	test ! -e "$$stage/usr/bin/$(PROGRAM)"; \
	test ! -e "$$stage/usr/share/man/man1/gambitree.1"; \
	test ! -e "$$stage/usr/share/applications/gambitree.desktop"; \
	test ! -e "$$stage/usr/share/gambitree/pieces/classic/white-king.png"; \
	test ! -e "$$stage/usr/share/gambitree/pieces/meridian-shaded/white-king.png"; \
	test ! -e "$$stage/usr/share/gambitree/pieces/cburnett/white-king.png"; \
	test ! -e "$$stage/usr/share/doc/gambitree"; \
	for size in $(ICON_SIZES); do test ! -e "$$stage/usr/share/icons/hicolor/$${size}x$${size}/apps/gambitree.png"; done
