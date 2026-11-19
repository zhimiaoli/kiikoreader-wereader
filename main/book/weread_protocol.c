/* SPDX-License-Identifier: MIT
 * Protocol details adapted from CrossMux WeReadWebApi (commit efee672).
 * The original notice is in licenses/LICENSE-CrossMux.txt.
 */
#include "weread_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_md5.h"
#include "psa/crypto.h"

static void hex_bytes(const uint8_t *data, unsigned count, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < count; ++i) {
        out[2*i] = hex[data[i] >> 4];
        out[2*i+1] = hex[data[i] & 15];
    }
    out[2*count] = 0;
}

static void md5(const void *data, size_t length, char out[33]) {
    md5_context_t context;
    uint8_t digest[16];
    esp_rom_md5_init(&context);
    esp_rom_md5_update(&context, data, length);
    esp_rom_md5_final(digest, &context);
    hex_bytes(digest, sizeof(digest), out);
}

static bool encode_id(const char *id, char out[128]) {
    if (!wr_safe(id) || strlen(id) >= WR_ID) return false;
    char hash[33];
    md5(id, strlen(id), hash);
    bool numeric = strspn(id, "0123456789") == strlen(id);
    int used = snprintf(out, 128, "%.3s%c2%.2s", hash, numeric ? '3' : '4', hash+30);
    if (numeric) {
        for (size_t i = 0, n = strlen(id); i < n; i += 9) {
            unsigned number = 0;
            for (size_t j = i; j < n && j < i+9; ++j) number = number*10 + (unsigned)(id[j]-'0');
            char chunk[12];
            int length = snprintf(chunk, sizeof(chunk), "%x", number);
            used += snprintf(out+used, 128-used, "%s%02x%s", i ? "g" : "", length, chunk);
        }
    } else {
        used += snprintf(out+used, 128-used, "%02x", (unsigned)strlen(id)*2);
        hex_bytes((const uint8_t *)id, strlen(id), out+used);
        used += strlen(id)*2;
    }
    if (used < 20) {
        memcpy(out+used, hash, 20-used);
        used = 20;
    }
    md5(out, (size_t)used, hash);
    memcpy(out+used, hash, 3);
    out[used+3] = 0;
    return true;
}

static void sign_query(const char *query, char signature[12]) {
    uint32_t a = 0x15051505, b = a;
    size_t n = strlen(query);
    for (size_t i = n; i > 1; i -= 2) {
        a = (a ^ ((uint32_t)(uint8_t)query[i-1] << ((n-i+1)%30))) & 0x7fffffff;
        b = (b ^ ((uint32_t)(uint8_t)query[i-2] << ((i-1)%30))) & 0x7fffffff;
    }
    snprintf(signature, 12, "%lx", (unsigned long)(a+b));
}

static bool field(const char *html, const char *key, char *out, size_t cap) {
    char match[24];
    snprintf(match, sizeof(match), "\"%s\"", key);
    const char *p = html;
    while ((p = strstr(p, match))) {
        p += strlen(match);
        while (isspace((unsigned char)*p)) ++p;
        if (*p != ':') continue;
        do { ++p; } while (isspace((unsigned char)*p));
        if (*p != '"') continue;
        const char *end = strchr(++p, '"');
        if (end && end > p && (size_t)(end-p) < cap) {
            memcpy(out, p, end-p);
            out[end-p] = 0;
            if (wr_safe(out)) return true;
        }
    }
    out[0] = 0;
    return false;
}

typedef struct {
    wr_reader_t *reader;
    /* 尾部 512 字节用于跨块的键名接续，正文区按一次读满一整块来开。 */
    /* The trailing 512 bytes carry a key split across two reads; the body part fits one full
     * read unit. */
    char window[512 + WR_FIELD_CHUNK + 1];
    size_t tail;
} reader_scan_t;

