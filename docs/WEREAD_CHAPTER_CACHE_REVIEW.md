<!-- SPDX-FileCopyrightText: 2026 YIYO-Designer -->
<!-- SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 -->

# 微信读书分章节缓存 · 源码审查报告

审查对象：`feature/weread-chapter-cache` 分支起点（`ad21bbd`，即 main `6ec1b45` + 工作区既有改动）。
审查方式：逐文件读实际实现，不采信 README、旧文档或历史分析的结论；文中结论均可回溯到具体函数。

---

## 1. 现有架构与调用关系

### 1.1 模块与职责

| 文件 | 实际职责 | 规模 |
| --- | --- | --- |
| `main/app/app_registry.c` | 主导航注册，微信读书页排在书架之后 | — |
| `main/apps/app_weread.c` | 微信读书书架/账户/扫码/操作菜单，只负责界面与任务投递 | 546 行 |
| `main/book/weread.c` | 单 worker 任务机（`xTaskCreateStatic`，优先级 2，静态栈）、账户与 Cookie、书架索引、阅读时长累计与上报队列 | 约 2100 行 |
| `main/book/weread_protocol.c` | HTTP/流式响应、登录、`/web/book/read` 读写进度、章节目录、正文分片 | 约 500 行 |
| `main/book/weread_epub.c` | 章节目录双层缓存、逐章取文→暂存→组装 EPUB、进度位置映射 | 约 1200 行 |
| `main/book/book_epub.c` | 原生 EPUB 解析：ZIP 中央目录 → container.xml → OPF manifest/spine → 章节与目录 | 约 2400 行 |
| `main/book/book_source.c` | TXT/EPUB 单实例书源分派，阅读页只认这一层 | 222 行 |
| `main/book/book_layout.c` | 排版与分页（行盒、图片、脚注），字号/行距/边距 | 约 1500 行 |
| `main/book/book_progress.c` | NVS 进度记录，键 = 路径 + 文件大小 | 307 行 |
| `main/apps/app_book.c` | 阅读页：装载章节、翻页、目录、书签、工具栏 | 9675 行 |

### 1.2 调用链（现状）

```
书架点击 → app_weread.activate() → wr_book_path() 必须成功（文件存在且 size>0）
        → app_book_request_open(path) → book_open(path)
        → book_epub_open(path) → zip_open() → container.xml → book.opf
        → chapters[] (spine) + navigation[] (NCX/NAV)
        → book_layout_build_blocks(chapter) → 分页/渲染

整书缓存：wr_start(WR_CACHE) → worker_task → run_work → wr_cache()
        → get_chapters()（TOC 双层缓存）→ 逐章 fetch_chapter_content() → stage_chapter()
        → 全部就绪后 assemble_book() 组装 EPUB → rename 原子替换
```

关键事实：**阅读器只接受"一个完整 ZIP 文件路径"**。`book_epub_open()` 第一件事是 `zip_open(path)`，之后所有章节、图片、CSS、字体都通过 ZIP 条目访问（`zip_find` / `zip_entry_name` / `zip_entry_size` / `zip_extract`）。

### 1.3 现有缓存目录与文件格式

```
/sdcard/.weread/
├── shelf.bin / cached.bin / toc/            # 书架索引、已缓存书索引、章节目录缓存（7 天）
└── stage-<owner>-<book>/                    # 逐章暂存（组装成功后整目录删除）
    ├── t/c<N>.xhtml                         # 该章正文（已重写图片引用的 XHTML）
    ├── i/<12位hex>.<ext>                    # 插图，按 URL 全书去重
    └── m/c<N>.meta                          # uid / len / crc32，meta 存在即代表整章可用
```

`m/c<N>.meta` 的语义很重要：先写正文并 `fsync`，再写 meta，因此 **meta 是"整章可用"的提交点**。这一点被本次改造直接继承。

---

## 2. 已经可以复用的功能

