/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 凭据校验和有界连接状态机；不含平台资源。/ Credential validation and bounded connection policy without platform resources.
 * 冻结：密码不进入公开状态；每轮连接最多五次、30秒。/ Frozen: passwords never enter public status; five attempts within 30 seconds.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"
#include "include/read_pico_transfer_network.h"

// 调用方在同一锁下检查并改变准入状态。/ Caller checks and changes admission under the same lock.
static bool admission_begin(bool stopping, bool *uploading) {
    if (stopping || *uploading) return false;
    *uploading = true; return true;
}

static bool admission_stop(bool *stopping, bool uploading) {
    if (uploading) return false;
    *stopping = true; return true;
}

typedef enum {
    TRANSFER_TIME_UNAVAILABLE,
    TRANSFER_TIME_TEMPORARY,
    TRANSFER_TIME_ONLINE,
} transfer_time_route_t;

// 在线 STA 对时不得重新初始化 WiFi；仅完全停服时可短暂联网。
// Never reinitialize WiFi for an online STA; connect briefly only when fully stopped.
static transfer_time_route_t time_route(bool wifi_active, bool sta_mode,
                                         bool network_ready, bool fully_stopped) {
    if (wifi_active) return sta_mode && network_ready ? TRANSFER_TIME_ONLINE : TRANSFER_TIME_UNAVAILABLE;
    return fully_stopped ? TRANSFER_TIME_TEMPORARY : TRANSFER_TIME_UNAVAILABLE;
}

// 正文须在15秒内收齐；每次阻塞接收前后检查停止。/ Accept bodies only within 15 seconds; check cancellation around each blocking receive.
static bool receive_credentials_body(char *body, size_t total,
        int (*receive)(void *, char *, size_t), bool (*stopped)(void *),
        int64_t (*now)(void *), void *ctx) {
    int64_t deadline = now(ctx) + 15000;
    size_t done = 0;
    while (done < total) {
        if (stopped(ctx) || now(ctx) >= deadline) return false;
        int got = receive(ctx, body + done, total - done);
        if (got <= 0 || (size_t)got > total - done || stopped(ctx) || now(ctx) >= deadline) return false;
        done += (size_t)got;
    }
    return true;
}

typedef struct {
    uint8_t version; ///< 格式版本 / Format version
    char ssid[33]; ///< 网络名，最多32字节 / Network name, at most 32 bytes
    char password[65]; ///< 私有凭据，用毕擦除 / Private secret, erased after use
} transfer_credentials_t;

static void clear_secret(void *ptr, size_t n) {
    volatile unsigned char *p = ptr;
    while (n--) *p++ = 0;
}

static bool credentials_valid(const transfer_credentials_t *c) {
    if (c->version != 1 || !memchr(c->ssid, 0, sizeof(c->ssid)) ||
        !memchr(c->password, 0, sizeof(c->password))) return false;
    size_t ssid_len = strlen(c->ssid), pass_len = strlen(c->password);
    if (!ssid_len || ssid_len > 32 || (pass_len && (pass_len < 8 || pass_len > 64))) return false;
    for (size_t i = 0; i < ssid_len; ++i)
        if ((unsigned char)c->ssid[i] < 32 || (unsigned char)c->ssid[i] == 127) return false;
    for (size_t i = 0; i < pass_len; ++i) {
        unsigned char p = (unsigned char)c->password[i];
        if (p < 32 || p > 126) return false;
        if (pass_len == 64 && !((p >= '0' && p <= '9') || (p >= 'a' && p <= 'f') || (p >= 'A' && p <= 'F'))) return false;
    }
    return true;
}

static bool credentials_from_text(const char *ssid, const char *password, transfer_credentials_t *out) {
    memset(out, 0, sizeof(*out));
    if (!ssid || !password || strnlen(ssid, 33) > 32 || strnlen(password, 65) > 64) return false;
    out->version = 1; strcpy(out->ssid, ssid); strcpy(out->password, password);
    if (credentials_valid(out)) return true;
    clear_secret(out, sizeof(*out)); return false;
}

static void scan_offer(read_pico_transfer_network_t out[READ_PICO_TRANSFER_SCAN_MAX], size_t *count,
                       const read_pico_transfer_network_t *candidate) {
    if (!candidate->ssid[0]) return;
    size_t index = 0;
    while (index < *count && strcmp(out[index].ssid, candidate->ssid)) ++index;
    if (index < *count) {
        if (candidate->rssi <= out[index].rssi) return;
    } else if (*count < READ_PICO_TRANSFER_SCAN_MAX) {
        ++*count;
    } else {
        index = *count - 1;
        if (candidate->rssi <= out[index].rssi) return;
    }
    out[index] = *candidate;
    while (index && out[index].rssi > out[index - 1].rssi) {
        read_pico_transfer_network_t swap = out[index - 1];
        out[index - 1] = out[index]; out[index] = swap; --index;
    }
}

