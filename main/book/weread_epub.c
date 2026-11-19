/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

#include "weread_internal.h"
#include "weread_cache.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "book_cover.h"
#include "book_epub.h"
#include "book_font_size.h"
#include "book_progress.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "html_text.h"
#include "read_pico_sd_access.h"
#include "zip_reader.h"
#include "zlib.h"

/* 缓存分三步：取章 → 落到阶段目录 → 统一组装。任何一章失败都不再丢弃已完成的章，
 * 下次缓存从缺口处继续；每章插图优先从章包（tar/zip）一次取回。
 * Caching is split into fetch, stage and assemble: a failed chapter no longer discards
 * completed ones, the next run resumes at the gap, and chapter images come from the
 * bundled archive instead of one request per <img>. */
#define WR_BUNDLE_ENTRY_MAX 512
#define WR_BUNDLE_MAX_BYTES (24u * 1024u * 1024u)
/*
 * 一个章包的下载预算。把发射功率从 12 dBm 提到 18 dBm 之后，实测 370 KB 的章包从 63 秒
 * 降到 14 秒（26 KB/s），3 MB 的漫画章包约需两分钟——150 秒留出余量，又不至于让整章缓存
 * 在"正在缓存"上挂太久。
 * Download budget for one bundle. After raising TX power from 12 to 18 dBm the measured 370 KB
 * bundle dropped from 63 s to 14 s (26 KB/s) and a 3 MB comic bundle needs about two minutes;
 * 150 s leaves margin without pinning the chapter job on "caching" for too long.
 */
#define WR_BUNDLE_BUDGET_MS 150000u
#define WR_IMAGE_MAX_BYTES (4u * 1024u * 1024u)
/// 单章延迟插图清单上限：一行一个 "<文件名>\t<url>"，URL 最长 720 字节。
/// Cap on one chapter's deferred-image list: one "name\turl" line each, URLs up to 720 bytes.
#define WR_IMAGE_LIST_MAX (32u * 1024u)
#define WR_TAR_BLOCK 512u
#define WR_NAME_MAX 48
#define WR_META_MAX 128
#define WR_TOC_MAGIC 0x314f4357u /* "WCO1" */
#define WR_TOC_VERSION 2u
#define WR_TOC_TTL_SECONDS (7u * 24u * 3600u)
#define WR_TOC_MAX_CHAPTERS 2048u

typedef struct { FILE *file, *directory; uint16_t count; } epub_writer_t;

/* ------------------------------------------------------------------ */
/* 基础文件工具                                                        */
/* ------------------------------------------------------------------ */

static bool dir_ensure(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    return errno == EEXIST;
}

/*
 * 递归删目录。不能边 readdir 边删：FatFs 上删掉当前项会让读指针跳过下一项，
 * 结果只删掉一半、rmdir 因目录非空再失败——实测出现过"概况文件没了、章节却还在"，
 * 书架显示未缓存、点开却全都能读。这里一次只把一批名字读出来，关闭目录再删。
 * Recursive removal. Never unlink while iterating: on FatFs deleting the current entry makes
 * the read pointer skip the next one, so only half the tree went away and rmdir then failed on
 * the still-populated directory. That produced "summary gone, chapters still there": the shelf
 * said uncached while the book opened with every chapter readable. Read a batch of names, close
 * the directory, then delete them.
 */
#define DIR_REMOVE_BATCH 24u
// FAT 单个名字上限 255 字节，留出结束符，避免 snprintf 截断告警（-Werror）。
// FAT names reach 255 bytes; size the slot for that plus the terminator so the compiler does
// not flag a possible truncation.
#define DIR_REMOVE_NAME 256u
static bool dir_remove_tree(const char *path) {
    char *names = heap_caps_malloc(DIR_REMOVE_BATCH * DIR_REMOVE_NAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!names) return false;
    bool ok = true;
    for (unsigned pass = 0; pass < 64; ++pass) {
        DIR *dir = opendir(path);
        if (!dir) { ok = errno == ENOENT; break; }
        size_t count = 0;
        struct dirent *entry;
        while (count < DIR_REMOVE_BATCH && (entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            snprintf(names + count * DIR_REMOVE_NAME, DIR_REMOVE_NAME, "%s", entry->d_name);
            ++count;
        }
        closedir(dir);
        if (!count) { ok = rmdir(path) == 0 || errno == ENOENT; break; }
        for (size_t i = 0; i < count; ++i) {
            char child[WR_PATH];
            if (snprintf(child, sizeof(child), "%s/%s", path, names + i * DIR_REMOVE_NAME) >=
                (int)sizeof(child)) { ok = false; continue; }
            struct stat info;
            if (stat(child, &info) == 0 && S_ISDIR(info.st_mode)) {
                if (!dir_remove_tree(child)) ok = false;
            } else if (unlink(child) != 0 && errno != ENOENT) ok = false;
        }
    }
    free(names);
    return ok;
}

static bool file_write_atomic(const char *path, const void *data, size_t size) {
    char temp[WR_PATH + 8];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return false;
    FILE *file = fopen(temp, "wb");
    if (!file) return false;
    (void)setvbuf(file, NULL, _IOFBF, 4096);
    bool ok = !size || fwrite(data, 1, size, file) == size;
    ok = ok && !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    if (!ok) { unlink(temp); return false; }
    if (rename(temp, path) != 0) { unlink(temp); return false; }
    return true;
}

static char *file_read_psram(const char *path, size_t max, size_t *size) {
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long end = ftell(file);
    if (end <= 0 || (size_t)end > max || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *data = heap_caps_malloc((size_t)end + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) { fclose(file); return NULL; }
    bool ok = fread(data, 1, (size_t)end, file) == (size_t)end;
    fclose(file);
    if (!ok) { free(data); return NULL; }
    data[end] = 0;
    *size = (size_t)end;
    return data;
}

static const char *image_extension(const char *data, size_t length, const char **mime) {
    if (length >= 3 && !memcmp(data, "\xff\xd8\xff", 3)) { *mime = "image/jpeg"; return "jpg"; }
    if (length >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) { *mime = "image/png"; return "png"; }
    if (book_webp_is((const uint8_t *)data, length)) { *mime = "image/webp"; return "webp"; }
    if (book_gif_is((const uint8_t *)data, length)) { *mime = "image/gif"; return "gif"; }
    return NULL;
}

static const char *mime_for_extension(const char *ext) {
    if (!strcmp(ext, "jpg")) return "image/jpeg";
    if (!strcmp(ext, "png")) return "image/png";
    if (!strcmp(ext, "webp")) return "image/webp";
    if (!strcmp(ext, "gif")) return "image/gif";
    return NULL;
}

/* 取 URL / 归档条目名的末段文件名，去掉查询串与锚点。 */
static void name_basename(const char *text, char out[96]) {
    const char *start = text;
    for (const char *p = text; *p; ++p) if (*p == '/') start = p + 1;
    size_t length = strcspn(start, "?#");
    if (length >= 96) length = 95;
    memcpy(out, start, length);
    out[length] = 0;
}

static uint64_t fnv1a64(const char *text) {
    uint64_t value = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        value ^= *p;
        value *= 0x100000001b3ULL;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* ZIP / EPUB 写入器                                                   */
/* ------------------------------------------------------------------ */
static void le16(uint8_t *dest, unsigned value) {
    dest[0] = value;
    dest[1] = value >> 8;
}

static void le32(uint8_t *dest, uint32_t value) {
    le16(dest, value);
    le16(dest + 2, value >> 16);
}

static bool zip_entry(epub_writer_t *zip, const char *name, const void *data, size_t size) {
    if (zip->count >= ZIP_ENTRY_MAX || size > ZIP_OUTPUT_MAX) return false;
    uint8_t local[30] = {0}, central[46] = {0};
    long offset = ftell(zip->file);
    if (offset < 0) return false;
    unsigned name_size = strlen(name);
    uint32_t crc = crc32(0, data, size);
    le32(local, 0x04034b50);
    le16(local + 4, 20);
    le32(local + 14, crc);
    le32(local + 18, size);
    le32(local + 22, size);
    le16(local + 26, name_size);
    le32(central, 0x02014b50);
    le16(central + 4, 20);
    memcpy(central + 6, local + 4, 24);
    le32(central + 42, offset);
    ++zip->count;
    return fwrite(local, 1, sizeof(local), zip->file) == sizeof(local) &&
        fwrite(name, 1, name_size, zip->file) == name_size && fwrite(data, 1, size, zip->file) == size &&
        fwrite(central, 1, sizeof(central), zip->directory) == sizeof(central) &&
        fwrite(name, 1, name_size, zip->directory) == name_size;
}

static bool zip_finish(epub_writer_t *zip) {
    long start = ftell(zip->file), size = ftell(zip->directory);
    if (start < 0 || size < 0 || fseek(zip->directory, 0, SEEK_SET)) return false;
    uint8_t buffer[1024];
    for (long remaining = size; remaining > 0;) {
        size_t n = remaining > (long)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (fread(buffer, 1, n, zip->directory) != n || fwrite(buffer, 1, n, zip->file) != n) return false;
        remaining -= (long)n;
    }
    uint8_t end[22] = {0};
    le32(end, 0x06054b50);
    le16(end + 8, zip->count);
    le16(end + 10, zip->count);
    le32(end + 12, size);
    le32(end + 16, start);
    return fwrite(end, 1, sizeof(end), zip->file) == sizeof(end);
}

static bool write_xml(FILE *file, const char *text, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char *escaped = NULL;
        switch (text[i]) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '"': escaped = "&quot;"; break;
        case '\'': escaped = "&apos;"; break;
        default: break;
        }
        if (escaped ? fputs(escaped, file) == EOF : fputc((unsigned char)text[i], file) == EOF) return false;
    }
    return true;
}

static bool path_entry(epub_writer_t *zip, const char *name, const char *path, size_t max) {
    size_t size = 0;
    char *data = file_read_psram(path, max, &size);
    if (!data) return false;
    bool ok = zip_entry(zip, name, data, size);
    free(data);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 文本缓冲：章节 XHTML 直接写在 PSRAM，不再落盘回读                      */
/* ------------------------------------------------------------------ */

typedef struct { char *data; size_t used, capacity; } textbuf_t;

static bool tb_reserve(textbuf_t *buffer, size_t extra) {
    if (buffer->capacity - buffer->used >= extra) return true;
    size_t needed = buffer->used + extra;
    if (needed > HTML_TEXT_MAX_BYTES - 1) return false;
    size_t next = buffer->capacity ? buffer->capacity : 8192;
    while (next < needed) {
        size_t grown = next * 2;
        if (grown <= next || grown > HTML_TEXT_MAX_BYTES - 1) { next = HTML_TEXT_MAX_BYTES - 1; break; }
        next = grown;
    }
    char *data = heap_caps_realloc(buffer->data, next + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return false;
    buffer->data = data;
    buffer->capacity = next;
    return true;
}

static bool tb_put(textbuf_t *buffer, const char *data, size_t size) {
    if (!size) return true;
    if (!tb_reserve(buffer, size)) return false;
    memcpy(buffer->data + buffer->used, data, size);
    buffer->used += size;
    buffer->data[buffer->used] = 0;
    return true;
}

static bool tb_puts(textbuf_t *buffer, const char *text) { return tb_put(buffer, text, strlen(text)); }

static bool tb_printf(textbuf_t *buffer, const char *format, ...) {
    char small[64];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(small, sizeof(small), format, args);
    va_end(args);
    if (n < 0) return false;
    if ((size_t)n < sizeof(small)) return tb_put(buffer, small, (size_t)n);
    char *large = heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!large) return false;
    va_start(args, format);
    vsnprintf(large, (size_t)n + 1, format, args);
    va_end(args);
    bool ok = tb_put(buffer, large, (size_t)n);
    free(large);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 阶段目录                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char root[WR_PATH - 96];
    char text[WR_PATH - 48];
    char images[WR_PATH - 48];
    char meta[WR_PATH - 48];
    /// 本书 id 与当前章的阅读页地址：相对插图地址要按它补成绝对地址。
    /// Book id plus this chapter's reader URL, the base for relative image sources.
    char book[WR_ID];
    char referer[384];
    /// 本章序号与待补插图清单句柄：清单在第一次缺图时懒打开，正文落盘前关掉。
    /// Chapter index plus the deferred-image list, opened on the first missing picture and
    /// closed once the body has landed.
    unsigned index;
    FILE *pending;
} stage_t;

static bool stage_open(stage_t *stage, const char *owner, const char *book) {
    (void)owner; /* 路径公式统一由 weread_cache.c 提供，避免两处漂移 */
    memset(stage, 0, sizeof(*stage));
    snprintf(stage->book, sizeof(stage->book), "%s", book);
    if (!wr_stage_root(stage->root, sizeof(stage->root), book)) return false;
    snprintf(stage->text, sizeof(stage->text), "%s/t", stage->root);
    snprintf(stage->images, sizeof(stage->images), "%s/i", stage->root);
    snprintf(stage->meta, sizeof(stage->meta), "%s/m", stage->root);
    return dir_ensure(stage->root) && dir_ensure(stage->text) &&
        dir_ensure(stage->images) && dir_ensure(stage->meta);
}

static bool stage_image_put(stage_t *stage, const char *key, const char *data, size_t size,
                            char name[WR_NAME_MAX]) {
    name[0] = 0;
    const char *mime = NULL;
    const char *ext = image_extension(data, size, &mime);
    (void)mime;
    /* 阅读器只渲染 png/jpeg/webp；gif 条目写进去只会留下坏图引用。 */
    if (!ext || !strcmp(ext, "gif")) return false;
    snprintf(name, WR_NAME_MAX, "%012llx.%s",
             (unsigned long long)(fnv1a64(key) & 0xffffffffffffULL), ext);
    char path[WR_PATH];
    if (snprintf(path, sizeof(path), "%s/%s", stage->images, name) >= (int)sizeof(path)) { name[0] = 0; return false; }
    if (access(path, F_OK) == 0) return true;
    if (!file_write_atomic(path, data, size)) { name[0] = 0; return false; }
    return true;
}

/*
 * 按 URL 定出插图名（哈希 + 扩展名）。延迟下载需要"先给名字、后给内容"：正文里的 <img>
 * 与第二轮下载写到盘上的文件名必须一致，否则正文引用的图永远读不到。
 * 扩展名只用于命名，阅读器按内容魔数判类型，所以 URL 的扩展名与真实内容不一致也无妨。
 * Name an illustration from its URL (hash + extension). Deferred downloading needs the name
 * before the bytes: the name referenced by the text and the file the second pass writes must
 * match, or the reference never resolves. The extension only names the file — the reader sniffs
 * magic bytes — so a URL that lies about its type is harmless.
 */
static void image_name_from_url(const char *url, char *out, size_t cap) {
    const char *ext = "jpg";
    const char *dot = strrchr(url, '.');
    if (dot && dot[1] && dot[1] != '/' && dot[1] != '?') {
        char raw[8];
        size_t n = 0;
        for (const char *p = dot + 1; *p && *p != '?' && *p != '&' && n + 1 < sizeof(raw); ++p)
            raw[n++] = (char)tolower((unsigned char)*p);
        raw[n] = 0;
        if (!strcmp(raw, "png") || !strcmp(raw, "webp")) ext = raw;
        else if (!strcmp(raw, "jpeg")) ext = "jpg";
    }
    snprintf(out, cap, "%012llx.%s", (unsigned long long)(fnv1a64(url) & 0xffffffffffffULL), ext);
}

/* 单章的延迟插图清单：m/cN.img，每行 "<文件名>\t<url>"。存在即表示正文已可读、插图待补。 */
static bool stage_list_path(const stage_t *stage, unsigned index, char out[WR_PATH]) {
    int n = snprintf(out, WR_PATH, "%s/c%u.img", stage->meta, index);
    return n > 0 && n < WR_PATH;
}

/* 清单打不开不算失败：这一章仍然可读，只是欠的图这轮补不上。 */
/* A list that cannot be opened is not a failure: the chapter is still readable, its pictures
 * just cannot be finished this round. */
static void stage_pending_open(stage_t *stage) {
    if (stage->pending) return;
    char path[WR_PATH];
    if (!stage_list_path(stage, stage->index, path)) return;
    stage->pending = fopen(path, "ab");
}

/*
 * 记一条待补插图。断网时章包与逐张下载会一起失败，此时正文仍要落盘（读者能读），
 * 但插图不能就这么丢掉：先按 URL 定下与正文引用一致的名字写进清单，第二轮补内容。
 * Record an illustration that could not be fetched. When the link drops, both the bundle and
 * the per-image requests fail together; the body must still land (the reader can read), but the
 * pictures must not be lost: name them exactly as the body references them and leave the bytes
 * to the second pass.
 */
static void stage_pending_add(stage_t *stage, const char *url, char name[WR_NAME_MAX]) {
    image_name_from_url(url, name, WR_NAME_MAX);
    stage_pending_open(stage);
    if (!stage->pending) { name[0] = 0; return; }
    fprintf(stage->pending, "%s\t%s\t0\n", name, url);
}

static void stage_pending_close(stage_t *stage) {
    if (!stage->pending) return;
    fclose(stage->pending);
    stage->pending = NULL;
}

/* ------------------------------------------------------------------ */
/* 章节插图资源包（chapterInfos 的 tar / zip 字段）                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char key[96];
    char file[WR_NAME_MAX];
} bundle_item_t;

typedef struct {
    bundle_item_t *items;
    unsigned count, capacity;
} bundle_t;

static void bundle_free(bundle_t *bundle) {
    free(bundle->items);
    memset(bundle, 0, sizeof(*bundle));
}

static bool bundle_add(bundle_t *bundle, const char *key, const char *file) {
    if (bundle->count >= WR_BUNDLE_ENTRY_MAX) return false;
    if (bundle->count == bundle->capacity) {
        unsigned next = bundle->capacity ? bundle->capacity * 2 : 16;
        bundle_item_t *items = heap_caps_realloc(bundle->items, next * sizeof(*items),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!items) return false;
        bundle->items = items;
        bundle->capacity = next;
    }
    snprintf(bundle->items[bundle->count].key, sizeof(bundle->items[0].key), "%s", key);
    snprintf(bundle->items[bundle->count].file, sizeof(bundle->items[0].file), "%s", file);
    ++bundle->count;
    return true;
}

static const char *bundle_find(const bundle_t *bundle, const char *basename) {
    for (unsigned i = 0; i < bundle->count; ++i) {
        if (!strcmp(bundle->items[i].key, basename)) return bundle->items[i].file;
    }
    return NULL;
}

static unsigned long tar_octal(const char *field, size_t size) {
    unsigned long value = 0;
    for (size_t i = 0; i < size; ++i) {
        char c = field[i];
        if (c == 0 || c == ' ') { if (value) break; continue; }
        if (c < '0' || c > '7') break;
        value = value * 8 + (unsigned long)(c - '0');
    }
    return value;
}

static bool bundle_tar(stage_t *stage, bundle_t *bundle, const char *data, size_t size, const char *uid) {
    char long_name[96] = {0};
    for (size_t offset = 0; offset + WR_TAR_BLOCK <= size;) {
        const char *header = data + offset;
        if (!header[0]) break;
        unsigned long entry_size = tar_octal(header + 124, 12);
        char type = header[156];
        size_t available = size - offset - WR_TAR_BLOCK;
        if (type == 'L' || type == 'K') {
            /* GNU 长文件名：本块内容是下一块的名字。 */
            if (entry_size && entry_size < sizeof(long_name) && entry_size <= available) {
                size_t n = entry_size < sizeof(long_name) - 1 ? (size_t)entry_size : sizeof(long_name) - 1;
                memcpy(long_name, data + offset + WR_TAR_BLOCK, n);
                long_name[n] = 0;
            }
        } else if ((type == '0' || type == 0) && entry_size && entry_size <= available) {
            char name[96] = {0};
            if (long_name[0]) {
                snprintf(name, sizeof(name), "%s", long_name);
                long_name[0] = 0;
            } else {
                size_t n = strnlen(header, 95);
                memcpy(name, header, n);
                name[n] = 0;
            }
            char base[96], file[WR_NAME_MAX], key[160];
            name_basename(name, base);
            snprintf(key, sizeof(key), "%s/%s", uid, base);
            if (base[0] && stage_image_put(stage, key, data + offset + WR_TAR_BLOCK,
                                           (size_t)entry_size, file)) {
                (void)bundle_add(bundle, base, file);
            }
        }
        size_t blocks = ((size_t)entry_size + WR_TAR_BLOCK - 1) / WR_TAR_BLOCK;
        if (blocks > (size - offset) / WR_TAR_BLOCK) break;
        offset += WR_TAR_BLOCK + blocks * WR_TAR_BLOCK;
    }
    return true;
}

