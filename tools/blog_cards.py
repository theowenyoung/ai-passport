#!/usr/bin/env python3
"""Turn public blog notes and quotes into a card pack and flash it to the device.

The device never talks to the blog. This tool reads the blog's public Thread API,
keeps untitled notes and quotes, cleans their text for a 240x320 screen, packs
them into one binary image, and writes only the `cards` data partition. The
reading-progress partition is never touched by `update`, so progress survives.

Subcommands:
  fetch     download every public Thread into build/blog_cards/corpus.json
  build     turn the cached corpus into cards.bin and report.txt
  update    fetch + build + write cards.bin to the device
  show      print cards from a built pack
  charset   write the glyph list the firmware font must cover
  font      regenerate the firmware fonts from the pinned Noto Sans SC (needs Node.js)
  backup    read the reading-progress partition into a file
  restore   write a reading-progress backup back to the device
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import html
import json
import os
import random
import re
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from dataclasses import dataclass, field
from html.parser import HTMLParser
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SITE = os.environ.get("BLOG_CARDS_SITE", "https://www.owenyoung.com")
CACHE_DIR = ROOT / "build" / "blog_cards"
CORPUS_PATH = CACHE_DIR / "corpus.json"
PACK_PATH = CACHE_DIR / "cards.bin"
REPORT_PATH = CACHE_DIR / "report.txt"
FONT_DIR = ROOT / "assets" / "fonts" / "blog-cards"
CHARSET_PATH = FONT_DIR / "charset.txt"
PARTITIONS_PATH = ROOT / "partitions.csv"
UI_SOURCES = (ROOT / "main" / "blog_ui.c",)  # Chinese UI strings the font must cover

# The firmware font: Noto Sans SC (SIL OFL 1.1), pinned to a release tag and
# checked by hash, converted with a pinned lv_font_conv.
FONT_REF = "Sans2.004"
FONT_URL = f"https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@{FONT_REF}/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf"
FONT_LICENSE_URL = f"https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@{FONT_REF}/LICENSE"
FONT_SHA256 = "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9"
FONT_SIZES = (16, 20, 24)
LV_FONT_CONV = "lv_font_conv@1.5.3"
CARDS_PARTITION = "cards"
STATE_PARTITION = "cardstat"
USER_AGENT = "blog-cards/1 (+FoloToy AI Passport)"

# Card selection and cleanup rules. See docs/blog-cards/design.md.
MIN_CARD_CHARS = 4             # shorter cards say nothing on their own
MIN_REPLY_NOTE_CHARS = 12      # shorter replies need their Thread to make sense
MERGE_MAX_CHARS = 240          # an all-note Thread up to this length becomes one card
MAX_SOURCE_BYTES = 240

KIND_QUOTE = 1
KIND_NOTE = 2
FLAG_FEATURED = 1 << 0
FLAG_HIDDEN_FROM_LATEST = 1 << 1
FLAG_SOURCE_INHERITED = 1 << 2
FLAG_MERGED_THREAD = 1 << 3

# Pack format v1, little-endian. Strings are UTF-8 and NUL-terminated.
PACK_MAGIC = b"BLGC"
PACK_VERSION = 1
#   header: magic, version, header_size, card_count, record_size, tz_offset_min,
#           index_offset, strings_offset, strings_size, total_size, built_at,
#           payload_crc32, reserved[20], header_crc32 (over the first 60 bytes)
#   card:   id, published_at, text_off, text_len, source_off, source_len, kind,
#           flags, thread_pos, thread_len, reserved
HEADER = struct.Struct("<4sHHIHhIIIIII20sI")
CARD = struct.Struct("<IIIIIHBBHHI")
SEGMENT_MARK = "\x1f"          # a line starting with this is a date line of a merged Thread
assert HEADER.size == 64 and CARD.size == 32


class BlogCardsError(RuntimeError):
    """A user-facing failure; the CLI prints it without a traceback."""


# ---------------------------------------------------------------------------
# Fetch
# ---------------------------------------------------------------------------

def http_json(url: str, retries: int = 5) -> dict:
    last_error: Exception | None = None
    for attempt in range(retries):
        request = urllib.request.Request(
            url, headers={"Accept": "application/json", "User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if error.code == 429 or error.code >= 500:
                last_error = error
                time.sleep(int(error.headers.get("Retry-After", "0") or 0) or 2 ** attempt)
                continue
            body = error.read()[:300].decode("utf-8", "replace")
            raise BlogCardsError(f"GET {url} failed: HTTP {error.code} {body}") from error
        except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
            last_error = error
            time.sleep(2 ** attempt)
    raise BlogCardsError(f"GET {url} failed after {retries} attempts: {last_error}")


def fetch_corpus(site: str, log=print) -> dict:
    """Walk every public Thread and return all of their published posts."""
    site = site.rstrip("/")
    threads: list[dict] = []
    cursor = None
    while True:
        params = {"visibility": "any", "sort": "published", "include": "fold", "limit": 100}
        if cursor:
            params["cursor"] = cursor
        page = http_json(f"{site}/api/public/threads?{urllib.parse.urlencode(params)}")
        threads += page["threads"]
        cursor = page.get("nextCursor")
        log(f"  threads: {len(threads)}")
        if not cursor:
            break

    posts: dict[str, dict] = {}
    for thread in threads:
        fold = thread.get("fold") or {}
        for post in [thread["root"], *(fold.get("leading") or []), *(fold.get("trailing") or [])]:
            posts[post["id"]] = post
        if fold.get("hidden"):
            slug = urllib.parse.quote(thread["root"]["slug"])
            cursor = None
            while True:
                params = {"limit": 100}
                if cursor:
                    params["cursor"] = cursor
                page = http_json(f"{site}/api/public/threads/{slug}/posts?{urllib.parse.urlencode(params)}")
                for post in page["posts"]:
                    posts[post["id"]] = post
                cursor = page.get("nextCursor")
                if not cursor:
                    break

    corpus = {
        "site": site,
        "fetchedAt": int(time.time()),
        "threads": [{key: thread[key] for key in ("id", "postCount")} for thread in threads],
        "posts": sorted(posts.values(), key=lambda post: post["id"]),
    }
    verify_corpus(corpus)
    log(f"  posts: {len(corpus['posts'])}")
    return corpus


def verify_corpus(corpus: dict) -> None:
    """Reject a corpus whose Threads are incomplete, so no card goes missing silently."""
    posts = {post["id"]: post for post in corpus["posts"]}
    counts: dict[str, int] = {}
    for post in posts.values():
        counts[post["threadId"]] = counts.get(post["threadId"], 0) + 1
    problems = []
    for thread in corpus["threads"]:
        have = counts.get(thread["id"], 0)
        if have != thread["postCount"]:
            problems.append(f"Thread {thread['id']}: {have} posts collected, postCount {thread['postCount']}")
    thread_ids = {thread["id"] for thread in corpus["threads"]}
    for post in posts.values():
        if post["threadId"] not in thread_ids:
            problems.append(f"post {post['id']}: Thread {post['threadId']} was not listed")
        parent = post.get("replyToId")
        if parent and (parent not in posts or posts[parent]["threadId"] != post["threadId"]):
            problems.append(f"post {post['id']}: parent {parent} is missing from its Thread")
    if problems:
        shown = "\n  ".join(problems[:10])
        raise BlogCardsError(f"corpus is incomplete ({len(problems)} problems):\n  {shown}")


# ---------------------------------------------------------------------------
# Text cleanup
# ---------------------------------------------------------------------------

URL_RE = re.compile(r"https?://[^\s<>\"'　-〿＀-￯一-鿿]+")
EMOJI_RANGES = (
    (0x200D, 0x200D),     # zero-width joiner
    (0x20E3, 0x20E3),     # combining keycap
    (0x2600, 0x27BF),     # miscellaneous symbols and dingbats
    (0x2B00, 0x2BFF),     # arrows and stars used as emoji
    (0xFE00, 0xFE0F),     # variation selectors
    (0x10000, 0x10FFFF),  # everything outside the BMP: emoji, rare glyphs
)
INVISIBLE = dict.fromkeys(map(ord, "​‌⁠﻿­"), None)


def url_label(url: str) -> str:
    host = urllib.parse.urlsplit(url).hostname or "link"
    return f"[链接 · {host.removeprefix('www.')}]"


def replace_bare_urls(text: str) -> str:
    def label(match: re.Match) -> str:
        url = match.group(0).rstrip(".,;:!?)]}'\"")
        return url_label(url) + match.group(0)[len(url):]
    return URL_RE.sub(label, text)


def strip_emoji(text: str) -> str:
    return "".join(ch for ch in text
                   if not any(lo <= ord(ch) <= hi for lo, hi in EMOJI_RANGES))


def tidy(text: str) -> str:
    """Normalize whitespace while keeping paragraph breaks."""
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace(" ", " ")
    text = text.translate(INVISIBLE)
    lines = [re.sub(r"[ \t]{2,}", " ", line).strip() for line in text.split("\n")]
    text = "\n".join(lines)
    return re.sub(r"\n{3,}", "\n\n", text).strip()


def clean(text: str) -> str:
    return tidy(strip_emoji(replace_bare_urls(text)))


class _HtmlToText(HTMLParser):
    """Keep paragraphs, list items, quotes, and link text from rendered note HTML."""

    BLOCKS = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "li",
              "ul", "ol", "blockquote", "hr", "figure", "table", "tr"}

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.blocks: list[tuple[str, str]] = []
        self.buffer: list[str] = []
        self.lists: list[list] = []
        self.containers = 0        # numbers each list/quote so neighbours stay apart
        self.quote_id = 0
        self.quote_depth = 0
        self.pre_depth = 0
        self.marker: str | None = None
        self.links: list[tuple[int, str]] = []

    def flush(self) -> None:
        text = "".join(self.buffer)
        self.buffer = []
        if not self.pre_depth:
            text = re.sub(r"[ \t\r\n]+", " ", text.replace("\n", "\x00")).replace("\x00", "\n")
        lines = [line.strip() for line in text.split("\n")]
        text = "\n".join(line for line in lines if line)
        if not text:
            return
        prefix = ("｜" if self.quote_depth else "") + (self.marker or "")
        self.marker = None
        if self.lists:
            group = f"list{self.lists[-1][2]}"
        elif self.quote_depth:
            group = f"quote{self.quote_id}"
        else:
            group = ""
        self.blocks.append((group, prefix + text))

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in self.BLOCKS:
            self.flush()
        if tag in ("ul", "ol"):
            self.containers += 1
            self.lists.append([tag, 0, self.containers])
        elif tag == "li" and self.lists:
            self.lists[-1][1] += 1
            kind, number, _ = self.lists[-1]
            self.marker = "• " if kind == "ul" else f"{number}. "
        elif tag == "blockquote":
            if not self.quote_depth:
                self.containers += 1
                self.quote_id = self.containers
            self.quote_depth += 1
        elif tag == "pre":
            self.pre_depth += 1
        elif tag == "br":
            self.buffer.append("\n")
        elif tag == "a":
            self.links.append((len(self.buffer), dict(attrs).get("href") or ""))

    def handle_endtag(self, tag: str) -> None:
        if tag == "a" and self.links:
            start, href = self.links.pop()
            anchor = "".join(self.buffer[start:]).strip()
            if not anchor or URL_RE.fullmatch(anchor) or anchor == href:
                self.buffer[start:] = [url_label(href or anchor)]
            return
        if tag in self.BLOCKS:
            self.flush()
        if tag in ("ul", "ol") and self.lists:
            self.lists.pop()
        elif tag == "blockquote" and self.quote_depth:
            self.quote_depth -= 1
        elif tag == "pre" and self.pre_depth:
            self.pre_depth -= 1

    def handle_data(self, data: str) -> None:
        self.buffer.append(data)

    def text(self) -> str:
        self.flush()
        out: list[str] = []
        previous = None
        for group, block in self.blocks:
            if out:
                # Items of one list or one quote sit on adjacent lines.
                out.append("\n" if group and group == previous else "\n\n")
            out.append(block)
            previous = group
        return "".join(out)


def html_to_text(markup: str) -> str:
    parser = _HtmlToText()
    parser.feed(markup or "")
    parser.close()
    return clean(parser.text())


MD_RULES = (
    (re.compile(r"^[ \t]{0,3}#{1,6}[ \t]+", re.M), ""),
    (re.compile(r"^[ \t]{0,3}>[ \t]?", re.M), "｜"),
    (re.compile(r"^[ \t]{0,3}[-*+][ \t]+", re.M), "• "),
    (re.compile(r"\*\*(.+?)\*\*"), r"\1"),
    (re.compile(r"__(.+?)__"), r"\1"),
    (re.compile(r"`([^`\n]+)`"), r"\1"),
    (re.compile(r"!?\[([^\]\n]+)\]\((https?://[^)\s]+)\)"), r"\1"),
)


def quote_to_text(text: str) -> str:
    text = html.unescape(text or "")
    for pattern, replacement in MD_RULES:
        text = pattern.sub(replacement, text)
    return clean(text)


def visible_len(text: str) -> int:
    return sum(1 for ch in text if not ch.isspace() and ch != SEGMENT_MARK)


# ---------------------------------------------------------------------------
# Cards
# ---------------------------------------------------------------------------

@dataclass
class Card:
    key: str
    kind: int
    published_at: int
    text: str
    source: str = ""
    flags: int = 0
    thread_id: str = ""
    thread_pos: int = 0
    thread_len: int = 1
    permalink: str = ""

    @property
    def id(self) -> int:
        return fnv1a32(self.key)


@dataclass
class BuildResult:
    cards: list[Card]
    excluded: dict[str, list[dict]] = field(default_factory=dict)
    stats: dict[str, int] = field(default_factory=dict)


def fnv1a32(text: str) -> int:
    value = 0x811C9DC5
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def has_media(post: dict) -> bool:
    return (any(item.get("type") == "media" for item in post.get("attachments") or [])
            or "<img" in (post.get("bodyHtml") or ""))


def has_text_attachment(post: dict) -> bool:
    return any(item.get("type") == "text" for item in post.get("attachments") or [])


def in_scope(post: dict) -> bool:
    if post["format"] == "quote":
        return True
    return post["format"] == "note" and not (post.get("title") or "").strip()


def exclusion_reason(post: dict, include_hidden: bool) -> str | None:
    if has_media(post):
        return "has_media"
    if post["format"] == "note" and has_text_attachment(post):
        return "text_attachment"
    if not include_hidden and post.get("visibility") == "latest_hidden":
        return "hidden_from_latest"
    return None


def post_text(post: dict) -> str:
    if post["format"] == "quote":
        return quote_to_text(post.get("quoteText") or "")
    return html_to_text(post.get("bodyHtml") or "")


def thread_order(posts: Iterable[dict], root_id: str) -> list[dict]:
    return sorted(posts, key=lambda post: (post["id"] != root_id, post["createdAt"], post["id"]))


def inherited_source(post: dict, by_id: dict[str, dict]) -> str:
    """Nearest ancestor's quote source, link title, or note title."""
    parent = post.get("replyToId")
    while parent and parent in by_id:
        ancestor = by_id[parent]
        if ancestor["format"] == "quote" and (ancestor.get("sourceName") or "").strip():
            return ancestor["sourceName"].strip()
        if ancestor["format"] in ("link", "note") and (ancestor.get("title") or "").strip():
            return ancestor["title"].strip()
        parent = ancestor.get("replyToId")
    return ""