static bool reader_fields_chunk(const char *data, size_t length, void *context) {
    reader_scan_t *scan = context;
    wr_reader_t *reader = scan->reader;
    // 契约外的块长直接拒绝：宁可少一次解析，也不能把调用方的缓冲写穿。
    // Reject out-of-contract lengths: a missed parse is better than writing past the buffer.
    if (length > WR_FIELD_CHUNK) return false;
    memcpy(scan->window + scan->tail, data, length);
    size_t size = scan->tail + length;
    scan->window[size] = 0;
    if (!reader->ps[0]) field(scan->window, "psvts", reader->ps, sizeof(reader->ps));
    if (!reader->pc[0]) field(scan->window, "pclts", reader->pc, sizeof(reader->pc));
    if (!reader->token[0]) field(scan->window, "token", reader->token, sizeof(reader->token));
    // 中文：进度上报必须使用 psvts；pclts 与 token 可由 send_position 补齐，无需读完整页。
    // English: Progress upload requires psvts; send_position supplies pclts and token fallbacks,
    // so stop before draining the full reader HTML.
    if (reader->ps[0]) return true;
    scan->tail = size < 512 ? size : 512;
    memmove(scan->window, scan->window + size - scan->tail, scan->tail);
    return false;
}

bool wr_reader_referer_for(const char *book, const char *chapter, char *out, size_t cap) {
    char b[128], c[128];
    if (!out || !cap || !encode_id(book, b) || !encode_id(chapter, c)) return false;
    int n = snprintf(out, cap, WR_HOST "/web/reader/%sk%s", b, c);
    return n > 0 && (size_t)n < cap;
}

bool wr_reader(const char *book, const char *chapter, wr_reader_t *reader) {
    char b[128], c[128];
    if (!encode_id(book, b) || !encode_id(chapter, c)) return false;
    memset(reader, 0, sizeof(*reader));
    snprintf(reader->referer, sizeof(reader->referer), WR_HOST "/web/reader/%sk%s", b, c);
    reader_scan_t *scan = heap_caps_calloc(1, sizeof(*scan), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scan) return false;
    scan->reader = reader;
    bool ok = wr_http_reader_fields(reader->referer + sizeof(WR_HOST)-1, 3*1024*1024,
                                    reader_fields_chunk, scan) && reader->ps[0];
    free(scan);
    return ok;
}

static bool content_body(const char *book, const char *chapter, const wr_reader_t *reader, char out[1000]) {
    char b[128], c[128], pc[128], epoch[20], signature[12];
    unsigned now = (unsigned)time(NULL);
    if (now < 1704067200 || !encode_id(book, b) || !encode_id(chapter, c)) return false;
    snprintf(epoch, sizeof(epoch), "%u", now);
    if (!encode_id(epoch, pc)) return false;
    if (!strcmp(pc, reader->ps)) {
        snprintf(epoch, sizeof(epoch), "%u", ++now);
        if (!encode_id(epoch, pc)) return false;
    }
    unsigned r = esp_random() % 10000;
    r *= r;
    snprintf(out, 1000, "b=%s&c=%s&ct=%u&pc=%s&prevChapter=false&ps=%s&r=%u&sc=1&st=0", b, c, now, pc, reader->ps, r);
    sign_query(out, signature);
    snprintf(out, 1000, "{\"b\":\"%s\",\"c\":\"%s\",\"r\":%u,\"ct\":%u,\"ps\":\"%s\",\"pc\":\"%s\","
             "\"sc\":1,\"prevChapter\":\"false\",\"st\":0,\"s\":\"%s\"}", b, c, r, now, reader->ps, pc, signature);
    return true;
}

static bool shard_valid(const char *body, size_t length) {
    if (!body || length <= 33) return false;
    char hash[33];
    md5(body+32, length-32, hash);
    return !strncasecmp(hash, body, 32);
}