static bool bundle_zip(stage_t *stage, bundle_t *bundle, const char *data, size_t size, const char *uid) {
    char path[WR_PATH];
    if (snprintf(path, sizeof(path), "%s/bundle.zip", stage->root) >= (int)sizeof(path)) return false;
    if (!file_write_atomic(path, data, size)) return false;
    zip_reader_t *zip = NULL;
    if (zip_open(path, &zip) != ESP_OK) { unlink(path); return false; }
    const size_t entries = zip_entry_count(zip);
    for (size_t i = 0; i < entries && bundle->count < WR_BUNDLE_ENTRY_MAX; ++i) {
        const char *name = zip_entry_name(zip, (int)i);
        size_t entry = zip_entry_size(zip, (int)i);
        if (!name || !entry || entry > WR_IMAGE_MAX_BYTES) continue;
        char *buffer = heap_caps_malloc(entry, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) continue;
        if (zip_extract(zip, (int)i, buffer, entry) == ESP_OK) {
            char base[96], file[WR_NAME_MAX], key[160];
            name_basename(name, base);
            snprintf(key, sizeof(key), "%s/%s", uid, base);
            if (base[0] && stage_image_put(stage, key, buffer, entry, file)) {
                (void)bundle_add(bundle, base, file);
            }
        }
        free(buffer);
    }
    zip_close(zip);
    unlink(path);
    return true;
}

/* 失败是允许的：没有章包或下载失败时，<img> 会退回逐张下载。 */
static bool bundle_load(stage_t *stage, bundle_t *bundle, const char *url, const char *uid) {
    memset(bundle, 0, sizeof(*bundle));
    if (!url || !*url) return true;
    char absolute[512];
    if (!strncmp(url, "//", 2)) snprintf(absolute, sizeof(absolute), "https:%s", url);
    else if (*url == '/') snprintf(absolute, sizeof(absolute), WR_HOST "%s", url);
    else snprintf(absolute, sizeof(absolute), "%s", url);
    size_t size = 0;
    int64_t started = esp_timer_get_time();
    /* 章包有预算：链路实测 3–10 KB/s，一本漫画的章包能到 3 MB，硬等就是八分钟。到点拿走
     * 已到的部分——顺序 tar 的截断处之前照样能解析出图，剩下的等下一轮补。 */
    /* Bundles run on a budget: the link measures 3–10 KB/s and a comic bundle can reach 3 MB,
     * so waiting it out means eight minutes. When the budget expires we take what arrived — a
     * truncated sequential tar still yields its leading entries — and let a later pass finish. */
    wr_http_budget(WR_BUNDLE_BUDGET_MS, true);
    char *data = wr_http(absolute, NULL, NULL, WR_BUNDLE_MAX_BYTES, &size);
    if (!data) {
        ESP_LOGW("WEREAD", "bundle fetch failed chapter=%s error=%s", uid, wr_error_message());
        return false;
    }
    bool ok = size >= 4 && !memcmp(data, "PK\x03\x04", 4)
        ? bundle_zip(stage, bundle, data, size, uid)
        : bundle_tar(stage, bundle, data, size, uid);
    ESP_LOGI("WEREAD", "bundle chapter=%s bytes=%u images=%u elapsed_ms=%lld result=%d",
             uid, (unsigned)size, bundle->count,
             (long long)((esp_timer_get_time() - started) / 1000), ok);
    free(data);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 章节渲染                                                            */
/* ------------------------------------------------------------------ */

static bool marker(textbuf_t *out, unsigned offset) {
    return tb_printf(out, "<!--wr-co:%u-->", offset);
}

static unsigned utf16_at(const char *text, size_t bytes) {
    unsigned count = 0;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned c = (uint8_t)text[i];
        if ((c & 0xc0) != 0x80) count += c >= 0xf0 ? 2 : 1;
    }
    return count;
}