def local_date(timestamp: int, tz_offset_min: int) -> str:
    zone = dt.timezone(dt.timedelta(minutes=tz_offset_min))
    return dt.datetime.fromtimestamp(timestamp, zone).strftime("%Y-%m-%d")


def truncate_utf8(text: str, limit: int) -> str:
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return text
    return raw[:limit].decode("utf-8", "ignore").rstrip() + "…"


def build_cards(corpus: dict, include_hidden: bool = True, tz_offset_min: int = 480) -> BuildResult:
    by_id = {post["id"]: post for post in corpus["posts"]}
    threads: dict[str, list[dict]] = {}
    for post in corpus["posts"]:
        threads.setdefault(post["threadId"], []).append(post)

    result = BuildResult(cards=[])
    excluded = result.excluded
    stats = {"source_own": 0, "source_inherited": 0, "source_none": 0, "merged_threads": 0}

    def exclude(reason: str, post: dict, text: str = "") -> None:
        excluded.setdefault(reason, []).append(
            {"id": post["id"], "permalink": post.get("permalink", ""),
             "text": re.sub(r"\s+", " ", text)[:40]})

    ordered_roots = sorted(threads, key=lambda tid: (by_id[tid].get("publishedAt") or 0, tid))
    for thread_id in ordered_roots:
        root = by_id[thread_id]
        posts = thread_order(threads[thread_id], thread_id)
        featured = bool(root.get("featuredAt"))
        hidden = root.get("visibility") == "latest_hidden"

        candidates: list[tuple[dict, str]] = []
        for post in posts:
            if not in_scope(post):
                continue
            reason = exclusion_reason(post, include_hidden)
            text = post_text(post) if reason is None else ""
            if reason is None and visible_len(text) < MIN_CARD_CHARS:
                reason = "too_short"
            if reason:
                exclude(reason, post, text or post_text(post))
                continue
            candidates.append((post, text))

        base_flags = (FLAG_FEATURED if featured else 0) | (FLAG_HIDDEN_FROM_LATEST if hidden else 0)
        all_notes = (len(posts) >= 2 and len(candidates) == len(posts)
                     and all(post["format"] == "note" for post, _ in candidates))
        total = sum(visible_len(text) for _, text in candidates)
        thread_cards: list[Card] = []
        if all_notes and total <= MERGE_MAX_CHARS:
            segments = [f"{SEGMENT_MARK}{local_date(post['publishedAt'], tz_offset_min)}\n{text}"
                        for post, text in candidates]
            thread_cards.append(Card(
                key=f"thread:{thread_id}", kind=KIND_NOTE,
                published_at=root.get("publishedAt") or 0, text="\n\n".join(segments),
                flags=base_flags | FLAG_MERGED_THREAD, thread_id=thread_id,
                permalink=root.get("permalink", "")))
            stats["merged_threads"] += 1
        else:
            # A short reply reads fine after its root card, but not when the root
            # is a link, a titled note, or an image post that never becomes a card.
            root_is_card = any(post["id"] == thread_id for post, _ in candidates)
            for post, text in candidates:
                if (post["format"] == "note" and post.get("replyToId") and not root_is_card
                        and visible_len(text) < MIN_REPLY_NOTE_CHARS):
                    exclude("short_reply_out_of_context", post, text)
                    continue
                flags = base_flags
                source = ""
                if post["format"] == "quote":
                    source = (post.get("sourceName") or "").strip()
                    if source:
                        stats["source_own"] += 1
                    else:
                        source = inherited_source(post, by_id)
                        if source:
                            flags |= FLAG_SOURCE_INHERITED
                            stats["source_inherited"] += 1
                        else:
                            stats["source_none"] += 1
                thread_cards.append(Card(
                    key=post["id"], kind=KIND_QUOTE if post["format"] == "quote" else KIND_NOTE,
                    published_at=post.get("publishedAt") or 0, text=text,
                    source=truncate_utf8(clean(source), MAX_SOURCE_BYTES), flags=flags,
                    thread_id=thread_id, permalink=post.get("permalink", "")))
        for position, card in enumerate(thread_cards):
            card.thread_pos = position
            card.thread_len = len(thread_cards)
        result.cards.extend(thread_cards)

    ids: dict[int, str] = {}
    for card in result.cards:
        if card.id in ids:
            raise BlogCardsError(f"card id collision: {card.key} and {ids[card.id]}")
        ids[card.id] = card.key

    stats["cards"] = len(result.cards)
    stats["quote_cards"] = sum(1 for card in result.cards if card.kind == KIND_QUOTE)
    stats["note_cards"] = sum(1 for card in result.cards if card.kind == KIND_NOTE)
    result.stats = stats
    return result


