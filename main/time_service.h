/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/// 空闲时启动有界后台对时；主循环独占 PMU 与 NVS 的交接。
/// Start bounded background sync while idle; the main loop alone hands time to PMU/NVS.
void app_time_service_poll(bool idle);

/// 首次配网成功后请求后台在线对时；不等待网络、不启动离线连接。
/// Request an online background sync after setup connects; never wait or start an offline connection.
void app_time_service_request_online_sync(void);
/// 请求/交接未完成时允许引导缩短界面空闲等待，其余资源门禁保持。
/// Let setup shorten UI-idle admission while a request/handoff is pending; preserve resource guards.
bool app_time_service_online_sync_pending(void);

/// 距上次成功校准是否已超过 min_seconds；供"联网时顺手对时"判断要不要再校一次。
/// Whether min_seconds have passed since the last successful calibration, for the
/// "sync while already online" path.
bool app_time_service_sync_due(uint32_t min_seconds);
