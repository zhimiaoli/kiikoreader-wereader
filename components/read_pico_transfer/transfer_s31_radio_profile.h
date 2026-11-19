/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：按Metalio公开网络文档统一S31运行期策略；不存取网络凭据。
 * English: Apply the documented Metalio S31 runtime profile without touching credentials.
 * Reference: CloudZao/Metalio-E-INK4-Plus@ad8edf4 docs/ap-file-transfer.md (MIT).
 */
#pragma once
#include <stdbool.h>
#include "esp_wifi.h"
/*
 * 单位 0.25 dBm。官方文档给的 48（12 dBm）在近距离传书够用，但换成公网取章包就露馅了：
 * 实测下行每 16 秒停一次（上行 ACK 到不了路由器，下行只能等重传），3 MB 的章包 120 秒只
 * 挪动 573 KB。72（18 dBm）仍在国标 20 dBm EIRP 之内，把上行拉回来是提升吞吐最直接的一刀。
 * Unit is 0.25 dBm. The documented 48 (12 dBm) is fine for a short local transfer, but fetching
 * chapter bundles over the internet exposed it: the downlink stalls every 16 s because uplink
 * ACKs never reach the router and TCP can only retransmit — a 3 MB bundle moved 573 KB in 120 s.
 * 72 (18 dBm) stays inside the 20 dBm EIRP limit and is the most direct fix for that stall.
 */
#define TRANSFER_S31_MAX_TX_POWER 72
// Short STA sessions (network scan, time sync and low-RAM connection starts) do
// not need an AP receive aggregation window. Keep their DMA-backed RX footprint
// small enough for the S31's 64 KiB internal allocation reserve.
static inline void transfer_s31_constrained_sta_config(wifi_init_config_t *init) {
    if (!init) return;
    if (init->static_rx_buf_num > 3) init->static_rx_buf_num = 3;
    if (init->dynamic_rx_buf_num > 16) init->dynamic_rx_buf_num = 16;
    init->ampdu_rx_enable = false;
}

/*
 * 内部 RAM 不够时逐级瘦身。实测这道坎非常窄：内部堆 68,599 B 时 3 个静态 RX 缓冲刚好能起来，
 * 67,063 B（只少 1.5 KB）就只拿到 2 个，而 esp_wifi_init 失败后的 deinit 并不彻底，用同一份
 * 配置重试只会更差（第三次连一个都拿不到），最后整个联网都起不来——扫描、获取目录全挂。
 * 所以按档下调：每档少要一块内存，才有机会真正起来。
 * Step the pool down when internal RAM is short. The cliff is narrow: 3 static RX buffers fit at
 * 68,599 B free and miss by one at 67,063 B (1.5 KB less). A failed esp_wifi_init does not fully
 * deinit, so retrying the identical config only does worse (the third attempt could not get any),
 * and then nothing network-related starts at all — scans and TOC fetches alike.
 * level 0: 只降静态 RX 与管理短缓冲；level 1: 再降动态 RX 与静态 TX（实测 63 KB 能过的档）；
 * level 2: 收到最小可用。
 * level 0 shrinks static RX and the management pool only; level 1 also trims dynamic RX and
 * static TX (measured to pass at 63 KB); level 2 is the minimum the driver accepts.
 */
static inline void transfer_s31_slim_sta_config(wifi_init_config_t *init, int level) {
    if (!init) return;
    if (init->static_rx_buf_num > 2) init->static_rx_buf_num = 2;
    /*
     * 管理短缓冲是另一道坎，而且只在"电平 0"里表现出来：实测 static RX 已经降到 2 块，仍
     * 卡在 `esf_buf_setup_static: alloc eb fail(1)`（默认 16 块），降到 8 块就过。所以第一档
     * 就要一起降——只降静态 RX 时第一次尝试必然白跑，还要再等 300 ms 重来。
     * The management short-buffer pool is the other step, and it only showed up at level 0: with
     * static RX already down to two, init still failed in `esf_buf_setup_static: alloc eb fail(1)`
     * at the default 16 and passed at 8. So the first level has to shrink it too; otherwise the
     * first attempt always wastes a round and another 300 ms.
     */
    if (init->mgmt_sbuf_num > 8) init->mgmt_sbuf_num = 8;
    if (init->cache_tx_buf_num > 2) init->cache_tx_buf_num = 2;
    if (level >= 1) {
        if (init->dynamic_rx_buf_num > 8) init->dynamic_rx_buf_num = 8;
        if (init->static_tx_buf_num > 4) init->static_tx_buf_num = 4;
    }
    if (level == 0 && init->dynamic_rx_buf_num > 12) init->dynamic_rx_buf_num = 12;
    if (level >= 2) {
        if (init->dynamic_rx_buf_num > 4) init->dynamic_rx_buf_num = 4;
        if (init->static_tx_buf_num > 3) init->static_tx_buf_num = 3;
        // 管理短缓冲的下限是 6，再少驱动会拒绝初始化。/ The driver refuses fewer than 6.
        if (init->mgmt_sbuf_num > 6) init->mgmt_sbuf_num = 6;
    }
    init->ampdu_rx_enable = false;
}

static inline esp_err_t transfer_s31_radio_profile(wifi_interface_t interface) {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) err = esp_wifi_set_protocol(interface,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (err == ESP_OK) err = esp_wifi_set_max_tx_power(TRANSFER_S31_MAX_TX_POWER);
    return err;
}

// SDK: AMPDU接收池应覆盖BA窗口；只给独占传书补齐，最大16槽，日常STA/BLE不增占用。
// SDK: cover the BA window with static RX buffers; bulk-only, capped at16, retaining normal STA/BLE memory.
static inline void transfer_s31_bulk_receive_config(wifi_init_config_t *init, bool bulk) {
    if (!init || !bulk || !init->ampdu_rx_enable || init->rx_ba_win <= 0 ||
        init->rx_ba_win > 16 ||
        (init->dynamic_rx_buf_num && init->dynamic_rx_buf_num < init->rx_ba_win)) return;
    int window=init->rx_ba_win;
#ifdef CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    // 独占传书统一到SDK建议的16槽；原静态池已经够大也不能跳过BA设置。
    // Bulk PSRAM reception uses the SDK's16-slot recommendation even when the static pool was already large.
    if (!init->dynamic_rx_buf_num || init->dynamic_rx_buf_num >= 16)window=16;
#endif
    init->rx_ba_win=window;
    if(init->static_rx_buf_num<window)init->static_rx_buf_num=window;
}