# ---------------------------------------------------------------------------
# Pack format
# ---------------------------------------------------------------------------

def pack_cards(cards: list[Card], built_at: int, tz_offset_min: int) -> bytes:
    strings = bytearray()

    def add(text: str) -> tuple[int, int]:
        offset = len(strings)
        raw = text.encode("utf-8")
        if b"\x00" in raw:
            raise BlogCardsError("card text contains NUL")
        strings.extend(raw + b"\x00")
        return offset, len(raw)

    records = bytearray()
    for card in cards:
        text_off, text_len = add(card.text)
        source_off, source_len = add(card.source)
        records += CARD.pack(card.id, card.published_at, text_off, text_len, source_off,
                             source_len, card.kind, card.flags, card.thread_pos,
                             card.thread_len, 0)
    index_offset = HEADER.size
    strings_offset = index_offset + len(records)
    payload = bytes(records) + bytes(strings)
    total = HEADER.size + len(payload)
    header = HEADER.pack(PACK_MAGIC, PACK_VERSION, HEADER.size, len(cards), CARD.size,
                         tz_offset_min, index_offset, strings_offset, len(strings), total,
                         built_at, zlib.crc32(payload), b"", 0)
    header = header[:-4] + struct.pack("<I", zlib.crc32(header[:-4]))
    return header + payload


