#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WR_ID 48
#define WR_PATH 288

typedef struct {
    char id[WR_ID], title[160], author[96], cover[384];
    uint32_t updated;
} wr_book_t;

typedef struct {
    char id[WR_ID], title[128];
    unsigned index;
    /* 章节插图资源包（tar 或 zip）地址，来自 chapterInfos 的 "tar" 字段。
     * 一次请求即可拿到整章插图，避免按 <img> 逐张联网。可为空。
     * Chapter illustration bundle (tar or zip) from chapterInfos "tar"; one request
     * replaces per-<img> downloads. May be empty. */
    char tar[300];
} wr_chapter_t;

typedef enum { WR_LOGIN, WR_SYNC, WR_CACHE, WR_UPLOAD, WR_DOWNLOAD, WR_REPORT, WR_CHECK, WR_PREFETCH } wr_job_t;
typedef enum { WR_IDLE, WR_BUSY, WR_OK, WR_FAILED, WR_EXPIRED, WR_CANCELLED } wr_result_t;
/*
 * 首开未缓存的书的云端进度解析状态：PENDING 表示后台还没问过云端，FOUND 带着要去的章节与
 * 章内偏移，ABSENT 表示云端也没有进度、按第一章处理。
 * Cloud-position resolution for a first open with nothing cached: PENDING while the background
 * job has not asked yet, FOUND carries the chapter and in-chapter offset to go to, ABSENT means
 * the cloud has no position either and chapter one stands.
 */
typedef enum { WR_SEED_IDLE = 0, WR_SEED_PENDING, WR_SEED_FOUND, WR_SEED_ABSENT } wr_seed_t;

bool wr_online(void);
void wr_init(void);
bool wr_account(char id[WR_ID], uint32_t *today, uint64_t *total);
const char *wr_error_message(void);
const char *wr_result_message(void);
bool wr_logout(bool remove_books);
int wr_shelf_count(void);
bool wr_shelf_get(int index, wr_book_t *book);
/// 把刚读过的书移到书架最前（只改本地索引顺序，不发请求）。
/// Move the book just read to the front of the shelf; local index order only, no request.
bool wr_shelf_touch(const char *book_id);
bool wr_book_path(const wr_book_t *book, char path[WR_PATH]);
/// 只算书架上的路径，不要求已登记缓存、也不要求文件存在（"删除本地缓存"用）。
/// The shelf path alone, without requiring a cache entry or an existing file (used by delete).
bool wr_book_storage(const wr_book_t *book, char path[WR_PATH]);
bool wr_cover_get(const wr_book_t *book, uint8_t *pixels);
bool wr_start(wr_job_t job, const wr_book_t *book, unsigned first, unsigned count);
/*
 * 分章阅读入口：不要求整本已缓存。写入标记文件、准备好目录与章节列表后返回，
 * 随后阅读器打开该书时由虚拟书源接管；章节内容在后台按窗口补齐。
 * start 返回本地记忆的章节号（无进度为 0），供进入阅读时定位。
 * Chapter-level reading entry: the whole book need not be cached. Writes the marker, prepares
 * the directories and the chapter list, then returns; the reader later opens the book through
 * the virtual source while chapters fill in from a background window. start reports the locally
 * remembered chapter (0 when there is no progress).
 */
bool wr_open_book(const wr_book_t *book, char path[WR_PATH], unsigned *start);
/// 登记"这本书首开、先问云端进度再决定下哪一章"；离线返回 false，阅读页按第一章。
/// Marker for "resolve the cloud position before the first download"; false when offline.
bool wr_cloud_seed_begin(const char *book_id);
/// 读一次解析结果（不清除）。IDLE 表示这本书没有待解析的云端进度。
/// Read the resolution once (no clearing). IDLE means nothing is pending for this book.
wr_seed_t wr_cloud_seed_state(unsigned *chapter, unsigned *offset);
/// 取走解析结果：之后同一本书不会再报 FOUND/ABSENT。
/// Take the result, so the same book never reports FOUND/ABSENT twice.
void wr_cloud_seed_ack(void);
/// 请求后台优先取某一章，并在空闲时拉起窗口任务。/ Prioritise one chapter and start the window job when idle.
void wr_prefetch_nudge(unsigned chapter);
/// 退出书籍时停掉这本书的缓存任务（预取 / 整书缓存），worker 立刻空出来给下一本。
/// 只影响这本书：换书时新书已起来的预取不受影响。非微读书籍是空操作。
/// Stop this book's cache jobs when the book closes, so the worker is free for the next one.
/// Scoped to that book: a prefetch already running for the newly opened book survives.
void wr_cache_pause(const char *path);
/*
 * 请求重下一章：正文里引用的插图在盘上找不到时用。只允许每章修一次（磁盘上有 .fix 标记），
 * 免得服务端本来就缺的图让每次翻页都触发一遍重下。
 * Ask for one chapter to be staged again when the body references an illustration the disk does
 * not have. Only once per chapter (a .fix marker), so a picture the server really lacks cannot
 * make every turn re-download the chapter.
 */
bool wr_cache_repair(const char *path, unsigned index);

/// 后台窗口任务是否在运行，供页面显示"缓存中"。/ Whether the window job is running, for UI hints.
bool wr_prefetch_running(void);
/// 取走一次"读者等待的章节已就绪"事件，没有则返回 -1；阅读页据此原地重排续读。
/// Take a pending "awaited chapter is ready" event, or -1; the reader re-layouts in place.
int wr_read_take_ready(void);
/// 取走"下载进度后应打开的章节"（一次性）。/ Take the chapter to open after a progress download (one shot).
bool wr_progress_take_target(unsigned *chapter);
/// 本地书架删除这本书时回收微读侧数据：章节缓存目录、目录缓存、缓存概况与旁挂封面。
/// 书籍 ID 从文件名解析，可在 unlink 之后调用；是否微读书籍由文件名判定。
/// Reclaim WeRead-side data when the local shelf deletes this book: chapter cache directory,
/// TOC cache, cache summary and cover sidecars. The id comes from the file name, so this may run
/// after the unlink; a non-WeRead file name simply returns false.
bool wr_forget_book(const char *path);
wr_result_t wr_status(char *qr, size_t qr_size, unsigned *done, unsigned *total);
void wr_cancel(void);
bool wr_reporting_book(const char *path, wr_book_t *book);
void wr_reading_start(const char *path, bool enabled, int64_t now_ms);
/// 每帧调用：active 为"正在读正文"，chapter/offset/pct 是当前阅读位置（投给未同步进度的书）。
/// Called per frame with the position, so a report can stand on the local place when the cloud
/// has no position for the book yet.
void wr_reading_tick(bool active, int64_t now_ms, unsigned chapter, unsigned offset, unsigned pct);
void wr_reading_stop(bool close);
void wr_reading_display(bool scanning);
