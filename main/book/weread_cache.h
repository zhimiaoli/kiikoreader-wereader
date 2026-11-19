/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

/*
 * 中文：微信读书分章缓存的章节存储与虚拟书源。
 * English: Chapter store and virtual book source for WeRead chapter-level caching.
 *
 * 设计要点：磁盘是唯一真相。章节是否可用只看该章正文与 meta（uid/长度/CRC）是否一致，
 * 不再维护第二份索引——少一份索引就少一种断电后的不一致，也不会出现"索引说已缓存、
 * 文件却不存在"。缓存类型与最后访问时间同样落在文件属性上（kind 文件 + mtime）。
 * Design: the filesystem is the single source of truth. A chapter is usable only when its body
 * and meta (uid/length/CRC) agree, so there is no second index to desynchronise after a power
 * loss. Cache kind and last access live on the files too (a kind file plus mtime).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "book_epub.h"
#include "esp_err.h"
#include "weread.h"

/// 阅读预取：当前章 + 下一章。每进入新章重新发起，滚动阅读时始终领先一章。
/// Read-ahead: the current chapter plus the next one, restarted on every chapter entry, so
/// reading stays exactly one chapter ahead.
#define WR_READ_AHEAD_CHAPTERS 2u
/// 单书条目上限（章节 + 插图）/ Per-book entry cap (chapters plus images).
#define WR_ENTRY_MAX 4096u

typedef enum {
    WR_CS_NOT_CACHED = 0, ///< 未缓存 / Not cached
    WR_CS_QUEUED, ///< 已排队 / Queued
    WR_CS_DOWNLOADING, ///< 下载中 / Downloading
    WR_CS_READY, ///< 已就绪且校验通过 / Ready and verified
    WR_CS_FAILED, ///< 失败，可重试 / Failed, retryable
    WR_CS_STALE, ///< 有缓存但校验不符 / Cached but failing verification
} wr_cache_state_t;

typedef enum {
    WR_CK_AUTO = 0, ///< 自动窗口缓存，可回收 / Auto window cache, reclaimable
    WR_CK_MANUAL, ///< 手动缓存，不自动删除 / Manual cache, not reclaimed
    WR_CK_FULL, ///< 整书缓存 / Whole-book cache
} wr_cache_kind_t;

/*
 * 标记文件：书籍路径上先放一个自描述占位文件，使书架/历史/进度等按路径工作的既有逻辑
 * 无需改动即可看到这本书；阅读器打开时由虚拟书源接管。整书组装完成后该文件被真正的
 * EPUB 原子替换，路径不变，进度与书签因此连续。
 * Marker file: a self-describing placeholder at the book path, so shelf/history/progress keep
 * working unchanged; the reader hands the path to the virtual source. When the full EPUB is
 * assembled it atomically replaces the marker at the same path, keeping progress continuous.
 */
#define WR_MARK_MAGIC "WRVBOOK1"
#define WR_MARK_SIZE 96u

/// 写入/刷新标记文件；chapters 为全书章数。/ Write or refresh the marker with the book's chapter count.
bool wr_mark_write(const wr_book_t *book, const char *path, unsigned chapters);
/// 判断路径是否为微信读书标记文件；是则回填书籍 ID。/ Whether the path is a marker; fills the book id.
bool wr_mark_read(const char *path, char book_id[WR_ID], unsigned *chapters);

/// 建立/确认章节暂存目录（t 正文、i 插图、m 校验）。/ Ensure the stage directories exist.
bool wr_cache_dir(const wr_book_t *book);

/// 单章状态；READY 表示正文与 meta 一致。/ Chapter state; READY means body and meta agree.
wr_cache_state_t wr_cache_state(const wr_book_t *book, unsigned index);
/// 单章正文字节数，未缓存返回 0。/ Chapter body size, zero when absent.
unsigned wr_cache_size(const wr_book_t *book, unsigned index);
/// 手动缓存标记（不随自动缓存回收）。/ Manual pin flag, never reclaimed with the auto window.
bool wr_cache_pinned(const wr_book_t *book, unsigned index);
bool wr_cache_pin(const wr_book_t *book, unsigned index, bool pinned);
/// 删除单章缓存（正文、meta、kind）。/ Drop one cached chapter.
bool wr_cache_drop(const wr_book_t *book, unsigned index);

/*
 * 为阅读器构造虚拟书源。chapters/count 来自微信读书章节目录；未缓存章节返回占位正文，
 * 保证目录完整、分页可用。返回的 release 会在关书时释放上下文。
 * Build the virtual source for the reader. chapters/count come from the WeRead TOC; uncached
 * chapters return placeholder text so the TOC stays complete and pagination keeps working.
 * The returned release frees the context when the book closes.
 */
bool wr_virtual_open(const char *path, const wr_chapter_t *chapters, unsigned count,
                     book_epub_virtual_t *out);

/// 后台补下某一章后就地刷新虚拟源，使阅读器不必退出重进。/ Refresh the virtual source in place.
bool wr_virtual_refresh(unsigned chapter);

/* 首开等待页：正在下载哪一章、该章走到哪个阶段（0-100）。 */
/* First-open waiting page: which chapter is being fetched and its stage percentage. */
void wr_warm_note(unsigned index, unsigned percent);
/// 当前这次读取的字节进度，让同一个阶段内进度条也能连续走动。/ Byte progress within a stage.
void wr_warm_bytes(unsigned done, unsigned total);
bool wr_warm_progress(unsigned *index, unsigned *percent);
void wr_warm_clear(void);

/// 标记/清理"读者正等待这一章"，供下载器优先处理；等待标记绑定书籍，换书后不会串章。
/// Mark the chapter the reader is waiting for. The mark is bound to a book so it cannot leak
/// into the next one.
void wr_read_wait(const char *book, unsigned index);
/// 关书时清掉等待标记：再没有人在等任何一章，预取不该继续围着旧书转。
/// Drop the wait mark when the book closes: nobody is waiting on any chapter any more.
void wr_read_wait_clear(void);
/// 下载器查询这本书读者当前等待的章节，-1 表示没有或属于别的书。
/// Chapter the reader waits for in this book, or -1 when there is none / it belongs elsewhere.
int wr_read_waiting(const char *book);
/// 下载器在读者等待的章节落盘后调用，交给阅读页一次续读机会。/ Signal that the awaited chapter landed.
void wr_read_mark_ready(unsigned index);

/// 章节缓存概况：ready 已就绪章数，total 全书章数；false 表示没有记录。
/// Chapter-cache summary: ready chapters out of total; false when there is no record.
bool wr_cache_summary(const wr_book_t *book, unsigned *ready, unsigned *total);
void wr_cache_summary_write(const wr_book_t *book, unsigned ready, unsigned total);
/// 逐章重算并落盘；逐章 stat 有开销，只应在后台任务里调用。/ Recompute chapter by chapter; background only.
void wr_cache_summary_refresh(const wr_book_t *book, unsigned total);
/// 单章落盘后把已就绪数加一。/ Count one more ready chapter.
void wr_cache_summary_bump(const wr_book_t *book);
/// 已落盘章数的变化计数，供目录判断徽标是否过期。/ Counter of landed chapters, for TOC badges.
unsigned wr_cache_ready_epoch(void);

/// 书级缓存窗口统计，供目录与书架展示。/ Cache stats for the TOC and shelf.
typedef struct {
    unsigned chapters, ready, pending, failed;
    uint64_t bytes;
} wr_cache_stats_t;
void wr_cache_stats(const wr_book_t *book, unsigned count, wr_cache_stats_t *out);