static const char *tag_end(const char *start) {
    char quote = 0;
    for (const char *p = start; *p; ++p) {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '\'' || *p == '"') quote = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

/* 取一个属性的值。属性名大小写不敏感；解析不出 key=value 就前进一个字符，避免原地打转。
 * Read one attribute value (case-insensitive). A segment that is not key=value advances by one
 * character so a malformed tag cannot spin the loop. */
static const char *image_attr(const char *p, const char *end, const char *name, size_t *length) {
    const size_t name_size = strlen(name);
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) ++p;
        const char *key = p;
        while (p < end && !strchr("= \r\n\t", *p)) ++p;
        size_t key_size = p - key;
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p == end) return NULL;
        if (*p != '=') { ++p; continue; }
        do { ++p; } while (p < end && isspace((unsigned char)*p));
        if (p == end) return NULL;
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        const char *value = p;
        while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p))) ++p;
        if (key_size == name_size && !strncasecmp(key, name, name_size)) {
            *length = p - value;
            return value;
        }
        if (quote && p < end) ++p;
    }
    return NULL;
}

/*
 * 微读正文里的图是懒加载写法：src 先放占位图（/web/Images/note.png），真图地址在 data-src
 * 一类属性里。只看 src 的后果实测很确定——一章二十个图位全部解析成同一个 404 占位图标，
 * 真图一次请求都不会发出，读者看到的就是满屏"此插图暂无法显示"。
 * WeRead ships its illustrations with lazy-loading attributes: src carries the placeholder
 * (/web/Images/note.png) while the real URL sits in data-src. Reading only src turned all twenty
 * picture slots of a chapter into the same 404 placeholder, never requested a single real image,
 * and left the reader with "this illustration is unavailable" everywhere.
 */
static const char *image_src(const char *p, const char *end, size_t *length) {
    static const char *const names[] = {
        "data-src", "data-original", "data-lazy-src", "data-wr-src", "src", "xlink:href",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const char *value = image_attr(p, end, names[i], length);
        /* 内联 base64 不是可取回的地址，跳过继续找下一个候选属性。 */
        /* An inline base64 payload is not a fetchable address; try the next candidate. */
        if (value && *length && !(*length >= 5 && !strncasecmp(value, "data:", 5))) return value;
    }
    return NULL;
}

static bool gif_source(const char *src, size_t size) {
    // 阅读器只渲染 png/jpeg/webp；gif 条目会被跳过，写入反而留下坏图引用。
    // The reader renders only png/jpeg/webp; a gif entry would become a broken reference.
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (!strncasecmp(src + i, ".gif", 4) && (i + 4 == size || src[i + 4] == '?' || src[i + 4] == '#'
                                                 || src[i + 4] == '&')) return true;
    }
    return false;
}

static bool render_plain(textbuf_t *out, const char *data) {
    const char *line = data;
    unsigned offset = 0;
    while (*line && !wr_cancelled()) {
        size_t length = strcspn(line, "\r\n");
        if (!tb_puts(out, "<p>") || !marker(out, offset)) return false;
        for (size_t i = 0; i < length; ++i) {
            unsigned c = (uint8_t)line[i];
            bool escape = c < 128 && strchr("&<>\"'", c);
            if (escape && !marker(out, offset)) return false;
            char text[2] = {line[i], 0};
            if (escape) {
                const char *entity = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;"
                    : c == '"' ? "&quot;" : "&apos;";
                if (!tb_puts(out, entity)) return false;
            } else if (!tb_put(out, text, 1)) return false;
            if ((c & 0xc0) != 0x80) offset += c >= 0xf0 ? 2 : 1;
            if (escape && !marker(out, offset)) return false;
        }
        line += length;
        while (*line == '\r' || *line == '\n') { ++line; ++offset; }
        if (!tb_puts(out, "</p>")) return false;
    }
    return true;
}

/* 把 <img src> 里的相对地址补成绝对地址。
 * 章节正文里的插图常写成 ../Images/xxx.jpg，原样拿去请求会被 scheme 校验拒掉（日志里那串
 * `image URL rejected scheme=none host=..`），插图就悄无声息地没了；读者看到的是"插图显示失败"。
 * Resolve a relative <img src> against the reader page. Chapter HTML often spells images as
 * ../Images/xxx.jpg; requesting that verbatim fails the scheme check (the `host=..` warnings) and
 * the illustration disappears without a trace. */
static bool absolute_url(const char *base, const char *src, char *out, size_t cap) {
    if (!src || !src[0] || cap < 64) return false;
    if (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8))
        return snprintf(out, cap, "%s", src) < (int)cap;
    if (!strncmp(src, "//", 2)) return snprintf(out, cap, "https:%s", src) < (int)cap;
    if (!base || !base[0]) return false;
    const char *scheme = strstr(base, "://");
    const size_t origin = scheme ? (size_t)(scheme - base) + 3 : 0;
    const char *slash = strrchr(base, '/');
    if (!slash || (size_t)(slash - base) <= origin) return false;
    char path[512];
    const size_t dir_len = (size_t)(slash - base) - origin;
    if (dir_len >= sizeof(path)) return false;
    memcpy(path, base + origin, dir_len);
    path[dir_len] = 0;
    char *w = path + strlen(path);
    const char *p = src;
    while (*p) {
        if (p[0] == '.' && p[1] == '/') { p += 2; continue; }
        if (p[0] == '.' && p[1] == '.' && p[2] == '/') {
            p += 3;
            // 只在 origin 之后退出目录段：别把主机名一起弹掉。
            // Pop one directory segment, never past the origin.
            while (w > path && w[-1] != '/') --w;
            if (w > path && memchr(path, '/', (size_t)(w - path))) --w;
            *w = 0;
            continue;
        }
        if (*p == '/') { ++p; continue; }
        if ((size_t)(w - path) + 2 >= sizeof(path)) return false;
        *w++ = '/';
        while (*p && *p != '/') {
            if ((size_t)(w - path) + 2 >= sizeof(path)) return false;
            *w++ = *p++;
        }
        *w = 0;
    }
    return snprintf(out, cap, "%.*s%s", (int)origin, base, path) < (int)cap;
}

/*
 * 数出本章会被替换的图片槽位。判定必须与 render_html 完全一致，否则按序对位会错开一张，
 * 读者看到的就是"这张图是别的图"。
 * Count the image slots this chapter will rewrite. The test must match render_html exactly,
 * otherwise positional pairing slips by one and the reader sees someone else's picture.
 */
static unsigned count_image_slots(const char *data) {
    const char *p = strcasestr(data, "<body");
    if (p) { p = tag_end(p); if (p) ++p; }
    if (!p) p = data;
    unsigned slots = 0;
    while (*p) {
        if (!strncasecmp(p, "</body", 6)) break;
        bool image = !strncasecmp(p, "<img", 4) && (isspace((unsigned char)p[4]) || p[4] == '>');
        bool svg = !strncasecmp(p, "<image", 6) && isspace((unsigned char)p[6]);
        if (image || svg) {
            const char *end = tag_end(p);
            if (!end) break;
            size_t length;
            const char *src = image_src(p + (image ? 4 : 6), end, &length);
            if (src && !gif_source(src, length)) ++slots;
            p = end + 1;
        } else if (*p == '<') {
            const char *end = tag_end(p);
            if (!end) break;
            p = end + 1;
        } else {
            p += strcspn(p, "<");
        }
    }
    return slots;
}

/*
 * 一章最多再补几张网图。微读的图一张 200–450 KB，而一次失败的读取要耗掉两个 15 秒超时；
 * 逐张硬下会把"这一章能读"推到七八分钟之后（实测第四章 392 秒仍未完成）。超出预算的先跳过，
 * 正文与章包自带的图不受影响。
 * Cap the network fallback per chapter: one WeRead illustration is 200–450 KB, and a failed
 * read costs two 15 s timeouts, so fetching them all serially pushed "chapter is readable" past
 * seven minutes (chapter 4 was still going at 392 s). Beyond the budget they are skipped; the
 * text and the bundle's own images are unaffected.
 */
#define WR_IMAGE_NET_MAX 3

/* 把 <img>/<image> 换成 EPUB 内部图片名：优先命中章包，未命中再逐张下载。 */
static bool render_html(textbuf_t *out, const char *data, stage_t *stage,
                        bundle_t *bundle, unsigned *image_count) {
    const char *p = strcasestr(data, "<body");
    if (p) { p = tag_end(p); if (p) ++p; }
    if (!p) p = data;
    /*
     * 章包里的插图是按章节顺序排的，名字是 image6-1.jpg 这种服务端命名；正文里却是
     * CB_812443_5ZLANt7Hl6sr3cb3H32c55Pkf3524.jpg 这样的哈希名，按名字永远对不上。
     * 结果就是下完 3 MB 章包后，又把同一批图逐张重下一遍。数量一致时按顺序对位。
     * Bundle entries are ordered like the chapter but carry server names (image6-1.jpg) while
     * the HTML carries hashes, so a name match never succeeds and the same pictures get
     * downloaded twice. When the counts agree, pair them by position.
     */
    const unsigned slots = count_image_slots(data);
    const bool positional = slots && bundle->count == slots;
    if (slots) ESP_LOGI("WEREAD", "chapter images slots=%u bundle_entries=%u pairing=%s",
                        slots, bundle->count, positional ? "positional" : "name");
    unsigned slot = 0;
    unsigned net_used = 0;
    unsigned offset = utf16_at(data, p - data);
    char url[2056];
    char base[96];
    unsigned unresolved_logged = 0;
    unsigned tag_logged = 0;
    /* 本轮已确认服务端不存在的图名（一章内去重，最多记满为止）。 */
    /* Basenames confirmed absent this run (deduplicated within the chapter, bounded). */
    char absent_names[8][96];
    unsigned absent_n = 0;
    while (*p && !wr_cancelled()) {
        if (!strncasecmp(p, "</body", 6)) break;
        bool image = !strncasecmp(p, "<img", 4) && (isspace((unsigned char)p[4]) || p[4] == '>');
        bool svg = !strncasecmp(p, "<image", 6) && isspace((unsigned char)p[6]);
        if (image || svg) {
            const char *end = tag_end(p);
            if (!end) return false;
            size_t length;
            const char *src = image_src(p + (image ? 4 : 6), end, &length);
            if (src && !gif_source(src, length)) {
                /* 前两个标签原样留档：图片"有槽位却一张都取不到"时，一眼能看出真图地址到底
                 * 写在哪个属性上，不必再靠猜。 */
                /* Keep the first two tags verbatim: when slots exist but no picture can be
                 * fetched, this shows which attribute actually carries the real URL. */
                if (tag_logged < 2) {
                    ESP_LOGI("WEREAD", "img tag=%.150s", p);
                    ++tag_logged;
                }
                size_t used = 0;
                if (length >= 2 && !memcmp(src, "//", 2)) { memcpy(url, "https:", 6); used = 6; }
                for (size_t i = 0; i < length && used + 1 < sizeof(url); ++i) {
                    if (i + 5 <= length && !memcmp(src + i, "&amp;", 5)) { url[used++] = '&'; i += 4; }
                    else url[used++] = src[i];
                }
                url[used] = 0;
                if (url[0] && !strstr(url, "://")) {
                    /* 相对地址先按本章阅读页补全；补不出来就跳过，不要发一个必被拒的请求。 */
                    /* Resolve relative sources against this chapter's reader page; a source that
                     * cannot be resolved is skipped rather than requested and rejected. */
                    char resolved[720];
                    if (absolute_url(stage->referer, url, resolved, sizeof(resolved)))
                        snprintf(url, sizeof(url), "%s", resolved);
                    else {
                        /*
                         * 补不出绝对地址就只能跳过；把前两条记下来——"整章图都缺"时，
                         * 一眼就能分出是地址解析失败还是下载失败。
                         * An unresolvable source is skipped; the first two are logged so a chapter
                         * losing all its pictures can be told apart from a download failure.
                         */
                        if (unresolved_logged < 2) {
                            ESP_LOGW("WEREAD", "image src unresolved base=%s src=%.64s",
                                     stage->referer, url);
                            ++unresolved_logged;
                        }
                        url[0] = 0;
                    }
                }
                name_basename(url, base);
                const char *staged = bundle_find(bundle, base);
                /* 名字对不上就按顺序取：这一章的第 slot 张图 = 章包的第 slot 个条目。 */
                if (!staged && positional && slot < bundle->count)
                    staged = bundle->items[slot].file;
                char name[WR_NAME_MAX] = {0};
                bool absent = false; // 服务端明确说这张图不存在：整个标签丢掉
                if (staged) {
                    snprintf(name, sizeof(name), "%s", staged);
                } else if (base[0]) {
                    /*
                     * 同名图本轮已经确认"服务端没有"：后面的同名标签直接丢掉，不再逐张重试。
                     * 微读正文里的界面图标（note.png 这类）在一章里会重复十几次，实测 index=8
                     * 一张 404 的图标被请求了 20 次、耗掉 11.5 秒，读者看到二十个"此插图暂无法
                     * 显示"。
                     * A basename already known to be absent is skipped without a second request.
                     * WeRead repeats its UI icons (note.png and friends) a dozen-plus times in one
                     * chapter; measured on index=8 the same 404 icon was requested 20 times over
                     * 11.5 s and left twenty "illustration unavailable" placeholders.
                     */
                    for (unsigned i = 0; i < absent_n; ++i)
                        if (!strcmp(absent_names[i], base)) { absent = true; break; }
                    bool fetched = false;
                    if (!absent && net_used < WR_IMAGE_NET_MAX) {
                        ++net_used;
                        size_t size = 0;
                        char *binary = wr_http(url, NULL, NULL, WR_IMAGE_MAX_BYTES, &size);
                        if (binary) {
                            /* 全书按 URL 去重：同一张外链图只落一次盘。 */
                            (void)stage_image_put(stage, url, binary, size, name);
                            free(binary);
                            fetched = name[0] != 0;
                        } else {
                            /*
                             * 404/410 是"这张图不存在"，不是"这次没取到"：微读正文里的界面图标
                             * 全走这条。丢掉标签、记下名字，后续同名不再请求；而超时、断网这类
                             * 仍然留在待补清单里，等下一轮补——白鹿原第四章就是这么修好的。
                             * 404/410 means the picture is not there, not that this attempt failed:
                             * that is what the reader's own UI icons return. Drop the tag and
                             * remember the name so repeats cost nothing, while timeouts and outages
                             * still go to the pending list for the next pass — the fix that got
                             * Bailuyuan chapter four its illustrations back.
                             */
                            const int status = wr_last_http_status();
                            if (status == 404 || status == 410) {
                                absent = true;
                                if (absent_n < sizeof(absent_names) / sizeof(absent_names[0]))
                                    snprintf(absent_names[absent_n++], sizeof(absent_names[0]), "%s", base);
                            }
                        }
                    }
                    if (!absent && !fetched) stage_pending_add(stage, url, name);
                }
                if (!absent && name[0]) {
                    /* 位置只在留下的图上推进：装饰图标不该占掉章包里的插图序号。 */
                    /* Position advances only for kept images: an ornament must not eat a bundle slot. */
                    ++slot;
                    ++*image_count;
                    if (!tb_printf(out, "<img src=\"%s\"/>", name)) return false;
                }
            }
            offset += utf16_at(p, end + 1 - p);
            p = end + 1;
        } else if (*p == '<') {
            const char *end = tag_end(p);
            if (!end || !tb_put(out, p, end + 1 - p)) return false;
            offset += utf16_at(p, end + 1 - p);
            p = end + 1;
        } else {
            size_t n = strcspn(p, "<");
            if (!marker(out, offset) || !tb_put(out, p, n)) return false;
            offset += utf16_at(p, n);
            p += n;
        }
    }
    return !wr_cancelled();
}

