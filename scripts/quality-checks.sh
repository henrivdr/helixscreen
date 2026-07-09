#!/bin/bash
# SPDX-FileCopyrightText: 2024 Patrick Brown <opensource@pbdigital.org>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Quality checks script - single source of truth for pre-commit and CI
# Usage:
#   ./scripts/quality-checks.sh                      # Check all files (for CI)
#   ./scripts/quality-checks.sh --staged-only        # Check only staged files (for pre-commit)
#   ./scripts/quality-checks.sh --auto-fix           # Auto-fix formatting issues
#   ./scripts/quality-checks.sh --staged-only --auto-fix  # Fix staged files

set -e

# Parse arguments
STAGED_ONLY=false
AUTO_FIX=false
for arg in "$@"; do
  case "$arg" in
    --staged-only) STAGED_ONLY=true ;;
    --auto-fix) AUTO_FIX=true ;;
  esac
done

# Change to repo root
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT"

EXIT_CODE=0
SCRIPT_START=$(date +%s)

# Timing helper - prints elapsed time for a section (seconds)
section_time() {
  local start=$1
  local end=$(date +%s)
  local elapsed=$((end - start))
  if [ $elapsed -gt 0 ]; then
    printf " (%ds)" "$elapsed"
  fi
}

echo "🔍 Running quality checks..."
if [ "$STAGED_ONLY" = true ]; then
  echo "   Mode: Staged files only (pre-commit)"
else
  echo "   Mode: All files (CI)"
fi
echo ""

# ====================================================================
# Copyright (C) 2025-2026 356C LLC
# ====================================================================
SECTION_START=$(date +%s)
echo -n "📝 Checking copyright headers..."

if [ "$STAGED_ONLY" = true ]; then
  # Pre-commit mode: check only staged files (git-ignored files can't be staged)
  FILES=$(git diff --cached --name-only --diff-filter=ACM | \
    grep -E '\.(cpp|c|h|mm)$' | \
    grep -v '^lib/' | \
    grep -v '^assets/fonts/' | \
    grep -v '^lv_conf\.h$' | \
    grep -v '^node_modules/' | \
    grep -v '^build/' | \
    grep -v '/\.' || true)
else
  # CI mode: check all files in src/ and include/ (lib/ and assets/fonts/ excluded as auto-generated)
  FILES=$(find src include -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.mm" 2>/dev/null | \
    grep -v '/\.' | \
    grep -v '^lv_conf\.h$' || true)
fi

if [ -n "$FILES" ]; then
  MISSING_HEADERS=""
  for file in $FILES; do
    if [ -f "$file" ]; then
      if ! head -3 "$file" | grep -q "SPDX-License-Identifier: GPL-3.0-or-later"; then
        echo "❌ Missing GPL v3 header: $file"
        MISSING_HEADERS="$MISSING_HEADERS $file"
        EXIT_CODE=1
      fi
    fi
  done

  if [ -n "$MISSING_HEADERS" ]; then
    section_time $SECTION_START
    echo ""
    echo "See docs/COPYRIGHT_HEADERS.md for the required header format"
  else
    section_time $SECTION_START
    echo ""
    echo "✅ All source files have proper copyright headers"
  fi
else
  if [ "$STAGED_ONLY" = true ]; then
    section_time $SECTION_START
    echo ""
    echo "ℹ️  No source files staged for commit"
  else
    section_time $SECTION_START
    echo ""
    echo "ℹ️  No source files found"
  fi
fi

echo ""

# ====================================================================
# Phase 1: Critical Checks
# ====================================================================

# Merge Conflict Markers Check
echo "⚠️  Checking for merge conflict markers..."
if [ -n "$FILES" ]; then
  CONFLICT_FILES=$(echo "$FILES" | xargs grep -l "^<<<<<<< \|^=======$\|^>>>>>>> " 2>/dev/null || true)
  if [ -n "$CONFLICT_FILES" ]; then
    echo "❌ Merge conflict markers found in:"
    echo "$CONFLICT_FILES" | sed 's/^/   /'
    EXIT_CODE=1
  else
    echo "✅ No merge conflict markers"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# Trailing Whitespace Check
echo "🧹 Checking for trailing whitespace..."
if [ -n "$FILES" ]; then
  TRAILING_WS=$(echo "$FILES" | xargs grep -n "[[:space:]]$" 2>/dev/null || true)
  if [ -n "$TRAILING_WS" ]; then
    echo "⚠️  Found trailing whitespace:"
    echo "$TRAILING_WS" | head -10 | sed 's/^/   /'
    if [ $(echo "$TRAILING_WS" | wc -l) -gt 10 ]; then
      echo "   ... and $(($(echo "$TRAILING_WS" | wc -l) - 10)) more"
    fi
    echo "ℹ️  Fix with: sed -i 's/[[:space:]]*$//' <file>"
  else
    echo "✅ No trailing whitespace"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# XML Validation
echo "📄 Validating XML files..."
if [ "$STAGED_ONLY" = true ]; then
  XML_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep "\.xml$" || true)