def unpack_pack(data: bytes) -> dict:
    """Parse and verify a pack exactly as the firmware must."""
    if len(data) < HEADER.size:
        raise BlogCardsError("pack is shorter than its header")
    (magic, version, header_size, count, record_size, tz_offset_min, index_offset,
     strings_offset, strings_size, total, built_at, payload_crc, _,
     header_crc) = HEADER.unpack_from(data)
    if magic != PACK_MAGIC or version != PACK_VERSION:
        raise BlogCardsError("not a v1 card pack")
    if zlib.crc32(data[:HEADER.size - 4]) != header_crc:
        raise BlogCardsError("pack header checksum mismatch")
    if header_size != HEADER.size or record_size != CARD.size or total > len(data):
        raise BlogCardsError("pack header is inconsistent")
    if index_offset + count * record_size != strings_offset or strings_offset + strings_size != total:
        raise BlogCardsError("pack layout is inconsistent")
    if zlib.crc32(data[HEADER.size:total]) != payload_crc:
        raise BlogCardsError("pack payload checksum mismatch")

    def string(offset: int, length: int) -> str:
        start = strings_offset + offset
        if offset + length >= strings_size or data[start + length] != 0:
            raise BlogCardsError("string out of range")
        return data[start:start + length].decode("utf-8")

    cards = []
    for index in range(count):
        (card_id, published_at, text_off, text_len, source_off, source_len, kind, flags,
         thread_pos, thread_len, _) = CARD.unpack_from(data, index_offset + index * record_size)
        cards.append({"id": card_id, "publishedAt": published_at, "kind": kind, "flags": flags,
                      "threadPos": thread_pos, "threadLen": thread_len,
                      "text": string(text_off, text_len), "source": string(source_off, source_len)})
    return {"builtAt": built_at, "tzOffsetMin": tz_offset_min, "size": total, "cards": cards}


