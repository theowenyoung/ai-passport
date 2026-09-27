<p align="right">
  <a href="design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Blog Cards: Design

Blog Cards turns the blog's untitled notes and quotes into pocket cards: flip to a random card, and the ones you keep come back on a schedule. The device stays offline. A script on the computer updates its content over USB, and reading progress survives every update.

Status: the pack builder (`tools/blog_cards.py`) and the firmware are implemented and pass the automated gates; on-device acceptance is pending.

## Data flow

```text
blog public Thread API ── tools/blog_cards.py ──> cards.bin ── esptool writes only the cards partition ──> device
                           filter / Threads / text cleanup
```

- All data comes from the public API, so no token is needed. The script, the repository, and the device store no blog credentials.
- The device never goes online: no Wi-Fi, provisioning, TLS, or time sync.
- Routine updates write only the `cards` partition. `update` never touches `cardstat`, which holds reading progress.

## Fetch

1. Page through every Thread, including those hidden from the homepage, with `GET /api/public/threads?visibility=any&sort=published&include=fold&limit=100`.
2. For each Thread whose fold reports `hidden > 0`, read all of its posts with `GET /api/public/threads/:slug/posts`.
3. Self-check: the posts collected for each Thread must equal its `postCount`, and every reply's parent must be in the same Thread. If either check fails, the script stops instead of producing an incomplete pack.

Jant 0.9 deprecated the old `/api/public/posts` list and `/api/public/archive`; the script uses neither.

## Card selection

| Rule | Detail |
| --- | --- |
| Included | Every quote, including Thread replies, and every untitled note |
| Excluded | Posts with image or video attachments or inline images; notes with a long-text attachment |
| Optional | `--exclude-hidden` drops Threads hidden from the homepage (kept by default) |
| Too short | Dropped when fewer than 4 visible characters remain after cleanup |
| Short reply without context | A note reply shorter than 12 characters is dropped when its Thread root is not a card, for example a link, a titled note, or an image post |

Quote cards show the quoted text only, never the author's commentary.

## Threads

- Each post becomes one card. Cards from one Thread sit next to each other in the pack and carry `thread_pos`/`thread_len`, so the device can show "3/12" and continue reading through the Thread.
- A Thread made only of untitled notes, totalling at most 240 characters, becomes a single card. Each segment starts with a date line that begins with U+001F; the device renders that line as a small date.
- A quote without a source takes the nearest ancestor's quote source, link title, or note title, walking up the reply chain.

## Text cleanup

- Notes are converted from `bodyHtml`, because `bodyText` flattens paragraphs and inlines link URLs. Paragraphs, list items (`• ` / `1. `), and line breaks are kept; block quotes start with U+FF5C, a full-width vertical bar; links keep only their text.
- Quotes are converted from `quoteText`: line breaks are kept, and bold, inline-code, and Markdown link markers are removed.
- A link whose text is its own URL, or a bare URL in the text, becomes a bracketed label that names its host, for example `[… · text.npr.org]`.
- Emoji, their variation selectors, and zero-width joiners are removed, because the screen has no glyphs for them.
- Whitespace is normalized, keeping at most one blank line between paragraphs.

## Pack format v1

Little-endian. Strings are UTF-8 and NUL-terminated, so the device can hand them straight to LVGL.

| Part | Content |
| --- | --- |
| Header (64 bytes) | `BLGC`, version, header size, card count, record size, time-zone offset in minutes for dates, index/string offsets and sizes, total size, build time, payload CRC32, reserved bytes, header CRC32 over the first 60 bytes |
| Card record (32 bytes each) | id, publish time, text offset/length, source offset/length, kind (1 quote / 2 note), flags, `thread_pos`, `thread_len`, reserved |
| String area | All card texts and sources |

Flag bits: `1` featured, `2` hidden from the homepage, `4` source inherited from an ancestor, `8` merged Thread. A card id is the 32-bit FNV-1a hash of the post ID, or of `thread:<root id>` for a merged card. Editing a post keeps its id, and therefore its reading progress. The script fails on an id collision.

The firmware must validate a pack exactly as `unpack_pack()` does: both CRCs, every region bound, and the terminating NUL of every string.

## Partitions