else
  XML_FILES=$(find ui_xml -name "*.xml" 2>/dev/null || true)
fi

if [ -n "$XML_FILES" ]; then
  if command -v xmllint >/dev/null 2>&1; then
    XML_ERRORS=0
    for xml in $XML_FILES; do
      if [ -f "$xml" ]; then
        # Use --recover to continue on namespace errors (LVGL uses colon syntax like
        # style_arc_color:indicator which xmllint interprets as namespace prefixes).
        # Filter: only keep error lines (file:line: format) and exclude namespace errors.
        XMLLINT_OUTPUT=$(xmllint --noout --recover "$xml" 2>&1 | grep -E "^[^:]+:[0-9]+:" | grep -v "namespace error" || true)
        if [ -n "$XMLLINT_OUTPUT" ]; then
          echo "❌ Invalid XML: $xml"
          echo "$XMLLINT_OUTPUT"
          XML_ERRORS=$((XML_ERRORS + 1))
          EXIT_CODE=1
        fi
      fi
    done
    if [ $XML_ERRORS -eq 0 ]; then
      echo "✅ All XML files are valid"
    fi
  else
    echo "⚠️  xmllint not found - skipping XML validation"
    echo "   Install with: brew install libxml2 (macOS) or apt install libxml2-utils (Linux)"
  fi
else
  echo "ℹ️  No XML files to validate"
fi

echo ""

# ====================================================================
# XML Constant Set Validation
# ====================================================================
echo "🔤 Validating XML constant sets..."

if [ -x "build/bin/validate-xml-constants" ]; then
  if ./build/bin/validate-xml-constants; then
    : # Success message already printed by tool
  else
    echo ""
    echo "   Incomplete constant sets can cause runtime warnings."
    echo "   - Responsive px: Need ALL of _small, _medium, _large (or none)"
    echo "   - Theme colors: Need BOTH _light and _dark (or neither)"
    EXIT_CODE=1
  fi
else
  echo "⚠️  validate-xml-constants not built - skipping"
  echo "   Run 'make' to build validation tools"
fi

echo ""

# ====================================================================
# XML Attribute Validation
# ====================================================================
echo "📄 Validating XML attributes..."

if [ -x "build/bin/validate-xml-attributes" ]; then
  if [ "$STAGED_ONLY" = true ]; then
    # Check only staged XML files in pre-commit mode
    STAGED_XML_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.xml$' || true)
    if [ -n "$STAGED_XML_FILES" ]; then
      # shellcheck disable=SC2086
      if ./build/bin/validate-xml-attributes --warn-only $STAGED_XML_FILES 2>/dev/null; then
        echo "✅ XML attribute validation passed"
      else
        echo "⚠️  Unknown XML attributes found (warnings only for now)"
        echo "   Run './build/bin/validate-xml-attributes' for details"
        # NOTE: Using --warn-only so this doesn't block commits during adoption
        # Remove --warn-only once all false positives are resolved
      fi
    else
      echo "ℹ️  No XML files staged for commit"
    fi
  else
    # CI mode: check all XML files with --warn-only
    if ./build/bin/validate-xml-attributes --warn-only 2>/dev/null; then
      echo "✅ XML attribute validation passed"
    else
      echo "⚠️  Unknown XML attributes found (warnings only for now)"
      echo "   Run './build/bin/validate-xml-attributes' for details"
    fi
  fi
