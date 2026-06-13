#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# HelixScreen Worktree Setup Script
# Creates or configures a git worktree for fast isolated builds

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

usage() {
    echo "Usage: $0 [OPTIONS] <branch-name> [worktree-path]"
    echo ""
    echo "Creates or sets up a git worktree for fast isolated builds."
    echo ""
    echo "Arguments:"
    echo "  branch-name     Branch to checkout (will be created if it doesn't exist)"
    echo "  worktree-path   Path for worktree (default: .worktrees/<branch-name>)"
    echo ""
    echo "Options:"
    echo "  --setup-only    Only set up an existing worktree, don't create it"
    echo "  --no-build      Skip the initial build after setup"
    echo "  -h, --help      Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 feature/new-panel           # Create worktree in .worktrees/new-panel"
    echo "  $0 feature/foo /tmp/foo        # Create worktree in /tmp/foo"
    echo "  $0 --setup-only feature/i18n   # Just set up existing worktree"
    echo ""
    echo "Strategy:"
    echo "  - Configures ccache for cross-worktree reuse (no cold rebuild per worktree)"
    echo "  - Clones build/obj/ from main tree (APFS copy-on-write — instant, zero disk)"
    echo "  - Symlinks lib/ from main tree (all submodule sources + generated headers)"
    echo "  - Symlinks compiled libraries (libhv.a) and PCH"
    echo "  - Copies compile_commands.json with rewritten paths for clangd"
    echo "  - Symlinks node_modules and .venv for font/python tools"
    echo "  - Uses .git/info/exclude for clean git status"
}

# Parse arguments
SETUP_ONLY=false
NO_BUILD=false
BRANCH=""
WORKTREE_PATH=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --setup-only)
            SETUP_ONLY=true
            shift
            ;;
        --no-build)
            NO_BUILD=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo -e "${RED}Unknown option: $1${RESET}"
            usage
            exit 1
            ;;
        *)
            if [[ -z "$BRANCH" ]]; then
                BRANCH="$1"
            elif [[ -z "$WORKTREE_PATH" ]]; then
                WORKTREE_PATH="$1"
            else
                echo -e "${RED}Too many arguments${RESET}"
                usage
                exit 1
            fi
            shift
            ;;
    esac
done

# Get the main tree root (where this script lives)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN_TREE="$(cd "$SCRIPT_DIR/.." && pwd)"

# Auto-detect: if run from inside an existing worktree with no args, set up in-place
if [[ -z "$BRANCH" ]]; then
    # Check if we're inside a git worktree (not the main tree)
    CURRENT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    GIT_COMMON="$(git rev-parse --git-common-dir 2>/dev/null || true)"
    if [[ -n "$CURRENT_ROOT" && -n "$GIT_COMMON" ]] && \
       [[ "$(cd "$CURRENT_ROOT" && pwd)" != "$(cd "$GIT_COMMON/.." && pwd)" ]]; then
        # We're in a worktree — infer branch and path
        BRANCH="$(git rev-parse --abbrev-ref HEAD)"
        WORKTREE_PATH="$CURRENT_ROOT"
        # Override MAIN_TREE: SCRIPT_DIR/.. would resolve to the worktree root,
        # not the actual main tree. Use git-common-dir to find the real main tree.
        MAIN_TREE="$(cd "$GIT_COMMON/.." && pwd)"
        SETUP_ONLY=true
        echo -e "${YELLOW}Auto-detected worktree: $WORKTREE_PATH (branch: $BRANCH)${RESET}"
        echo -e "${YELLOW}Main tree: $MAIN_TREE${RESET}"
    else
        echo -e "${RED}Error: branch-name is required (or run from inside a worktree)${RESET}"
        usage
        exit 1
    fi
fi

# Default worktree path: .worktrees/<branch-basename>
if [[ -z "$WORKTREE_PATH" ]]; then
    # Extract just the last part of the branch name (e.g., feature/foo -> foo)
    BRANCH_BASENAME="${BRANCH##*/}"
    WORKTREE_PATH="$MAIN_TREE/.worktrees/$BRANCH_BASENAME"
fi

