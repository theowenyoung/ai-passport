#!/usr/bin/env python3
"""Host tests for the blog card pack builder; no network or device needed."""

from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("blog_cards", ROOT / "tools" / "blog_cards.py")
assert SPEC and SPEC.loader
CARDS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CARDS
SPEC.loader.exec_module(CARDS)

DAY = 86400
BASE_TIME = 1_700_000_000


def post(post_id: str, fmt: str = "note", thread: str | None = None, reply_to: str | None = None,
         when: int = 0, **fields) -> dict:
    data = {
        "id": post_id, "format": fmt, "threadId": thread or post_id, "replyToId": reply_to,
        "publishedAt": BASE_TIME + when, "createdAt": BASE_TIME + when,
        "visibility": "public", "attachments": [], "permalink": f"/{post_id}",
        "title": None, "bodyHtml": "", "quoteText": None, "sourceName": None, "featuredAt": None,
    }
    data.update(fields)
    return data


def corpus(*posts: dict) -> dict:
    counts: dict[str, int] = {}
    for item in posts:
        counts[item["threadId"]] = counts.get(item["threadId"], 0) + 1
    return {"site": "https://example.com", "fetchedAt": BASE_TIME,
            "threads": [{"id": tid, "postCount": n} for tid, n in counts.items()],
            "posts": list(posts)}


def texts(result) -> list[str]:
    return [card.text for card in result.cards]


class TextCleanupTest(unittest.TestCase):
    def test_html_keeps_paragraphs_lists_and_link_text(self) -> None:
        markup = ("<p>和<a href=\"https://weibo.com/u/1\">李马克</a> 录了一期。</p>"
                  "<ol><li><p>第一</p></li><li><p>第二</p></li></ol>"
                  "<ul><li>甲</li></ul><p>末尾<br>换行</p>")
        self.assertEqual(CARDS.html_to_text(markup),
                         "和李马克 录了一期。\n\n1. 第一\n2. 第二\n\n• 甲\n\n末尾\n换行")

    def test_html_labels_links_whose_text_is_the_url(self) -> None:
        markup = '<p>看 <a href="https://www.text.npr.org/x">https://www.text.npr.org/x</a> 吧</p>'
        self.assertEqual(CARDS.html_to_text(markup), "看 [链接 · text.npr.org] 吧")

    def test_html_marks_blockquotes(self) -> None:
        markup = "<p>他说：</p><blockquote><p>第一句</p><p>第二句</p></blockquote>"
        self.assertEqual(CARDS.html_to_text(markup), "他说：\n\n｜第一句\n｜第二句")

    def test_quote_markdown_is_stripped_but_lines_kept(self) -> None:
        text = "- **不要**推销\n- 看 [这篇](https://a.com/b)\n\n\n\n> 引用"
        self.assertEqual(CARDS.quote_to_text(text), "• 不要推销\n• 看 这篇\n\n｜引用")

    def test_emoji_and_bare_urls(self) -> None:
        self.assertEqual(CARDS.clean("开心✌️ 冲💪💪 见 https://x.com/a)。"),
                         "开心 冲 见 [链接 · x.com])。")

    def test_visible_length_ignores_whitespace(self) -> None:
        self.assertEqual(CARDS.visible_len("一 二\n\n三"), 3)