else
  echo "⚠️  validate-xml-attributes not built - skipping"
  echo "   Run 'make validate-xml-attrs' to build validation tool"
fi

echo ""

# ====================================================================
# Phase 2: Code Quality Checks
# ====================================================================

# Code Formatting Check (clang-format) - WARNING ONLY
# NOTE: clang-format versions differ between local (macOS Homebrew) and CI (Ubuntu)
# which can cause false positives. Use pre-commit hook for local enforcement.
echo "🎨 Checking code formatting (clang-format)..."
# Resolve clang-format to the EXACT pinned wheel (clang-format==18.1.8 in
# requirements.txt, installed into .venv by `make deps`). Preference order:
# $CLANG_FORMAT override, then the project .venv (the single source of truth —
# byte-identical on every OS + CI), then a system clang-format-18, then bare
# clang-format. The .venv wins over the system binary so a machine's Homebrew
# (newer) or distro (older 18.1.x patch) clang-format never affects formatting.
# Auto-fix only runs when the resolved binary is v18, so a non-18 fallback can
# never reflow whole files.
CF_BIN=""
CF_VER=""
for cf_cand in "${CLANG_FORMAT:-}" "$REPO_ROOT/.venv/bin/clang-format" clang-format-18 clang-format; do
  [ -n "$cf_cand" ] || continue
  command -v "$cf_cand" >/dev/null 2>&1 || [ -x "$cf_cand" ] || continue
  cf_v="$("$cf_cand" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
  [ -n "$cf_v" ] || continue
  CF_BIN="$cf_cand"
  CF_VER="$cf_v"
  case "$cf_v" in 18.*) break ;; esac
done
if [ -n "$FILES" ]; then
  if [ -n "$CF_BIN" ]; then
    if [ -f ".clang-format" ]; then
      FORMAT_ISSUES=""
      for file in $FILES; do
        if [ -f "$file" ]; then
          # Skip auto-generated sources: their on-disk format is owned by the
          # generator (e.g. src/generated/lv_i18n_translations.c from
          # generate_translations.py). Running clang-format on them fights the
          # generator on every build, producing perpetual post-build churn.
          if head -5 "$file" | grep -qiE 'auto-generated|DO NOT EDIT'; then
            continue
          fi
          # Check if file needs formatting
          if ! "$CF_BIN" --dry-run --Werror "$file" >/dev/null 2>&1; then
            FORMAT_ISSUES="$FORMAT_ISSUES $file"
            if [ "$AUTO_FIX" = true ]; then
              case "$CF_VER" in
                18.*)
                  "$CF_BIN" -i "$file"
                  echo "   ✓ Auto-formatted: $file"
                  ;;
                *)
                  echo "   ⚠️  Skipping auto-format of $file: resolved clang-format $CF_VER != 18"
                  echo "       (auto-formatting with a non-CI version would reflow the whole file)"
                  echo "       Install v18: pip install 'clang-format==18.1.8' into .venv, or set CLANG_FORMAT=clang-format-18"
                  ;;
              esac
            fi
          fi
        fi
      done

      if [ -n "$FORMAT_ISSUES" ]; then
        if [ "$AUTO_FIX" = true ]; then
          # Auto-stage formatted files when in pre-commit mode (--staged-only)
          if [ "$STAGED_ONLY" = true ]; then
            git add $FORMAT_ISSUES
            echo "✅ Auto-formatted and re-staged files:"
            echo "$FORMAT_ISSUES" | tr ' ' '\n' | grep -v '^$' | sed 's/^/   /'
          else
            echo "✅ Auto-formatted files - re-stage them before committing:"
            echo "$FORMAT_ISSUES" | tr ' ' '\n' | grep -v '^$' | sed 's/^/   /'
            echo ""
            echo "ℹ️  Stage formatted files with:"
            echo "   git add$FORMAT_ISSUES"
          fi
        else
          echo "⚠️  Files may need formatting (version differences may cause false positives):"
          echo "$FORMAT_ISSUES" | tr ' ' '\n' | grep -v '^$' | sed 's/^/   /'
          echo ""
          echo "ℹ️  Fix with: clang-format -i <file>"
          echo "ℹ️  Or run: ./scripts/quality-checks.sh --auto-fix"
          # NOTE: Don't fail CI for formatting - version differences cause issues
          # EXIT_CODE=1
        fi
      else
        echo "✅ All files properly formatted"
      fi
    else
      echo "ℹ️  No .clang-format file found - skipping format check"
    fi
  else
    echo "⚠️  clang-format not found - skipping format check"
    echo "   Install with: brew install clang-format (macOS) or apt install clang-format (Linux)"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# XML Formatting Check
