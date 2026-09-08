# GRAVITON — build
#
#   make            build the game
#   make test       build and run the full test suite
#   make sanitize   run the suite under AddressSanitizer + UBSan
#   make smoke      drive the whole application headlessly through every screen
#   make tools      build the screenshot and balance-probe utilities
#   make app        build GRAVITON.app (macOS only)
#   make dmg        build a distributable disk image (macOS only)
#   make clean      remove build output
#
# The only external dependency is SDL2.

NAME      := graviton
BUILD     := build
BIN       := $(BUILD)/$(NAME)

CC        ?= cc
UNAME_S   := $(shell uname -s)

# --------------------------------------------------------------------- flags
# -ffp-contract=off keeps the compiler from fusing multiply-adds, which would
# otherwise let an optimised build drift from a debug one and break the
# determinism the simulation and its replay tests depend on.
CSTD      := -std=c11
WARN      := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wpointer-arith \
             -Wcast-align -Wwrite-strings -Wno-unused-parameter
OPT       ?= -O2
CFLAGS    := $(CSTD) $(WARN) $(OPT) -g -ffp-contract=off -fno-common
LDFLAGS   :=
LIBS      := -lm

# ----------------------------------------------------------------- SDL2 setup
# Try pkg-config, then sdl2-config, then a macOS framework. Failing all three
# we emit a clear message rather than a wall of missing-header errors.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

ifeq ($(strip $(SDL_LIBS)),)
  SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
  SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)
endif

ifeq ($(strip $(SDL_LIBS)),)
  ifeq ($(UNAME_S),Darwin)
    ifneq ($(wildcard /Library/Frameworks/SDL2.framework),)
      SDL_CFLAGS := -F/Library/Frameworks -I/Library/Frameworks/SDL2.framework/Headers
      SDL_LIBS   := -F/Library/Frameworks -framework SDL2
    endif
  endif
endif

ifeq ($(UNAME_S),Darwin)
  # Universal by default so one binary covers Apple silicon and Intel.
  ARCHS ?= -arch arm64 -arch x86_64
  CFLAGS  += $(ARCHS) -mmacosx-version-min=11.0
  LDFLAGS += $(ARCHS) -mmacosx-version-min=11.0
endif

# Appended, not assigned, so the packaging script can add flags (an @rpath for
# the bundled SDL2, say) without discarding anything set above. Overriding
# CFLAGS or LDFLAGS from the command line would silently drop the arch and
# deployment-target flags along with them.
EXTRA_CFLAGS  ?=
EXTRA_LDFLAGS ?=
CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

# --------------------------------------------------------------------- source
CORE_SRC  := $(wildcard src/core/*.c) $(wildcard src/game/*.c)
APP_SRC   := $(wildcard src/render/*.c) $(wildcard src/audio/*.c) $(wildcard src/app/*.c) \
             src/main.c
GAME_SRC  := $(CORE_SRC) $(APP_SRC)

# gv_save.c is the save format and the score table with no SDL in it (locating
# the file on disk lives in gv_save_path.c), so the suite can link it directly
# and feed the parser deliberately corrupt files.
TEST_SRC  := $(wildcard tests/*.c) $(CORE_SRC) src/app/gv_save.c

CORE_OBJ  := $(CORE_SRC:%.c=$(BUILD)/%.o)
GAME_OBJ  := $(GAME_SRC:%.c=$(BUILD)/%.o)

# ---------------------------------------------------------------------- rules
.PHONY: all check-sdl test sanitize smoke tools clean app dmg run help

all: check-sdl $(BIN)

check-sdl:
ifeq ($(strip $(SDL_LIBS)),)
	@echo "SDL2 was not found."
	@echo "  macOS:  brew install sdl2      (or run tools/build_macos.sh, which fetches it)"
	@echo "  Debian: sudo apt install libsdl2-dev"
	@exit 1
endif

$(BIN): $(GAME_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(SDL_LIBS) $(LIBS)
	@echo "built $@"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

run: all
	./$(BIN)

# The test suite links only the platform-independent simulation, so it builds
# and runs anywhere — no SDL, no window, no audio device.
test: $(BUILD)/tests/runner
	@./$(BUILD)/tests/runner

$(BUILD)/tests/runner: $(TEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $^ $(LIBS)

sanitize:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CSTD) $(WARN) -O1 -g -fno-omit-frame-pointer \
	  -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -Itests -o $(BUILD)/tests/runner-asan $(TEST_SRC) $(LIBS)
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
	  ./$(BUILD)/tests/runner-asan

# The suite above never touches the window, the menus, the mixer or the save
# file. This one drives the real application headlessly through every screen.
smoke: check-sdl $(BUILD)/gv_smoke
	@mkdir -p $(BUILD)/shots
	@./$(BUILD)/gv_smoke $(BUILD)/shots

$(BUILD)/gv_smoke: tools/gv_smoke.c $(filter-out src/main.c,$(GAME_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $^ $(SDL_LIBS) $(LIBS)

tools: check-sdl $(BUILD)/gv_shot $(BUILD)/gv_balance $(BUILD)/gv_smoke $(BUILD)/gv_icon

# The application icon is drawn with the game's own renderer rather than
# shipped as an image file, so it can never drift from the art direction.
$(BUILD)/gv_icon: tools/gv_icon.c $(wildcard src/render/*.c) $(CORE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $^ $(SDL_LIBS) $(LIBS)

$(BUILD)/gv_shot: tools/gv_shot.c $(CORE_SRC) $(wildcard src/render/*.c)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $^ $(SDL_LIBS) $(LIBS)

$(BUILD)/gv_balance: tools/gv_balance.c $(CORE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

app:
	@tools/build_macos.sh

dmg:
	@tools/build_macos.sh --dmg

clean:
	rm -rf $(BUILD) dist

help:
	@sed -n '2,12p' Makefile

-include $(GAME_OBJ:.o=.d) $(CORE_OBJ:.o=.d)