| Partition | Offset | Size | Purpose | Written by |
| --- | --- | --- | --- | --- |
| `factory` | `0x10000` | 6.5 MB | Program and Chinese fonts | First install and firmware updates |
| `cards` (data `0x40`) | `0x690000` | 1 MB | Card pack | `blog_cards.py update` |
| `cardstat` (data `0x41`) | `0x7F0000` | 64 KB | Reading progress, two 32 KB slots | The device only; `backup`/`restore` copy it out and back |

`partitions.csv` is authoritative for offsets, and the script reads it at run time. Flashing the merged `full.bin` at `0x0` resets NVS but ends before `cards`, so neither the pack nor the progress is overwritten by a firmware install. NVS holds only the brightness setting.

The baseline hardware-test demo sources (`main/demo_*.c`, `main/ui_pixel*.c`) stay in the tree for the upstream host tests but are not built into this firmware. Bluetooth is disabled in `sdkconfig.defaults`.

## Fonts

The body text uses Noto Sans SC at 24, 20, and 16 px, converted to LVGL bitmap fonts that cover all of GB2312 plus every character of the current cards and the UI strings (see [assets/README.md](../../assets/README.md#fonts)). Because the font lives in the firmware, new blog content normally needs only `update`; when the build report lists missing characters, run `font`, then rebuild and flash the firmware.

## Device behavior

**Layout.** Try the large font first, then the medium and small fonts. If the small font still overflows, paginate by whole lines, keeping punctuation off line starts. The header holds only the date (plus the Thread position, such as `12/39`), a star for kept cards, and a battery icon at the top right; quotes and notes are told apart by colour and the quote bar rather than a label, which keeps the header on one line. The last page shows the source at the bottom, and the page number sits at the bottom right. Line pitch is about 1.4-1.5 em (34/29/24 px). Measured with the real font metrics on the current 840 cards: 44% use the large font, 74% fit one screen, 19% need two pages, and the longest needs eight.

**Buttons.**

| Button | Click | Double click | Long press |
| --- | --- | --- | --- |
| OK | Next card | Keep | Menu (hide this card, statistics, brightness) |
| DOWN | Next page; on the last page, the next card in the same Thread | | |
| UP | Previous page; on the first page, the previous card | | |

**Scheduling (a simplified Anki).** The device has no reliable date, so the number of cards viewed stands in for time:

- A global counter increases with every card shown. Each card stores the counter value at which it may return and its current interval.
- A normal flip lengthens the interval, for example 200 → 500 → 1200 cards.
- Keep uses short, slowly growing intervals, for example 30 → 60 → 120 cards, and stars the card.
- Hide stops the card from appearing until it is restored from the menu.
- Selection order: pick at random among due cards; if none is due, draw a new card, balanced across years and favoring featured and newly imported cards; otherwise take the card due soonest.

**Menu.** A long OK press (700 ms) opens: keep/unkeep this card, hide this card, restore hidden cards, brightness (high/medium/low), back. It also shows how many cards were read and kept, and when the pack was built.

**Power.** After 30 s without input the backlight dims; after 2 minutes it turns off. The first press after that only wakes the screen and draws a fresh card. Deep sleep is not used yet.

**Progress storage.** Only remembered cards take space, 12 bytes each. The `cardstat` partition holds two alternating copies with a CRC. The device writes 8 s after the last change, before the screen turns off, and immediately after hiding or restoring cards. After a content update, cards with the same id keep their progress, and entries for cards that disappeared are dropped on the next write.

## Commands

```bash
python3 tools/blog_cards.py update            # fetch, build, and write the cards partition
python3 tools/blog_cards.py update --offline  # rebuild from the cached fetch and write
python3 tools/blog_cards.py build             # build only; see build/blog_cards/report.txt
python3 tools/blog_cards.py show --random -n 5
python3 tools/blog_cards.py backup --output progress.bin
python3 tools/blog_cards.py restore --input progress.bin
python3 tools/blog_cards.py font              # regenerate the firmware fonts (Node.js)
```

First install: build and flash the firmware (`./tools/validate.sh --firmware`, then write `build/FoloToy-AI-Passport-full.bin` at `0x0`), then run `update` to write the cards. Until a pack is written the device asks for it on screen.

`update`, `backup`, and `restore` need esptool. The script uses the one in the current Python, or the copy that ESP-IDF installed under `~/.espressif`, so ESP-IDF does not have to be activated first. The build report lists characters the font does not cover. When any appear, run `charset` to regenerate the glyph list, then rebuild the font and firmware.