echo "📐 Checking XML formatting..."
if [ "$STAGED_ONLY" = true ]; then
  XML_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep "\.xml$" || true)
else
  XML_FILES=$(find ui_xml -name "*.xml" 2>/dev/null || true)
fi

VENV_PYTHON=".venv/bin/python"

if [ -n "$XML_FILES" ]; then
  # Prefer Python formatter with attribute wrapping, fallback to xmllint
  if [ -x "$VENV_PYTHON" ] && $VENV_PYTHON -c "import lxml" 2>/dev/null; then
    # Use our custom formatter with --check mode
    if $VENV_PYTHON scripts/format-xml.py --check $XML_FILES 2>/dev/null; then
      echo "✅ All XML files properly formatted"
    else
      echo "⚠️  XML files need formatting"
      echo "ℹ️  Fix with: .venv/bin/python scripts/format-xml.py <files>"
      echo "ℹ️  Or run: make format"
      # Don't fail CI for XML formatting - it's a style preference
      # EXIT_CODE=1
    fi
  elif command -v xmllint >/dev/null 2>&1; then
    echo "ℹ️  Python formatter not available, using xmllint (basic check only)"
    FORMAT_ISSUES=""
    for file in $XML_FILES; do
      if [ -f "$file" ]; then
        # Check if file needs formatting (xmllint --format for consistent indentation)
        FORMATTED=$(xmllint --format "$file" 2>/dev/null || echo "PARSE_ERROR")
        if [ "$FORMATTED" = "PARSE_ERROR" ]; then
          echo "⚠️  Cannot format $file (may have XML errors)"
        else
          ORIGINAL=$(cat "$file")
          if [ "$FORMATTED" != "$ORIGINAL" ]; then
            FORMAT_ISSUES="$FORMAT_ISSUES $file"
          fi
        fi
      fi
    done

    if [ -n "$FORMAT_ISSUES" ]; then
      echo "⚠️  XML files may need formatting (basic check):"
      echo "$FORMAT_ISSUES" | tr ' ' '\n' | grep -v '^$' | sed 's/^/   /'
      echo "ℹ️  For proper formatting: make venv-setup && make format"
    else
      echo "✅ All XML files pass basic formatting check"
    fi
  else
    echo "ℹ️  No XML formatter available - skipping XML format check"
    echo "   Run 'make venv-setup' to enable full XML formatting"
  fi
else
  echo "ℹ️  No XML files to check"
fi

echo ""