| 能力 | 复用点 | 说明 |
| --- | --- | --- |
| 登录/会话/扫码 | `login()`、`save_account()`、NVS `weread` | 完全不改 |
| 书架与封面 | `sync_shelf()`、`remember_cached()`、`book_cover` | 完全不改 |
| 章节目录获取 | `get_chapters()`，内存 10 分钟 + SD 7 天双层缓存，上限 2048 章 | 直接作为全书目录来源 |
| 单章取文 | `fetch_chapter_content()`：阅读页取 psvts → 3 个正文分片 → 还原顺序 → base64 解码，3 次重试 | 逐章缓存直接调用 |
| 单章图文渲染 | `stage_chapter()` → `render_html()`：章包（`tar`/zip）优先，未命中逐张下载，按 URL 去重，GIF 跳过 | 逐章缓存直接调用 |
| 断点续传 | `chapter_ready()`：meta 的 uid/长度/CRC 与正文一致即跳过 | 逐章缓存直接调用 |
| 阅读时长 | `wr_reading_start/tick/stop`、上报队列 | 完全不改 |
| 进度定位 | `map_position()` + `html_weread_position()`：本地字节偏移 ↔ 云端 `co` 偏移 | 继续作为唯一映射通道 |
| 原生阅读器 | 排版、分页、字体、书签、目录、实体按键 | **本次改造不改其中任何逻辑** |

---

## 3. 需要新增的模块

| 新增 | 目的 |
| --- | --- |
| `main/book/weread_cache.{h,c}` | 章节存储状态、标记文件、虚拟书源提供方、读者等待事件 |
| `book_epub_virtual_t` + `book_epub_open_virtual()` | 让 `book_epub` 能从一个"条目提供方"读书，而不是只能从 ZIP 读 |
| `book_set_virtual_resolver()` | 阅读器打开 `.epub` 路径时，先问虚拟解析器是否接管 |
| `WR_PREFETCH` 任务 + `prefetch_window()` | 后台按窗口补章，读者等待的章节优先，不阻塞阅读 |
| `wr_open_book()` / `wr_prefetch_nudge()` / `wr_read_take_ready()` | 书架直接打开、按需优先、就绪后续读 |

---

## 4. 需要修改的模块

| 文件 | 修改 | 风险 |
| --- | --- | --- |
| `book_epub.c` | 把 53 处 `zip_*` 调用统一收口到 `epub_find/entry_name/entry_size/extract/extract_prefix/entry_count/prefetch` 适配层；新增虚拟书源打开路径 | **中**：ZIP 路径必须行为完全不变；已用"同签名、同顺序、仅多一个接收者参数"的方式机械替换，并通过编译 |
| `book_epub_close()` | 虚拟书源关闭时调用 `vsrc.release()` | 低 |
| `book_source.{h,c}` | 新增解析器注册；`book_open()` 优先问解析器，未命中退回 ZIP | 低：旧书走原分支 |
| `weread_epub.c` | `stage_open()` 改用统一路径公式；`wr_cache()` 不再把标记文件当损坏缓存删除；新增 `wr_chapter_list()`、`wr_stage_chapter()` | 低 |
| `weread.c` | 注册虚拟解析器；新增 `WR_PREFETCH` 分支与 `prefetch_window()`；新增 `wr_open_book()` | 中：worker 是单任务，需保证不与登录/上传互斥死锁 |
| `app_weread.c` | "打开"不再要求整书已缓存，改为先落标记再进书 | 低 |
| `book_reader_controls_runtime.inc` | `rc_tick()` 增加"等待的章节已就绪 → 原地重排" | **中**：重排必须保留章节内偏移，否则会跳页 |
| `main/CMakeLists.txt` | 加入 `weread_cache.c` | 低 |

---

## 5. 目前存在的技术限制（实测，非推测）

