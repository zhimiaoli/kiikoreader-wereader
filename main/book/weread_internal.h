#pragma once

#include "weread.h"
#include "cJSON.h"

#define WR_HOST "https://weread.qq.com"
#define WR_DIR "/sdcard/.weread"
/*
 * 流式读取的单次块大小。读块与阅读页扫描窗口共用这个常量：窗口按 2 KB 开、读块给了 4 KB，
 * 就会在 reader_fields_chunk 里写越界（实测 Store access fault）。改一处必须改另一处。
 * One streaming read unit. The reader-page scan window is derived from this constant: a 4 KB
 * read against a window sized for 2 KB overran it and faulted inside reader_fields_chunk.
 */
#define WR_FIELD_CHUNK 4096u
#define WR_AGENT "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36 Edg/135.0.0.0"

typedef struct { char ps[128], pc[128], token[128], referer[384]; } wr_reader_t;

bool wr_value(const cJSON *value, char *out, size_t size);
bool wr_safe(const char *value);
bool wr_cancelled(void);
void wr_error_set(const char *message);
char *wr_http(const char *url, const char *body, const char *referer, size_t limit, size_t *length);
/// 给下一次 wr_http 设时间预算。到点收手；partial 为真时把已读到的字节交出来——章包是
/// 顺序 tar，截断的部分仍能解析出前面的条目，比整包重来八分钟划算。
/// Time budget for the next wr_http. When it expires the request stops; with partial set the
/// bytes read so far are returned — a chapter bundle is a sequential tar, so a truncated one
/// still yields the entries before the cut, which beats restarting an eight-minute download.
void wr_http_budget(unsigned ms, bool partial);
/// 把微读封面写成本地书架可读的旁挂灰阶快照。/ Write a local-shelf readable grayscale cover sidecar.
bool wr_ensure_cover(const wr_book_t *book, const char *book_path);
/// 只读目录磁盘缓存，供 UI 线程开书使用。/ Disk-cached chapter list for opening on the UI thread.
wr_chapter_t *wr_chapter_list_cached(const wr_book_t *book, unsigned *count);
typedef bool (*wr_http_chunk_fn)(const char *data, size_t length, void *context);
bool wr_http_reader_fields(const char *url, size_t limit, wr_http_chunk_fn chunk, void *context);
char *wr_http_report(const char *body, const char *referer, bool *post_started);
cJSON *wr_json(const char *url, const char *body);
void wr_work_progress(unsigned done, unsigned total);
bool wr_reader(const char *book, const char *chapter, wr_reader_t *reader);
/// 只拼本章的阅读页地址（Referer 与 /web/reader/<b>k<c>）——复用签名取正文时需要重算。
/// Build this chapter's reader URL/Referer only; needed when reusing a signature for content.
bool wr_reader_referer_for(const char *book, const char *chapter, char *out, size_t cap);
bool wr_reader_fields_cached(const char *book, const char *chapter, wr_reader_t *reader);
/// 把刚取到的签名留给时长上报（同一章复用，避免上报再读一次整页）。
/// Hand a freshly fetched signature to the reporter, so reporting the same chapter is free.
void wr_reader_fields_store(const char *book, const char *chapter, const wr_reader_t *fields);
char *wr_chapter_content(const char *book, const char *chapter, const wr_reader_t *reader, bool *plain, size_t *length);
bool wr_remote_position(const char *id, wr_chapter_t *chapter, unsigned *offset, unsigned *pct);
/// absent 区分"请求成功但云端没有这本书的进度"与网络/会话失败：只有前者可以退回第一章。
/// absent separates "the request went through but this book has no cloud position" from a
/// network or session failure: only the former may fall back to chapter one.
bool wr_remote_position_with_chapters(const char *id, wr_chapter_t *chapter, unsigned *offset, unsigned *pct,
                                      const wr_chapter_t *chapters, unsigned chapter_count, bool *absent);
bool wr_send_position(const char *id, const wr_chapter_t *chapter, unsigned offset, unsigned pct);
bool wr_send_duration(const char *id, const wr_chapter_t *chapter, unsigned offset, unsigned pct,
                      unsigned seconds, const wr_reader_t *reader, bool *post_started);
bool wr_cache(const wr_book_t *book, const char *path);
/// 整书缓存：逐章把全书落盘（可断点续传），不组装单个 EPUB。/ Whole-book cache, chapter by chapter.
bool wr_cache_full(const wr_book_t *book, const char *path);
/// 上传/下载进度；失败时把可直接展示的原因写入 why（可为 NULL）。
/// 下载成功时 *target_chapter 为目标章节索引，供界面直接打开（可为 NULL）。
/// Upload/download progress; on failure fills why with a user-facing reason (may be NULL).
/// On a successful download *target_chapter is the target chapter index (may be NULL).
bool wr_progress(const wr_book_t *book, const char *path, bool upload, const char **why,
                 unsigned *target_chapter);

/* ---- 分章缓存（weread_cache.c / weread_epub.c 共用） ---- */

/// 章节暂存根目录：/sdcard/.weread/stage-<账号>-<书籍>。/ Stage root for one book.
bool wr_stage_root(char *out, size_t cap, const char *book);
/// 单章正文路径。/ Path of one chapter body.
bool wr_stage_path(const wr_book_t *book, unsigned index, char out[WR_PATH]);
/// 读取单章 meta（uid / 字节数 / CRC）；缺项返回 false。/ Read one chapter's meta, false when absent.
bool wr_stage_meta(const wr_book_t *book, unsigned index, char uid[WR_ID], unsigned *length,
                   unsigned *crc);
/// 章节列表：优先内存/磁盘目录缓存，必要时联网。/ Chapter list from the TOC caches, else network.
wr_chapter_t *wr_chapter_list(const wr_book_t *book, unsigned *count);
/// 取一章正文并落盘（含插图与校验）；已就绪直接返回 true。/ Fetch and stage one chapter.
bool wr_stage_chapter(const wr_book_t *book, const wr_chapter_t *chapter, unsigned index);
/// 这一章的正文已可读、插图还在等第二轮。/ The body is readable while its images await a second pass.
bool wr_stage_images_pending(const wr_book_t *book, unsigned index);
/// 第二轮：把正文登记过的插图下下来（失败即跳过）。/ Second pass: fetch the deferred images.
bool wr_stage_chapter_images(const wr_book_t *book, unsigned index);
/// 正文引用的插图在盘上缺失：请下一轮重新落这一章（每章一次）。/ Ask for one re-stage of a chapter whose images are missing (once per chapter).
bool wr_stage_chapter_repair(const wr_book_t *book, unsigned index);
/// 最近一次 HTTP 响应码（0=没拿到响应）。/ Last HTTP status code (0 when no response arrived).
int wr_last_http_status(void);