# Build Verification
if [ "$STAGED_ONLY" = true ]; then
  SECTION_START=$(date +%s)
  echo -n "🔨 Verifying incremental build..."

  # Fast timestamp check (avoids 2-3s make startup overhead)
  # Check if binary exists and no source files are newer
  TARGET="build/bin/helix-screen"
  BUILD_NEEDED=false

  if [ ! -f "$TARGET" ]; then
    BUILD_NEEDED=true
  elif find src include -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.mm' \) -newer "$TARGET" 2>/dev/null | grep -q .; then
    BUILD_NEEDED=true
  fi

  if [ "$BUILD_NEEDED" = false ]; then
    section_time $SECTION_START
    echo ""
    echo "✅ Build up to date"
  else
    # Something needs building - run actual build
    # Use SKIP_COMPILE_COMMANDS=1 to avoid slow LSP re-indexing
    if make SKIP_COMPILE_COMMANDS=1 -j >/dev/null 2>&1; then
      section_time $SECTION_START
      echo ""
      echo "✅ Build successful"
    else
      section_time $SECTION_START
      echo ""
      echo "❌ Build failed - fix compilation errors before committing"
      echo "   Run 'make' to see full error output"
      EXIT_CODE=1
    fi
  fi
  echo ""
fi

# ====================================================================
# Icon Font Validation
# ====================================================================
SECTION_START=$(date +%s)
echo -n "🔤 Validating icon font codepoints..."

# Check if all icons in ui_icon_codepoints.h are present in compiled fonts
# This prevents the bug where icons are added to code but fonts aren't regenerated
if [ -f "scripts/validate_icon_fonts.sh" ]; then
  if ./scripts/validate_icon_fonts.sh 2>/dev/null; then
    section_time $SECTION_START
    echo ""
    echo "✅ All icon codepoints present in fonts"
  else
    section_time $SECTION_START
    echo ""
    echo "❌ Missing icon codepoints in fonts!"
    echo ""
    echo "   Some icons in include/ui_icon_codepoints.h are not in the compiled fonts."
    echo "   Run './scripts/regen_mdi_fonts.sh' to regenerate fonts, then rebuild."
    echo ""
    echo "   Or run './scripts/validate_icon_fonts.sh --fix' to auto-regenerate."
    EXIT_CODE=1
  fi
else
  section_time $SECTION_START
  echo ""
  echo "⚠️  validate_icon_fonts.sh not found - skipping icon validation"
fi

echo ""

# ====================================================================
# MDI Codepoint Label Verification
# ====================================================================
SECTION_START=$(date +%s)
echo -n "🔤 Verifying MDI codepoint labels..."

if [ -f "scripts/verify_mdi_codepoints.py" ]; then
  python3 scripts/verify_mdi_codepoints.py 2>/dev/null
  RESULT=$?
  section_time $SECTION_START
  echo ""
  if [ $RESULT -eq 0 ]; then
    echo "✅ All MDI codepoint labels verified"
  elif [ $RESULT -eq 1 ]; then
    echo "❌ MDI codepoint verification failed!"
    echo "   Some icon codepoints don't match their labels."
    echo "   Run: python3 scripts/verify_mdi_codepoints.py"
    EXIT_CODE=1
  elif [ $RESULT -eq 2 ]; then
    echo "⚠️  MDI metadata cache missing"
    echo "   Run: make update-mdi-cache"
  fi
else
  section_time $SECTION_START
  echo ""
  echo "⚠️  verify_mdi_codepoints.py not found - skipping"
fi

echo ""

# ====================================================================
# Code Style Check
# ====================================================================
echo "🔍 Checking for TODO/FIXME markers..."

# Check for TODO/FIXME/XXX comments (informational only)
if [ -n "$FILES" ]; then
  if echo "$FILES" | xargs grep -n "TODO\|FIXME\|XXX" 2>/dev/null | head -20; then
    echo "ℹ️  Found TODO/FIXME markers (informational only)"
  else
    echo "✅ No TODO/FIXME markers found"
  fi
else
  echo "ℹ️  No source files to check"
fi

echo ""

