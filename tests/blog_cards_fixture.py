#!/usr/bin/env python3
"""Write the card pack that tests/test_card_pack.c parses (Python/C format contract)."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("blog_cards", ROOT / "tools" / "blog_cards.py")
assert SPEC and SPEC.loader
CARDS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CARDS
SPEC.loader.exec_module(CARDS)

# Keep these values in step with the expectations in tests/test_card_pack.c.
FIXTURE = [
    CARDS.Card(key="pst_a", kind=CARDS.KIND_QUOTE, published_at=1_700_000_000,
               text="引用\n\n第二段", source="《书名》",
               flags=CARDS.FLAG_FEATURED | CARDS.FLAG_SOURCE_INHERITED,
               thread_id="pst_a", thread_pos=0, thread_len=2),
    CARDS.Card(key="pst_b", kind=CARDS.KIND_QUOTE, published_at=1_700_000_100,
               text="Hello, world", source="", thread_id="pst_a", thread_pos=1, thread_len=2),
    CARDS.Card(key="thread:pst_c", kind=CARDS.KIND_NOTE, published_at=1_600_000_000,
               text="\x1f2020-09-13\n第一天", flags=CARDS.FLAG_MERGED_THREAD, thread_id="pst_c"),
]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: blog_cards_fixture.py OUTPUT", file=sys.stderr)
        return 2
    Path(sys.argv[1]).write_bytes(CARDS.pack_cards(FIXTURE, 1_700_000_123, 480))
    return 0


if __name__ == "__main__":
    sys.exit(main())