# ---------------------------------------------------------------------------
# Glyph coverage
# ---------------------------------------------------------------------------

def gb2312_chars() -> set[str]:
    chars = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                chars.add(bytes((high, low)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def base_charset() -> set[str]:
    # Python maps GB2312 A1A4 to U+30FB, so the Latin middle dot used in link
    # labels is added explicitly with the other typographic marks.
    return gb2312_chars() | {chr(code) for code in range(0x20, 0x7F)} | set("·…—–“”‘’•")


def card_chars(cards: Iterable[Card]) -> set[str]:
    return {ch for card in cards for ch in card.text + card.source
            if ch not in "\n" + SEGMENT_MARK}


def load_charset(path: Path) -> set[str]:
    if path.exists():
        return set(path.read_text(encoding="utf-8").replace("\n", ""))
    return base_charset()


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def build_report(result: BuildResult, corpus: dict, pack: bytes, charset_label: str,
                 missing: dict[str, str], tz_offset_min: int) -> str:
    formats: dict[str, int] = {}
    for post in corpus["posts"]:
        formats[post["format"]] = formats.get(post["format"], 0) + 1
    lengths = sorted(visible_len(card.text) for card in result.cards) or [0]
    stats = result.stats
    fetched = local_date(corpus.get("fetchedAt") or 0, tz_offset_min)
    lines = [
        f"Blog cards — {corpus.get('site', '')} (fetched {fetched})",
        f"Source: {len(corpus['threads'])} threads, {len(corpus['posts'])} posts "
        + ", ".join(f"{key} {value}" for key, value in sorted(formats.items())),
        f"Cards: {stats['cards']} (quote {stats['quote_cards']}, note {stats['note_cards']}, "
        f"merged note threads {stats['merged_threads']})",
        f"Quote sources: own {stats['source_own']}, inherited {stats['source_inherited']}, "
        f"none {stats['source_none']}",
        f"Length (visible chars): median {lengths[len(lengths) // 2]}, "
        f"p90 {lengths[int(len(lengths) * 0.9)]}, max {lengths[-1]}",
        f"Pack: {len(pack)} bytes",
        f"Glyphs: {len(card_chars(result.cards))} distinct; checked against {charset_label}",
    ]
    if missing:
        lines.append(f"  MISSING {len(missing)} glyphs (regenerate the font and rebuild firmware):")
        for ch, where in sorted(missing.items()):
            lines.append(f"    U+{ord(ch):04X} {ch}  e.g. {where}")
    else:
        lines.append("  all glyphs covered")
    lines.append("Excluded:")
    for reason, items in sorted(result.excluded.items()):
        lines.append(f"  {reason}: {len(items)}")
        for item in items[:5]:
            lines.append(f"    {item['permalink']}  {item['text']}".rstrip())
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Device access
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class PartitionEntry:
    name: str
    offset: int
    size: int


def parse_partitions(text: str) -> dict[str, PartitionEntry]:
    entries = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cells = [cell.strip() for cell in line.split(",")]
        if len(cells) < 5 or not cells[3] or not cells[4]:
            continue
        entries[cells[0]] = PartitionEntry(cells[0], parse_size(cells[3]), parse_size(cells[4]))
    return entries


def parse_size(value: str) -> int:
    value = value.strip().upper()
    for suffix, scale in (("K", 1024), ("M", 1024 * 1024)):
        if value.endswith(suffix):
            return int(value[:-1], 0) * scale
    return int(value, 0)


def partition(name: str) -> PartitionEntry:
    entries = parse_partitions(PARTITIONS_PATH.read_text(encoding="utf-8"))
    if name not in entries:
        raise BlogCardsError(f"partitions.csv has no '{name}' partition")
    return entries[name]


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        raise BlogCardsError("no device found: turn it on and connect it with a data USB cable")
    if len(ports) > 1:
        raise BlogCardsError(f"several serial ports found ({', '.join(ports)}); pass --port")
    return ports[0]


def esptool_candidates() -> Iterable[list[str]]:
    yield [sys.executable, "-m", "esptool"]
    for name in ("esptool.py", "esptool"):
        yield [name]
    # An installed ESP-IDF keeps esptool in its own Python environment, so content
    # updates work without activating ESP-IDF first.
    tools = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    for python in sorted(tools.glob("python_env/idf*/bin/python"), reverse=True):
        yield [str(python), "-m", "esptool"]


def esptool_command() -> tuple[list[str], int]:
    """Return a working esptool command and its major version."""
    for command in esptool_candidates():
        try:
            completed = subprocess.run(command + ["version"], capture_output=True, text=True)
        except OSError:
            continue
        match = re.search(r"(\d+)\.\d+", completed.stdout)
        if completed.returncode == 0 and match:
            return command, int(match.group(1))
    raise BlogCardsError("esptool not found: install ESP-IDF 5.5.3 or run `pip install esptool`")


def run_esptool(port: str, subcommand: str, *args: str) -> None:
    command, major = esptool_command()
    if major < 5:
        subcommand = subcommand.replace("-", "_")  # esptool v4 spells subcommands this way
    completed = subprocess.run(command + ["--chip", "esp32c3", "-p", port, "-b", "460800",
                                          subcommand, *args])
    if completed.returncode != 0:
        raise BlogCardsError(f"esptool {subcommand} failed with exit code {completed.returncode}")


def write_partition(name: str, image: Path, port: str | None) -> None:
    target = partition(name)
    size = image.stat().st_size
    if size > target.size:
        raise BlogCardsError(f"{image} is {size} bytes; '{name}' holds only {target.size}")
    port = find_port(port)
    print(f"Writing {image.name} ({size} bytes) to '{name}' at 0x{target.offset:X} on {port}")
    run_esptool(port, "write-flash", f"0x{target.offset:X}", str(image))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def local_tz_offset_min() -> int:
    offset = dt.datetime.now().astimezone().utcoffset() or dt.timedelta()
    return int(offset.total_seconds() // 60)


def load_corpus(path: Path) -> dict:
    if not path.exists():
        raise BlogCardsError(f"{path} not found; run `fetch` first")
    return json.loads(path.read_text(encoding="utf-8"))


def command_fetch(args: argparse.Namespace) -> None:
    print(f"Fetching public Threads from {args.site}")
    corpus = fetch_corpus(args.site)
    CORPUS_PATH.parent.mkdir(parents=True, exist_ok=True)
    CORPUS_PATH.write_text(json.dumps(corpus, ensure_ascii=False), encoding="utf-8")
    print(f"Saved {CORPUS_PATH.relative_to(ROOT)}")


def command_build(args: argparse.Namespace) -> None:
    corpus = load_corpus(CORPUS_PATH)
    tz_offset_min = args.tz_offset if args.tz_offset is not None else local_tz_offset_min()
    result = build_cards(corpus, include_hidden=not args.exclude_hidden, tz_offset_min=tz_offset_min)
    pack = pack_cards(result.cards, int(time.time()), tz_offset_min)
    if len(unpack_pack(pack)["cards"]) != len(result.cards):
        raise BlogCardsError("pack round trip lost cards")
    charset_path = Path(args.charset) if args.charset else CHARSET_PATH
    charset = load_charset(charset_path)
    label = (str(charset_path.relative_to(ROOT)) if charset_path.exists()
             and charset_path.is_relative_to(ROOT) else "GB2312 + ASCII")
    missing: dict[str, str] = {}
    for card in result.cards:
        for ch in card.text + card.source:
            if ch not in charset and ch not in "\n" + SEGMENT_MARK and ch not in missing:
                missing[ch] = card.permalink or card.key
    report = build_report(result, corpus, pack, label, missing, tz_offset_min)
    PACK_PATH.parent.mkdir(parents=True, exist_ok=True)
    PACK_PATH.write_bytes(pack)
    REPORT_PATH.write_text(report, encoding="utf-8")
    print(report, end="")
    print(f"Saved {PACK_PATH.relative_to(ROOT)} and {REPORT_PATH.relative_to(ROOT)}")


def command_update(args: argparse.Namespace) -> None:
    if not args.offline:
        command_fetch(args)
    command_build(args)
    write_partition(CARDS_PARTITION, PACK_PATH, args.port)
    print("Done. The device restarts with the new cards; reading progress is kept.")


def command_show(args: argparse.Namespace) -> None:
    pack = unpack_pack(Path(args.pack).read_bytes())
    cards = pack["cards"]
    chosen = random.sample(cards, min(args.count, len(cards))) if args.random else cards[:args.count]
    for card in chosen:
        kind = "quote" if card["kind"] == KIND_QUOTE else "note"
        date = local_date(card["publishedAt"], pack["tzOffsetMin"])
        where = f" · {card['source']}" if card["source"] else ""
        thread = f" · {card['threadPos'] + 1}/{card['threadLen']}" if card["threadLen"] > 1 else ""
        print(f"── {kind} {date}{where}{thread} [{card['id']:08x}]")
        print(card["text"].replace(SEGMENT_MARK, "— "))
        print()


def ui_chars() -> set[str]:
    return {ch for path in UI_SOURCES if path.exists()
            for ch in path.read_text(encoding="utf-8") if ord(ch) > 0x7E}


def write_charset(output: Path) -> list[str]:
    chars = base_charset() | ui_chars()
    if CORPUS_PATH.exists():
        chars |= card_chars(build_cards(load_corpus(CORPUS_PATH)).cards)
    ordered = sorted(chars - {"\n", SEGMENT_MARK})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(ordered) + "\n", encoding="utf-8")
    return ordered


def command_charset(args: argparse.Namespace) -> None:
    output = Path(args.output) if args.output else CHARSET_PATH
    print(f"Wrote {len(write_charset(output))} glyphs to {output}")


def download(url: str, path: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            path.write_bytes(response.read())
    except (urllib.error.URLError, TimeoutError) as error:
        raise BlogCardsError(f"download failed: {url}: {error}") from error


def command_font(args: argparse.Namespace) -> None:
    """Regenerate the firmware fonts; rebuild and flash the firmware afterwards."""
    import hashlib
    cache = CACHE_DIR / "font"
    cache.mkdir(parents=True, exist_ok=True)
    source = cache / "NotoSansSC-Regular.otf"
    if not source.exists() or hashlib.sha256(source.read_bytes()).hexdigest() != FONT_SHA256:
        print(f"Downloading {FONT_URL}")
        download(FONT_URL, source)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != FONT_SHA256:
        raise BlogCardsError(f"font hash mismatch: {digest}")
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    download(FONT_LICENSE_URL, FONT_DIR / "OFL.txt")

    glyphs = write_charset(CHARSET_PATH)
    print(f"Charset: {len(glyphs)} glyphs -> {CHARSET_PATH.relative_to(ROOT)}")
    symbols = "".join(glyphs)
    for size in FONT_SIZES:
        output = FONT_DIR / f"blog_font_{size}.c"
        command = ["npx", "--yes", LV_FONT_CONV, "--font", str(source), "--symbols", symbols,
                   "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
                   "--lv-font-name", f"blog_font_{size}", "--lv-include", "lvgl.h",
                   "-o", str(output)]
        print(f"Converting {size}px -> {output.relative_to(ROOT)}")
        if subprocess.run(command).returncode != 0:
            raise BlogCardsError(f"lv_font_conv failed for {size}px (is Node.js installed?)")
    print("Fonts updated. Rebuild the firmware and flash it to use them.")


def command_backup(args: argparse.Namespace) -> None:
    target = partition(STATE_PARTITION)
    port = find_port(args.port)
    run_esptool(port, "read-flash", f"0x{target.offset:X}", f"0x{target.size:X}", args.output)
    print(f"Saved reading progress to {args.output}")


def command_restore(args: argparse.Namespace) -> None:
    write_partition(STATE_PARTITION, Path(args.input), args.port)
    print("Restored reading progress.")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def with_build_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--exclude-hidden", action="store_true",
                             help="drop Threads hidden from the homepage")
        command.add_argument("--tz-offset", type=int, default=None,
                             help="minutes east of UTC for dates (default: this computer)")
        command.add_argument("--charset", help="glyph list to check against")

    fetch = sub.add_parser("fetch", help="download every public Thread")
    fetch.add_argument("--site", default=DEFAULT_SITE)
    fetch.set_defaults(func=command_fetch)

    build = sub.add_parser("build", help="build cards.bin from the cached corpus")
    with_build_options(build)
    build.set_defaults(func=command_build)

    update = sub.add_parser("update", help="fetch, build, and write cards to the device")
    update.add_argument("--site", default=DEFAULT_SITE)
    update.add_argument("--port")
    update.add_argument("--offline", action="store_true", help="reuse the cached corpus")
    with_build_options(update)
    update.set_defaults(func=command_update)

    show = sub.add_parser("show", help="print cards from a pack")
    show.add_argument("--pack", default=str(PACK_PATH))
    show.add_argument("-n", "--count", type=int, default=5)
    show.add_argument("--random", action="store_true")
    show.set_defaults(func=command_show)

    charset = sub.add_parser("charset", help="write the glyph list for the firmware font")
    charset.add_argument("--output")
    charset.set_defaults(func=command_charset)

    font = sub.add_parser("font", help="regenerate the firmware fonts (needs Node.js)")
    font.set_defaults(func=command_font)

    backup = sub.add_parser("backup", help="save reading progress from the device")
    backup.add_argument("--port")
    backup.add_argument("--output", default="blog-cards-progress.bin")
    backup.set_defaults(func=command_backup)

    restore = sub.add_parser("restore", help="write a reading-progress backup to the device")
    restore.add_argument("--port")
    restore.add_argument("--input", required=True)
    restore.set_defaults(func=command_restore)

    args = parser.parse_args(argv)
    try:
        args.func(args)
    except BlogCardsError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
