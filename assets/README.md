<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

| File | Content | Source, license, and use |
| --- | --- | --- |
| [`fonts/blog-cards/blog_font_16.c`](fonts/blog-cards/blog_font_16.c), [`blog_font_20.c`](fonts/blog-cards/blog_font_20.c), [`blog_font_24.c`](fonts/blog-cards/blog_font_24.c) | LVGL 4 bpp bitmap fonts, 16/20/24 px (body text 20/24, header and footer 16), uncompressed; about 0.9/1.3/1.9 MB of glyph bitmaps in Flash | Converted from Noto Sans SC Regular (`notofonts/noto-cjk` tag `Sans2.004`, SHA-256 checked by the tool) under the SIL Open Font License 1.1, copied to [`fonts/blog-cards/OFL.txt`](fonts/blog-cards/OFL.txt). Built into the Blog Cards firmware by `main/CMakeLists.txt`; needs `CONFIG_LV_FONT_FMT_TXT_LARGE`. |
| [`fonts/blog-cards/charset.txt`](fonts/blog-cards/charset.txt) | The 7565 glyphs the fonts cover | All of GB2312, printable ASCII, a few typographic marks, the Blog Cards UI strings, and every character in the current blog cards. |

Regenerate all four files with `python3 tools/blog_cards.py font` (needs Node.js; it runs `lv_font_conv@1.5.3`). The command downloads the pinned font into `build/`, verifies its hash, rewrites the charset, and converts each size; rebuild and flash the firmware afterwards. `python3 tools/blog_cards.py build` reports any character in new blog content that the charset does not cover.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