class BuildCardsTest(unittest.TestCase):
    def test_scope_and_exclusions(self) -> None:
        result = CARDS.build_cards(corpus(
            post("q1", "quote", quoteText="引用内容足够长", sourceName="书"),
            post("n1", bodyHtml="<p>一条没有标题的笔记</p>", when=1),
            post("n2", title="有标题", bodyHtml="<p>长文</p>", when=2),
            post("l1", "link", title="链接", url="https://a.com", when=3),
            post("m1", bodyHtml="<p>带图的笔记内容</p>", when=4,
                 attachments=[{"type": "media", "mimeType": "image/png"}]),
            post("t1", bodyHtml="<p>带长文附件的笔记</p>", when=5, attachments=[{"type": "text"}]),
            post("s1", bodyHtml="<p>开心</p>", when=6),
        ))
        self.assertEqual(texts(result), ["引用内容足够长", "一条没有标题的笔记"])
        self.assertEqual(sorted(result.excluded), ["has_media", "text_attachment", "too_short"])

    def test_hidden_threads_are_optional(self) -> None:
        data = corpus(post("n1", bodyHtml="<p>首页隐藏的笔记内容</p>", visibility="latest_hidden"))
        self.assertEqual(len(CARDS.build_cards(data).cards), 1)
        self.assertTrue(CARDS.build_cards(data).cards[0].flags & CARDS.FLAG_HIDDEN_FROM_LATEST)
        self.assertEqual(CARDS.build_cards(data, include_hidden=False).cards, [])

    def test_quote_replies_inherit_the_nearest_source(self) -> None:
        result = CARDS.build_cards(corpus(
            post("r", "quote", quoteText="第一段摘录内容", sourceName="《爱的艺术》"),
            post("a", "quote", "r", "r", 10, quoteText="第二段摘录内容"),
            post("b", "quote", "r", "a", 20, quoteText="第三段摘录内容", sourceName="别的书"),
            post("c", "quote", "r", "b", 30, quoteText="第四段摘录内容"),
        ))
        self.assertEqual([card.source for card in result.cards],
                         ["《爱的艺术》", "《爱的艺术》", "别的书", "别的书"])
        self.assertEqual([card.thread_pos for card in result.cards], [0, 1, 2, 3])
        self.assertTrue(all(card.thread_len == 4 for card in result.cards))
        self.assertEqual(result.stats["source_inherited"], 2)

    def test_quote_under_a_link_uses_the_link_title(self) -> None:
        result = CARDS.build_cards(corpus(
            post("l", "link", title="买一匹马｜周六晚报", url="https://a.com"),
            post("q", "quote", "l", "l", 10, quoteText="文章里的一段话"),
        ))
        self.assertEqual(result.cards[0].source, "买一匹马｜周六晚报")
        self.assertTrue(result.cards[0].flags & CARDS.FLAG_SOURCE_INHERITED)

    def test_short_note_thread_becomes_one_card_with_dates(self) -> None:
        result = CARDS.build_cards(corpus(
            post("r", bodyHtml="<p>第一天的记录</p>"),
            post("a", thread="r", reply_to="r", when=DAY, bodyHtml="<p>第二天的记录</p>"),
        ), tz_offset_min=0)
        self.assertEqual(len(result.cards), 1)
        card = result.cards[0]
        self.assertEqual(card.key, "thread:r")
        self.assertTrue(card.flags & CARDS.FLAG_MERGED_THREAD)
        self.assertEqual(card.text, "\x1f2023-11-14\n第一天的记录\n\n\x1f2023-11-15\n第二天的记录")

    def test_long_note_thread_stays_separate_and_keeps_short_replies(self) -> None:
        long_text = "很长的记录" * 60
        result = CARDS.build_cards(corpus(
            post("r", bodyHtml=f"<p>{long_text}</p>"),
            post("a", thread="r", reply_to="r", when=10, bodyHtml="<p>锁了两小时。</p>"),
        ))
        self.assertEqual(texts(result), [long_text, "锁了两小时。"])
        self.assertEqual([card.thread_pos for card in result.cards], [0, 1])

    def test_short_reply_without_a_root_card_is_dropped(self) -> None:
        result = CARDS.build_cards(corpus(
            post("l", "link", title="报名聚会", url="https://a.com"),
            post("n", thread="l", reply_to="l", when=10, bodyHtml="<p>刚刚报名了...💪</p>"),
            post("m", thread="l", reply_to="n", when=20, bodyHtml="<p>这是一条足够长、可以独立阅读的回复。</p>"),
        ))
        self.assertEqual(texts(result), ["这是一条足够长、可以独立阅读的回复。"])
        self.assertEqual(len(result.excluded["short_reply_out_of_context"]), 1)

    def test_threads_are_ordered_by_root_publication(self) -> None:
        result = CARDS.build_cards(corpus(
            post("new", bodyHtml="<p>较新的一条笔记</p>", when=100),
            post("old", bodyHtml="<p>较早的一条笔记</p>", when=0),
            post("reply", thread="old", reply_to="old", when=200, bodyHtml="<p>" + "后续" * 200 + "</p>"),
        ))
        self.assertEqual([card.key for card in result.cards], ["old", "reply", "new"])

    def test_featured_root_marks_its_cards(self) -> None:
        result = CARDS.build_cards(corpus(
            post("q", "quote", quoteText="精选的一段引用", featuredAt=BASE_TIME)))
        self.assertTrue(result.cards[0].flags & CARDS.FLAG_FEATURED)


