/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

#include "weread_cache.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "weread_internal.h"

// 阶段目录布局与下载器共用，避免两处各写一份路径公式。
// The stage layout is shared with the downloader so the path formula exists only once.
#define WR_STAGE_FMT WR_DIR "/stage-%s-%s"

static const char *TAG = "WRCACHE";

/*
 * "又有章节落盘"的计数。目录靠它判断徽标是否过期：没有这个计数，读者停在目录上看着
 * "下载中"，章节下完了界面也不会重画，只能退出目录再进来才看到"已缓存"。
 * A counter of landed chapters. The TOC uses it to know when its badges went stale: without
 * it, a reader watching "下载中" never sees "已缓存" until the TOC is reopened.
 */
static volatile unsigned s_ready_epoch;

unsigned wr_cache_ready_epoch(void) { return s_ready_epoch; }

/* ------------------------------------------------------------------ */
/* 标记文件                                                            */
/* ------------------------------------------------------------------ */

bool wr_mark_write(const wr_book_t *book, const char *path, unsigned chapters) {
    if (!book || !path || !path[0]) return false;
    char owner[WR_ID] = {0};
    (void)wr_account(owner, NULL, NULL);
    char text[WR_MARK_SIZE];
    int n = snprintf(text, sizeof(text), "%s\nbook=%s\nowner=%s\nchapters=%u\n", WR_MARK_MAGIC,
                     book->id, owner, chapters);
    if (n <= 0 || n >= (int)sizeof(text)) return false;
    // 直接覆盖而不是先删：路径上的文件是阅读器识别这本书的依据，中间不能缺失。
    // Overwrite in place: the file at this path is how the reader identifies the book.
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    (void)setvbuf(file, NULL, _IOFBF, 512);
    bool ok = fwrite(text, 1, (size_t)n, file) == (size_t)n;
    ok = ok && !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    return ok;
}

