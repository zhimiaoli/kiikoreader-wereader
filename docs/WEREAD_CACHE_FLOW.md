<!-- SPDX-FileCopyrightText: 2026 YIYO-Designer -->
<!-- SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 -->

# 微信读书缓存机制梳理 · 两条入口路径

范围：`feature/weread-chapter-cache`（截至 `90a755f`）。
证据级别：L1 源码核对 + L4 部分实机（首开缓存、进度下载进书均已实测跑通）。
配套：`WEREAD_CHAPTER_CACHE_DESIGN.md`（架构论证，其中 §4/§5/§6 的"10 章窗口 + 组装 EPUB"已过期，以本文为准）。

---

## 1. 一句话

**书只以两种形态存在于本地**：完整 EPUB（服务端整包直发那条退路），或者"96 字节标记文件 + 章节暂存目录"。
后者才是常态——阅读器打开路径时由虚拟书源接管，章节正文按需从暂存目录取，缺章由后台 worker 补。

所以"未缓存"不是"这本书不在本地"，而是**标记文件可能还没写、章节目录是空的**。两条入口路径的差别，只是
"标记文件和章节目录什么时候准备好、从第几章开始补"。

---

## 2. 数据放在哪

| 路径 | 内容 | 谁写 | 谁读 |
| --- | --- | --- | --- |
| `<books>/wr-<lib>+<bookid>.epub` | 96 B 标记文件（`WRVBOOK1` / book / owner / chapters）或真的 EPUB | `wr_mark_write` / 整包退路 | 书架、历史、进度、书签（全按路径） |
| `/sdcard/.weread/stage-<owner>-<book>/t/c<N>.xhtml` | 单章正文（已重写图片引用为扁平名） | `stage_chapter` | 虚拟书源、进度映射 |
| `.../i/<hash>.<ext>` | 插图，全书扁平命名空间，按 URL 去重 | `stage_image_put` | 虚拟书源 |
| `.../m/c<N>.meta` | `uid` / `len` / `crc` | `stage_chapter` | 状态判定 |
| `.../m/c<N>.kind` | 空文件 = 手动缓存（不被窗口回收） | 无人调用（见 §8.5） | — |
| `/sdcard/.weread/toc/<owner>-<id>.bin` | 章节目录缓存 | `get_chapters` | `wr_chapter_list_cached`（阅读器线程，绝不联网） |
| `/sdcard/.weread/<owner>+<id>.cc` | 已就绪章数 / 全书章数 | `wr_cache_summary_*` | 书架徽标 |
| `/sdcard/.weread/cached.bin` | 只有"整包 EPUB"的书才进这个索引 | `remember_cached` | `wr_book_path` |

标记文件的作用是**身份**：书架、历史、进度、书签都按"路径存在且有长度"工作，写完它这本书就是"可打开的书"，
不需要改任何既有逻辑。

## 3. 一章算不算"可用"

```
wr_cache_state()：m/c<N>.meta 存在 → 读 len
                  t/c<N>.xhtml 不存在        → STALE
                  文件长度 != len            → STALE
                  否则                        → READY
```

- **meta 是唯一提交点**：先写正文并 `fsync`，再写 meta。断电最多丢当前章，不会出现"半章可读"。
- `crc` 写进了 meta，但 `wr_cache_state` **只比长度**。CRC 校验只发生在落盘前的 `chapter_ready()`（续传去重）。
  即读路径不做内容校验——同长度损坏读不出来（低概率，见 §8.3）。
- `WR_CS_QUEUED / DOWNLOADING / FAILED` 三个枚举**无人产出**，`wr_cache_state` 只会返回 NOT_CACHED / STALE / READY。
  目录行的"下载中/需重试"因此来自等待标记与 6 秒超时判定，不来自磁盘状态（见 §8.2）。

---

## 4. 路径 A：未缓存、从未打开过的书，直接点"阅读书籍"

线程标注：`[UI]` 应用任务，`[reader]` 阅读页 tick，`[worker]` 单后台任务（优先级 2）。

1. `[UI]` `activate(0)` → `open_selected()`。
2. `[UI]` `wr_book_path()` 失败（不在 `cached.bin`）→ `wr_open_book()`：
   - `restore_session_cookie()`（这条路径绕过 worker 的会话恢复，必须自己补）；
   - `get_chapters()`：内存缓存 → 磁盘 TOC → **联网 `POST /web/book/chapterInfos`**。首开必然走到第三步，
     这是全流程**唯一在 UI 线程发生的网络请求**；
   - `wr_cache_dir()` 建 `t/i/m`；
   - `wr_mark_write()` 落 96 B 标记；`store_book_title()` 写书名；
   - `*start = 本地进度章`（新书无进度 → 0）；
   - **一章都没落盘**（`.cc` 概况缺失或 `ready=0`）且在线时 `wr_cloud_seed_begin()`：只登记"先问云端进度"，
     真正联网留给 worker（UI 线程不再加一次请求）。已缓存过章节的书不登记，本地进度就是真实位置。
