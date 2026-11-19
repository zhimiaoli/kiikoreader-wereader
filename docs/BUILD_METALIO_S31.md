<!-- SPDX-FileCopyrightText: 2026 YIYO-Designer -->
<!-- SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 -->

# Metalio S31 构建指南

## 环境

- ESP-IDF `v6.1`（使用其他版本可能导致依赖锁文件或芯片驱动不兼容）。
- Python 和 CMake/Ninja 由 ESP-IDF 环境提供。
- 本机 Windows 开发优先使用 ESP-IDF PowerShell；Linux/macOS 可 source ESP-IDF 的 `export.sh`。

### 必须用 v6.1 正式版，不要用 dev 快照

2026-11-17 实测：本机 `C:/Users/36604/esp/esp-idf` 当时停在 `v6.1-dev-7333-g7b9cc1ac79`（开发快照）。
用该快照编出的固件在同一台设备上**全局卡顿**，而 GitHub Release v2.0（用 `v6.1` 正式版构建）流畅：

| 指标（同一设备、同一张卡） | `v6.1-dev-7333` 构建 | Release v2.0（`v6.1`） |
| --- | --- | --- |
| 启动 55 s 内 Task WDT 次数 | 9（180 s 内 28） | **0** |
| 单次首页刷新 | 主任务长时间不让出 CPU | `draw=694ms total=738ms` |

现象是 `main` 卡在界面绘制（`ui_kit.c` 图标、`ui_home_clock.c` 时钟背景、`app_device_settings.c` 拖动缓存），
既不是应用逻辑也不是实验开关（关掉 `--s31-pie/--s31-text-ripple/--s31-reader-panel37` 无差异）。
判别方法：看启动日志 `app_init: ESP-IDF:` 那一行——正式版打印 `v6.1`，快照会带 `-dev-<n>-g<hash>`。

切换方式：

```bash
cd "$IDF_PATH" && git fetch --tags && git checkout v6.1
# 然后按 IDF 提示重装工具链（python tools/idf_tools.py install）
```

切版本后需要重新全量构建；`build/boards/<board>/sdkconfig` 若来自旧快照，应先删除让其按板级默认重建。

## 构建

在仓库根目录、ESP-IDF 环境已启用时执行：

```bash
python tools/build_firmware.py --board metalio47 --idf-metalio47 "$IDF_PATH" --python "$(command -v python)" --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

Windows PowerShell 示例（请把 IDF 路径替换为本机 v6.1 安装目录）：

```powershell
python tools/build_firmware.py --board metalio47 --idf-metalio47 $env:IDF_PATH --python (Get-Command python).Source --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

构建脚本负责选取板级源文件、调用 ESP-IDF 并验证输出包。实际产物路径以脚本末尾报告为准；临时中间文件保存在 `build/`。不要将本地构建目录或烧写包提交到源码仓库。

## 生成 16 MiB 全闪存包

全闪存包把 bootloader、分区表、OTAdata 初始值和应用拼成一颗完整的 16 MiB 镜像，只能从地址 `0x0` 写入，写入后设备内数据全部清除。构建完成后再执行：

```bash
python tools/make_full_flash.py --board metalio47 \
  --package build/boards/packages/metalio47 \
  --output build/boards/packages/kiikoread-metalio47-<版本>-full-flash-16MB.bin
```

| 内容 | 地址 |
| --- | ---: |
| Bootloader | `0x2000` |
| 分区表 | `0x8000` |
| OTA 初始化数据 | `0xD000` |
| 应用 | `0x10000` |
| 其余地址 | `0xFF` 填充（nvs / phy_init / ota_1 / storage / coredump 均为空） |

工具会校验镜像头的芯片 ID、应用扩展头标记与分区表魔数，并检查每段是否越界，最后输出长度与 SHA-256。地址取自 `boards/<板型>/partitions.csv` 与 `board.json`，改动分区后不会静默错位。刷写命令与注意事项见 `docs/FULL_FLASH_16MB.md`。

## Windows 构建的四个已知坑

非 MSYS2 的 Windows 环境（Git Bash / 常见 ESP-IDF 安装）直接执行上面的命令会遇到以下问题，仓库已修复前两项，后两项需要在本机命令中配合：

1. **板级覆盖源目录的头文件找不到**（已修复）。`cmake/board.cmake` 里 `get_filename_component(... ABSOLUTE ...)` 在 Windows 上返回反斜杠路径；源文件级 `INCLUDE_DIRECTORIES` 会被 CMake 再以原生反斜杠写进 ninja 响应文件，gcc 解析 `@rsp` 时把 `\U` `\b` `\s` 当转义符吃掉，`-I` 指向不存在的目录，表现为 `fatal error: lut.h / lcd_driver.h / render_context.h: No such file or directory`。现在改用 `COMPILE_OPTIONS` 传递正斜杠 `-I`。
2. **`lcd_ll_select_clk_src` / `lcd_ll_set_group_clock_coeff` 参数类型不匹配**（已修复）。ESP-IDF 6.x 起这两个 LL 函数的首个参数从 HAL 上下文改为组号，板级 `lcd_driver.c` 已改为传 `0`。
3. **`MSYSTEM` 存在时 `idf.py` 只告警不执行**。esp-idf 的 `idf.py` 检测到 `MSYSTEM` 会打印 "MSys/Mingw is no longer supported" 后直接退出（退出码 0，看起来像成功）。若你的 shell 里存在该变量，需要先清除再调用构建脚本。
4. **默认满并发会偶发卡死**。Windows 上按 CPU 数（如 20）并发编译时，偶发 `cc1` 子进程永久阻塞在读取上、CPU 占用为 0，`ninja` 会一直等待。用 `--jobs` 或 `KIIKO_BUILD_JOBS` 限制并发（4~8 即可稳定）；`cc1` 卡住后杀掉重跑会从缺口继续。

另外这台机器上需要匹配 IDF 版本的 CMake 与 GCC：CMake 3.30 生成的响应文件同样有反斜杠问题（用 4.0.3），GCC 必须是 IDF 要求的版本（本机为 `esp-16.1.0_20260609`），而 `.espressif/tools/riscv32-esp-elf/` 下多版本并存时 PATH 顺序会决定选中哪个，构建前应显式前置正确版本。

本机已验证的一键入口（封装了上述全部处理）：

```bash
bash .workbuddy/build_s31.sh        # 等价于 --jobs 4 + 正确 PATH + 清 MSYSTEM
```

## CI

`.github/workflows/build.yml` 包含板级构建和主机侧回归检查。推送到 GitHub 后可在仓库 **Actions** 页面查看结果。发布固件前仍需对目标设备型号、镜像分区布局和校验值做独立确认。

## 设备烧写

本项目文档以源码构建为主。写入设备会覆盖固件分区并影响设备运行状态；务必确认板型、分区方案和镜像来源。不要把应用 OTA 镜像当作全量基础包，也不要在此 README 中引用个人串口号或本机绝对路径。