bool wr_mark_read(const char *path, char book_id[WR_ID], unsigned *chapters) {
    if (!path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char text[WR_MARK_SIZE + 1];
    size_t n = fread(text, 1, WR_MARK_SIZE, file);
    fclose(file);
    text[n] = 0;
    if (strncmp(text, WR_MARK_MAGIC "\n", sizeof(WR_MARK_MAGIC)) != 0) return false;
    const char *id = strstr(text, "\nbook=");
    if (!id) return false;
    id += 6;
    const char *end = strchr(id, '\n');
    size_t len = end ? (size_t)(end - id) : strlen(id);
    if (!len || len >= WR_ID) return false;
    if (book_id) { memcpy(book_id, id, len); book_id[len] = 0; }
    if (chapters) {
        const char *count = strstr(text, "\nchapters=");
        unsigned value = 0;
        if (count) (void)sscanf(count + 10, "%u", &value);
        *chapters = value;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 目录与单章状态                                                      */
/* ------------------------------------------------------------------ */

static bool owner_of(char owner[WR_ID]) {
    owner[0] = 0;
    return wr_account(owner, NULL, NULL) && owner[0];
}

bool wr_stage_root(char *out, size_t cap, const char *book) {
    char owner[WR_ID];
    if (!out || !cap || !book || !owner_of(owner)) return false;
    int n = snprintf(out, cap, WR_STAGE_FMT, owner, book);
    return n > 0 && (size_t)n < cap;
}

bool wr_stage_path(const wr_book_t *book, unsigned index, char out[WR_PATH]) {
    if (!book) return false;
    char root[WR_PATH];
    if (!wr_stage_root(root, sizeof(root), book->id)) return false;
    int n = snprintf(out, WR_PATH, "%s/t/c%u.xhtml", root, index);
    return n > 0 && n < WR_PATH;
}

static bool stage_dir(const wr_book_t *book, const char *sub, char out[WR_PATH]) {
    char root[WR_PATH];
    if (!book || !wr_stage_root(root, sizeof(root), book->id)) return false;
    int n = snprintf(out, WR_PATH, "%s/%s", root, sub);
    return n > 0 && n < WR_PATH;
}

static bool dir_ensure(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool wr_cache_dir(const wr_book_t *book) {
    char root[WR_PATH], sub[WR_PATH];
    if (!wr_stage_root(root, sizeof(root), book->id) || !dir_ensure(root)) return false;
    static const char *const names[] = {"t", "i", "m"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!stage_dir(book, names[i], sub) || !dir_ensure(sub)) return false;
    }
    return true;
}

bool wr_stage_meta(const wr_book_t *book, unsigned index, char uid[WR_ID], unsigned *length,
                   unsigned *crc) {
    char root[WR_PATH], path[WR_PATH];
    if (!wr_stage_root(root, sizeof(root), book->id)) return false;
    if (snprintf(path, sizeof(path), "%s/m/c%u.meta", root, index) >= (int)sizeof(path)) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char text[128];
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    char stored[WR_ID] = {0};
    unsigned len = 0, sum = 0;
    if (sscanf(text, "uid=%47s\nlen=%u\ncrc=%x", stored, &len, &sum) != 3) return false;
    if (uid) snprintf(uid, WR_ID, "%s", stored);
    if (length) *length = len;
    if (crc) *crc = sum;
    return true;
}

unsigned wr_cache_size(const wr_book_t *book, unsigned index) {
    char path[WR_PATH];
    struct stat info;
    if (!wr_stage_path(book, index, path)) return 0;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) return 0;
    return (unsigned)info.st_size;
}

wr_cache_state_t wr_cache_state(const wr_book_t *book, unsigned index) {
    unsigned len = 0;
    if (!wr_stage_meta(book, index, NULL, &len, NULL)) return WR_CS_NOT_CACHED;
    unsigned size = wr_cache_size(book, index);
    if (!size) return WR_CS_STALE; // meta 在但正文不在：断电或 SD 卡异常后的残缺
    if (size != len) {
        ESP_LOGW(TAG, "chapter size mismatch index=%u meta=%u file=%u", index + 1, len, size);
        return WR_CS_STALE;
    }
    return WR_CS_READY;
}

static bool kind_path(const wr_book_t *book, unsigned index, char out[WR_PATH]) {
    char root[WR_PATH];
    if (!wr_stage_root(root, sizeof(root), book->id)) return false;
    int n = snprintf(out, WR_PATH, "%s/m/c%u.kind", root, index);
    return n > 0 && n < WR_PATH;
}

bool wr_cache_pinned(const wr_book_t *book, unsigned index) {
    char path[WR_PATH];
    return kind_path(book, index, path) && access(path, F_OK) == 0;
}

bool wr_cache_pin(const wr_book_t *book, unsigned index, bool pinned) {
    char path[WR_PATH];
    if (!kind_path(book, index, path)) return false;
    if (!pinned) { unlink(path); return true; }
    if (!wr_cache_dir(book)) return false;
    FILE *file = fopen(path, "wb"); // 存在即手动缓存，内容为空
    if (!file) return false;
    return fclose(file) == 0;
}

bool wr_cache_drop(const wr_book_t *book, unsigned index) {
    char root[WR_PATH], path[WR_PATH];
    if (!wr_stage_root(root, sizeof(root), book->id)) return false;
    // 先删 meta：meta 不在，残缺正文就不会被当成可用章节。
    // Drop meta first: without it a partial body can never pass as a usable chapter.
    if (snprintf(path, sizeof(path), "%s/m/c%u.meta", root, index) < (int)sizeof(path)) unlink(path);
    if (snprintf(path, sizeof(path), "%s/m/c%u.kind", root, index) < (int)sizeof(path)) unlink(path);
    if (wr_stage_path(book, index, path)) unlink(path);
    return true;
}

void wr_cache_stats(const wr_book_t *book, unsigned count, wr_cache_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->chapters = count;
    for (unsigned i = 0; i < count; ++i) {
        wr_cache_state_t state = wr_cache_state(book, i);
        if (state == WR_CS_READY) { ++out->ready; out->bytes += wr_cache_size(book, i); }
        else ++out->pending;
    }
}

/* ------------------------------------------------------------------ */
/* 虚拟书源                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[24]; ///< 章节 "c12.xhtml" 或插图名 / Chapter or image entry name
    char path[WR_PATH];
    uint32_t size;
    uint16_t chapter; ///< UINT16_MAX 表示插图 / UINT16_MAX for images
    bool placeholder; ///< 未缓存章节的占位正文 / Placeholder body for an uncached chapter
} wr_entry_t;

typedef struct {
    char book[WR_ID];
    wr_entry_t *entries;
    size_t count;
    size_t chapters;
    /* 标题按"偏移表 + 连续块"存放：几千章也只多几百 KB，且不需要书外生命周期。 */
    /* Titles live in an offset table plus one blob: a few hundred KB at most, self-contained. */
    uint32_t *title_off;
    char *title_blob;
    size_t title_bytes;
} wr_virtual_t;

/// 当前打开的虚拟源；一本书同时只会有一个。/ The one virtual source currently open.
static wr_virtual_t *s_virtual;

static const char k_placeholder_head[] =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>正在缓存本章…</p><p>";
static const char k_placeholder_tail[] = "</p></body></html>";

static const char *title_of(const wr_virtual_t *ctx, size_t index) {
    if (!ctx->title_off || index >= ctx->chapters) return "";
    return ctx->title_blob + ctx->title_off[index];
}

static size_t placeholder_size(const char *title) {
    return sizeof(k_placeholder_head) - 1 + strlen(title) + sizeof(k_placeholder_tail) - 1;
}

static esp_err_t placeholder_build(const char *title, void *dst, size_t cap) {
    size_t needed = placeholder_size(title);
    if (cap < needed) return ESP_ERR_INVALID_SIZE;
    char *out = dst;
    size_t head = sizeof(k_placeholder_head) - 1, tail = sizeof(k_placeholder_tail) - 1;
    memcpy(out, k_placeholder_head, head);
    memcpy(out + head, title, strlen(title));
    memcpy(out + head + strlen(title), k_placeholder_tail, tail);
    return ESP_OK;
}

// 一次读取整条；章节与插图都是普通文件，虚拟源不需要解压。
// One whole read: chapters and images are plain files, so a virtual entry needs no inflate.
static esp_err_t entry_read_file(const char *path, uint32_t size, void *dst, size_t cap) {
    if (cap < size) return ESP_ERR_INVALID_SIZE;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    size_t got = fread(dst, 1, size, file);
    bool extra = fgetc(file) != EOF;
    fclose(file);
    // 长度必须与枚举时一致，否则说明文件被并发改写，宁可按缺失处理。
    // The length must match discovery time; otherwise the file changed under us.
    return got == size && !extra ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static const char *entry_name_cb(void *ctx, size_t index) {
    wr_virtual_t *v = ctx;
    return index < v->count ? v->entries[index].name : NULL;
}

static size_t entry_size_cb(void *ctx, size_t index) {
    wr_virtual_t *v = ctx;
    return index < v->count ? v->entries[index].size : 0;
}

static esp_err_t entry_read_cb(void *ctx, size_t index, void *dst, size_t cap) {
    wr_virtual_t *v = ctx;
    if (index >= v->count) return ESP_ERR_NOT_FOUND;
    wr_entry_t *entry = &v->entries[index];
    if (entry->placeholder) {
        // 阅读器读到占位正文，说明读者正停在这一章：登记优先权并唤醒后台窗口。
        // 这里只写内存与投递任务，不做任何网络请求，阅读页主线程不会被阻塞。
        // Reading a placeholder means the reader is sitting on this chapter: register priority
        // and wake the window job. Only memory writes and a task notify happen here, so the
        // reader's main task never blocks on the network.
        wr_prefetch_nudge(entry->chapter);
        return placeholder_build(title_of(v, entry->chapter), dst, cap);
    }
    return entry_read_file(entry->path, entry->size, dst, cap);
}

/* 实时条目数：章节在前一次性排好，插图随章节落盘陆续追加。阅读器打开时拷走的是快照，
 * 查找插图必须问这里，否则刚落盘的插图永远 ESP_ERR_NOT_FOUND。
 * Live entry count: chapters are laid out once up front, images are appended as chapters land.
 * The reader holds a snapshot taken at open, so image lookups have to ask here — otherwise a
 * freshly landed illustration stays ESP_ERR_NOT_FOUND forever. */
static size_t entry_count_cb(void *ctx) {
    wr_virtual_t *v = ctx;
    return v ? v->count : 0;
}

static esp_err_t entry_prefix_cb(void *ctx, size_t index, void *dst, size_t cap) {    wr_virtual_t *v = ctx;
    if (index >= v->count) return ESP_ERR_NOT_FOUND;
    wr_entry_t *entry = &v->entries[index];
    if (entry->placeholder) return placeholder_build(title_of(v, entry->chapter), dst, cap);
    size_t want = entry->size < cap ? entry->size : cap;
    FILE *file = fopen(entry->path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    size_t got = fread(dst, 1, want, file);
    fclose(file);
    return got == want ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t chapter_title_cb(void *ctx, size_t index, char *buf, size_t cap) {
    wr_virtual_t *v = ctx;
    if (index >= v->chapters || !cap) return ESP_ERR_INVALID_ARG;
    const char *title = title_of(v, index);
    size_t n = strlen(title);
    if (n >= cap) n = cap - 1;
    memcpy(buf, title, n);
    buf[n] = 0;
    return ESP_OK;
}

static void release_cb(void *ctx) {
    wr_virtual_t *v = ctx;
    if (!v) return;
    if (s_virtual == v) s_virtual = NULL;
    free(v->entries);
    free(v->title_off);
    free(v->title_blob);
    free(v);
}

static bool image_name(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    const char *ext = dot + 1;
    return !strcasecmp(ext, "jpg") || !strcasecmp(ext, "png") || !strcasecmp(ext, "webp");
}

/*
 * 一次性扫出 t/ 下已经落盘的章节号。
 * FatFs 的每次 stat 都要从头扫目录，逐章判断"在不在"是 O(N²)：1398 章的书实测光这一步
 * 就用掉 12.9 秒，读者点开书后有十几秒没有任何反应。readdir 只遍历一遍目录，之后只为
 * 真的存在的那几章做 meta/长度校验，未缓存的章一次 I/O 都不发。
 * One directory pass instead of one lookup per chapter. Each FatFs stat rescans the directory,
 * so asking "is this chapter here" for every chapter is O(N²) — 12.9 s on a 1398-chapter book,
 * i.e. a dozen silent seconds after the tap. readdir walks the directory once; only chapters
 * that really exist get a meta/length check, and uncached chapters cost no I/O at all.
 */
static bool scan_staged(const wr_book_t *book, uint8_t *present, size_t chapters, size_t *found) {
    char dir[WR_PATH];
    *found = 0;
    if (!stage_dir(book, "t", dir)) return false;
    DIR *handle = opendir(dir);
    // 打不开目录就交给调用方退回逐章判断：宁可慢，也不能把已缓存的章当成没缓存。
    // If the directory cannot be opened the caller falls back to per-chapter checks: slower is
    // fine, reporting cached chapters as missing is not.
    if (!handle) return false;
    struct dirent *item;
    while ((item = readdir(handle))) {
        unsigned index = 0;
        char ext[8];
        if (sscanf(item->d_name, "c%u.%7s", &index, ext) != 2) continue;
        if (strcmp(ext, "xhtml") || index >= chapters) continue;
        if (!present[index]) { present[index] = 1; ++*found; }
    }
    closedir(handle);
    return true;
}

// 插图：已缓存的一律进命名空间，章节里引用的是扁平文件名，与组装后的 EPUB 一致。
// 重复调用只追加新名字，后台补下的插图因此在本书内立即可见。
// Images join the namespace by bare name, matching the assembled EPUB. Repeated calls append
// only new names, so illustrations fetched later become visible without reopening the book.
static void wr_virtual_scan_images(wr_virtual_t *ctx, const wr_book_t *book) {
    char dir[WR_PATH];
    if (!stage_dir(book, "i", dir)) return;
    DIR *handle = opendir(dir);
    if (!handle) return;
    struct dirent *item;
    while ((item = readdir(handle)) && ctx->count < WR_ENTRY_MAX) {
        if (!image_name(item->d_name)) continue;
        size_t name_len = strlen(item->d_name);
        wr_entry_t *entry = &ctx->entries[ctx->chapters]; // 章节之后的条目都是插图
        if (name_len >= sizeof(entry->name)) continue;
        bool known = false;
        for (size_t i = ctx->chapters; i < ctx->count; ++i) {
            if (!strcmp(ctx->entries[i].name, item->d_name)) { known = true; break; }
        }
        if (known) continue;
        char full[WR_PATH];
        struct stat info;
        if (snprintf(full, sizeof(full), "%s/%s", dir, item->d_name) >= (int)sizeof(full)) continue;
        if (stat(full, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) continue;
        entry = &ctx->entries[ctx->count];
        memcpy(entry->name, item->d_name, name_len + 1);
        snprintf(entry->path, sizeof(entry->path), "%s", full);
        entry->size = (uint32_t)info.st_size;
        entry->chapter = UINT16_MAX;
        entry->placeholder = false;
        ++ctx->count;
    }
    closedir(handle);
}

bool wr_virtual_open(const char *path, const wr_chapter_t *chapters, unsigned count,
                     book_epub_virtual_t *out) {
    if (!out || !chapters || !count) return false;
    char book_id[WR_ID] = {0};
    if (!wr_mark_read(path, book_id, NULL)) return false;
    wr_book_t book;
    memset(&book, 0, sizeof(book));
    snprintf(book.id, sizeof(book.id), "%s", book_id);
    if (!wr_cache_dir(&book)) return false;

    wr_virtual_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return false;
    snprintf(ctx->book, sizeof(ctx->book), "%s", book_id);

    // 标题一次性拷进上下文：阅读器打开时只读一遍，之后提供方不再依赖外部数组。
    // Titles are copied once: the reader reads them during open, then the provider stands alone.
    size_t bytes = 0;
    for (unsigned i = 0; i < count; ++i) bytes += strlen(chapters[i].title) + 1;
    ctx->title_blob = heap_caps_malloc(bytes ? bytes : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->title_off = heap_caps_malloc((count + 1) * sizeof(*ctx->title_off),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->entries = heap_caps_calloc(WR_ENTRY_MAX, sizeof(*ctx->entries),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->title_blob || !ctx->title_off || !ctx->entries) { release_cb(ctx); return false; }
    size_t used = 0;
    for (unsigned i = 0; i < count; ++i) {
        ctx->title_off[i] = (uint32_t)used;
        size_t n = strlen(chapters[i].title);
        memcpy(ctx->title_blob + used, chapters[i].title, n + 1);
        used += n + 1;
    }
    ctx->title_bytes = used;
    ctx->chapters = count;

    // 章节条目：已缓存读文件，未缓存占位。顺序必须与 spine 一致。
    // Chapter entries: cached ones read files, the rest fall back to placeholders, in spine order.
    const int64_t scan_started = esp_timer_get_time();
    size_t staged_count = 0;
    uint8_t *staged = heap_caps_calloc((size_t)count + 1u, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (staged && !scan_staged(&book, staged, count, &staged_count)) {
        free(staged);
        staged = NULL; // 目录读不了：退回逐章判断，不能把已缓存的章当成缺失
    }
    for (unsigned i = 0; i < count && ctx->count < WR_ENTRY_MAX; ++i) {
        wr_entry_t *entry = &ctx->entries[ctx->count];
        snprintf(entry->name, sizeof(entry->name), "c%u.xhtml", i);
        entry->chapter = (uint16_t)i;
        /* 位图里没有这一章就不必问文件系统；没有位图（内存不足）时退回逐章判断。 */
        /* No bit means no I/O; without a bitmap (no PSRAM) fall back to per-chapter checks. */
        bool present = staged ? staged[i] != 0 : true;
        if (present && wr_cache_state(&book, i) == WR_CS_READY && wr_stage_path(&book, i, entry->path)) {
            entry->size = wr_cache_size(&book, i);
            entry->placeholder = false;
        } else {
            entry->path[0] = 0;
            entry->size = (uint32_t)placeholder_size(title_of(ctx, i));
            entry->placeholder = true;
        }
        ++ctx->count;
    }
    free(staged);

    wr_virtual_scan_images(ctx, &book);

    s_virtual = ctx;

    memset(out, 0, sizeof(*out));
    out->ctx = ctx;
    out->entries = ctx->count;
    out->chapters = ctx->chapters;
    out->entry_count = entry_count_cb;
    out->entry_name = entry_name_cb;
    out->entry_size = entry_size_cb;
    out->entry_read = entry_read_cb;
    out->entry_read_prefix = entry_prefix_cb;
    out->chapter_title = chapter_title_cb;
    out->release = release_cb;
    ESP_LOGI(TAG, "virtual open book=%s chapters=%u entries=%u staged=%u titles=%u scan_ms=%lld",
             book_id, count, (unsigned)ctx->count, (unsigned)staged_count, (unsigned)used,
             (long long)((esp_timer_get_time() - scan_started) / 1000));
    return true;
}

/* ------------------------------------------------------------------ */
/* 读者等待事件                                                        */
/* ------------------------------------------------------------------ */

static volatile int s_wait_chapter = -1;
static volatile int s_ready_chapter = -1;
/* 等待章属于某一本书：换书后旧标记还在，会把新书的窗口拽到上一本书的章号上去。
 * The awaited chapter belongs to one book: a leftover mark would drag the next book's window
 * to a chapter index that only meant something in the previous one. */
static char s_wait_book[WR_ID];

void wr_read_wait(const char *book, unsigned index) {
    if (book && book[0]) snprintf(s_wait_book, sizeof(s_wait_book), "%s", book);
    else s_wait_book[0] = 0;
    s_wait_chapter = (int)index;
    s_ready_chapter = -1;
}

void wr_read_wait_clear(void) {
    s_wait_chapter = -1;
    s_ready_chapter = -1;
    s_wait_book[0] = 0;
}

int wr_read_waiting(const char *book) {
    if (!book || !book[0] || strcmp(s_wait_book, book)) return -1;
    return s_wait_chapter;
}

void wr_read_mark_ready(unsigned index) {
    if (s_wait_chapter == (int)index) s_ready_chapter = (int)index;
}

int wr_read_take_ready(void) {
    int value = s_ready_chapter;
    s_ready_chapter = -1;
    return value;
}

/* ------------------------------------------------------------------ */
/* 章节落盘后的即时刷新                                                */
/* ------------------------------------------------------------------ */

/*
 * 书打开时条目表是快照：未缓存的章记得是"占位正文"及占位长度。后台把这一章取回后，
 * 阅读器若还按快照读，就只会一遍遍拿到占位文字。这里就地更新该条目（正文路径、长度、
 * 是否占位），并补扫一次插图目录，使刚到的图片也能被本章引用到。
 * Entry sizes are snapshotted when the book opens: an uncached chapter is remembered as a
 * placeholder of a fixed length. Once the background job lands the chapter, the reader would
 * otherwise keep reading that placeholder forever. Update the entry in place and rescan images
 * so newly arrived files are reachable from this chapter.
 */
bool wr_virtual_refresh(unsigned chapter) {
    wr_virtual_t *v = s_virtual;
    if (!v || chapter >= v->chapters) return false;
    wr_entry_t *entry = &v->entries[chapter]; // 章节条目按 spine 顺序排在最前
    if (entry->chapter != (uint16_t)chapter) return false;
    if (!entry->placeholder) return true;
    wr_book_t book;
    memset(&book, 0, sizeof(book));
    snprintf(book.id, sizeof(book.id), "%s", v->book);
    if (wr_cache_state(&book, chapter) != WR_CS_READY || !wr_stage_path(&book, chapter, entry->path))
        return false;
    unsigned size = wr_cache_size(&book, chapter);
    if (!size) { entry->path[0] = 0; return false; }
    entry->size = size;
    entry->placeholder = false;
    ++s_ready_epoch;
    wr_virtual_scan_images(v, &book);
    ESP_LOGI(TAG, "virtual chapter ready book=%s index=%u bytes=%u entries=%u", v->book,
             chapter + 1, size, (unsigned)v->count);
    return true;
}

/* ------------------------------------------------------------------ */
/* 单章下载阶段进度（供首开等待页显示）                                */
/* ------------------------------------------------------------------ */

static volatile int s_warm_index = -1;
static volatile int s_warm_percent;
/* 上一档百分比：把"本阶段已下字节数"插进 [上一档, 本档] 区间，进度条才会连续走。 */
/* Previous milestone: the in-stage byte count is interpolated into [previous, current]. */
static volatile int s_warm_prev;
static volatile unsigned s_warm_bytes_done, s_warm_bytes_total;
/* 本档开始时刻：没有 Content-Length 时靠它按时间往前蠕。/ Stage start, used for timed creep. */
static volatile int64_t s_warm_stage_us;

void wr_warm_note(unsigned index, unsigned percent) {
    if (percent > 100) percent = 100;
    /*
     * 只记"读者正在等的那一章"。预取窗口会同时下下一章，如果两章的阶段都往这里写，
     * 等待卡片会在两个章节的百分比之间跳，看起来就是"进度条不动"或"突然满格"。
     * Only record the chapter the reader is waiting for. The prefetch window fetches the next
     * chapter too; letting both report here makes the card jump between two chapters' figures,
     * which reads as "the bar never moves" or "it jumped to full".
     */
    if (s_wait_chapter >= 0 && s_wait_chapter != (int)index) return;
    const unsigned old = (unsigned)s_warm_percent;
    s_warm_prev = (int)(old <= percent ? old : 0);
    s_warm_percent = (int)percent;
    s_warm_index = (int)index;
    s_warm_bytes_done = s_warm_bytes_total = 0;
    s_warm_stage_us = esp_timer_get_time();
    /* 一章只有五六条，直接留痕：卡片不动时能立刻看出是"没收到打点"还是"打点收下了但没重画"。 */
    /* Five or six lines per chapter: when the bar stalls, this says whether the milestone ever
     * arrived or arrived and was never repainted. */
    ESP_LOGI(TAG, "warm note chapter=%u pct=%u awaited=%d", index, percent, s_wait_chapter);
}

/* 当前这一次 HTTP 读取的字节进度，由下载循环每读一块就报一次。
 * Per-read byte progress, reported by the download loop on every chunk. */
void wr_warm_bytes(unsigned done, unsigned total) {
    if (s_warm_index < 0 || total < 2048) return;
    if (done > total) done = total;
    s_warm_bytes_done = done;
    s_warm_bytes_total = total;
}

bool wr_warm_progress(unsigned *index, unsigned *percent) {
    int chapter = s_warm_index;
    if (chapter < 0) return false;
    unsigned value = (unsigned)s_warm_percent;
    const unsigned prev = (unsigned)s_warm_prev;
    if (value > prev) {
        const unsigned span = value - prev;
        if (s_warm_bytes_total) {
            value = prev + (unsigned)((uint64_t)span * s_warm_bytes_done / s_warm_bytes_total);
        } else {
            /*
             * 没有 Content-Length 的请求（`/web/reader` 就是 chunked，实测 declared=0）
             * 没有字节可依据，整档就只能停在前一个数字上。按时间往前蠕：约 30 秒走完本档，
             * 封顶到本档 90%，真打点到达时再跳上去，绝不会提前到达下一档。
             * Responses without a Content-Length (`/web/reader` is chunked, measured declared=0)
             * give no byte base, so the bar would sit on the previous figure for the whole
             * stage. Creep by time instead: about thirty seconds to cross the stage, capped at
             * 90% of it, jumping only when the real milestone arrives.
             */
            const int64_t elapsed = esp_timer_get_time() - s_warm_stage_us;
            const unsigned cap = prev + span * 9u / 10u;
            value = prev + (unsigned)((int64_t)span * (elapsed > 0 ? elapsed : 0) / 30000000);
            if (value > cap) value = cap;
        }
        /*
         * 量化到 5%：墨水屏每次重画都是整屏（实测 588–705 ms），而下载时 2% 一档意味着每下
         * 7 KB 就整屏刷一次——370 KB 的章包光刷进度条就吃掉二十多秒，还把网络任务饿住。
         * 5% 一档把这个开销减到五分之一，进度条仍然走得动。
         * Quantise to 5%: every repaint is a full panel refresh (588–705 ms measured), and at
         * 2% steps a 370 KB bundle refreshes the whole screen every 7 KB — twenty-odd seconds
         * of the download spent painting a progress bar, starving the network task. Five-percent
         * steps cut that cost to a fifth while the bar still visibly moves.
         */
        value -= value % 5u;
        if (value < prev) value = prev;
        if (value > (unsigned)s_warm_percent) value = (unsigned)s_warm_percent;
    }
    if (index) *index = (unsigned)chapter;
    if (percent) *percent = value;
    return true;
}

void wr_warm_clear(void) {
    s_warm_index = -1;
    s_warm_percent = 0;
    s_warm_prev = 0;
    s_warm_stage_us = 0;
    s_warm_bytes_done = s_warm_bytes_total = 0;
}

/* ------------------------------------------------------------------ */
/* 章节缓存概况（供微信读书书架显示）                                  */
/* ------------------------------------------------------------------ */

/*
 * 书架每屏要画多本书，逐本逐章 stat 显然不行（128 章就是 256 次）。这里把
 * "已就绪章数 / 全书章数"记成一个小文件，后台任务全量重算一次，之后每落一章加一。
 * The shelf paints several books per page, so per-chapter stat() for each is out of the
 * question (128 chapters means 256 calls). Keep "ready / total" in a tiny file: recomputed in
 * full by a background job, then incremented once per landed chapter.
 */
static bool summary_path(const wr_book_t *book, char out[WR_PATH]) {
    char owner[WR_ID];
    if (!book || !book->id[0] || !owner_of(owner)) return false;
    int n = snprintf(out, WR_PATH, WR_DIR "/%s+%s.cc", owner, book->id);
    return n > 0 && n < WR_PATH;
}

bool wr_cache_summary(const wr_book_t *book, unsigned *ready, unsigned *total) {
    char path[WR_PATH];
    unsigned pair[2] = {0, 0};
    if (!summary_path(book, path)) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fread(pair, sizeof(pair), 1, file) == 1;
    fclose(file);
    if (!ok || !pair[1] || pair[0] > pair[1]) return false;
    if (ready) *ready = pair[0];
    if (total) *total = pair[1];
    return true;
}

void wr_cache_summary_write(const wr_book_t *book, unsigned ready, unsigned total) {
    char path[WR_PATH], temp[WR_PATH + 8];
    if (!summary_path(book, path) || !total) return;
    unsigned pair[2] = {ready > total ? total : ready, total};
    if (snprintf(temp, sizeof(temp), "%s.part", path) >= (int)sizeof(temp)) return;
    FILE *file = fopen(temp, "wb");
    if (!file) return;
    bool ok = fwrite(pair, sizeof(pair), 1, file) == 1 && !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    if (ok) {
        (void)unlink(path);
        ok = rename(temp, path) == 0;
    }
    if (!ok) unlink(temp);
}

void wr_cache_summary_refresh(const wr_book_t *book, unsigned total) {
    if (!book || !total) return;
    unsigned ready = 0;
    for (unsigned i = 0; i < total; ++i)
        if (wr_cache_state(book, i) == WR_CS_READY) ++ready;
    wr_cache_summary_write(book, ready, total);
}

void wr_cache_summary_bump(const wr_book_t *book) {
    ++s_ready_epoch;
    unsigned ready = 0, total = 0;
    if (!wr_cache_summary(book, &ready, &total)) return;
    if (ready < total) wr_cache_summary_write(book, ready + 1, total);
}