3. `[UI]` `wr_prefetch_nudge(0)`：登记"读者等待第 1 章"，worker 空闲则 `wr_start(WR_PREFETCH, first=0)`。
   有云端解析在等时**不登记等待章**——真实目标马上由解析结果给出，先登记只会让 worker 抢着下第一章。
4. `[UI]` `open_book()` → 书源层识别标记 → `wr_virtual_open()`：逐章 `wr_cache_state()`，
   READY 给文件路径，其余给**占位正文**（"正在缓存本章…"），目录与分页保持完整。
5. `[reader]` `load_chapter(0)` 拿到的就是占位正文 → `entry_read_cb` 再 nudge 一次（幂等）→
   `s_warm_chapter = 0`，画**等待卡片**（固件弹窗版式：568 宽 / 20 圆角 / 0x50 描边 / 系统字号），
   不把占位文字当正文显示。等待期间阅读页吞掉所有点击与翻页（卡片下面不是正文的版式）。
6. `[worker]` `prefetch_window(first)`：
   - **先解析云端进度**（`resolve_cloud_seed` → `wr_progress`，一次 `getProgress`）：
     找到 → `wr_read_wait(云端章)`、窗口锚在该章；云端也没有 → 回到 `first`（第一章）；
   - 先补封面、校一次 `.cc` 概况；
   - 读者等待章优先 → `wr_stage_chapter(N)`；`wr_warm_note` 5%→45%→70%→85%→100%；
   - 落盘 `t/cN.xhtml` → `m/cN.meta` → `wr_cache_summary_bump()` → `wr_read_mark_ready(N)`。
7. `[reader]` `rc_tick` 每 300 ms 只读本地状态：
   - 解析结果 `WR_SEED_FOUND` → 把等待章与偏移换成云端那一章（`s_warm_offset = 云端章内偏移`），重新计时；
   - `WR_SEED_ABSENT` → 按第一章继续；`PENDING` 超过 10 s（弱网）→ 不再等，按第一章继续；
   - 等待章 READY → `wr_virtual_refresh(N)`（就地改条目长度/路径、补扫插图）→
     `load_chapter(N, s_warm_offset)` 原地重排 → 正文。**不需要退出重进**。
8. `[worker]` 同一轮继续扫窗口 `[0, 0+2)`：第 1 章已 READY，第 2 章未就绪 → 顺手落盘第 2 章。**这就是"预缓存下一章"**。
9. 读者翻到第 2 章时，`load_chapter()` 末尾无条件 `book_chapter_cache_request(chapter)` → 窗口以第 2 章为锚滚动到
   `[2, 4)`，永远领先一章。

---

## 5. 路径 B：同步云端进度

1. `[UI]` 操作菜单 index=3 → `start(WR_DOWNLOAD)`。
2. `[worker]` `WR_DOWNLOAD`：路径上没有书 → 先取目录、写标记（**在 worker 线程，不卡 UI**）。
3. `[worker]` `wr_progress()`：
   - `GET` 云端进度 → 章节 uid + 章内文本偏移 `co`；
   - **云端没有这本书的进度**（请求成功、只是没有字段，`absent=1`）→ 不报错，直接落在第一章
     （`index=0, offset=0`）。网络/会话失败才报错——两者靠 `absent` 区分，不靠错误文案；
   - 目标章未缓存 → 本地没有可映射的 HTML，**不报错**，把 `co` 原样当目标偏移（`uncached_target`）；
   - 保存本地进度（`chapter=index`、`byte_off=co`、`pct`），键是"路径 + 文件长度"；
   - `s_progress_target = index`。
4. `[UI]` `tick` 收到 `WR_OK` → `wr_progress_take_target()`（一次性）→ `open_selected()` 直接进书。
5. 之后与路径 A 的第 2–8 步完全相同，差别只有三处：
   - `first = index`（进场章就是云端那章），等待卡片显示"正在缓存第 index+1 章"；
   - `s_warm_offset = co`，章节落盘后 `load_chapter(index, co)` 直接停在手机上读到的那一句；
   - 窗口 `[index, index+2)` → **预取的是进度章的下一章**，不是第 2 章。
6. 若该书是整包 EPUB（`s_selected_full`），`tick` 不自动进书——整本已在本地，回书架打开即可。

---

## 6. 预取窗口

- 大小：`WR_READ_AHEAD_CHAPTERS = 2` → 窗口 `[进场章, 进场章+2)`，即**当前章 + 下一章**，领先 1 章。
- 优先级：P0 读者等待章（占位正文被读取时登记）→ P1 窗口内第一个未就绪章。
- **锚点是进场章，不是"刚下完的章"**：否则一轮就把整本书拉完，worker 会长期被占，读者点同步/进度只能被告知"请等待"。
  窗口只在两处重新锚定：`open_selected()` 进书时、`load_chapter()` 每次换章时。
- 失败只结束本轮，已落盘章节全部保留；下次进书/换章从缺口继续。
- 并发固定 1：`wr_reader()` 依赖 keep-alive 连接与全局 Cookie 会话，多任务会争用会话与 PSRAM。
- 用户任务可抢占预取：`wr_start()` 发现当前是 `WR_PREFETCH` 时把它取消、把用户任务排队，下一轮立刻执行。

