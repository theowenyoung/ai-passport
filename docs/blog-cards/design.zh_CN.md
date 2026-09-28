<p align="right">
  <strong>简体中文</strong> · <a href="design.md">English</a>
</p>

# 博客卡片：设计

把博客里的无标题 note 和 quote 做成随身卡片：随手翻一张，喜欢的会定期回来。设备完全离线，内容靠电脑上的脚本经 USB 更新，阅读进度在更新时保留。

状态：打包脚本（`tools/blog_cards.py`）和固件均已实现，并通过了自动化检查；真机验收尚未进行。

## 数据流

```text
博客公开 Thread API ── tools/blog_cards.py ──> cards.bin ── esptool 只写 cards 分区 ──> 设备
                        过滤 / Thread 处理 / 文字清洗
```

- 数据来自公开 API，全程不需要 token。脚本、仓库和设备里都不保存任何博客凭据。
- 设备不联网：没有 Wi-Fi、配网、TLS 和校时。
- 日常更新只写 `cards` 分区；`cardstat`（阅读进度）不被 `update` 触碰。

## 抓取

1. `GET /api/public/threads?visibility=any&sort=published&include=fold&limit=100` 分页遍历全部 Thread（包含首页隐藏的）。
2. 对 fold 中 `hidden > 0` 的 Thread，用 `GET /api/public/threads/:slug/posts` 取全。
3. 自检：每个 Thread 收集到的篇数必须等于 `postCount`；每条回复的上级必须在同一 Thread 内。任何一项不满足，脚本直接失败，不生成残缺的卡片包。

旧的 `/api/public/posts` 列表和 `/api/public/archive` 已在 Jant 0.9 标记废弃，脚本不使用。

## 选卡规则

| 规则 | 说明 |
| --- | --- |
| 纳入 | 所有 quote（包括 Thread 回复）和所有无标题 note |
| 排除 | 带图片/视频附件或内嵌图片的；带长文附件的 note |
| 可选排除 | `--exclude-hidden` 去掉首页隐藏的 Thread（默认保留） |
| 过短 | 清洗后少于 4 个可见字符的丢弃 |
| 脱离上下文的短回复 | 短于 12 字的 note 回复，若其串首不是卡片（例如串首是链接、带标题的 note 或带图的帖子），丢弃 |

quote 只显示引文，不显示作者的评论。

## Thread 处理

- 一条帖子一张卡；同一 Thread 的卡片在包里相邻存放，并记录 `thread_pos`/`thread_len`，设备据此显示“3/12”并支持顺着读。
- 全部由无标题 note 组成、总长不超过 240 字的 Thread 合并成一张卡。每段以 U+001F 开头的日期行起首，设备把它渲染成小号日期。
- quote 没有出处时，沿回复链向上找最近的祖先：quote 的出处、链接的标题或 note 的标题。

## 文字清洗

- note 从 `bodyHtml` 转换（`bodyText` 会把段落压平并把链接地址塞进正文）：保留段落、列表（`• ` / `1. `）、换行；引用块以 U+FF5C 全角竖线开头；链接只保留文字。
- quote 从 `quoteText` 转换：保留换行，去掉加粗、行内代码、Markdown 链接等标记。
- 链接文字本身就是网址或正文里的裸网址，替换成带主机名的方括号标签（例如 `[… · text.npr.org]`）。
- 删除 emoji 及其变体选择符和零宽连接符（屏幕没有这些字形）。
- 统一空白，最多保留一个空行作为段落间隔。

## 卡片包格式 v1

小端序；字符串为 UTF-8 并以 NUL 结尾，设备可以直接交给 LVGL。

| 部分 | 内容 |
| --- | --- |
| 文件头（64 字节） | `BLGC`、版本、头长、卡片数、记录长、日期用的时区偏移（分钟）、索引/字符串区偏移与长度、总长、生成时间、正文 CRC32、保留区、文件头 CRC32（前 60 字节） |
| 卡片记录（每张 32 字节） | id、发布时间、正文偏移/长度、出处偏移/长度、类型（1 quote / 2 note）、标记、`thread_pos`、`thread_len`、保留 |
| 字符串区 | 所有正文与出处 |

标记位：`1` 精选，`2` 首页隐藏，`4` 出处继承自祖先，`8` 合并的 Thread。卡片 id 是 FNV-1a 32 位哈希：单条帖子用帖子 ID，合并卡片用 `thread:<串首 ID>`。文章被编辑时 id 不变，阅读进度随之保留。id 冲突时脚本报错退出。

设备端必须像 `unpack_pack()` 一样校验：两个 CRC、各区边界，以及每个字符串的结尾 NUL。

## 分区

| 分区 | 偏移 | 大小 | 用途 | 由谁写入 |
| --- | --- | --- | --- | --- |
| `factory` | `0x10000` | 6.5 MB | 程序和中文字库 | 首次安装和固件更新 |
| `cards`（data `0x40`） | `0x690000` | 1 MB | 卡片包 | `blog_cards.py update` |
| `cardstat`（data `0x41`） | `0x7F0000` | 64 KB | 阅读进度，两个 32 KB 槽位 | 只由设备写；`backup`/`restore` 可导出和写回 |