/*
 * 同书上一章取到的签名（psvts/pclts/token）先拿来直接取本章正文。
 * 抠 ps 要读整页 /web/reader（约 900 KB，实测只有 25 KB/s → 20~40 秒），而正文三个分片
 * 只有十几 KB、一秒内就下完——跳一章的等待九成花在这页上。若服务端按章校验签名，这一次
 * 请求会被拒，随即回退到读整页，代价只是一次几百毫秒的失败请求；若签名是书级或会话级，
 * 省下的就是每章二三十秒。连续两次被拒就认定这本书不吃复用，不再试。
 * A later chapter first tries the signature (psvts/pclts/token) obtained for the previous one.
 * Scraping ps means reading the whole /web/reader page (~900 KB at the measured 25 KB/s, i.e.
 * 20-40 s), while the three content shards are tens of KB and land in a second: almost all of a
 * chapter jump is that page. If the server binds the signature to the chapter the attempt is
 * rejected and we fall back to the page, costing a few hundred milliseconds; if the signature is
 * book- or session-scoped, every chapter saves 20-30 s. Two rejections disable it for that book.
 */
static wr_reader_t s_content_fields;
static char s_content_book[WR_ID];
static unsigned s_content_rejects;

static char *reuse_content(const wr_book_t *book, const wr_chapter_t *chapter,
                           bool *plain, size_t *size) {
    wr_reader_t reader = s_content_fields;
    if (!reader.ps[0] || strcmp(s_content_book, book->id) || s_content_rejects >= 2) return NULL;
    // Referer 换成本章：服务端可能核对 Referer 与实际章节。
    // The Referer must name this chapter: the server may compare it with the request.
    if (!wr_reader_referer_for(book->id, chapter->id, reader.referer, sizeof(reader.referer)))
        return NULL;
    return wr_chapter_content(book->id, chapter->id, &reader, plain, size);
}

static char *fetch_chapter_content(const wr_book_t *book, const wr_chapter_t *chapter,
                                   bool *plain, size_t *size, unsigned index) {
    int64_t started = esp_timer_get_time();
    wr_reader_t reader;
    if (strcmp(s_content_book, book->id)) {
        snprintf(s_content_book, sizeof(s_content_book), "%s", book->id);
        memset(&s_content_fields, 0, sizeof(s_content_fields));
        s_content_rejects = 0;
    }
    char *content = reuse_content(book, chapter, plain, size);
    if (content) {
        ESP_LOGI("WEREAD", "content signature reused index=%u elapsed_ms=%lld", index + 1,
                 (long long)((esp_timer_get_time() - started) / 1000));
    } else if (s_content_fields.ps[0] && s_content_rejects < 2) {
        ++s_content_rejects;
        ESP_LOGI("WEREAD", "content signature rejected index=%u count=%u", index + 1, s_content_rejects);
    }
    for (unsigned attempt = 0; !content && attempt < 3 && !wr_cancelled(); ++attempt) {
        if (wr_reader(book->id, chapter->id, &reader)) {
            content = wr_chapter_content(book->id, chapter->id, &reader, plain, size);
            if (content) s_content_fields = reader; // 留着给下一章试复用
        }
        if (!content && attempt < 2) vTaskDelay(pdMS_TO_TICKS(400u * (attempt + 1)));
    }
    if (!content) {
        if (!wr_cancelled() && !strcmp(wr_error_message(), "操作失败，请检查网络后重试"))
            wr_error_set("缓存失败：本章内容未能取回（已自动重试 3 次）");
        ESP_LOGW("WEREAD", "cache fetch failed index=%u chapter=%s error=%s",
                 index + 1, chapter->id, wr_error_message());
        return NULL;
    }
    /* 这一章的签名顺手留给时长上报：上报同一章时不必再读一次整页 /web/reader。
     * Referer 改成本章——复用的签名来自同书上一章，直接拿去上报会被当成串章。 */
    /* Hand this chapter's signature to the reporter, so reporting it never re-reads the page.
     * Point the Referer at this chapter: a reused signature came from the previous one. */
    if (s_content_fields.ps[0] && !strcmp(s_content_book, book->id)) {
        wr_reader_t fields = s_content_fields;
        (void)wr_reader_referer_for(book->id, chapter->id, fields.referer, sizeof(fields.referer));
        wr_reader_fields_store(book->id, chapter->id, &fields);
    }
    ESP_LOGI("WEREAD", "cache fetch index=%u bytes=%u elapsed_ms=%lld result=1",
             index + 1, (unsigned)*size,
             (long long)((esp_timer_get_time() - started) / 1000));
    return content;
}

static bool stage_images_pass(const char *book, unsigned index);
static bool wr_stage_images_pending_name(const char *book, unsigned index);

static bool stage_chapter(stage_t *stage, unsigned index, const wr_chapter_t *chapter,
                          const char *content, bool plain) {
    int64_t started = esp_timer_get_time();
    stage->index = index;
    // 本章阅读页地址：相对插图路径以它为基准（服务端渲染时也是这么解析的）。
    // This chapter's reader URL: relative image sources resolve against it, as in the real page.
    (void)wr_reader_referer_for(stage->book, chapter->id, stage->referer, sizeof(stage->referer));
    textbuf_t out = {0};
    bool ok = tb_reserve(&out, 8192) &&
        tb_puts(&out, "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>");
    unsigned images = 0;
    bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    if (ok && !plain) (void)bundle_load(stage, &bundle, chapter->tar, chapter->id);
    if (ok) wr_warm_note(index, 70);
    if (ok) ok = plain ? render_plain(&out, content)
                       : render_html(&out, content, stage, &bundle, &images);
    if (ok) wr_warm_note(index, 85);
    if (ok) ok = tb_puts(&out, "</body></html>");
    /* 正文里出现 NUL 说明响应不是文本，写进 XHTML 只会让阅读器解析失败。 */
    if (ok && memchr(content, 0, strlen(content)) != NULL) ok = false;
    bundle_free(&bundle);
    /* 待补清单必须先落盘：下面第二轮会重新打开并改写它。 */
    /* The deferred list must hit the disk first: the second pass below reopens and rewrites it. */
    stage_pending_close(stage);
    if (ok) {
        char path[WR_PATH], meta[WR_PATH];
        snprintf(path, sizeof(path), "%s/c%u.xhtml", stage->text, index);
        snprintf(meta, sizeof(meta), "%s/c%u.meta", stage->meta, index);
        /* 先写正文并落盘，再写 meta：meta 存在即代表整章可读（插图可能还在等第二轮）。 */
        ok = file_write_atomic(path, out.data, out.used);
        if (ok) {
            char text[WR_META_MAX];
            int n = snprintf(text, sizeof(text), "uid=%s\nlen=%u\ncrc=%08x\n", chapter->id,
                             (unsigned)out.used, (unsigned)crc32(0, (const Bytef *)out.data, out.used));
            ok = n > 0 && n < (int)sizeof(text) && file_write_atomic(meta, text, (size_t)n);
        }
    }
    ESP_LOGI("WEREAD", "cache chapter book=%s index=%u html_bytes=%u images=%u elapsed_ms=%lld result=%d",
             chapter->id, index + 1, (unsigned)out.used, images,
             (long long)((esp_timer_get_time() - started) / 1000), ok);
    free(out.data);
    /* 欠着插图就地补一次：链路正常时这一轮就能补齐，断网时清单留着给下一次窗口。 */
    /* Finish any missing illustration right here: a healthy link completes it in this pass,
     * while an outage leaves the list for the next prefetch window. */
    if (ok && wr_online() && wr_stage_images_pending_name(stage->book, index))
        (void)stage_images_pass(stage->book, index);
    return ok;
}

bool wr_stage_images_pending(const wr_book_t *book, unsigned index) {
    return book && wr_stage_images_pending_name(book->id, index);
}

/*
 * 第二轮：把正文里登记过的插图真正下下来。
 * 放到正文落盘之后，是因为一章的"可读"不该由一张几百 KB 的插图决定；失败的那条留在清单里
 * 带一个计数，下一轮窗口再试一次，第三次仍失败就丢掉——否则一张永远 404 的图会让每轮预取
 * 都白跑一遍。
 * Second pass: actually fetch the illustrations the text references. It runs after the body has
 * landed, because whether a chapter is readable must not depend on one several-hundred-KB image.
 * A failure stays in the list with a counter and gets one more window pass; after the third
 * failure it is dropped, otherwise one permanently-404 picture would tax every prefetch window.
 */
