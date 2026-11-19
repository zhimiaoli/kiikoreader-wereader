/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

/*
 * 中文：目录与阅读页查询"这一章是否已在本地"的窄接口。
 * English: Narrow interface for the TOC and reader to ask whether a chapter is local.
 *
 * 只有逐章缓存的书源（目前是微信读书）会注册实现；普通 EPUB/TXT 一律返回 UNKNOWN，
 * 目录因此不需区分书源，也不会给本地书画出并不存在的缓存状态。
 * Only chapter-cached sources (WeRead today) register an implementation. Plain EPUB/TXT
 * always report UNKNOWN, so the TOC needs no source-specific branches and never paints a
 * cache state that does not apply.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BOOK_CH_CACHE_UNKNOWN = 0, ///< 非逐章书源，状态无意义 / Not a chapter-cached source
    BOOK_CH_CACHE_READY,       ///< 已缓存，可直接跳转 / Cached and readable now
    BOOK_CH_CACHE_PENDING,     ///< 正在下载 / Downloading now
    BOOK_CH_CACHE_MISSING,     ///< 未缓存 / Not cached
    BOOK_CH_CACHE_FAILED,      ///< 上次失败，可重试 / Last attempt failed, retryable
} book_ch_cache_t;

typedef struct {
    book_ch_cache_t (*state)(size_t chapter);
    /// 请求下载；返回 false 表示当前无法排队。/ Queue a download; false when it cannot start.
    bool (*request)(size_t chapter);
    /*
     * 把"这一章已落盘"同步进书源自己的条目表，返回是否可用。
     * 书源在开书时把每章是正文还是占位记成快照；后台补下之后必须刷新，否则目录跳过去
     * 仍然读到占位正文（正文里那句"正在缓存本章…"）。
     * Pull "this chapter has landed" into the source's own entry table. The source snapshots
     * placeholder-vs-body at open; without a refresh a TOC jump still reads the placeholder.
     */
    bool (*refresh)(size_t chapter);
    /// 已落盘章数的变化计数：目录据此判断是否需要重画徽标。
    /// Counter of landed chapters, so the TOC knows when its badges are stale.
    unsigned (*epoch)(void);
    /*
     * 全书缓存进度：done/total 为章节数；percent 为正在下载那一章的阶段百分比。
     * Whole-book progress: done/total are chapters, percent is the in-chapter stage of the
     * chapter currently being fetched.
     */
    bool (*progress)(unsigned *done, unsigned *total, unsigned *percent);
} book_chapter_cache_ops_t;

/// 注册实现；传 NULL 注销。/ Register an implementation; NULL unregisters.
void book_set_chapter_cache_ops(const book_chapter_cache_ops_t *ops);
/// 当前打开的书是否由逐章缓存书源提供。/ Whether the open book comes from a chapter cache.
bool book_chapter_cache_active(void);
/// 查询某一 spine 章节的缓存状态。/ Query the cache state of one spine chapter.
book_ch_cache_t book_chapter_cache_state(size_t chapter);
/// 请求下载某一章；非逐章书源返回 false。/ Request one chapter; false for other sources.
bool book_chapter_cache_request(size_t chapter);
/// 把已落盘的章节同步进书源条目表；非逐章书源返回 false。/ Sync a landed chapter into the source.
bool book_chapter_cache_refresh(size_t chapter);
/// 已落盘章数的变化计数；没有实现时返回 0。/ Counter of landed chapters; 0 without ops.
unsigned book_chapter_cache_epoch(void);
/// 读取整书缓存进度；没有实现时返回 false。/ Read whole-book progress; false without ops.
bool book_chapter_cache_progress(unsigned *done, unsigned *total, unsigned *percent);