# Make worktree path absolute
if [[ ! "$WORKTREE_PATH" = /* ]]; then
    WORKTREE_PATH="$MAIN_TREE/$WORKTREE_PATH"
fi

echo -e "${BOLD}${CYAN}HelixScreen Worktree Setup${RESET}"
echo -e "Main tree:    $MAIN_TREE"
echo -e "Worktree:     $WORKTREE_PATH"
echo -e "Branch:       $BRANCH"
echo ""

# Step 1: Create or verify the worktree
if [[ "$SETUP_ONLY" == "false" ]]; then
    if [[ -d "$WORKTREE_PATH" ]]; then
        echo -e "${YELLOW}Worktree already exists at $WORKTREE_PATH${RESET}"
    else
        echo -e "${CYAN}Creating worktree...${RESET}"
        mkdir -p "$(dirname "$WORKTREE_PATH")"

        # Check if branch exists
        if git -C "$MAIN_TREE" rev-parse --verify "$BRANCH" >/dev/null 2>&1; then
            git -C "$MAIN_TREE" worktree add "$WORKTREE_PATH" "$BRANCH"
        else
            echo -e "${YELLOW}Branch '$BRANCH' doesn't exist, creating from current HEAD...${RESET}"
            git -C "$MAIN_TREE" worktree add -b "$BRANCH" "$WORKTREE_PATH"
        fi
        echo -e "${GREEN}✓ Worktree created${RESET}"
    fi
else
    if [[ ! -d "$WORKTREE_PATH" ]]; then
        echo -e "${RED}Error: Worktree doesn't exist at $WORKTREE_PATH${RESET}"
        echo -e "Use without --setup-only to create it"
        exit 1
    fi
fi

cd "$WORKTREE_PATH"

# Step 2: Symlink lib/ submodules from main tree (instead of cloning fresh)
# This includes source headers AND generated files (like libhv/include/hv/)
# We symlink each submodule directory individually to preserve lib/ structure
echo -e "${CYAN}Symlinking lib/ submodules from main tree...${RESET}"

# Get list of submodules in lib/
SUBMODULES=$(git -C "$MAIN_TREE" config --file .gitmodules --get-regexp path | grep "^submodule\." | awk '{print $2}' | grep "^lib/")
# Also include non-submodule files in lib/
LIB_ITEMS=("tuibox.h" "mdns")

# Ensure lib/ directory exists
mkdir -p "$WORKTREE_PATH/lib"

# Symlink each submodule directory
for submod in $SUBMODULES; do
    SUBMOD_NAME=$(basename "$submod")
    MAIN_SUBMOD="$MAIN_TREE/$submod"
    WORKTREE_SUBMOD="$WORKTREE_PATH/$submod"

    if [[ -L "$WORKTREE_SUBMOD" ]]; then
        echo -e "  $submod: ${GREEN}already symlinked${RESET}"
    elif [[ -d "$WORKTREE_SUBMOD" ]]; then
        echo -e "  $submod: ${YELLOW}replacing with symlink${RESET}"
        rm -rf "$WORKTREE_SUBMOD"
        ln -s "$MAIN_SUBMOD" "$WORKTREE_SUBMOD"
    else
        ln -s "$MAIN_SUBMOD" "$WORKTREE_SUBMOD"
        echo -e "  $submod: ${GREEN}symlinked${RESET}"
    fi
done

# Symlink non-submodule items
for item in "${LIB_ITEMS[@]}"; do
    MAIN_ITEM="$MAIN_TREE/lib/$item"
    WORKTREE_ITEM="$WORKTREE_PATH/lib/$item"

    if [[ -e "$MAIN_ITEM" ]]; then
        if [[ -L "$WORKTREE_ITEM" ]]; then
            echo -e "  lib/$item: ${GREEN}already symlinked${RESET}"
        elif [[ -e "$WORKTREE_ITEM" ]]; then
            rm -rf "$WORKTREE_ITEM"
            ln -s "$MAIN_ITEM" "$WORKTREE_ITEM"
            echo -e "  lib/$item: ${GREEN}symlinked${RESET}"
        else
            ln -s "$MAIN_ITEM" "$WORKTREE_ITEM"
            echo -e "  lib/$item: ${GREEN}symlinked${RESET}"
        fi
    fi
done

# Step 3: Create build directory structure and clone object files
echo -e "${CYAN}Setting up build directory...${RESET}"
mkdir -p build/lib build/obj build/bin

# Step 3b: Hardlink object files from main tree
# This is the big win — avoids recompiling 900+ .o files from scratch.
# Hardlinks are instant, zero disk cost (same filesystem), and make will
# only recompile files whose sources diverge in the worktree.
MAIN_OBJ="$MAIN_TREE/build/obj"
WORKTREE_OBJ="$WORKTREE_PATH/build/obj"
if [[ -d "$MAIN_OBJ" ]]; then
    # Check if obj/ already has content (re-run of setup)
    OBJ_COUNT=$(find "$WORKTREE_OBJ" -name "*.o" 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$OBJ_COUNT" -gt 10 ]]; then
        echo -e "  build/obj: ${GREEN}already populated ($OBJ_COUNT objects)${RESET}"
    else
        echo -e "${CYAN}Cloning build objects from main tree...${RESET}"
        # Clone all build artifacts (.o, .d, .ccj) from main tree.
        # On macOS/APFS: cp -Rc uses clonefile() — instant, zero disk until modified,
        #   and each side is independent (no risk of worktree clobbering main tree).
        # On Linux: falls back to regular copy (still faster than recompiling).
        # Either way, make only recompiles files whose sources diverge in the worktree.
        CLONE_START=$(date +%s)
        rm -rf "$WORKTREE_OBJ"
        cp -Rc "$MAIN_OBJ" "$WORKTREE_OBJ" 2>/dev/null || cp -a "$MAIN_OBJ" "$WORKTREE_OBJ"
        CLONE_END=$(date +%s)
        NEW_COUNT=$(find "$WORKTREE_OBJ" -name "*.o" 2>/dev/null | wc -l | tr -d ' ')
        echo -e "  build/obj: ${GREEN}cloned $NEW_COUNT objects in $((CLONE_END - CLONE_START))s${RESET}"

        # Validate object file architecture — cross-compilation leaves wrong-arch .o files
        SAMPLE_OBJ=$(find "$WORKTREE_OBJ" -name "*.o" -print -quit 2>/dev/null)
        if [[ -n "$SAMPLE_OBJ" ]]; then
            SAMPLE_ARCH=$(objdump -f "$SAMPLE_OBJ" 2>/dev/null | grep -m1 "architecture:" | awk -F',' '{print $1}' | awk '{print $NF}')
            HOST_ARCH_CHECK=$(uname -m)
            ARCH_MISMATCH=false
            case "$HOST_ARCH_CHECK" in
                x86_64)
                    [[ "$SAMPLE_ARCH" != *"x86-64"* && "$SAMPLE_ARCH" != *"i386"* ]] && ARCH_MISMATCH=true
                    ;;
                aarch64)
                    [[ "$SAMPLE_ARCH" != *"aarch64"* ]] && ARCH_MISMATCH=true
                    ;;
            esac
            if [[ "$ARCH_MISMATCH" == "true" ]]; then
                echo -e "  build/obj: ${YELLOW}wrong architecture ($SAMPLE_ARCH for $HOST_ARCH_CHECK), clearing — will rebuild from scratch${RESET}"
                rm -rf "$WORKTREE_OBJ"
                mkdir -p "$WORKTREE_OBJ"
            fi
        fi
    fi
else
    echo -e "  build/obj: ${YELLOW}main tree not built yet (will build from scratch)${RESET}"
fi

# Step 3c: Copy compile_commands.json for clangd support
if [[ -f "$MAIN_TREE/compile_commands.json" ]]; then
    # Use sed to rewrite paths from main tree to worktree
    sed "s|${MAIN_TREE}|${WORKTREE_PATH}|g" "$MAIN_TREE/compile_commands.json" > "$WORKTREE_PATH/compile_commands.json"
    echo -e "  compile_commands.json: ${GREEN}copied and paths rewritten${RESET}"
fi

# Step 4: Symlink compiled libraries from main tree
# These are expensive to build and rarely change
echo -e "${CYAN}Symlinking compiled libraries from main tree...${RESET}"

MAIN_LIBS=("libhv.a" "libwpa_client.a")
for lib in "${MAIN_LIBS[@]}"; do
    MAIN_LIB="$MAIN_TREE/build/lib/$lib"
    WORKTREE_LIB="$WORKTREE_PATH/build/lib/$lib"

    if [[ -f "$MAIN_LIB" ]]; then
        if [[ -L "$WORKTREE_LIB" ]]; then
            echo -e "  $lib: ${GREEN}already symlinked${RESET}"
        elif [[ -f "$WORKTREE_LIB" ]]; then
            echo -e "  $lib: ${YELLOW}exists as regular file, replacing with symlink${RESET}"
            rm -f "$WORKTREE_LIB"
            ln -s "$MAIN_LIB" "$WORKTREE_LIB"
        else
            ln -s "$MAIN_LIB" "$WORKTREE_LIB"
            echo -e "  $lib: ${GREEN}symlinked${RESET}"
        fi
        # Touch the symlink so it appears newer than source files
        # This prevents make from trying to rebuild based on source timestamps
        touch -h "$WORKTREE_LIB" 2>/dev/null || true
    else
        echo -e "  $lib: ${YELLOW}not found in main tree (will build from scratch)${RESET}"
    fi
done

# Step 5: Symlink the precompiled header if it exists
MAIN_PCH="$MAIN_TREE/build/lvgl_pch.h.gch"
WORKTREE_PCH="$WORKTREE_PATH/build/lvgl_pch.h.gch"
if [[ -f "$MAIN_PCH" ]]; then
    if [[ -L "$WORKTREE_PCH" ]]; then
        echo -e "  lvgl_pch.h.gch: ${GREEN}already symlinked${RESET}"
    elif [[ -f "$WORKTREE_PCH" ]]; then
        echo -e "  lvgl_pch.h.gch: ${YELLOW}exists as regular file, replacing with symlink${RESET}"
        rm -f "$WORKTREE_PCH"
        ln -s "$MAIN_PCH" "$WORKTREE_PCH"
    else
        ln -s "$MAIN_PCH" "$WORKTREE_PCH"
        echo -e "  lvgl_pch.h.gch: ${GREEN}symlinked${RESET}"
    fi
    touch -h "$WORKTREE_PCH" 2>/dev/null || true
else
    echo -e "  lvgl_pch.h.gch: ${YELLOW}not found in main tree (will build from scratch)${RESET}"
fi

# Step 5b: Validate library architectures
# Cross-compilation (make pi-test) can leave ARM .a files in build/lib/.
# Detect and remove them so make rebuilds for the correct architecture.
echo -e "${CYAN}Validating library architectures...${RESET}"
HOST_ARCH=$(uname -m)
for lib_file in "$WORKTREE_PATH/build/lib/"*.a; do
    [[ -f "$lib_file" ]] || continue
    [[ -L "$lib_file" ]] && continue  # Skip symlinks — they point to main tree
    LIB_NAME=$(basename "$lib_file")
    # Check first object file's architecture
    LIB_ARCH=$(objdump -f "$lib_file" 2>/dev/null | grep -m1 "architecture:" | awk -F',' '{print $1}' | awk '{print $NF}')
    if [[ -n "$LIB_ARCH" ]]; then
        case "$HOST_ARCH" in
            x86_64)
                if [[ "$LIB_ARCH" != *"x86-64"* && "$LIB_ARCH" != *"i386"* ]]; then
                    echo -e "  $LIB_NAME: ${YELLOW}wrong architecture ($LIB_ARCH), removing — will rebuild${RESET}"
                    rm -f "$lib_file"
                fi
                ;;
            aarch64)
                if [[ "$LIB_ARCH" != *"aarch64"* ]]; then
                    echo -e "  $LIB_NAME: ${YELLOW}wrong architecture ($LIB_ARCH), removing — will rebuild${RESET}"
                    rm -f "$lib_file"
                fi
                ;;
        esac
    fi
done

echo -e "${GREEN}✓ Libraries configured${RESET}"

# Step 6: Symlink node_modules (font converter tools)
echo -e "${CYAN}Symlinking node_modules...${RESET}"
if [[ -d "$MAIN_TREE/node_modules" ]]; then
    if [[ -L "$WORKTREE_PATH/node_modules" ]]; then
        echo -e "  node_modules: ${GREEN}already symlinked${RESET}"
    elif [[ -d "$WORKTREE_PATH/node_modules" ]]; then
        echo -e "  node_modules: ${YELLOW}exists as real directory, replacing with symlink${RESET}"
        rm -rf "$WORKTREE_PATH/node_modules"
        ln -s "$MAIN_TREE/node_modules" "$WORKTREE_PATH/node_modules"
    else
        ln -s "$MAIN_TREE/node_modules" "$WORKTREE_PATH/node_modules"
        echo -e "  node_modules: ${GREEN}symlinked${RESET}"
    fi
else
    echo -e "  node_modules: ${YELLOW}not found in main tree${RESET}"
fi

# Step 7: Symlink Python venv
echo -e "${CYAN}Symlinking Python venv...${RESET}"
if [[ -d "$MAIN_TREE/.venv" ]]; then
    if [[ -L "$WORKTREE_PATH/.venv" ]]; then
        echo -e "  .venv: ${GREEN}already symlinked${RESET}"
    elif [[ -d "$WORKTREE_PATH/.venv" ]]; then
        echo -e "  .venv: ${YELLOW}exists as real directory, replacing with symlink${RESET}"
        rm -rf "$WORKTREE_PATH/.venv"
        ln -s "$MAIN_TREE/.venv" "$WORKTREE_PATH/.venv"
    else
        ln -s "$MAIN_TREE/.venv" "$WORKTREE_PATH/.venv"
        echo -e "  .venv: ${GREEN}symlinked${RESET}"
    fi
else
    echo -e "  .venv: ${YELLOW}not found in main tree${RESET}"
fi
echo -e "${GREEN}✓ Development tools configured${RESET}"

# Step 8: Configure git excludes for symlinks (keeps git status clean)
echo -e "${CYAN}Configuring git excludes...${RESET}"

# Get the common git dir (shared by all worktrees)
# Note: info/exclude is read from GIT_COMMON_DIR, not the worktree's gitdir
GIT_COMMON_DIR=$(git -C "$WORKTREE_PATH" rev-parse --git-common-dir)
# Make it absolute if it's relative
if [[ ! "$GIT_COMMON_DIR" = /* ]]; then
    GIT_COMMON_DIR="$WORKTREE_PATH/$GIT_COMMON_DIR"
fi

EXCLUDE_FILE="$GIT_COMMON_DIR/info/exclude"
mkdir -p "$(dirname "$EXCLUDE_FILE")"

# Items to exclude (symlinks we created + build artifacts)
# Note: We exclude lib/* specifically because lib/ itself is a real directory
EXCLUDES=(
    "# HelixScreen worktree setup - auto-generated excludes"
    "lib/*"
    "node_modules"
    ".venv"
    "build/"
    "compile_commands.json"
    ".fonts.stamp"
)

# Add excludes if not already present
for exclude in "${EXCLUDES[@]}"; do
    if ! grep -qF "$exclude" "$EXCLUDE_FILE" 2>/dev/null; then
        echo "$exclude" >> "$EXCLUDE_FILE"
    fi
done

# Mark symlinked paths as skip-worktree so git ignores typechanges
echo -e "${CYAN}Marking symlinks as skip-worktree...${RESET}"
cd "$WORKTREE_PATH"

# Mark all lib/ submodules
for submod in $SUBMODULES; do
    git update-index --skip-worktree "$submod" 2>/dev/null || true
done

# Mark other lib items
git update-index --skip-worktree lib/tuibox.h 2>/dev/null || true
# lib/mdns is a directory, not a submodule - mark its contents
git update-index --skip-worktree lib/mdns/mdns.h 2>/dev/null || true

echo -e "${GREEN}✓ Git excludes configured${RESET}"

# Step 9: Create build marker files to skip redundant checks
echo -e "${CYAN}Creating build markers...${RESET}"
touch "$WORKTREE_PATH/build/.deps-checked"
touch "$WORKTREE_PATH/build/.patches-applied"
echo "native" > "$WORKTREE_PATH/build/.build-target"
touch "$WORKTREE_PATH/.fonts.stamp"
echo -e "${GREEN}✓ Build markers created${RESET}"

# Step 9b: Configure ccache for cross-worktree reuse
#
# The native build compiles with -g (debug info). With ccache's default
# hash_dir=true, the absolute working directory is folded into the cache key,
# so an object compiled in the main tree NEVER matches the same source compiled
# under .worktrees/<name>/ — every worktree starts cold and recompiles from
# scratch. Two settings fix this:
#   - base_dir: rewrite absolute paths under it to relative before hashing
#   - hash_dir=false: stop hashing the cwd (the -g debug-path component)
# With both, a worktree build reuses the main tree's cached objects, so the
# "rebuild everything" make does after a fresh checkout becomes cache hits
# instead of real compiles.
CCACHE_BIN="$(command -v ccache 2>/dev/null || true)"
if [[ -n "$CCACHE_BIN" ]]; then
    echo -e "${CYAN}Configuring ccache for cross-worktree reuse...${RESET}"

    # Longest common ancestor of the main tree and this worktree — covers both
    # the default .worktrees/<name> layout and out-of-tree paths like /tmp/foo.
    common_ancestor() {
        local p1="$1" p2="$2"
        while [[ "$p1" != "$p2" ]]; do
            if [[ ${#p1} -gt ${#p2} ]]; then p1="$(dirname "$p1")"; else p2="$(dirname "$p2")"; fi
        done
        echo "$p1"
    }
    BUILD_BASEDIR="$(common_ancestor "$MAIN_TREE" "$WORKTREE_PATH")"

    # Persist global ccache config so later manual `make` runs in the worktree
    # hit the shared cache too — not just this script's initial build. base_dir
    # defaults to $HOME (the standard broad choice covering all in-home worktrees);
    # only set when unset so we never override an intentional user value.
    CUR_BASEDIR="$(ccache --get-config base_dir 2>/dev/null || true)"
    if [[ -z "$CUR_BASEDIR" ]]; then
        ccache --set-config "base_dir=$HOME" 2>/dev/null \
            && echo -e "  base_dir: ${GREEN}set to $HOME${RESET}"
    else
        echo -e "  base_dir: ${GREEN}already set ($CUR_BASEDIR)${RESET}"
    fi
    if [[ "$(ccache --get-config hash_dir 2>/dev/null || true)" == "true" ]]; then
        ccache --set-config hash_dir=false 2>/dev/null \
            && echo -e "  hash_dir: ${GREEN}disabled (paths no longer in cache key)${RESET}"
    else
        echo -e "  hash_dir: ${GREEN}already disabled${RESET}"
    fi

    # The shared cache thrashes hard once a couple of worktrees + cross-compiles
    # pile in (default 5 GiB fills and evicts constantly, re-causing cold misses).
    # Raise the ceiling so objects survive between builds. Only ever raise it.
    CUR_MAX="$(ccache --get-config max_size 2>/dev/null || true)"
    MAX_NUM="$(echo "$CUR_MAX" | grep -oE '[0-9]+' | head -1)"
    MAX_UNIT="$(echo "$CUR_MAX" | grep -oiE '[KMGT]i?B' | head -1)"
    if [[ "$MAX_UNIT" =~ ^[Kk] || "$MAX_UNIT" =~ ^[Mm] ]] || \
       { [[ "$MAX_UNIT" =~ ^[Gg] ]] && [[ -n "$MAX_NUM" ]] && [[ "$MAX_NUM" -lt 25 ]]; }; then
        ccache --max-size=25G >/dev/null 2>&1 \
            && echo -e "  max_size: ${GREEN}raised to 25G (was $CUR_MAX)${RESET}"
    else
        echo -e "  max_size: ${GREEN}already ample ($CUR_MAX)${RESET}"
    fi

    # Export for this script's own build below. base_dir must be a prefix of the
    # worktree path; use the precise common ancestor in case the worktree lives
    # outside $HOME (e.g. /tmp/foo), which the global $HOME base_dir wouldn't cover.
    export CCACHE_BASEDIR="$BUILD_BASEDIR"
    export CCACHE_NOHASHDIR=1
    echo -e "  this build: ${GREEN}CCACHE_BASEDIR=$BUILD_BASEDIR CCACHE_NOHASHDIR=1${RESET}"
else
    echo -e "${YELLOW}ccache not found — worktree builds will not reuse the main tree cache${RESET}"
fi

# Step 10: Build (optional)
if [[ "$NO_BUILD" == "false" ]]; then
    echo ""
    echo -e "${BOLD}${CYAN}Running initial build...${RESET}"
    cd "$WORKTREE_PATH"
    if make -j; then
        echo ""
        echo -e "${GREEN}${BOLD}✓ Build successful!${RESET}"
    else
        echo ""
        echo -e "${RED}${BOLD}✗ Build failed${RESET}"
        exit 1
    fi
fi

echo ""
echo -e "${GREEN}${BOLD}✓ Worktree setup complete!${RESET}"
echo ""
echo -e "To work in this worktree:"
echo -e "  ${CYAN}cd $WORKTREE_PATH${RESET}"
echo ""
echo -e "Git status should be clean. To verify:"
echo -e "  ${CYAN}cd $WORKTREE_PATH && git status${RESET}"
echo ""
echo -e "${YELLOW}Note: lib/ is symlinked from main tree. If you need to modify"
echo -e "library code, un-symlink that specific directory first.${RESET}"