static bool restore_order(char *data, size_t size) {
    unsigned positions[10] = {0, 2};
    unsigned count = 2;
    if (size >= 11) {
        size_t tail = (size+9)/10;
        if (tail > 4) tail = 4;
        char decimal[40];
        unsigned used = 0;
        for (size_t i = 0; i < tail; ++i) {
            unsigned value = (uint8_t)data[size-1-i], spread = 0;
            for (unsigned bit = 0; bit < 8; ++bit) spread |= ((value >> bit) & 1u) << (bit*2);
            used += snprintf(decimal+used, sizeof(decimal)-used, "%u", spread);
        }
        unsigned modulus = (unsigned)(size-tail-2), step = 1;
        for (unsigned i = modulus; i >= 10; i /= 10) ++step;
        count = 0;
        for (unsigned i = 0; count < 10 && i+step < used; i += step) {
            unsigned a = 0, b = 0;
            for (unsigned j = 0; j < step; ++j) {
                a = a*10 + (unsigned)(decimal[i+j]-'0');
                b = b*10 + (unsigned)(decimal[i+j+1]-'0');
            }
            positions[count++] = a % modulus;
            positions[count++] = b % modulus;
        }
    }
    if (size < 4 || !count) return false;
    while (count >= 2) {
        unsigned right = positions[--count], left = positions[--count];
        for (int delta = 1; delta >= 0; --delta) {
            char swap = data[left+delta];
            data[left+delta] = data[right+delta];
            data[right+delta] = swap;
        }
    }
    return true;
}

static size_t decode_base64(char *data, size_t length) {
    unsigned bits = 0, accumulator = 0;
    size_t written = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned c = (uint8_t)data[i], value;
        if (c >= 'A' && c <= 'Z') value = c-'A';
        else if (c >= 'a' && c <= 'z') value = c-'a'+26;
        else if (c >= '0' && c <= '9') value = c-'0'+52;
        else if (c == '-' || c == '+') value = 62;
        else if (c == '_' || c == '/') value = 63;
        else continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) { bits -= 8; data[written++] = (char)(accumulator >> bits); }
    }
    data[written] = 0;
    return bits == 6 ? 0 : written;
}

char *wr_chapter_content(const char *book, const char *chapter, const wr_reader_t *reader, bool *plain, size_t *length) {
    char json[1000];
    if (!content_body(book, chapter, reader, json)) return NULL;
    static const char *const epub[] = {"/web/book/chapter/e_0", "/web/book/chapter/e_1", "/web/book/chapter/e_3"};
    static const char *const text[] = {"/web/book/chapter/t_0", "/web/book/chapter/t_1"};
    const size_t limit = 6u*1024*1024;
    size_t n;
    char *data = wr_http(epub[0], json, reader->referer, limit, &n);
    if (!data) return NULL;
    *plain = *data == '{' && strstr(data, "\"bookId\"");
    if (n >= 4 && !memcmp(data, "PK\003\004", 4)) { *length = n; return data; }
    const char *const *endpoints = *plain ? text : epub;
    unsigned shards = *plain ? 2 : 3;
    if (*plain) { free(data); data = wr_http(text[0], json, reader->referer, limit, &n); }
    if (!shard_valid(data, n)) { free(data); return NULL; }
    size_t used = n-33;
    memmove(data, data+33, used);
    for (unsigned i = 1; i < shards; ++i) {
        char *shard = wr_http(endpoints[i], json, reader->referer, limit, &n);
        bool valid = shard_valid(shard, n) && n-32 <= limit-used;
        char *grown = valid ? heap_caps_realloc(data, used+n-31, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) : NULL;
        if (!grown) { free(shard); free(data); return NULL; }
        data = grown;
        memcpy(data+used, shard+32, n-32);
        used += n-32;
        free(shard);
    }
    if (!restore_order(data, used) || !(*length = decode_base64(data, used))) { free(data); return NULL; }
    return data;
}

static bool unsigned_field(const cJSON *object, const char *key, unsigned *value) {
    char text[20];
    if (!wr_value(cJSON_GetObjectItemCaseSensitive(object, key), text, sizeof(text))) return false;
    char *end;
    unsigned long long n = strtoull(text, &end, 10);
    if (!*text || *end || n > UINT32_MAX) return false;
    *value = (unsigned)n;
    return true;
}

