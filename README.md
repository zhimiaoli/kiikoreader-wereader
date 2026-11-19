# kiikoreader WeRead（微信读书）固件

面向 **kiikoreader Metalio S31** 的社区固件源码项目，以 `0.3.3-rc113` 为固件基线，集成微信读书书架、账户、书籍缓存、进度同步与阅读时长上报。项目沿用 kiikoreader 的本地书架、阅读器和系统字体设置。

> 本项目不是腾讯、微信读书或 Metalio/kiikoread 官方发布。微信读书接口为非公开服务接口，服务端策略、登录校验或网络要求变化可能影响功能。

English: [README.en.md](README.en.md)

## 功能

- 微信读书账户扫码登录、账户状态管理及安全退出。
- 同步微信读书书架与封面，书架按服务端顺序展示并标识缓存状态。
- 缓存书籍正文与章节图片为本地 EPUB；缓存书籍通过固件原生阅读器打开。
- 在本地阅读器中上传本地阅读进度、下载云端阅读进度。
- 对符合条件的微信读书缓存书籍累计阅读时长并通过 Wi-Fi 上报；离线阅读按交互选择连接 Wi-Fi 上报或继续本地阅读。
- 复用固件现有书架 UI、分页、字体和字号设置。

## 开始使用

源码索引见 [`docs/SOURCE_LAYOUT.md`](docs/SOURCE_LAYOUT.md)，微信读书模块边界和数据流见 [`docs/WEREAD_ARCHITECTURE.md`](docs/WEREAD_ARCHITECTURE.md)，S31 构建方法见 [`docs/BUILD_METALIO_S31.md`](docs/BUILD_METALIO_S31.md)。

当前源码为 **3.0.0**。本版改为分章节按需缓存，支持中断续传；更新内容见 [`docs/RELEASE_3.0.md`](docs/RELEASE_3.0.md) 与 [`docs/CHANGELOG.md`](docs/CHANGELOG.md)。

需要 ESP-IDF **v6.1**。在 ESP-IDF 环境中，从仓库根目录构建：

```bash
python tools/build_firmware.py --board metalio47 --idf-metalio47 "$IDF_PATH" --python "$(command -v python)" --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

仓库的 GitHub Actions 工作流会运行板级构建和主机侧检查。设备刷写前请确认具体板型、分区布局和镜像类型。

## 源码导航

- `boards/metalio47/`：Metalio S31 板级配置、显示/电源/启动适配。
- `components/`：ESP-IDF 组件和共享驱动。
- `main/app/`、`main/apps/`：应用生命周期、主导航和页面。
- `main/book/`：本地书籍引擎、微信读书网络协议与 EPUB 缓存。
- `main/ui/`、`main/font/`：共享 UI 和字体系统。
- `tools/`：构建、打包及开发检查工具。
- `docs/`：功能架构、源码地图和构建说明。

## 完整固件刷写包

Metalio S31 0.3.3-rc113 最终版 16 MiB 全闪存镜像与 Windows 使用说明见 [`docs/FULL_FLASH_16MB.md`](docs/FULL_FLASH_16MB.md)。镜像从 `0x0` 写入并覆盖整片闪存，会清除设备设置、账户与内部缓存；下载链接和校验文件位于 [GitHub Release](https://github.com/YIYO-Designer/kiikoreader-wereader/releases/latest)。

## 许可

仓库内容适用范围不同。既有固件、第三方组件、字体和资源按各自原许可执行；本项目许可说明见 [`LICENSE`](LICENSE) 与 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。新增文档的标注许可见文件头。请勿将整个仓库或历史固件概括为单一许可。

## 安全与隐私

请勿提交 Cookie、登录二维码、账户凭据、NVS 导出、设备日志或含有个人书籍信息的截图。发现凭据泄露请立即在微信读书端撤销登录会话，并按 [`SECURITY.md`](SECURITY.md) 报告安全问题。