# ====================================================================
# Memory Safety Audit (Critical Patterns Only)
# ====================================================================
if [ "$STAGED_ONLY" = true ]; then
  # Get all staged .cpp and .xml files for audit
  AUDIT_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(cpp|xml)$' || true)

  if [ -n "$AUDIT_FILES" ]; then
    echo "🛡️  Running memory safety audit on staged files..."

    if [ -f "scripts/audit_codebase.sh" ]; then
      # Run audit in file mode - only check critical patterns (errors fail, warnings pass)
      # shellcheck disable=SC2086
      if ./scripts/audit_codebase.sh --files $AUDIT_FILES 2>/dev/null; then
        echo "✅ Memory safety audit passed"
      else
        echo "❌ Memory safety audit found critical issues!"
        echo "   Run './scripts/audit_codebase.sh --files <files>' to see details"
        EXIT_CODE=1
      fi
    else
      echo "⚠️  audit_codebase.sh not found - skipping memory safety audit"
    fi
    echo ""
  fi
fi

# ====================================================================
# Subscription Null-Safety Check
# ====================================================================
# Background: Moonraker delivers JSON null for subscribed fields the underlying
# Klipper object lacks. .value() and .get<T>() throw type_error.302 on null;
# an uncaught throw inside a subscription handler exits 134 → watchdog crash
# loop (#filament_motion_sensor, fixed in f75b961d8).
#
# Baseline ratchets down as violations are fixed. New code adds to the count
# only via opt-out comment (`// JSON_NULL_SAFE: <reason>`).
SECTION_START=$(date +%s)
echo -n "🔒 Checking subscription null-safety..."

if [ -f "scripts/check_subscription_null_safety.py" ]; then
  # Baseline: 0 — every subscription-handler `.get<T>()` must have an
  # `.is_<type>()` guard within 15 lines, every `.value("k", default)` must
  # have an explicit `// JSON_NULL_SAFE` opt-out. Don't regress.
  if python3 scripts/check_subscription_null_safety.py --max-allowed 0 --summary >/tmp/null_safety.out 2>&1; then
    section_time $SECTION_START
    echo ""
    cat /tmp/null_safety.out
  else
    section_time $SECTION_START
    echo ""
    cat /tmp/null_safety.out
    echo "   Run: python3 scripts/check_subscription_null_safety.py"
    EXIT_CODE=1
  fi
else
  section_time $SECTION_START
  echo ""
  echo "⚠️  check_subscription_null_safety.py not found — skipping"
fi

echo ""

# ====================================================================
# L081 Mechanism C anti-pattern (cluster:pstat-async-delete)
# ====================================================================
# Background: bg-thread `tok.expired()` followed by `this->`/`api_->` member
# access races on owner destruction → UAF crashes that look unrelated in
# backtraces. The runtime detector emits `cluster:pstat-async-delete Mechanism C`
# warnings; this gate catches new instances at commit time.
#
# Scope: known bg-thread directories only (src/printer, src/calibration,
# src/led, src/print, src/system, src/sensors, src/api, src/network,
# src/bluetooth). src/ui/ is excluded — observer cbs there fire on main thread
# and would false-positive without AST-level lambda-context analysis.
SECTION_START=$(date +%s)
echo -n "🧵 Checking L081 bg-thread anti-pattern..."

if [ -f "scripts/check_l081_anti_pattern.py" ]; then
  if [ "$STAGED_ONLY" = true ]; then
    L081_ARGS="--staged-only"
  else
    L081_ARGS=""
  fi
  if python3 scripts/check_l081_anti_pattern.py $L081_ARGS >/tmp/l081_check.out 2>&1; then
    section_time $SECTION_START
    echo ""
    echo "✅ No L081 anti-pattern sites found"
  else
    section_time $SECTION_START
    echo ""
    cat /tmp/l081_check.out
    echo "   Run: python3 scripts/check_l081_anti_pattern.py"
    echo "   See include/async_lifetime_guard.h for the canonical fix."
    EXIT_CODE=1
  fi
else
  section_time $SECTION_START
  echo ""
  echo "⚠️  check_l081_anti_pattern.py not found — skipping"