---

## 7. 与目标描述的对照

| 描述 | 实际实现 | 结论 |
| --- | --- | --- |
| 未缓存未打开的书直接打开 → 进入第一章缓存后打开 | 先打开（占位 + 等待卡片），同时后台缓存该章，落盘后原地换正文 | **行为等价，顺序相反**。不是"缓存完才进书"；换来的是点击即进书，代价是等待卡片停留 1–3 s |
| 预缓存下一章节 | 窗口 `[first, first+2)`，首轮就顺带落下一章 | 成立 |
| 同步云端进度 → 进入对应进度章节缓存后打开 | 进度下载成功即自动进书，定位到该章该偏移；未缓存则同 A 流程 | 成立（已实测：`cloud progress landed, opening chapter=5`） |
| 同样预缓存下一章节 | 窗口锚在该进度章 | 成立 |

---

## 8. 已知空窗（按影响排序）

### 8.1 预取请求会被"worker 忙"静默丢弃 —— 已修（`wr_prefetch_nudge` / `park_prefetch`）

`wr_prefetch_nudge()` 排不进队列时不再直接返回，而是把请求登记到 `s_prefetch_pending`；
worker 每轮收尾检查一次，取回后就地 `wr_start(WR_PREFETCH, ...)` 补跑。登记位按书保存，
关书时会连同这本书的一起丢掉。

补充规则：同一本书的预取在跑时 nudge 直接返回（窗口自己会重锚，重排只会掐断正在下的一章）；
换了书则走 `wr_start` 的抢占路径——旧书的预取收手，新书的预取接班。

### 8.2 磁盘状态没有 FAILED

见 §3：`WR_CS_FAILED` 无人产出。目录行的"需重试"和等待页的失败提示都靠 `ctx->now_ms - s_warm_start_ms > 6000`
且 `wr_status() != WR_BUSY` 猜出来。断网、服务端拒绝、状态机残留无法区分，重试入口也只能靠用户再点一次。
**未修**。

### 8.3 读路径不做 CRC 校验

`chapter_ready()` 在落盘前校验 CRC，`wr_cache_state()` 只比长度。同长度损坏（SD 位翻转）会被当 READY 读出来。
概率低，但"磁盘即索引"这个设计承诺的是自校验，这里少了一道。**未修**。

### 8.4 首开的目录请求在 UI 线程 —— 已缓解（`sync_toc` / `WR_TOC_WARM_BOOKS`）

书架同步成功后，在封面之后顺手把前 6 本书的目录落到 `toc/`。已落盘的目录由 `get_chapters` 直接命中磁盘，
所以重复同步几乎不花时间；只有磁盘上没有目录的书才真的联网。效果：最近在读的这几本，第一次打开从"联网"变成"读盘"。

超过 6 本、或从未同步过的书，首开仍会在 UI 线程发一次 `chapterInfos`。彻底解决要把取目录挪到 worker
（UI 先给等待页），本轮没做。

### 8.5 手动缓存入口未接

`wr_cache_pin / wr_cache_pinned / wr_cache_drop` 三件套没有调用者，`m/c<N>.kind` 永远不存在，
"手动缓存不随窗口回收"这条规则目前是死条款。

### 8.6 退出书籍不停缓存任务 —— 已修（`wr_cache_pause`）

旧行为：关书后预取继续下上一本的章节，worker 一直被占着。打开下一本书时，新书的 `nudge` 进不了队列，
等待页空转到 6 秒；点"缓存全书/同步/进度"则被告知"请等待当前操作完成"。

现在 `free_book()` 调用 `wr_cache_pause(s_path)`：
- 预取/整书缓存正在下这本书 → 置取消位，本章边界收手，worker 立刻空出来；
- 这本书排队中的、登记待补的预取一并清掉；
- 内存里的开书交接（`s_open`）只在这本书自己身上清——换书时 `s_open` 已经是新书，不能误清；
- 判断依据是这本书的 book id，**换书时新书已经起来的预取不受影响**；非微读书籍是空操作。

### 8.7 旧文档与代码不一致

`WEREAD_CHAPTER_CACHE_DESIGN.md` §4 架构图、§5 窗口（写的 10 章 / `WR_WINDOW_CHAPTERS`）、§6 兼容表（写的组装
EPUB 原子替换）都停留在改造中期；现在窗口是 2 章、整书缓存是逐章落盘、不组装。本文是当前口径。

---

## 9. 已定的取舍

1. **首开保持"点击即进书 + 等待卡片"**，不改成"首章落盘后再进书"：后者要多等一次网络取章，而且是没有任何反馈的空白。
2. **§8.1 的空窗已修**（登记 + worker 补跑）。
3. **同步后预落前 6 本目录已做**（§8.4），把最近在读的书首开从"联网"变成"读盘"。
4. **关书暂停缓存任务已做**（§8.6），worker 不再被上一本书的任务占着。