bool wr_remote_position_with_chapters(const char *id, wr_chapter_t *chapter, unsigned *offset, unsigned *pct,
                                      const wr_chapter_t *known_chapters, unsigned known_count, bool *absent) {
    char path[120];
    if (absent) *absent = false;
    if (!wr_safe(id)) return false;
    snprintf(path, sizeof(path), "/web/book/getProgress?bookId=%s", id);
    cJSON *response = wr_json(path, NULL);
    const cJSON *root = cJSON_GetObjectItemCaseSensitive(response, "data");
    if (!cJSON_IsObject(root)) root = response;
    const cJSON *book = cJSON_GetObjectItemCaseSensitive(root, "book");
    if (!cJSON_IsObject(book)) book = root;
    memset(chapter, 0, sizeof(*chapter));
    bool ok = wr_value(cJSON_GetObjectItemCaseSensitive(book, "chapterUid"), chapter->id, sizeof(chapter->id)) &&
        wr_safe(chapter->id) && unsigned_field(book, "chapterOffset", offset) &&
        unsigned_field(book, "progress", pct) && *pct <= 100;
    if (ok && !unsigned_field(book, "chapterIdx", &chapter->index)) {
        bool found = false;
        for (unsigned i = 0; known_chapters && i < known_count; ++i) {
            if (!strcmp(known_chapters[i].id, chapter->id)) {
                chapter->index = known_chapters[i].index;
                found = true;
                break;
            }
        }
        if (known_chapters) ok = found;
        else {
            char query[100];
            snprintf(query, sizeof(query), "{\"bookIds\":[\"%s\"]}", id);
            cJSON *toc = wr_json("/web/book/chapterInfos", query);
            const cJSON *updated = cJSON_GetObjectItemCaseSensitive(toc, "updated");
            const cJSON *item = cJSON_GetArrayItem(updated, 0);
            const cJSON *chapters = cJSON_GetObjectItemCaseSensitive(item, "chapterInfos");
            const cJSON *entry;
            ok = false;
            cJSON_ArrayForEach(entry, chapters) {
                char uid[WR_ID];
                if (wr_value(cJSON_GetObjectItemCaseSensitive(entry, "chapterUid"), uid, sizeof(uid)) && !strcmp(uid, chapter->id)) {
                    ok = unsigned_field(entry, "chapterIdx", &chapter->index);
                    break;
                }
            }
            cJSON_Delete(toc);
        }
    }
    if (!ok && response) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(response, "errCode");
        if (!err) err = cJSON_GetObjectItemCaseSensitive(response, "errcode");
        ESP_LOGW("WEREAD", "cloud progress fields unavailable root_type=%d data_type=%d book_type=%d err_type=%d",
                 response->type, root ? root->type : 0, book ? book->type : 0, err ? err->type : 0);
        // 请求拿到了应答、只是里面没有进度字段：这是"云端还没有读这本书"，不是网络故障。
        // A parsed response without the fields means the book was never read, not a failure.
        if (absent) *absent = true;
        if (!wr_error_message() || !strcmp(wr_error_message(), "操作失败，请检查网络后重试"))
            wr_error_set("微信读书云端尚无可用阅读进度或进度数据格式已变化");
    }
    cJSON_Delete(response);
    return ok;
}

bool wr_remote_position(const char *id, wr_chapter_t *chapter, unsigned *offset, unsigned *pct) {
    return wr_remote_position_with_chapters(id, chapter, offset, pct, NULL, 0, NULL);
}

static void web_app_id(char id[40]) {
    char lengths[13] = {0};
    unsigned count = 0, hash = 0;
    for (const char *p = WR_AGENT; *p; ++p) hash = (hash*131u + (uint8_t)*p) & 0x7fffffff;
    const char *p = WR_AGENT;
    while (*p && count < 12) {
        size_t n = strcspn(p, " ");
        lengths[count++] = (char)('0'+n%10);
        p += n;
        if (*p) ++p;
    }
    snprintf(id, 40, "wb%sh%u", lengths, hash);
}