fi

echo ""

# ====================================================================
# Translation format-specifier parity (crash #1073)
# ====================================================================
# Background: format strings passed to snprintf/fmt::format via lv_tr() are
# runtime-translated. If a translation adds an extra %s/%d (or {} field), the
# format call reads an argument that was never passed → SIGSEGV (snprintf) or
# fmt::format_error (fmt). #1073 was the French '%d additional fan%s' translated
# with two %s, crashing the Controls panel for French users.
SECTION_START=$(date +%s)
echo -n "🌐 Checking translation format specifiers..."

TRANS_FMT_PY="${VENV_PYTHON:-python3}"
[ -x "$TRANS_FMT_PY" ] || TRANS_FMT_PY=python3
if [ -f "scripts/check_translation_format_specifiers.py" ]; then
  if "$TRANS_FMT_PY" scripts/check_translation_format_specifiers.py >/tmp/trans_fmt.out 2>&1; then
    section_time $SECTION_START
    echo ""
    echo "✅ All translated format strings preserve their source placeholders"
  else
    section_time $SECTION_START
    echo ""
    cat /tmp/trans_fmt.out
    echo "   Run: $TRANS_FMT_PY scripts/check_translation_format_specifiers.py"
    echo "   Fix the offending translation in translations/<locale>.yml, then run: make translations"
    EXIT_CODE=1
  fi
else
  section_time $SECTION_START
  echo ""
  echo "⚠️  check_translation_format_specifiers.py not found — skipping"
fi

echo ""

# ====================================================================
# Shell Script Linting (shellcheck)
# ====================================================================
SECTION_START=$(date +%s)
echo -n "🐚 Checking shell scripts (shellcheck)..."

# Platform hook scripts and init script
SHELL_FILES=""
if [ "$STAGED_ONLY" = true ]; then
  SHELL_FILES=$(git diff --cached --name-only --diff-filter=ACM | \
    grep -E '(config/platform/.*\.sh|config/helixscreen\.init)$' || true)
else
  SHELL_FILES=$(find config/platform -name "*.sh" 2>/dev/null || true)
  if [ -f "config/helixscreen.init" ]; then
    SHELL_FILES="$SHELL_FILES config/helixscreen.init"
  fi
fi

if [ -n "$SHELL_FILES" ]; then
  if command -v shellcheck >/dev/null 2>&1; then
    SHELL_ERRORS=0
    for script in $SHELL_FILES; do
      if [ -f "$script" ]; then
        if ! shellcheck "$script" 2>/dev/null; then
          SHELL_ERRORS=$((SHELL_ERRORS + 1))
        fi
      fi
    done
    section_time $SECTION_START
    echo ""
    if [ $SHELL_ERRORS -eq 0 ]; then
      echo "✅ All shell scripts pass shellcheck"
    else
      echo "❌ shellcheck found issues in $SHELL_ERRORS file(s)"
      echo "   Run: shellcheck config/platform/*.sh config/helixscreen.init"
      EXIT_CODE=1
    fi
  else
    section_time $SECTION_START
    echo ""
    echo "⚠️  shellcheck not found - skipping shell script linting"
    echo "   Install with: brew install shellcheck (macOS) or apt install shellcheck (Linux)"
  fi
else
  section_time $SECTION_START
  echo ""
  if [ "$STAGED_ONLY" = true ]; then
    echo "ℹ️  No shell scripts staged for commit"
  else
    echo "ℹ️  No shell scripts found"
  fi
fi

echo ""

# ====================================================================
# Final Result
# ====================================================================
SCRIPT_END=$(date +%s)
TOTAL_SEC=$((SCRIPT_END - SCRIPT_START))

if [ $EXIT_CODE -eq 0 ]; then
  echo "✅ Quality checks passed! (${TOTAL_SEC}s total)"
  exit 0
else
  echo "❌ Quality checks failed! (${TOTAL_SEC}s total)"
  exit 1
fi
