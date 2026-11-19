<!-- SPDX-FileCopyrightText: 2026 YIYO-Designer -->
<!-- SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 -->

# 分章缓存改造 · 变更文件清单

分支：`feature/weread-chapter-cache`
基线：`ad21bbd`（main `6ec1b45` + 改造前本地工作）
产物：`0.3.3-rc113`，应用镜像 5,075,504 bytes，`free_app_bytes` 1,215,952（OTA 槽余量约 19%）

---

## 1. 新增文件

| 文件 | 行数 | 作用 |
| --- | ---: | --- |
| `main/book/weread_cache.h` | 96 | 章节状态枚举、缓存类型、标记文件格式、虚拟书源与读者等待事件接口 |
| `main/book/weread_cache.c` | 444 | 标记文件读写、章节目录/状态判定（meta 为提交点）、虚拟书源 provider、窗口统计、读者等待事件 |
| `docs/WEREAD_CHAPTER_CACHE_REVIEW.md` | 142 | 第一阶段源码审查报告 |
| `docs/WEREAD_CHAPTER_CACHE_DESIGN.md` | 334 | 三项技术验证、最终架构、限制、完成度矩阵、实机验证清单 |
| `docs/WEREAD_CHAPTER_CACHE_CHANGES.md` | 本文件 | 变更清单 |

## 2. 修改文件

| 文件 | 改动 | 风险 |
| --- | --- | --- |
| `main/book/book_epub.h` | 新增 `book_epub_virtual_t`、`book_epub_open_virtual()` | 低（纯新增） |
| `main/book/book_epub.c` | 新增 7 个条目访问适配函数；53 处 `zip_*` → `epub_*`；新增虚拟书源打开；`close` 分支释放 provider | **中**（ZIP 路径必须零回归） |
| `main/book/book_source.h/.c` | 新增 `book_virtual_resolver_t` 与注册接口；`book_open()` 优先派发虚拟解析器 | 低（旧书走原分支） |
| `main/book/weread.h` | 新增 `WR_PREFETCH`、`wr_open_book`、`wr_prefetch_nudge/running`、`wr_read_take_ready` | 低 |
| `main/book/weread_internal.h` | 新增 `wr_stage_root/path/meta`、`wr_chapter_list`、`wr_stage_chapter` | 低 |
| `main/book/weread.c` | 注册虚拟解析器；`WR_PREFETCH` 分支 + `prefetch_window()`；`wr_open_book()` 等实现；阅读时长上报接受标记文件 | 中（worker 单任务互斥） |
| `main/book/weread_epub.c` | `stage_open()` 改用统一路径公式（显式容量）；`wr_cache()` 不再删除标记文件；新增两个入口函数 | 低 |
| `main/apps/app_weread.c` | "打开"改为不要求整书已缓存；失败时在操作菜单内显示原因 | 低 |
| `main/apps/book_reader_controls_runtime.inc` | `rc_tick()` 增加"等待章节已就绪 → 原地重排" | **中**（须保留章内偏移） |
| `main/CMakeLists.txt` | 加入 `book/weread_cache.c` | 低 |
| `docs/CHANGELOG.md` | 新增本次改造条目 | 低 |

## 3. 未触碰的关键文件（回归安全的依据）

`main/book/book_layout.c`、`main/book/book_progress.c`、`main/book/book_lock_bookmark.c`、
`main/book/ttf_font.*`、`main/book/html_text.c`、`main/book/zip_reader.c` 均**未修改**。

也就是说：排版、分页、字体加载、书签存储、HTML 解析与 ZIP 读取的实现没有变化；本次改造只替换了"条目从哪里来"。

## 4. 编译结果

```
{"board":"metalio47","target":"esp32s31","version":"0.3.3-rc113",
 "size":5075504,"free_app_bytes":1215952,"built":true,"device_flashed":false}
```

- 与改造前基线相比：+7,808 bytes（5,067,696 → 5,075,504），OTA 槽余量 19%。
- 构建过程在 `-Werror` 下通过（期间修掉两处真实问题：`char[192]` 传入声明为 288 的接口；插图名 `snprintf` 可能截断）。
- **未刷写设备**：`device_flashed: false`，本文所有功能结论止于编译级。

## 5. 复核方式

```bash
git checkout feature/weread-chapter-cache
git diff ad21bbd --stat          # 改动面
git diff ad21bbd -- main/book/book_epub.c   # 条目访问层，重点复核
```

旧行为回退：`git checkout main`。