class CorpusVerificationTest(unittest.TestCase):
    def test_accepts_complete_corpus(self) -> None:
        CARDS.verify_corpus(corpus(post("r"), post("a", thread="r", reply_to="r")))

    def test_rejects_missing_reply(self) -> None:
        data = corpus(post("r"), post("a", thread="r", reply_to="r"))
        data["posts"].pop()
        with self.assertRaisesRegex(CARDS.BlogCardsError, "postCount 2"):
            CARDS.verify_corpus(data)

    def test_rejects_missing_parent(self) -> None:
        data = corpus(post("r"), post("b", thread="r", reply_to="gone"))
        with self.assertRaisesRegex(CARDS.BlogCardsError, "parent gone"):
            CARDS.verify_corpus(data)


class PackFormatTest(unittest.TestCase):
    def sample(self) -> list:
        return CARDS.build_cards(corpus(
            post("q", "quote", quoteText="引用\n\n第二段", sourceName="书名"),
            post("n", bodyHtml="<p>一条笔记的内容</p>", when=1),
        )).cards

    def test_round_trip(self) -> None:
        cards = self.sample()
        pack = CARDS.pack_cards(cards, BASE_TIME, 480)
        parsed = CARDS.unpack_pack(pack)
        self.assertEqual(parsed["tzOffsetMin"], 480)
        self.assertEqual(parsed["builtAt"], BASE_TIME)
        self.assertEqual([(c["id"], c["text"], c["source"], c["kind"]) for c in parsed["cards"]],
                         [(c.id, c.text, c.source, c.kind) for c in cards])

    def test_layout_constants(self) -> None:
        self.assertEqual(CARDS.HEADER.size, 64)
        self.assertEqual(CARDS.CARD.size, 32)
        pack = CARDS.pack_cards(self.sample(), BASE_TIME, 480)
        self.assertEqual(pack[:4], b"BLGC")
        self.assertEqual(struct.unpack_from("<I", pack, 8)[0], 2)

    def test_rejects_corruption(self) -> None:
        pack = bytearray(CARDS.pack_cards(self.sample(), BASE_TIME, 480))
        header_bad = bytes(pack[:20]) + b"\x01" + bytes(pack[21:])
        with self.assertRaisesRegex(CARDS.BlogCardsError, "header checksum"):
            CARDS.unpack_pack(header_bad)
        pack[-3] ^= 0xFF
        with self.assertRaisesRegex(CARDS.BlogCardsError, "payload checksum"):
            CARDS.unpack_pack(bytes(pack))

    def test_card_ids_are_stable_fnv1a(self) -> None:
        self.assertEqual(CARDS.fnv1a32(""), 0x811C9DC5)
        self.assertEqual(CARDS.fnv1a32("a"), 0xE40C292C)
        self.assertEqual(self.sample()[0].id, CARDS.fnv1a32("q"))


class PartitionTest(unittest.TestCase):
    def test_parses_sizes_and_skips_comments(self) -> None:
        table = CARDS.parse_partitions(
            "# Name, Type, SubType, Offset, Size\n"
            "nvs, data, nvs, 0x9000, 24K\n"
            "cards, data, 0x40, 0x610000, 1M\n")
        self.assertEqual(table["nvs"].size, 0x6000)
        self.assertEqual(table["cards"], CARDS.PartitionEntry("cards", 0x610000, 0x100000))


if __name__ == "__main__":
    unittest.main()