static bool wr_stage_images_pending_name(const char *book, unsigned index) {
    if (!book || !book[0]) return false;
    stage_t stage;
    if (!stage_open(&stage, NULL, book)) return false;
    char list[WR_PATH];
    if (!stage_list_path(&stage, index, list)) return false;
    struct stat info;
    return stat(list, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

static bool stage_images_pass(const char *book, unsigned index) {
    // 断网时直接退出，别把清单里的重试计数白烧掉。
    // Leave at once while offline: burning the retry counters would waste the next window.
    if (!book || !book[0] || !wr_online()) return false;
    stage_t stage;
    if (!stage_open(&stage, NULL, book)) return false;
    char list[WR_PATH];
    if (!stage_list_path(&stage, index, list)) return false;
    FILE *file = fopen(list, "rb");
    if (!file) return true;
    char *text = heap_caps_malloc(WR_IMAGE_LIST_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = text ? fread(text, 1, WR_IMAGE_LIST_MAX - 1, file) : 0;
    fclose(file);
    if (!text) return false;
    text[n] = 0;
    char *keep = heap_caps_malloc(n + WR_IMAGE_LIST_MAX / 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t keep_cap = n + WR_IMAGE_LIST_MAX / 2;
    size_t keep_used = 0;
    unsigned ok = 0, failed = 0, dropped = 0;
    /*
     * 前两条失败的地址与原因：这一轮"20 张全失败"在日志里只有计数，看不出是相对地址没补全、
     * 服务端 404，还是根本没发出去（离线/取消），只能再跑一次实测去猜。
     * The first two failures carry their URL and reason: "20 failed" alone cannot tell a
     * mis-resolved relative path from a server 404 or a request that never left the device.
     */
    static char first_fail[2][768];
    first_fail[0][0] = first_fail[1][0] = 0;
    int64_t started = esp_timer_get_time();
    for (char *line = text; line && *line && !wr_cancelled();) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            const char *name = line;
            char *url = tab + 1;
            char *count = strrchr(url, '\t');
            unsigned tries = 0;
            if (count) { *count = 0; tries = (unsigned)atoi(count + 1); }
            char path[WR_PATH];
            if (name[0] && url[0] &&
                snprintf(path, sizeof(path), "%s/%s", stage.images, name) < (int)sizeof(path)) {
                if (access(path, F_OK) == 0) {
                    ++ok;
                } else {
                    size_t size = 0;
                    char *binary = wr_http(url, NULL, NULL, WR_IMAGE_MAX_BYTES, &size);
                    bool landed = binary && size && file_write_atomic(path, binary, size);
                    free(binary);
                    if (landed) ++ok;
                    else {
                        if (failed < 2 && !first_fail[failed][0])
                            snprintf(first_fail[failed], sizeof(first_fail[0]), "%s | %s",
                                     url, wr_error_message());
                        ++failed;
                        if (tries + 1 < 3) {
                            int written = snprintf(keep + keep_used, keep_cap - keep_used,
                                                   "%s\t%s\t%u\n", name, url, tries + 1);
                            if (written > 0 && keep_used + (size_t)written < keep_cap)
                                keep_used += (size_t)written;
                        } else {
                            ++dropped;
                        }
                    }
                }
            }
        }
        line = next;
    }
    free(text);
    if (keep && keep_used) (void)file_write_atomic(list, keep, keep_used);
    else unlink(list);
    free(keep);
    ESP_LOGI("WEREAD", "chapter images book=%s index=%u ok=%u failed=%u dropped=%u elapsed_ms=%lld",
             book, index + 1, ok, failed, dropped,
             (long long)((esp_timer_get_time() - started) / 1000));
    if (failed)
        ESP_LOGW("WEREAD", "chapter images failed book=%s index=%u first1=%s first2=%s",
                 book, index + 1, first_fail[0], first_fail[1]);
    return failed == 0;
}

bool wr_stage_chapter_images(const wr_book_t *book, unsigned index) {
    return book && stage_images_pass(book->id, index);
}

/*
 * 正文里引用的插图在盘上找不到：把 meta 删掉，让下一轮预取重新落这一章（正文会重下，
 * 缺的图这次按待补清单走）。每章只修一次——服务端本来就缺的图，不能让它每次翻页都重下一章。
 * Drop the meta so the next prefetch stages this chapter again; the body is re-fetched and the
 * missing pictures go through the deferred list. Once per chapter: a picture the server really
 * lacks must not make every turn re-download the chapter.
 */
bool wr_stage_chapter_repair(const wr_book_t *book, unsigned index) {
    if (!book) return false;
    stage_t stage;
    if (!stage_open(&stage, NULL, book->id)) return false;
    char fix[WR_PATH], meta[WR_PATH];
    /*
     * 标记带版本号：ver2 起装饰图（/web/Images/）不再算插图，所以旧版修过一次的章还要再修
     * 一次才能摆脱那些占位。用新名字，等于给每章再放行一次；老标记留着不影响判断。
     * The marker is versioned: from ver2 the UI ornaments under /web/Images/ no longer count as
     * illustrations, so a chapter "repaired" by the older build needs one more pass to shed their
     * placeholders. A new name grants exactly that one pass; the old marker is simply ignored.
     */
    if (snprintf(fix, sizeof(fix), "%s/c%u.fix2", stage.meta, index) >= (int)sizeof(fix)) return false;
    if (access(fix, F_OK) == 0) return false;
    if (!file_write_atomic(fix, "2\n", 2)) return false;
    if (snprintf(meta, sizeof(meta), "%s/c%u.meta", stage.meta, index) < (int)sizeof(meta)) unlink(meta);
    ESP_LOGW("WEREAD", "chapter repair queued book=%s index=%u missing illustration",
             book->id, index + 1);
    return true;
}

/* 已完成的章：meta 里的 uid/长度/CRC 与正文文件一致即跳过。 */
static bool chapter_ready(stage_t *stage, unsigned index, const char *uid) {
    char path[WR_PATH], meta[WR_PATH];
    snprintf(path, sizeof(path), "%s/c%u.xhtml", stage->text, index);
    snprintf(meta, sizeof(meta), "%s/c%u.meta", stage->meta, index);
    FILE *file = fopen(meta, "rb");
    if (!file) return false;
    char text[WR_META_MAX];
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    char stored[WR_ID] = {0};
    unsigned length = 0, crc = 0;
    if (sscanf(text, "uid=%47s\nlen=%u\ncrc=%x", stored, &length, &crc) != 3) return false;
    if (strcmp(stored, uid)) return false;
    size_t size = 0;
    char *data = file_read_psram(path, HTML_TEXT_MAX_BYTES, &size);
    if (!data) return false;
    bool ok = size == length && (unsigned)crc32(0, (const Bytef *)data, size) == crc;
    free(data);
    if (!ok) ESP_LOGW("WEREAD", "staged chapter rejected index=%u uid=%s", index, uid);
    return ok;
}

/* ------------------------------------------------------------------ */
/* EPUB 组装                                                           */
/* ------------------------------------------------------------------ */

static bool cover_entry(epub_writer_t *zip, FILE *opf, const char *source) {
    if (!source[0]) return true;
    static EXT_RAM_BSS_ATTR char url[2056];
    size_t used = 0;
    if (!strncmp(source, "//", 2)) { memcpy(url, "https:", 6); used = 6; }
    for (const char *p = source; *p && used + 1 < sizeof(url); ++p) url[used++] = *p;
    url[used] = 0;
    size_t length = 0;
    char *data = NULL;
    for (unsigned attempt = 0; attempt < 3 && !data && !wr_cancelled(); ++attempt) {
        data = wr_http(url, NULL, NULL, WR_IMAGE_MAX_BYTES, &length);
        if (!data && !strcmp(wr_error_message(), "微信读书图片地址不受支持")) break;
        if (!data && attempt < 2) vTaskDelay(pdMS_TO_TICKS(350u * (attempt + 1)));
    }
    if (!data) return true; /* 封面是尽力而为，缺图不该让整本书失败。 */
    const char *mime = NULL;
    const char *ext = image_extension(data, length, &mime);
    bool ok = true;
    if (ext) {
        char name[WR_NAME_MAX];
        snprintf(name, sizeof(name), "cover.%s", ext);
        ok = zip_entry(zip, name, data, length) &&
            fprintf(opf, "<item id=\"cover\" href=\"%s\" media-type=\"%s\" properties=\"cover-image\"/>",
                    name, mime) >= 0;
    }
    free(data);
    return ok;
}

static bool images_manifest(epub_writer_t *zip, FILE *opf, stage_t *stage) {
    DIR *dir = opendir(stage->images);
    if (!dir) return true;
    struct dirent *entry;
    unsigned id = 0;
    bool ok = true;
    while (ok && (entry = readdir(dir))) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;
        const char *mime = mime_for_extension(dot + 1);
        if (!mime) continue;
        char path[WR_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", stage->images, entry->d_name) >= (int)sizeof(path)) continue;
        if (!path_entry(zip, entry->d_name, path, WR_IMAGE_MAX_BYTES)) { ok = false; break; }
        ok = fprintf(opf, "<item id=\"i%u\" href=\"%s\" media-type=\"%s\"/>", id++, entry->d_name, mime) >= 0;
    }
    closedir(dir);
    return ok;
}

static bool assemble_book(const wr_book_t *book, FILE *target, stage_t *stage,
                          const wr_chapter_t *chapters, unsigned count) {
    static const char container[] = "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    static const char type[] = "application/epub+zip";
    FILE *central = fopen(WR_DIR "/central.part", "w+b");
    FILE *opf = fopen(WR_DIR "/opf.part", "w+b");
    FILE *nav = fopen(WR_DIR "/nav.part", "w+b");
    if (central) (void)setvbuf(central, NULL, _IOFBF, 4096);
    if (opf) (void)setvbuf(opf, NULL, _IOFBF, 2048);
    if (nav) (void)setvbuf(nav, NULL, _IOFBF, 2048);
    epub_writer_t zip = {.file = target, .directory = central};
    const char *step = "temp-files";
    unsigned failed_index = count;
    bool ok = central && opf && nav && zip_entry(&zip, "mimetype", type, sizeof(type) - 1) &&
        zip_entry(&zip, "META-INF/container.xml", container, sizeof(container) - 1);
    if (!ok) step = "container";
    if (ok) ok = fputs("<package version=\"3.0\" xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"book\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"book\">", opf) != EOF &&
        write_xml(opf, book->id, strlen(book->id)) && fputs("</dc:identifier><dc:title>", opf) != EOF &&
        write_xml(opf, book->title, strlen(book->title)) && fputs("</dc:title><dc:creator>", opf) != EOF &&
        write_xml(opf, book->author, strlen(book->author)) &&
        fputs("</dc:creator><dc:language>zh</dc:language></metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>", opf) != EOF &&
        fputs("<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body><nav epub:type=\"toc\"><ol>", nav) != EOF;
    if (ok) { step = "opf-header"; ok = cover_entry(&zip, opf, book->cover); }
    if (ok) { step = "cover"; ok = images_manifest(&zip, opf, stage); }
    for (unsigned i = 0; ok && i < count && !wr_cancelled(); ++i) {
        char name[32], path[WR_PATH];
        snprintf(name, sizeof(name), "c%u.xhtml", i);
        snprintf(path, sizeof(path), "%s/c%u.xhtml", stage->text, i);
        ok = path_entry(&zip, name, path, HTML_TEXT_MAX_BYTES) &&
            fprintf(opf, "<item id=\"c%u\" href=\"%s\" media-type=\"application/xhtml+xml\"/>", i, name) >= 0 &&
            fprintf(nav, "<li><a href=\"%s\">", name) >= 0 &&
            write_xml(nav, chapters[i].title, strlen(chapters[i].title)) &&
            fputs("</a></li>", nav) != EOF;
        if (!ok) { step = "chapter"; failed_index = i; }
    }
    if (ok && !wr_cancelled()) {
        step = "spine";
        /* opf/nav 还带缓冲，先落盘再作为条目读入。 */
        ok = !fflush(opf) && !fflush(nav) && !fflush(central) && fputs("</manifest><spine>", opf) != EOF;
        for (unsigned i = 0; ok && i < count; ++i) ok = fprintf(opf, "<itemref idref=\"c%u\"/>", i) >= 0;
        ok = ok && fputs("</spine></package>", opf) != EOF && !fflush(opf) &&
            fputs("</ol></nav></body></html>", nav) != EOF && !fflush(nav) &&
            path_entry(&zip, "book.opf", WR_DIR "/opf.part", HTML_TEXT_MAX_BYTES) &&
            path_entry(&zip, "nav.xhtml", WR_DIR "/nav.part", HTML_TEXT_MAX_BYTES) && zip_finish(&zip);
    } else ok = false;
    /* 组装失败此前完全静默：用户只看到"缓存失败"，89 章缓存却已落盘。
     * 这里把失败阶段、章节序号和 errno 一起打出来，下次能直接定位。
     * Assembly failures used to be silent; log the stage, index and errno instead. */
    if (!ok) ESP_LOGE("WEREAD", "assemble failed step=%s index=%u errno=%d", step, failed_index, errno);
    if (nav) fclose(nav);
    if (opf) fclose(opf);
    if (central) fclose(central);
    unlink(WR_DIR "/central.part");
    unlink(WR_DIR "/opf.part");
    unlink(WR_DIR "/nav.part");
    return ok;
}

/* ------------------------------------------------------------------ */
/* 章节目录：内存 + SD 卡双层缓存                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t magic, version, saved, count;
    char book[WR_ID], owner[WR_ID];
} wr_toc_header_t;

static struct {
    char owner[WR_ID];
    char book[WR_ID];
    wr_chapter_t *items;
    unsigned count;
    int64_t expires;
} s_chapter_cache;

static bool toc_path(char path[WR_PATH], const char *owner, const char *book) {
    if (!wr_safe(owner) || !wr_safe(book)) return false;
    int n = snprintf(path, WR_PATH, WR_DIR "/toc/%s-%s.bin", owner, book);
    return n > 0 && n < WR_PATH;
}

static void toc_cache_save(const char *owner, const char *book,
                           const wr_chapter_t *chapters, unsigned count) {
    char path[WR_PATH];
    if (!count || count > WR_TOC_MAX_CHAPTERS || !toc_path(path, owner, book)) return;
    if (!dir_ensure(WR_DIR "/toc")) return;
    const size_t bytes = sizeof(wr_toc_header_t) + count * sizeof(wr_chapter_t);
    char *data = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return;
    wr_toc_header_t header = {
        .magic = WR_TOC_MAGIC, .version = WR_TOC_VERSION,
        .saved = (uint32_t)time(NULL), .count = count,
    };
    snprintf(header.book, sizeof(header.book), "%s", book);
    snprintf(header.owner, sizeof(header.owner), "%s", owner);
    memcpy(data, &header, sizeof(header));
    memcpy(data + sizeof(header), chapters, count * sizeof(wr_chapter_t));
    (void)file_write_atomic(path, data, bytes);
    free(data);
}

static wr_chapter_t *toc_cache_load(const char *owner, const char *book, unsigned *count) {
    char path[WR_PATH];
    *count = 0;
    if (!toc_path(path, owner, book)) return NULL;
    size_t size = 0;
    char *data = file_read_psram(path, 4u * 1024u * 1024u, &size);
    if (!data) return NULL;
    wr_toc_header_t header;
    bool ok = size >= sizeof(header);
    if (ok) memcpy(&header, data, sizeof(header));
    ok = ok && header.magic == WR_TOC_MAGIC && header.version == WR_TOC_VERSION &&
        header.count && header.count <= WR_TOC_MAX_CHAPTERS &&
        !strcmp(header.book, book) && !strcmp(header.owner, owner) &&
        size >= sizeof(header) + header.count * sizeof(wr_chapter_t) &&
        (uint32_t)time(NULL) - header.saved < WR_TOC_TTL_SECONDS;
    if (!ok) { free(data); return NULL; }
    wr_chapter_t *items = heap_caps_malloc(header.count * sizeof(*items), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (items) {
        memcpy(items, data + sizeof(header), header.count * sizeof(*items));
        *count = header.count;
    }
    free(data);
    return items;
}

static const cJSON *chapter_array(const cJSON *root, const char *book_id, unsigned depth) {
    if (!root || depth > 5) return NULL;
    if (cJSON_IsArray(root)) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, root) {
            if (cJSON_GetObjectItemCaseSensitive(entry, "chapterUid")) return root;
            char entry_id[WR_ID];
            if (wr_value(cJSON_GetObjectItemCaseSensitive(entry, "bookId"), entry_id, sizeof(entry_id)) &&
                strcmp(entry_id, book_id)) continue;
            const cJSON *chapters = chapter_array(entry, book_id, depth + 1);
            if (chapters) return chapters;
        }
    } else if (cJSON_IsObject(root)) {
        static const char *const keys[] = {"chapterInfos", "updated", "data"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
            const cJSON *chapters = chapter_array(cJSON_GetObjectItemCaseSensitive(root, keys[i]), book_id, depth + 1);
            if (chapters) return chapters;
        }
    }
    return NULL;
}

static wr_chapter_t *get_chapters(const wr_book_t *book, unsigned *count) {
    int64_t started = esp_timer_get_time();
    char query[100];
    char owner[WR_ID] = {0};
    *count = 0;
    (void)wr_account(owner, NULL, NULL);
    if (s_chapter_cache.items && s_chapter_cache.expires > started &&
        !strcmp(owner, s_chapter_cache.owner) && !strcmp(book->id, s_chapter_cache.book)) {
        wr_chapter_t *copy = heap_caps_malloc(s_chapter_cache.count * sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, s_chapter_cache.items, s_chapter_cache.count * sizeof(*copy));
            *count = s_chapter_cache.count;
            ESP_LOGI("WEREAD", "chapter list cache hit book=%s chapters=%u", book->id, *count);
            return copy;
        }
    }
    wr_chapter_t *result = owner[0] ? toc_cache_load(owner, book->id, count) : NULL;
    if (result) {
        ESP_LOGI("WEREAD", "chapter list disk hit book=%s chapters=%u", book->id, *count);
        goto remember;
    }
    snprintf(query, sizeof(query), "{\"bookIds\":[\"%s\"]}", book->id);
    cJSON *toc = wr_json("/web/book/chapterInfos", query);
    if (!toc) return NULL;
    const cJSON *chapters = chapter_array(toc, book->id, 0);
    int n = cJSON_GetArraySize(chapters);
    *count = 0;
    result = n > 0 && n <= ZIP_ENTRY_MAX - 4
        ? heap_caps_calloc(n, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!result) {
        ESP_LOGW("WEREAD", "cache toc unavailable count=%d updated_type=%d data_type=%d", n,
                 cJSON_GetObjectItemCaseSensitive(toc, "updated") ? cJSON_GetObjectItemCaseSensitive(toc, "updated")->type : 0,
                 cJSON_GetObjectItemCaseSensitive(toc, "data") ? cJSON_GetObjectItemCaseSensitive(toc, "data")->type : 0);
        wr_error_set(n == 0 ? "缓存失败：微信读书未返回可用章节目录" :
                     n > ZIP_ENTRY_MAX - 4 ? "缓存失败：章节数量超出 EPUB 支持范围" :
                     "缓存失败：内存不足，无法载入章节目录");
    }
    const cJSON *entry;
    if (result) cJSON_ArrayForEach(entry, chapters) {
        wr_chapter_t *item = &result[*count];
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry, "chapterUid"), item->id, sizeof(item->id)) || !wr_safe(item->id)) {
            ESP_LOGW("WEREAD", "cache toc invalid chapter at index=%u", *count);
            wr_error_set("缓存失败：章节目录中有无效的章节标识");
            free(result);
            result = NULL;
            break;
        }
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(entry, "chapterIdx");
        item->index = cJSON_IsNumber(index) && index->valuedouble >= 0 && index->valuedouble <= UINT32_MAX ? (uint32_t)index->valuedouble : *count;
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "title"), item->title, sizeof(item->title));
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "tar"), item->tar, sizeof(item->tar));
        ++*count;
    }
    cJSON_Delete(toc);
    if (result && owner[0]) toc_cache_save(owner, book->id, result, *count);
remember:
    if (result && owner[0]) {
        wr_chapter_t *saved = heap_caps_malloc(*count * sizeof(*saved), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (saved) {
            memcpy(saved, result, *count * sizeof(*saved));
            free(s_chapter_cache.items);
            snprintf(s_chapter_cache.owner, sizeof(s_chapter_cache.owner), "%s", owner);
            snprintf(s_chapter_cache.book, sizeof(s_chapter_cache.book), "%s", book->id);
            s_chapter_cache.items = saved;
            s_chapter_cache.count = *count;
            s_chapter_cache.expires = started + 600000000;
        }
    }
    ESP_LOGI("WEREAD", "cache toc book=%s chapters=%u elapsed_ms=%lld", book->id, *count,
             (long long)((esp_timer_get_time() - started) / 1000));
    return result;
}

/* ------------------------------------------------------------------ */
/* 缓存入口                                                            */
/* ------------------------------------------------------------------ */

static size_t epub_chapters(const char *path) {
    book_epub_t *book = NULL;
    if (book_epub_open(path, &book) != ESP_OK) return 0;
    size_t count = book_epub_chapter_count(book);
    book_epub_close(book);
    return count;
}

bool wr_cache(const wr_book_t *book, const char *path) {
    /* 分章缓存书在磁盘上只是一个标记文件：它不是完整 EPUB，但也不能当成损坏缓存删掉，
     * 否则用户读到一半的分章缓存会被整书缓存流程清空。 */
    /* A chapter-cached book is only a marker file on disk: not a complete EPUB, but it must not
     * be treated as corrupt and unlinked, or the user's partial cache would be wiped. */
    bool marked = wr_mark_read(path, NULL, NULL);
    (void)marked;
    if (access(path, F_OK) == 0 && !marked) {
        if (epub_chapters(path) > 0) return true;
        ESP_LOGW("WEREAD", "remove invalid cached epub book=%s path=%s", book->id, path);
        if (unlink(path) && errno != ENOENT) { wr_error_set("缓存失败：旧缓存损坏且无法从 SD 卡删除"); return false; }
    }
    unsigned count = 0;
    /* 取目录失败时，s_error_message 可能还是上一次操作留下的文案，直接展示会指错方向。
     * 先记下旧文案，失败后若没被更新，就换成如实的"章节目录获取失败"。
     * A failed TOC fetch may leave the previous operation's message in place, which points the user
     * at the wrong cause; only keep the message when this attempt actually set a new one. */
    char before[96];
    snprintf(before, sizeof(before), "%s", wr_error_message());
    wr_chapter_t *chapters = get_chapters(book, &count);
    if (!chapters) {
        if (!strcmp(wr_error_message(), before)) wr_error_set("缓存失败：章节目录获取失败");
        return false;
    }
    char owner[WR_ID] = {0};
    (void)wr_account(owner, NULL, NULL);
    stage_t stage;
    if (!stage_open(&stage, owner, book->id)) {
        wr_error_set("缓存失败：无法创建缓存暂存目录");
        free(chapters);
        return false;
    }

    bool ok = true;
    unsigned staged = 0;
    for (unsigned i = 0; ok && i < count && !wr_cancelled(); ++i) {
        if (chapter_ready(&stage, i, chapters[i].id)) {
            ++staged;
            wr_work_progress(i + 1, count);
            continue;
        }
        size_t size = 0;
        bool plain = false;
        char *content = fetch_chapter_content(book, &chapters[i], &plain, &size, i);
        if (!content) { ok = false; break; }
        if (size >= 4 && !memcmp(content, "PK\x03\x04", 4)) {
            /* 服务端整本直发 ZIP：直接落成 EPUB，不做逐章组装。 */
            ok = !i && file_write_atomic(path, content, size) && epub_chapters(path) > 0;
            free(content);
            if (!ok) wr_error_set("缓存失败：微信读书返回的整书文件校验失败");
            goto finish;
        }
        ok = stage_chapter(&stage, i, &chapters[i], content, plain);
        free(content);
        if (ok) {
            ++staged;
            wr_work_progress(i + 1, count);
            ESP_LOGI("WEREAD", "cache staged chapters=%u/%u", staged, count);
        }
        /* 让空闲任务和看门狗有机会在密集的章节转换之间运行。 */
        vTaskDelay(1);
    }
    if (wr_cancelled()) ok = false;
    if (ok && staged < count) {
        ok = false;
        wr_error_set("缓存失败：仍有章节未完成，已保留进度供下次继续");
    }
    if (ok) {
        char temp[WR_PATH + 8];
        snprintf(temp, sizeof(temp), "%s.part", path);
        /* 组装阶段最容易整盘失败却没有任何线索：把剩余空间一并记下来。
         * Assembly is the most opaque failure point; record free space alongside it. */
        uint64_t total_bytes = 0, free_bytes = 0;
        (void)esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
        ESP_LOGI("WEREAD", "cache assemble start free=%lluKB", (unsigned long long)(free_bytes / 1024));
        FILE *file = fopen(temp, "w+b");
        if (file) (void)setvbuf(file, NULL, _IOFBF, 8192);
        bool assembled = assemble_book(book, file, &stage, chapters, count);
        ok = file && assembled && !fflush(file) && !fsync(fileno(file));
        /* 别把上一次操作残留的文案当成这次的原因。 / Never surface a stale message here. */
        if (!ok && !wr_cancelled()) wr_error_set("缓存失败：EPUB 组装失败，已完成章节已保留");
        if (file && fclose(file)) ok = false;
        if (ok && !wr_cancelled()) {
            ok = (unsigned)epub_chapters(temp) == count;
            if (ok && rename(temp, path) != 0) { ok = false; wr_error_set("缓存失败：SD 卡无法保存书籍文件"); }
            else if (!ok && !wr_cancelled()) wr_error_set("缓存失败：组装后的 EPUB 校验失败");
        } else ok = false;
        if (!ok) unlink(temp);
    }
finish:
    if (ok) {
        /* 缓存完成，清掉暂存目录。 */
        dir_remove_tree(stage.root);
    } else if (wr_cancelled()) {
        wr_error_set("缓存已取消，已完成的章节已保留");
    }
    free(chapters);
    return ok;
}

/*
 * 读一章正文，供进度映射使用：优先章节缓存（分章阅读的书在磁盘上只有标记文件），
 * 再退回整书 EPUB。两条路径拿到的是同一份 HTML，云端字节偏移在两边的含义一致。
 * Load one chapter body for position mapping: the chapter cache first (a chapter-wise book is
 * only a marker file on disk), then the complete EPUB. Both hold the same HTML, so a cloud
 * byte offset means the same thing on either path.
 */
static char *chapter_body(const wr_book_t *book, const char *path, unsigned chapter, size_t *size) {
    *size = 0;
    char body[WR_PATH];
    if (book && wr_stage_path(book, chapter, body)) {
        struct stat info;
        if (!stat(body, &info) && S_ISREG(info.st_mode) && info.st_size > 0 &&
            (uint64_t)info.st_size <= HTML_TEXT_MAX_BYTES) {
            FILE *file = fopen(body, "rb");
            if (file) {
                const size_t want = (size_t)info.st_size;
                char *data = heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (data) {
                    size_t got = fread(data, 1, want, file);
                    if (got == want) { data[want] = 0; fclose(file); *size = want; return data; }
                    free(data);
                }
                fclose(file);
            }
        }
    }
    if (!path || !path[0]) return NULL;
    zip_reader_t *zip = NULL;
    if (zip_open(path, &zip) != ESP_OK) return NULL;
    char name[32];
    snprintf(name, sizeof(name), "c%u.xhtml", chapter);
    int entry = zip_find(zip, name);
    size_t length = zip_entry_size(zip, entry);
    char *data = length && length <= HTML_TEXT_MAX_BYTES
        ? heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!data || zip_extract(zip, entry, data, length) != ESP_OK) { free(data); data = NULL; }
    else *size = length;
    zip_close(zip);
    return data;
}

static bool map_position(const wr_book_t *book, const char *path, unsigned chapter, bool upload,
                         unsigned offset, unsigned *mapped) {
    size_t size = 0;
    char *data = chapter_body(book, path, chapter, &size);
    bool ok = data && html_weread_position(data, size, upload, offset, mapped);
    free(data);
    // Chapter starts also work with complete EPUBs delivered by WeRead.
    if (!ok && !offset) { *mapped = 0; return true; }
    return ok;
}

bool wr_progress(const wr_book_t *book, const char *path, bool upload, const char **why,
                 unsigned *target_chapter) {
    if (why) *why = NULL;
    if (target_chapter) *target_chapter = UINT_MAX;
    wr_error_set("操作失败，请检查网络后重试");
    struct stat st;
    book_progress_t local = {0};
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > UINT32_MAX) {
        if (why) *why = "本地书籍文件不可用，请重新缓存";
        return false;
    }
    if (upload && !book_progress_load(path, (uint32_t)st.st_size, &local)) {
        if (why) *why = "这本书还没有本地阅读进度，先读一段再上传";
        return false;
    }
    unsigned count, offset = 0, pct = 0;
    wr_chapter_t remote;
    wr_chapter_t *chapters = get_chapters(book, &count);
    if (!chapters) {
        const char *detail = wr_error_message();
        if (why) *why = detail && *detail ? detail : "无法获取云端章节列表";
        return false;
    }
    unsigned index = local.chapter;
    if (!upload) {
        bool absent = false;
        if (!wr_remote_position_with_chapters(book->id, &remote, &offset, &pct, chapters, count, &absent)) {
            if (!absent) {
                free(chapters);
                const char *detail = wr_error_message();
                if (why) *why = detail && *detail ? detail : "微信读书里还没有这本书的阅读进度";
                return false;
            }
            /*
             * 云端确实没有这本书的进度（请求成功、只是没有字段）：下载进度的结果就是"从头读"，
             * 直接落在第一章，而不是报一句"云端还没有进度"再让读者回书架点一次。
             * The cloud really has no position for this book (the request succeeded, the fields are
             * simply absent): the honest result of "download progress" is reading from the start,
             * so land on chapter one instead of reporting a failure the reader must click away.
             */
            index = 0;
            offset = 0;
            pct = 0;
            wr_error_set("云端尚无阅读进度，已从第一章开始");
            ESP_LOGI("WEREAD", "cloud progress absent book=%s, download falls back to chapter 1", book->id);
        } else {
            index = count;
            for (unsigned i = 0; i < count; ++i) if (!strcmp(chapters[i].id, remote.id)) { index = i; break; }
            if (index >= count) {
                free(chapters);
                if (why) *why = "云端进度所在章节与本地版本不一致，无法定位";
                return false;
            }
        }
    }
    unsigned mapped = 0;
    bool ok = index < count && map_position(book, path, index, upload,
                                            upload ? local.byte_off : offset, &mapped);
    /*
     * 下载进度时目标章节可能还没缓存：正文不在本地就没有可映射的 HTML。这时不报错，
     * 直接把云端偏移记成目标位置——章节下载完成后正文与云端字节流一致，偏移随即有效，
     * 阅读器就能停在手机上的那一句。上传方向没有这个问题（本地进度一定对应本地正文）。
     * The target chapter may not be cached yet when downloading: without a local body there is
     * no HTML to map against. Do not fail; keep the cloud offset as the target. Once the
     * chapter lands its bytes match the cloud stream, the offset becomes valid, and the reader
     * resumes at the very sentence the phone stopped on. Upload never hits this (local progress
     * always has local text).
     */
    bool uncached_target = false;
    if (!ok && !upload && index < count) {
        mapped = offset;
        ok = true;
        uncached_target = true;
        ESP_LOGI("WEREAD", "download progress targets uncached chapter index=%u offset=%u",
                 index + 1, offset);
    }
    if (!ok && why) *why = "本地位置无法映射到云端章节，翻一页后再试";
    if (ok && upload) {
        ok = wr_send_position(book->id, &chapters[index], mapped, local.pct);
        if (!ok && why) *why = "上传进度失败，请检查网络后重试";
    } else if (ok) {
        if (target_chapter) *target_chapter = index;
        (void)book_progress_load(path, (uint32_t)st.st_size, &local);
        if (local.px < BOOK_READER_PX_MIN || local.px > BOOK_READER_PX_MAX)
            local.px = BOOK_READER_PX_DEFAULT;
        local.file_size = (uint32_t)st.st_size;
        local.chapter = (uint16_t)index;
        local.byte_off = mapped;
        local.pct = (uint8_t)pct;
        esp_err_t save_err = wr_cancelled() ? ESP_ERR_INVALID_STATE : book_progress_save(path, &local);
        ok = save_err == ESP_OK;
        if (!ok) ESP_LOGW("WEREAD", "download cloud progress local save failed err=%s", esp_err_to_name(save_err));
        if (!ok && why) *why = "云端进度已获取，但设备保存本地阅读位置失败";
        if (ok && uncached_target && why) *why = NULL;
    }
    free(chapters);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 分章缓存入口（供 weread.c 的后台任务调用）                          */
/* ------------------------------------------------------------------ */

wr_chapter_t *wr_chapter_list(const wr_book_t *book, unsigned *count) {
    if (!book || !count) return NULL;
    return get_chapters(book, count);
}

/*
 * 只读磁盘目录缓存，绝不联网。阅读器在 UI 线程打开书籍，那里多一次请求就是一次卡顿；
 * 重启后从本地书架直接打开这本书时，靠它把章节目录接上。
 * Disk cache only, never the network. The reader opens books on the UI thread, where one
 * request means one freeze; this is what lets a rebooted device reopen the book from the
 * local shelf.
 */
wr_chapter_t *wr_chapter_list_cached(const wr_book_t *book, unsigned *count) {
    char owner[WR_ID] = {0};
    if (!book || !book->id[0] || !count) return NULL;
    *count = 0;
    if (!wr_account(owner, NULL, NULL)) return NULL;
    return toc_cache_load(owner, book->id, count);
}

/*
 * 取一章并落盘。复用整书缓存的同一套取文与渲染路径，因此图文混排、章包（tar/zip）
 * 与去重规则完全一致；差别只在于不再等全书就绪才组装 EPUB。
 * Stage a single chapter through the same fetch and render path as the full-book cache, so
 * image handling, chapter bundles and de-duplication stay identical; only the "wait for the
 * whole book before assembling" step is gone.
 */
/*
 * 服务端极少数书籍会整本直发 ZIP：逐章接口拿不回章节正文，只能改走整书缓存。
 * 用一个标志把这件事告诉上层的"缓存全本"，避免靠比较错误文案来分支。
 * A handful of books are served as one whole ZIP: the per-chapter endpoint cannot return a
 * chapter body, so the whole-book path has to take over. A flag carries that up to the
 * "cache the whole book" caller instead of branching on an error string.
 */
static bool s_whole_zip_expected;

bool wr_stage_chapter(const wr_book_t *book, const wr_chapter_t *chapter, unsigned index) {
    if (!book || !chapter || !chapter->id[0]) return false;
    char owner[WR_ID] = {0};
    (void)wr_account(owner, NULL, NULL);
    stage_t stage;
    if (!stage_open(&stage, owner, book->id)) { wr_error_set("缓存失败：无法创建缓存目录"); return false; }
    if (chapter_ready(&stage, index, chapter->id)) return true;
    wr_warm_note(index, 5);
    size_t size = 0;
    bool plain = false;
    char *content = fetch_chapter_content(book, chapter, &plain, &size, index);
    if (!content) return false;
    if (size >= 4 && !memcmp(content, "PK\x03\x04", 4)) {
        free(content);
        /* 服务端偶尔整本直发 ZIP；逐章路径无法承接，交给整书缓存流程。 */
        s_whole_zip_expected = true;
        wr_error_set("这本书由服务端整包直发，请使用「缓存全书」");
        return false;
    }
    wr_warm_note(index, 45);
    bool ok = stage_chapter(&stage, index, chapter, content, plain);
    free(content);
    if (ok) {
        wr_warm_note(index, 100);
        wr_cache_summary_bump(book); // 书架据此显示"已缓存 N/M"
        ESP_LOGI("WEREAD", "chapter staged book=%s index=%u", book->id, index + 1);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* 整书缓存（用户点"缓存全本书籍"）                                    */
/* ------------------------------------------------------------------ */

/*
 * 整书缓存不再把全书组装成单个 EPUB，而是逐章把全书落盘：
 *  · 组装是整条链路里最容易整盘失败的一步（曾出现 89/89 章就绪后组装失败）；
 *  · 标记文件一旦被 EPUB 原子替换，文件长度就变了，按"路径 + 长度"校验的本地进度会
 *    整本失效，读者刚同步好的位置当场丢掉；
 *  · 逐章落盘天然可断点续传，中途取消或断电只丢当前章。
 * 读起来和已缓存的章完全一致：都走虚拟书源，不牺牲排版、字体、分页与书签。
 *
 * The whole-book cache no longer assembles one EPUB; it stages every chapter instead:
 *  · assembly is the most failure-prone step of the chain (89/89 chapters ready, then it failed);
 *  · replacing the marker with an EPUB changes the file length, which invalidates every
 *    size-validated local progress record and throws away the position just synced;
 *  · chapter staging resumes after a cancel or power loss, losing only the current chapter.
 * Reading is identical to any cached chapter: same virtual source, same typography, fonts,
 * pagination and bookmarks.
 */
bool wr_cache_full(const wr_book_t *book, const char *path) {
    if (!book || !book->id[0] || !path || !path[0]) return false;
    s_whole_zip_expected = false;
    /* 已经是完整 EPUB 的书不要被标记文件覆盖：界面按理不会给这个入口，但这条防线更便宜。
     * Never overwrite a complete EPUB with a marker: the UI should not offer this entry, and
     * this guard is far cheaper than recovering a wiped book. */
    if (access(path, F_OK) == 0 && !wr_mark_read(path, NULL, NULL) && epub_chapters(path) > 0)
        return true;
    unsigned count = 0;
    wr_chapter_t *chapters = wr_chapter_list(book, &count);
    if (!chapters || !count) {
        free(chapters);
        wr_error_set("缓存失败：章节目录获取失败");
        return false;
    }
    if (!wr_cache_dir(book)) {
        free(chapters);
        wr_error_set("缓存失败：无法创建章节缓存目录");
        return false;
    }
    /* 标记文件先落盘：书架、首页"继续阅读"和进度都按"路径存在且有长度"识别这本书，
     * 即使只缓存了一半，它也已经是一本可打开的书。
     * The marker lands first: shelf, home strip and progress all key on an existing path, so
     * even a half-cached book is already openable. */
    if (!wr_mark_write(book, path, count)) {
        free(chapters);
        wr_error_set("缓存失败：无法写入书籍占位文件，请检查 SD 卡");
        return false;
    }
    wr_cache_summary_refresh(book, count);
    bool ok = true;
    for (unsigned i = 0; i < count; ++i) {
        if (wr_cancelled()) { ok = false; break; }
        wr_work_progress(i + 1, count);
        if (wr_cache_state(book, i) == WR_CS_READY) continue; // 已有章节不重复下载
        if (!wr_stage_chapter(book, &chapters[i], i)) { ok = false; break; }
        vTaskDelay(1); // 让空闲任务与看门狗在密集的章节转换之间运行
    }
    if (wr_cancelled()) {
        ok = false;
        wr_error_set("缓存已取消，已完成的章节已保留");
    } else if (!ok && s_whole_zip_expected) {
        /* 整本直发的书没有逐章正文可拿，退回原来的整书路径；那条路本来就会先落成 EPUB。 */
        ESP_LOGW("WEREAD", "chapter staging unavailable book=%s, falling back to full book", book->id);
        free(chapters);
        return wr_cache(book, path);
    } else if (ok) {
        /* 复核一遍：只有全书都真的 READY 才敢报"已缓存"，半成品不许冒充完成。 */
        unsigned ready = 0;
        for (unsigned i = 0; i < count; ++i) if (wr_cache_state(book, i) == WR_CS_READY) ++ready;
        if (ready < count) {
            ok = false;
            wr_error_set("缓存失败：仍有章节未完成，已保留进度供下次继续");
            ESP_LOGW("WEREAD", "full cache incomplete book=%s ready=%u/%u", book->id, ready, count);
        } else {
            wr_cache_summary_write(book, ready, count);
            ESP_LOGI("WEREAD", "full cache done book=%s chapters=%u", book->id, count);
        }
    }
    free(chapters);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 删除本地书籍时回收微信读书侧的缓存                                  */
/* ------------------------------------------------------------------ */

/*
 * 分章缓存的数据不在书籍文件里，而在 .weread/stage-<账号>-<书籍>/ 这样的独立目录里；
 * 本地书架删掉的只是那个 96 字节的标记文件，章节正文、插图、目录缓存、缓存概况和旁挂
 * 封面全都留着。于是出现了"书架说删了、微读书架还说已缓存 N/M、SD 卡上还占着几十兆"
 * 的三方不一致。删除书籍时必须在这里把这一整套一起收回。
 *
 * The chapter cache does not live inside the book file but in a separate
 * .weread/stage-<owner>-<book>/ directory; deleting the book from the local shelf only removed
 * the 96-byte marker, leaving bodies, images, the TOC cache, the cache summary and the cover
 * sidecar behind. That produced three-way disagreement: the shelf says deleted, the WeRead shelf
 * still says "cached N/M", and the card still holds tens of megabytes. Deleting a book has to
 * reclaim all of it here.
 *
 * 书籍 ID 从文件名解析（wr-<账号库>+<bookid>.epub），所以调用时机可以在 unlink 之后。
 * The book id comes from the file name (wr-<library>+<bookid>.epub), so this may run after the
 * unlink.
 */
static bool book_id_from_path(const char *path, char id[WR_ID]) {
    if (!path || !path[0] || !id) return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, "wr-", 3)) return false;
    const char *plus = strchr(base, '+');
    const char *dot = strrchr(base, '.');
    if (!plus || !dot || dot <= plus + 1) return false;
    size_t len = (size_t)(dot - plus - 1);
    if (!len || len >= WR_ID) return false;
    memcpy(id, plus + 1, len);
    id[len] = 0;
    return wr_safe(id);
}

bool wr_forget_book(const char *path) {
    char id[WR_ID] = {0};
    if (!book_id_from_path(path, id)) return false;
    char owner[WR_ID] = {0};
    if (!wr_account(owner, NULL, NULL) || !owner[0]) return false;
    wr_book_t book;
    memset(&book, 0, sizeof(book));
    snprintf(book.id, sizeof(book.id), "%s", id);
    bool dropped = false;
    // 章节暂存目录：正文、插图、meta 与手动缓存标记都在里面。
    char root[WR_PATH];
    struct stat info;
    if (wr_stage_root(root, sizeof(root), book.id) && stat(root, &info) == 0) {
        /* 章节没删干净就别动概况文件：宁可让书架继续显示"已缓存"，也不能把还有章节的
         * 书写成"未缓存"——那才是真正的误导（点开发现全都能读）。
         * Never clear the summary while chapters remain: keeping "cached" is honest, while
         * calling a book uncached when its chapters still read is the actual lie. */
        if (!dir_remove_tree(root) || stat(root, &info) == 0) {
            ESP_LOGW("WEREAD", "forget book=%s stage tree incomplete, keep summary", id);
            return false;
        }
        dropped = true;
    }
    char path_buf[WR_PATH];
    // 目录缓存 / 缓存概况，都是按"账号+书籍"命名的单文件。
    if (snprintf(path_buf, sizeof(path_buf), WR_DIR "/toc/%s-%s.bin", owner, id) < (int)sizeof(path_buf) &&
        unlink(path_buf) == 0) dropped = true;
    if (snprintf(path_buf, sizeof(path_buf), WR_DIR "/%s+%s.cc", owner, id) < (int)sizeof(path_buf) &&
        unlink(path_buf) == 0) dropped = true;
    /*
     * 微读书架自己的封面（WR_DIR/<账号>+<书>.gray）不能跟着删：删的是本地书架上的这本书，
     * 它在微信读书里仍然存在、仍然要显示封面。删掉之后书架要等下一次同步才补回来，
     * 中间就是一张空白封面。
     * The WeRead shelf's own cover (WR_DIR/<owner>+<book>.gray) must survive: what was deleted is
     * the local copy of the book, while it still exists in WeRead and still needs artwork. Dropping
     * it leaves a blank cover until the next sync.
     */
    // 书籍文件旁的灰阶封面快照是本地书架的产物，书没了就一起清。
    if (snprintf(path_buf, sizeof(path_buf), "%s.gray", path) < (int)sizeof(path_buf) &&
        unlink(path_buf) == 0) dropped = true;
    ESP_LOGI("WEREAD", "forget book=%s dropped=%d", id, dropped);
    return dropped;
}

/* ------------------------------------------------------------------ */
/* 本地书架的封面：把微信读书封面落到书籍路径旁                        */
/* ------------------------------------------------------------------ */

/*
 * 逐章缓存的微读书在磁盘上只是一个标记文件，本地书架/首页"继续阅读"要按 EPUB 解封面，
 * 自然拿不到图，只能显示默认封面。这里把同一张封面按书架封面尺寸落成旁挂灰阶快照
 * （<书籍路径>.gray），book_cover 读到它就当成真封面，三个界面一次对齐。
 * A chapter-cached WeRead book is only a marker file on disk, so the local shelf and the home
 * "continue reading" strip cannot decode a cover from it and fall back to the generated one.
 * Write the same artwork beside the book as a grayscale snapshot (<path>.gray); book_cover
 * treats it as real artwork, which fixes all three surfaces at once.
 */
// 与 weread.c 相同的 SD 访问作用域写法；封面文件同样要走挂载准入。
// Same SD-access scope idiom as weread.c: cover files go through the mount admission too.
#define WR_COVER_SD_SCOPE(path) __attribute__((cleanup(read_pico_sd_access_scope_end))) \
    read_pico_sd_access_scope_t scope = read_pico_sd_access_scope(path)

static bool gray_write_atomic(const char *path, const uint8_t *pixels) {
    char temp[WR_PATH + 8];
    int n = snprintf(temp, sizeof(temp), "%s.part", path);
    if (n <= 0 || (size_t)n >= sizeof(temp)) return false;
    FILE *file = fopen(temp, "wb");
    if (!file) return false;
    bool ok = fwrite(pixels, 1, BOOK_COVER_W * BOOK_COVER_H, file) == BOOK_COVER_W * BOOK_COVER_H &&
        !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    if (ok) {
        (void)unlink(path);
        ok = rename(temp, path) == 0;
    }
    if (!ok) unlink(temp);
    return ok;
}

bool wr_ensure_cover(const wr_book_t *book, const char *book_path) {
    if (!book || !book->id[0] || !book_path || !book_path[0]) return false;
    const size_t bytes = BOOK_COVER_W * BOOK_COVER_H;
    char side[WR_PATH];
    if (snprintf(side, sizeof(side), "%s.gray", book_path) >= (int)sizeof(side)) return false;
    WR_COVER_SD_SCOPE(side);
    if (!scope.allowed) return false;
    struct stat info;
    if (!stat(side, &info) && info.st_size == (off_t)bytes) return true;
    if (!stat(side, &info)) unlink(side); // 丢掉上次中断留下的半截文件

    uint8_t *pixels = malloc(bytes);
    if (!pixels) return false;
    bool ok = false;
    // 书架同步已经准备好同一张灰阶图时直接复用，省掉一次图片请求。
    // Reuse the raster the shelf sync already produced instead of requesting the same image.
    char owner[WR_ID] = {0};
    if (wr_account(owner, NULL, NULL) && wr_safe(owner)) {
        char staged[WR_PATH];
        if (snprintf(staged, sizeof(staged), WR_DIR "/%s+%s.gray", owner, book->id) < (int)sizeof(staged) &&
            !stat(staged, &info) && info.st_size == (off_t)bytes) {
            FILE *file = fopen(staged, "rb");
            if (file) {
                ok = fread(pixels, 1, bytes, file) == bytes;
                fclose(file);
            }
        }
    }
    if (!ok && book->cover[0]) {
        char url[sizeof(book->cover) + 8];
        snprintf(url, sizeof(url), "%s%s", !strncmp(book->cover, "//", 2) ? "https:" : "", book->cover);
        size_t length = 0;
        char *image = wr_http(url, NULL, NULL, 4u * 1024u * 1024u, &length);
        if (image) {
            ok = book_cover_thumbnail((uint8_t *)image, length,
                                      length >= 8 && (uint8_t)image[0] == 0x89, pixels);
            free(image);
        }
    }
    if (ok) ok = gray_write_atomic(side, pixels);
    ESP_LOGI("WEREAD", "cover sidecar book=%s result=%d", book->id, ok);
    free(pixels);
    return ok;
}
