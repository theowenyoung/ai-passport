#!/usr/bin/env python3
"""Regenerate the speaker timer's LVGL fonts from the pinned Noto Sans SC Medium.

The firmware only ever shows the fixed strings in main/timer_ui.c, so the text
fonts are subsets: printable ASCII plus every non-ASCII character in that file's
string literals. The digit fonts hold only what the clock draws. Needs Node.js
(it runs a pinned lv_font_conv through npx). Rebuild the firmware afterwards.

    python3 tools/timer_fonts.py           regenerate charset.txt and the fonts
    python3 tools/timer_fonts.py --check   only report characters the charset misses
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UI_SOURCE = ROOT / "main" / "timer_ui.c"
FONT_DIR = ROOT / "assets" / "fonts" / "timer"
CHARSET_PATH = FONT_DIR / "charset.txt"
CACHE = ROOT / "build" / "timer" / "font"

FONT_REF = "Sans2.004"
FONT_URL = (f"https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@{FONT_REF}"
            "/Sans/SubsetOTF/SC/NotoSansSC-Medium.otf")
FONT_LICENSE_URL = f"https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@{FONT_REF}/LICENSE"
FONT_SHA256 = "7633f5a016d4dd95e685a69633d818aabc4644c4b08e26bd35b1b30c45ed5dda"
LV_FONT_CONV = "lv_font_conv@1.5.3"

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
# (symbol, size, glyphs or None for the UI charset)
FONTS = (
    ("timer_digits_100", 100, "0123456789:"),
    ("timer_digits_80", 80, "0123456789:+"),
    ("timer_text_26", 26, None),
    ("timer_text_20", 20, None),
)

STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def ui_characters(source: Path = UI_SOURCE) -> set[str]:
    """Every non-ASCII character inside a string literal of the UI source."""
    text = source.read_text(encoding="utf-8")
    text = re.sub(r"//[^\n]*", "", text)  # comments may hold Chinese that is never drawn
    found: set[str] = set()
    for literal in STRING_LITERAL.findall(text):
        found.update(ch for ch in literal if ord(ch) > 0x7E)
    return found


def charset() -> str:
    return ASCII + "".join(sorted(ui_characters()))


def read_charset() -> str:
    return CHARSET_PATH.read_text(encoding="utf-8").rstrip("\n") if CHARSET_PATH.exists() else ""


def missing_from_charset() -> list[str]:
    covered = set(read_charset())
    return sorted(ch for ch in ui_characters() if ch not in covered)


def download(url: str, path: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "ai-passport-timer-fonts"})
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            path.write_bytes(response.read())
    except (urllib.error.URLError, TimeoutError) as error:
        raise SystemExit(f"download failed: {url}: {error}") from error


def source_font() -> Path:
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / "NotoSansSC-Medium.otf"
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != FONT_SHA256:
        print(f"Downloading {FONT_URL}")
        download(FONT_URL, path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != FONT_SHA256:
        raise SystemExit(f"font hash mismatch: {digest}")
    return path


def generate() -> None:
    font = source_font().relative_to(ROOT)
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    download(FONT_LICENSE_URL, FONT_DIR / "OFL.txt")
    text_glyphs = charset()
    CHARSET_PATH.write_text(text_glyphs + "\n", encoding="utf-8")
    print(f"Charset: {len(text_glyphs)} glyphs -> {CHARSET_PATH.relative_to(ROOT)}")
    for name, size, glyphs in FONTS:
        output = (FONT_DIR / f"{name}.c").relative_to(ROOT)
        # Relative paths keep the generated header free of this machine's paths.
        command = ["npx", "--yes", LV_FONT_CONV, "--font", str(font),
                   "--symbols", glyphs or text_glyphs, "--size", str(size), "--bpp", "4",
                   "--format", "lvgl", "--no-compress", "--lv-font-name", name,
                   "--lv-include", "lvgl.h", "-o", str(output)]
        print(f"Converting {name} ({size}px) -> {output}")
        if subprocess.run(command, cwd=ROOT).returncode != 0:
            raise SystemExit(f"lv_font_conv failed for {name} (is Node.js installed?)")
    print("Fonts updated. Rebuild the firmware to use them.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="only list UI characters missing from charset.txt")
    args = parser.parse_args()
    if args.check:
        missing = missing_from_charset()
        if missing:
            codes = " ".join(f"U+{ord(ch):04X}({ch})" for ch in missing)
            print(f"MISSING {len(missing)}: {codes}; run python3 tools/timer_fonts.py")
            return 1
        print("charset covers every UI string")
        return 0
    generate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