static bool send_position(const char *id, const wr_chapter_t *chapter, unsigned offset, unsigned pct,
                          unsigned seconds, wr_reader_t reader, bool *post_started) {
    char b[128], c[128], app[40], date[20], sig[12], sg[65], text[190], body[1400];
    unsigned now = (unsigned)time(NULL), rn = esp_random() % 1000;
    unsigned long long ts = (unsigned long long)now*1000 + esp_random()%1000;
    if (now < 1704067200 || !encode_id(id, b) || !encode_id(chapter->id, c)) return false;
    if (!reader.pc[0]) {
        snprintf(date, sizeof(date), "%u", now);
        if (!encode_id(date, reader.pc)) return false;
    }
    web_app_id(app);
    snprintf(text, sizeof(text), "%llu%u%s", ts, rn, reader.token[0] ? reader.token : "3c5c8717f3daf09iop3423zafeqoi");
    uint8_t digest[32];
    size_t written;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)text, strlen(text),
                                                             digest, sizeof(digest), &written) != PSA_SUCCESS || written != sizeof(digest)) return false;
    hex_bytes(digest, sizeof(digest), sg);
    char rt_query[32] = "", rt_body[32] = "";
    if (seconds) {
        snprintf(rt_query, sizeof(rt_query), "&rt=%u", seconds);
        snprintf(rt_body, sizeof(rt_body), ",\"rt\":%u", seconds);
    }
    if (!seconds) {
        snprintf(body, sizeof(body), "appId=%s&b=%s&c=%s&ci=%u&co=%u&ct=%u&pc=%s&pr=%u&ps=%s&sm=",
                 app, b, c, chapter->index, offset, now, reader.pc, pct, reader.ps);
        sign_query(body, sig);
        snprintf(body, sizeof(body), "{\"appId\":\"%s\",\"b\":\"%s\",\"c\":\"%s\",\"ci\":%u,\"co\":%u,\"ct\":%u,"
                 "\"pc\":\"%s\",\"pr\":%u,\"ps\":\"%s\",\"sm\":\"\",\"s\":\"%s\"}",
                 app, b, c, chapter->index, offset, now, reader.pc, pct, reader.ps, sig);
        free(wr_http("/web/book/read", body, reader.referer, 16384, NULL));
    }
    snprintf(body, sizeof(body), "appId=%s&b=%s&c=%s&ci=%u&co=%u&ct=%u&pc=%s&pr=%u&ps=%s&rn=%u%s&sg=%s&sm=&ts=%llu",
             app, b, c, chapter->index, offset, now, reader.pc, pct, reader.ps, rn, rt_query, sg, ts);
    sign_query(body, sig);
    snprintf(body, sizeof(body), "{\"appId\":\"%s\",\"b\":\"%s\",\"c\":\"%s\",\"ci\":%u,\"co\":%u,\"ct\":%u,"
             "\"pc\":\"%s\",\"pr\":%u,\"ps\":\"%s\",\"rn\":%u%s,\"sg\":\"%s\",\"sm\":\"\",\"ts\":%llu,\"s\":\"%s\"}",
             app, b, c, chapter->index, offset, now, reader.pc, pct, reader.ps, rn, rt_body, sg, ts, sig);
    char *data = post_started ? wr_http_report(body, reader.referer, post_started)
                              : wr_http("/web/book/read", body, reader.referer, 16384, NULL);
    cJSON *json = data ? cJSON_ParseWithOpts(data, NULL, true) : NULL;
    free(data);
    const cJSON *succ = cJSON_GetObjectItemCaseSensitive(json, "succ");
    bool ok = cJSON_IsTrue(succ) || (cJSON_IsNumber(succ) && succ->valueint == 1);
    cJSON_Delete(json);
    if (!seconds) {
        wr_chapter_t observed;
        unsigned co, pr;
        return wr_remote_position(id, &observed, &co, &pr) && !strcmp(observed.id, chapter->id) && co == offset;
    }
    return ok;
}

bool wr_send_position(const char *id, const wr_chapter_t *chapter, unsigned offset, unsigned pct) {
    wr_reader_t reader;
    if (!wr_reader_fields_cached(id, chapter->id, &reader) && !wr_reader(id, chapter->id, &reader)) return false;
    return send_position(id, chapter, offset, pct, 0, reader, NULL);
}

bool wr_send_duration(const char *id, const wr_chapter_t *chapter, unsigned offset, unsigned pct,
                      unsigned seconds, const wr_reader_t *reader, bool *post_started) {
    return send_position(id, chapter, offset, pct, seconds, *reader, post_started);
}
