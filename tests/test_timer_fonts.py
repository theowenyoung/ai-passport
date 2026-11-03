#!/usr/bin/env python3
"""Glyph coverage for the speaker timer: every character the UI draws must be in
the generated font that draws it, and the fonts must be the ones the build uses."""

from __future__ import annotations

from pathlib import Path
import re
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import timer_fonts  # noqa: E402


def covered(font_name: str) -> set[str]:
    source = (timer_fonts.FONT_DIR / f"{font_name}.c").read_text(encoding="utf-8")
    return {chr(int(code, 16)) for code in re.findall(r"/\* U\+([0-9A-F]{4,6}) ", source)}


class TimerFontCoverageTest(unittest.TestCase):
    def test_charset_matches_ui_strings(self) -> None:
        self.assertEqual(timer_fonts.missing_from_charset(), [],
                         "run python3 tools/timer_fonts.py after changing UI text")
        self.assertEqual(timer_fonts.read_charset(), timer_fonts.charset())

    def test_text_fonts_cover_every_ui_character(self) -> None:
        needed = set(timer_fonts.charset()) - {" "}
        self.assertIn("确", needed)
        for name in ("timer_text_26", "timer_text_20"):
            glyphs = covered(name)
            missing = sorted(needed - glyphs)
            self.assertEqual(missing, [], f"{name} lacks "
                             + " ".join(f"U+{ord(ch):04X}" for ch in missing))
            # A character no string uses stays out, so the check above can fail.
            self.assertNotIn("龘", glyphs)

    def test_digit_fonts_cover_the_clock(self) -> None:
        for name, _size, glyphs in timer_fonts.FONTS:
            if glyphs is None:
                continue
            self.assertEqual(set(glyphs) - covered(name), set(), name)
        self.assertIn("+", covered("timer_digits_80"))  # the overtime prefix

    def test_extraction_reads_literals_not_comments(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sample = Path(tmp) / "ui.c"
            sample.write_text('// 注释\nstatic const char *A = "中 \\"文\\"";  // 尾\n',
                              encoding="utf-8")
            self.assertEqual(timer_fonts.ui_characters(sample), {"中", "文"})

    def test_ui_and_build_use_the_generated_fonts(self) -> None:
        ui = timer_fonts.UI_SOURCE.read_text(encoding="utf-8")
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        for name, _size, _glyphs in timer_fonts.FONTS:
            self.assertIn(f"LV_FONT_DECLARE({name});", ui)
            self.assertIn(f"&{name}", ui)
            self.assertIn(f"../assets/fonts/timer/{name}.c", cmake)
            source = (timer_fonts.FONT_DIR / f"{name}.c").read_text(encoding="utf-8")
            self.assertIn(f"const lv_font_t {name} = {{", source)
        # The baseline Montserrat fonts have no Chinese; the timer UI must not use them.
        self.assertNotIn("lv_font_montserrat", ui)


if __name__ == "__main__":
    unittest.main()