static bool parse_credentials(const char *body, size_t length, transfer_credentials_t *out) {
    memset(out, 0, sizeof(*out));
    if (!length || length > 256 || memchr(body, 0, length) || body[length] != 0) return false;
    // cJSON使用C字符串；拒绝嵌入零以免字符串静默截断。/ cJSON uses C strings; reject embedded zero escapes to prevent truncation.
    if (strstr(body, "\\u0000")) return false;
    // 配置只有平面字符串字段；在递归解析前限制容器深度，保护HTTP任务栈。
    // Configuration has flat string fields; bound container depth before recursive parsing to protect the HTTP stack.
    bool quoted = false, escaped = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        char ch = body[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 1) return false; }
        else if (ch == '}' || ch == ']') { if (!depth) return false; --depth; }
    }
    cJSON *json = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
    if (!json) return false;
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 2 && cJSON_IsString(ssid) && cJSON_IsString(password);
    if (valid) valid = strlen(ssid->valuestring) <= 32 && strlen(password->valuestring) <= 64;
    if (valid) {
        out->version = 1;
        strcpy(out->ssid, ssid->valuestring); strcpy(out->password, password->valuestring);
        valid = credentials_valid(out);
    }
    // 解析树退出前擦除密码，包括验证失败的重复字段。/ Erase parsed passwords, including duplicate fields on invalid requests.
    for (cJSON *item = json->child; item; item = item->next)
        if (item->valuestring) clear_secret(item->valuestring, strlen(item->valuestring));
    cJSON_Delete(json);
    if (!valid) clear_secret(out, sizeof(*out));
    return valid;
}

typedef struct {
    bool online, pending, failed; ///< 在线、待连接与"本轮已放弃" / Online, pending, round given up
    unsigned attempts; ///< 本轮尝试次数 / Attempts in this round
    unsigned rounds;   ///< 已放弃的轮数，决定退避档位 / Rounds given up so far, picks the backoff step
    int64_t deadline_ms, next_ms; ///< 总截止与下次尝试时刻 / Overall deadline and next attempt time
    int64_t retry_ms;  ///< 下一轮重连的起点 / When the next round may begin
} transfer_connection_t;

static void connection_begin(transfer_connection_t *c, int64_t now) {
    // 退避档位跨轮保留：每轮都从零开始就等于永不退避。 / Keep the backoff step across rounds.
    const unsigned rounds = c->rounds;
    *c = (transfer_connection_t){.pending = true, .deadline_ms = now + 30000, .next_ms = now,
                                 .rounds = rounds};
}

/* 用户重新开启 Wi-Fi 这类"从头再来"用这个：退避档位与失败态一并清掉。 */
/* For a deliberate restart such as re-enabling Wi-Fi: clear the failure latch and the backoff. */
static void connection_reset(transfer_connection_t *c, int64_t now) {
    *c = (transfer_connection_t){.pending = true, .deadline_ms = now + 30000, .next_ms = now};
}

static void connection_lost(transfer_connection_t *c, int64_t now) {
    if (c->failed) return;
    if (c->online) connection_begin(c, now);
    c->online = false; c->pending = true; c->next_ms = now + 1000;
}

/*
 * 0 等待、1 连接、-1 放弃本轮。/ 0 waits, 1 connects, -1 gives up this round.
 *
 * 放弃不是终态：窗口内试满 10 次就按 30s/60s/120s/300s 退避，到期自动重开一轮。
 * 以前这里把 failed 置成永久终态，路由抖一下设备就一直离线，直到用户进设置页手动重开 Wi-Fi。
 * Giving up is not terminal: after ten tries inside the window the next round waits
 * 30/60/120/300 s and then restarts on its own. This used to latch `failed` forever, so a single
 * router hiccup left the device offline until the user reopened Wi-Fi settings by hand.
 *
 * 次数从 5 提到 10 是实机逼出来的：AP 换信道后第一次关联常回 reason=201（NO_AP_FOUND），
 * 连试五次都在十几秒内打完，然后退避 30 秒才重来——读者要等四十多秒才看到"已连接"。
 * 让它在同一个 30 秒窗口里一直试，通常第二次或第三次就上去了。
 * Ten instead of five came from a real session: after the AP changed channel the first
 * association returned reason=201 (NO_AP_FOUND), five tries burned out in fourteen seconds, and
 * the 30 s backoff meant forty-odd seconds before the reader saw a link. Trying for the whole
 * window normally lands on the second or third attempt.
 */
static int connection_poll(transfer_connection_t *c, int64_t now) {
    if (c->online) return 0;
    if (c->failed) {
        if (now < c->retry_ms) return 0;
        c->failed = false; c->pending = true; c->attempts = 0;
        c->deadline_ms = now + 30000; c->next_ms = now;
    }
    if (now >= c->deadline_ms || (c->pending && c->attempts >= 10)) {
        static const int64_t steps[] = {30000, 60000, 120000, 300000};
        c->failed = true; c->pending = false;
        c->retry_ms = now + steps[c->rounds < 4 ? c->rounds : 3];
        if (c->rounds < 8) ++c->rounds;
        return -1;
    }
    if (!c->pending || now < c->next_ms) return 0;
    c->pending = false; ++c->attempts; return 1;
}