具体偏移以 `partitions.csv` 为准，脚本运行时读取。整包 `full.bin` 从 `0x0` 刷写会重置 NVS，但它在 `cards` 之前就结束了，所以安装固件不会覆盖卡片包和阅读进度。NVS 里只存亮度设置。

基线硬件测试 demo 的源文件（`main/demo_*.c`、`main/ui_pixel*.c`）保留在仓库里供上游宿主测试使用，但不编进这个固件。`sdkconfig.defaults` 关闭了蓝牙。

## 字库

正文使用 Noto Sans SC 的 24、20 px 两档，顶栏、底栏和菜单使用 16 px；三档都转换成 LVGL 位图字库，覆盖 GB2312 全部字符、当前所有卡片里出现的字符和界面文案（见 [assets/README.zh_CN.md](../../assets/README.zh_CN.md#字库fonts)）。字库在固件里，所以新增博客内容通常只需要 `update`；打包报告列出缺字时，先运行 `font`，再重新编译并刷写固件。

## 设备行为

**排版。** 先用大号字试排，放不下换中号；中号仍放不下就用中号按整行分页，并处理标点避头尾。正文不再降到 16 px，实测太小，看着吃力。顶栏只放日期（以及 Thread 位置，例如 `12/39`）、留住卡片的星标和右上角的电池图标；quote 和 note 靠配色和引文竖线区分，不再写类型文字，这样顶栏始终放得下一行。最后一页底部显示出处，右下角显示页码。行距约为字号的 1.4～1.5 倍（34/29 px）。用真实字体宽度对当前 840 张卡片实测：44% 使用大字，59% 一屏放得下，24% 需要翻 2 页，最长的一张 11 页。

**按键。**

| 按键 | 单击 | 双击 | 长按 |
| --- | --- | --- | --- |
| 确定 | 换一张 | 留住 | 菜单（隐藏这张、统计、亮度） |
| 下 | 下一页；在最后一页时进入同一 Thread 的下一张卡 | | |
| 上 | 上一页；在第一页时回到上一张卡 | | |

**排期（简化版 Anki）。** 设备没有可靠的日期，所以用“已经看过的卡片张数”作为时间：

- 每翻一张卡，全局计数加 1；每张卡记录“到第几张时可以再出现”和当前间隔。
- 普通翻过：间隔逐渐拉长（例如 200 → 500 → 1200 张）。
- 留住：间隔短且增长慢（例如 30 → 60 → 120 张），并带星标。
- 隐藏：不再出现，可以在菜单里恢复。
- 抽卡顺序：先从已到期的卡中随机挑；没有到期的就抽新卡（按年份均衡，精选的和新导入的优先）；都没有就取最快到期的。

**菜单。** 长按确定键（700 ms）打开：留住/取消留住这张、隐藏这张、恢复隐藏的卡片、亮度（高/中/低）、返回。菜单底部显示已读和留住的张数，以及卡片包的生成日期。

**省电。** 30 秒无操作屏幕变暗；2 分钟后保存进度、关闭屏幕并进入深度睡眠，三个键任意一个都能唤醒。唤醒会重启固件并显示一张新卡。为了唤醒更快，深睡唤醒时 bootloader 直接启动上次冷启动时校验过的程序，不再重新计算 5 MB 的哈希（`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`）；复位或上电仍会完整校验。唤醒那一下按键产生的事件会被忽略，按得稍久也不会顺带翻页或打开菜单。有键按住时，或者通过 USB 连着电脑时，不进入深度睡眠，只关背光，因为深度睡眠会断开 `update` 要用的串口。如果 `update` 找不到设备，说明它已经睡了：按任意键唤醒后再运行一次。待机电流尚未实测。

**进度存储。** 只有被记住的卡占空间，每张 12 字节；`cardstat` 分区里保存两份，轮流写入并带 CRC。最后一次变化后 8 秒、关闭屏幕前，以及隐藏或恢复卡片后立即写入。内容更新后，id 相同的卡保留进度，消失的卡的进度在下一次写入时清理。

## 命令

```bash
python3 tools/blog_cards.py update            # 抓取、打包、写入 cards 分区
python3 tools/blog_cards.py update --offline  # 用缓存的抓取结果重新打包并写入
python3 tools/blog_cards.py build             # 只打包，输出 build/blog_cards/report.txt
python3 tools/blog_cards.py show --random -n 5
python3 tools/blog_cards.py backup --output progress.bin
python3 tools/blog_cards.py restore --input progress.bin
python3 tools/blog_cards.py font              # 重新生成固件字库（需要 Node.js）
```

首次安装：先编译并刷写固件（`./tools/validate.sh --firmware`，再把 `build/FoloToy-AI-Passport-full.bin` 写到 `0x0`），然后运行 `update` 写入卡片。写入卡片包之前，设备屏幕会提示运行这条命令。

`update`、`backup`、`restore` 需要 esptool：可以用当前 Python 里的 esptool，也可以用 `~/.espressif` 下 ESP-IDF 自带的那份，无需先激活 ESP-IDF。打包报告会列出字库未覆盖的字；出现缺字时，运行 `charset` 重新生成字符表，再重建字库和固件。