1. **ZIP 不可原地修改**。已组装的 EPUB 无法"补一章进去"：中央目录在文件尾部，条目偏移固定。任何"边读边补"的方案都必须绕开 ZIP，或整本重写。整本重写在 20 MB 书上约等于每章一次全量写盘，不可接受。
2. **`wr_book_path()` 要求文件存在且 `st_size > 0`**。所有"这本书能否打开"的判断都建立在真实文件上，因此分章书必须在目标路径上有一个真实文件（本次用标记文件）。
3. **`book_progress` 以"路径 + 文件大小"为键**。标记文件与最终 EPUB 体积不同，整书组装完成的那一刻，旧进度记录会因 size 不匹配而失效。
4. **阅读页主任务是单线程**。任何在 `book_epub` 回调里发起的网络请求都会冻住翻页；虚拟书源必须纯本地应答。
5. **`zip_reader` 有硬上限**：8192 条目、单次解压 8 MiB、`HTML_TEXT_MAX_BYTES` 限制单章 XHTML。虚拟源不受 8192 限制，但单章上限仍然适用。
6. **TOC 上限 2048 章**（`WR_TOC_MAX_CHAPTERS`），而 `book_epub` 允许 8192 章。超长连载书会在 TOC 阶段被截断——这是既有行为，本次不放大。
7. **服务端偶发整本直发 ZIP**（正文首字节为 `PK\x03\x04`）。逐章路径无法承接，只能回落到整书缓存。
8. **`co` 偏移由服务端坐标系定义**，靠正文里的 `<!--wr-co:N-->` 标记携带。一旦同一章在服务端的正文发生改版（修订、插图替换），旧偏移会指向别处。

---

## 6. 可能影响原生阅读器的风险

| 风险 | 触发条件 | 缓解 |
| --- | --- | --- |
| ZIP 路径回归 | 适配层替换有误 | 同签名机械替换；`book_epub.c` 单独编译验证；旧 EPUB 回归测试列为必测项 |
| 排版/分页变化 | 只有改动 `book_layout.c` 才会发生 | 本次**未改** `book_layout.c`、`book_progress.c`、`ttf_font.*` |
| 书签失效 | 章节索引与 byte offset 语义变化 | 虚拟源保持"spine 序号 = 章节号"，`chapter->offset` 仍为前序章节字节累计 |
| 目录错乱 | 标题来源变化 | 虚拟源把提供方标题灌进书内池，走原生 spine 回退目录，`toc_visible` 规则不变 |
| 关闭书泄漏 | `vsrc.release` 未调用 | `book_epub_close()` 已分支处理（`zip` 与 `vsrc` 互斥） |
| 内存压力 | 虚拟源条目表 + 标题块 | 条目上限 4096，标题按偏移表 + 连续块存放；均已实测编译，实机内存待测 |
| 翻页卡顿 | 占位章节每次装载都触发一次任务投递 | 仅在占位正文被读取时投递，且任务在跑时直接返回 |

---

## 7. 与旧"整书缓存"的关系

- 旧路径（`WR_CACHE` → `wr_cache()`）**完全保留**，仍是"整书缓存"入口，产物仍是单个 EPUB。
- 分章路径只新增：标记文件 + 章节存储 + 虚拟源 + 窗口任务。
- 两者共用同一套 `stage-*` 目录与 `c<N>.meta` 校验规则，因此可以互相接续：先分章读，后点"缓存全书"，已下载的章节会被 `chapter_ready()` 跳过，只补缺口。
- 整书组装完成后 `rename()` 原子覆盖标记文件，路径不变。

---

## 8. 下一阶段的三个验证结论入口

- 验证 A（章节级图文缓存）：见 `WEREAD_CHAPTER_CACHE_DESIGN.md` §2。
- 验证 B（原生阅读器动态加载）：同上 §3，结论为"原生解析器不支持按需数据源，但存在单点改造位置"。
- 验证 C（双向进度同步）：同上 §4，重点说明 `co` 偏移的真实字段与精度边界。
