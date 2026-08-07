# RK3588 Media Gateway

运行在 RK3588 嵌入式 Linux 上的硬件加速视频转码与任务管理服务。自行开发的
`gatewayd` 使用 C17 管理 RTSP 通道和 FFmpeg 子进程；FFmpeg-Rockchip 负责
MPP/RGA 媒体处理，MediaMTX 负责 RTSP/WebRTC 分发、录像和回放。

最短的数据流是：

```text
RTSP 输入 -> gatewayd 监督的 FFmpeg-Rockchip -> MPP/RGA -> MediaMTX -> 浏览器/播放器
```

这个项目的重点不是重新做一个摄像头 App，而是在真实 RK3588 上实现 Linux C 服务、
子进程监督、单通道故障隔离、配置热重载、硬件媒体链路和可复现测试。当前没有自行开发
Web 管理前端、音频或 AI。

## 五分钟真实摄像头演示

前提是 PC 与板卡在同一可信局域网，板卡已经按部署文档安装服务，PC 的 MediaMTX v1.20
二进制位于持久目录（示例为 `$HOME/mediamtx/mediamtx`）。当前实板地址是
`192.168.1.45`；换网络后只替换以下 `BOARD_IP` 和浏览器 URL 中的同一个地址。

先确认板端服务，不要重启它：

```bash
BOARD_IP=192.168.1.45
ssh cat@$BOARD_IP 'systemctl is-active rk-media-gateway.service mediamtx.service'
```

在 PC 项目目录先做检查，再启动专用 PC MediaMTX 和摄像头 FFmpeg：

```bash
./scripts/run_pc_camera_source.sh \
  --board-ip "$BOARD_IP" \
  --mediamtx "$HOME/mediamtx/mediamtx" \
  --check-only

./scripts/run_pc_camera_source.sh \
  --board-ip "$BOARD_IP" \
  --mediamtx "$HOME/mediamtx/mediamtx"
```

脚本明确检查 `/dev/video0`、MJPEG 1280×720@30、PC 路由地址和 TCP 8554 冲突，并使用
[PC 专用配置](config/mediamtx.pc-source.yml)关闭不需要的 WebRTC。若 8554 已被占用，先回到
那个已知服务的终端正常停止；脚本不会用 `pkill` 误杀其他进程。看到 `[READY]` 后保持终端
运行。

另开 PC 终端，只转发无认证、回环监听的 gatewayd 9080：

```bash
ssh -N -L 9080:127.0.0.1:9080 cat@$BOARD_IP
```

唯一推荐演示 URL 是：

```text
http://127.0.0.1:9080/view/cam01?media_host=192.168.1.45
```

预期同一页面显示 WebRTC 实时画面，以及 state、FFmpeg PID、输入编码/分辨率、FPS、码率、
帧数/丢帧、失败/重启计数和 CPU/RSS。结束时分别在摄像头源终端和 SSH 隧道终端按
`Ctrl+C`；源脚本只清理本轮启动的两个明确 PID。需要演示重复断流时，在源脚本终端输入
`s` 并按回车只停止 FFmpeg，观察离线后输入 `r` 并按回车恢复发布；PC MediaMTX 在两步
之间保持运行。

网络上，SSH 隧道只承载 gatewayd 页面和 JSON（9080）。iframe 从浏览器直接访问板卡
MediaMTX `BOARD_IP:8889` 完成 WebRTC HTTP 信令，媒体数据直接走板卡 `8189/UDP` ICE；
因此不能把 `-L 8889` 当作完整 WebRTC 隧道。详细原理、故障恢复和手工排查见
[完整演示指南](docs/demo.md)。

当前已实现的主要能力包括：

- YAML 配置读取、环境变量展开及边界校验。
- RTSP URL 密码脱敏。
- 不经过 shell 的 FFmpeg `argv` 参数构造。
- FFmpeg `-progress pipe:1` 增量解析。
- 配置检查与脱敏 dry-run 命令输出。
- 每通道独立的 FFmpeg 工作进程创建、输出管道读取和退出回收。
- 接收 `SIGINT`/`SIGTERM` 后先正常停止，超时再强制清理工作进程组。
- 每通道独立的状态转换、启动/progress 超时、稳定窗口和有上限的退避重试。
- 启动 FFmpeg 前执行 ffprobe 输入探测，并检查视频编码与硬件解码器是否匹配。
- 多个启用通道并行运行，一个通道失败不会停止其他通道。
- 支持通过 SIGHUP 重新读取配置，只新增、删除或重启发生变化的通道。
- 提供本地 HTTP 健康检查、通道状态查询和启动/停止/重启控制。
- 提供录像文件系统容量、最低空闲阈值和降级状态查询。
- RK3588 媒体环境检查脚本。

历史 Phase 4 已完成开发机测试和 RK3588 单路实机验收：PC 摄像头可经 RTSP 输入、MPP/RGA
硬件转码、MediaMTX 录像与回放，并由 systemd 以非 root 账号开机启动和异常恢复。
Phase 5 已完成短时软硬件性能、分辨率、码率、重复固定样本容量和画面时间戳延迟测试。
当前媒体链路只有视频，不采集或输出音频；长时间压力和多路真实输入留待后续验收。

组件职责边界：

| 组件 | 来源 | 职责 |
| --- | --- | --- |
| `gatewayd`、`gateway-metrics`、测试脚本 | 本项目 | 配置、状态机、进程监督、HTTP 控制、指标与验收 |
| FFmpeg-Rockchip、MPP、RGA | 上游项目 | 视频解码、缩放、格式转换和编码 |
| MediaMTX | 上游项目 | RTSP/WebRTC 分发、录像和回放 |
| systemd 单元和示例配置 | 本项目 | 非 root 服务编排和安全边界 |

## 第一次应该看哪里

不要从 1700 多行的测试记录开始。按你的目标选择入口：

| 目标 | 从这里开始 |
| --- | --- |
| 第一次跑出 PC 摄像头画面 | [从零跑通与演示完整视频链路](docs/demo.md) |
| 逐层理解源码 | [源码阅读与面试准备路线](docs/code-reading-guide.md) |
| 前台测试通过后安装为 systemd 服务 | [systemd 部署指南](docs/deployment.md) |
| 查看设计取舍和状态机 | [架构设计](ARCHITECTURE.md) |
| 复现测试或查验收证据 | [测试与验收指南](docs/test-plan.md) |
| 查看 CPU、RSS、吞吐和延迟数据 | [性能测试结果](docs/benchmark-results.md) |

第一次运行只需要 `docs/demo.md`。`docs/test-plan.md` 和 `docs/development-status.md` 是
开发证据，不是入门教程。

## 演示重点

1. 浏览器播放经 RK3588 MPP/RGA 转码后的真实画面。
2. HTTP 查询通道 PID、状态、FPS、帧数和重启次数。
3. 停止 PC RTSP 源，展示通道退避；恢复源后展示新 FFmpeg PID 和画面恢复。
4. 单独停止、启动或重启 `cam01`，网关和其他通道无需整体重启。
5. 展示固定样本下软件路径与 MPP/RGA 的 CPU、RSS 和吞吐实测。

本页开头给出唯一推荐命令。求职时应展示这条工程链路，不应把项目描述成比成熟摄像头
App 功能更多的消费级监控产品。

## 构建

依赖 CMake、支持 C17 的 C 编译器以及 libyaml 开发包。

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## 配置检查与 dry-run

示例配置通过环境变量注入源地址：

```bash
export CAM01_RTSP_URL='rtsp://user:password@camera.example/live'
./build/gatewayd --config config/gateway.example.yaml --check-config
./build/gatewayd --config config/gateway.example.yaml --dry-run
./build/gatewayd --config config/gateway.example.yaml
```

dry-run 输出会隐藏 URL 密码。输出内容只用于诊断；程序会将参数数组直接交给
`posix_spawnp()`，不会交给 shell 执行。不带检查选项时，当前版本会并行启动
所有启用的通道；需要使用包含 Rockchip MPP/RGA 支持的 FFmpeg。

网关向 MediaMTX 发布时固定使用 RTSP/TCP，并将编码 GOP 设置为输出帧率的两倍，
即默认约 2 秒一个关键帧，以限制新播放器等待关键帧的时间。实际播放延迟还会受到
输入源、网络、MediaMTX 和播放器缓冲影响；这不是亚秒级 WebRTC 调优承诺。

## 配置重载

修改正在使用的 YAML 文件后，向 `gatewayd` 发送 SIGHUP：

```bash
kill -HUP <gatewayd-pid>
```

程序会先完整加载并校验候选配置。配置无效时保留现有通道并记录拒绝原因；配置有效
时，未变化通道继续运行，只对新增、删除或内容变化的通道执行对应操作。SIGINT 和
SIGTERM 仍用于停止全部通道。

## HTTP 状态与控制

示例配置默认在 `127.0.0.1:9080` 启用 HTTP。`server.listen` 必须是数值 IPv4 或
IPv6 地址；如不需要控制面，可设置 `server.enabled: false`。

```bash
curl http://127.0.0.1:9080/v1/health
curl http://127.0.0.1:9080/v1/channels
curl http://127.0.0.1:9080/v1/channels/cam01
curl http://127.0.0.1:9080/v1/channels/cam01/metrics
curl http://127.0.0.1:9080/v1/recording
curl -X POST http://127.0.0.1:9080/v1/channels/cam01/stop
curl -X POST http://127.0.0.1:9080/v1/channels/cam01/start
curl -X POST http://127.0.0.1:9080/v1/channels/cam01/restart
```

查询接口只接受 GET，控制接口只接受 POST。成功接受控制命令时返回 202；通道不存在
返回 404；命令与当前状态冲突返回 409。接口不返回输入 URL 或密码。

每路通道还有一个轻量诊断页面。它使用原生 HTML/CSS/JavaScript，在浏览器中把
gatewayd 指标覆盖到 MediaMTX WebRTC 画面上方；OSD 不修改或重新编码视频，也不会写入
录像。由于 gatewayd 默认只监听回环地址，只从 PC 转发诊断页/API 的 9080：

```bash
ssh -N \
  -L 9080:127.0.0.1:9080 \
  cat@BOARD_IP
```

然后打开 `http://127.0.0.1:9080/view/cam01?media_host=BOARD_IP`；第二路只替换为
`/view/cam02?media_host=BOARD_IP`。`media_host` 只接受合法 IPv4、IPv6 或 DNS 主机名，
不接受端口、路径、凭据或 IPv6 zone ID，也不会被响应回显。页面只是嵌入式诊断界面，
不是带用户、告警或录像管理功能的复杂监控前端。画面由浏览器直连板卡 MediaMTX
8889/8189 提供；状态和 FFmpeg 指标由 gatewayd 每秒刷新。

当前 HTTP 服务没有身份认证，默认回环监听是安全边界，不应直接暴露到不受信任的网络。
监听地址、端口和启用状态的修改需要重启 `gatewayd`，不会通过 SIGHUP 生效。
`gatewayd` 默认保持常驻，即使所有通道均已停止也可通过接口重新启动；仅批处理场景可
使用 `--exit-when-idle` 让程序在全部通道结束后退出。启用通道超过 `max_retries` 后会显示
`FAILED`，但默认守护模式不会放弃该通道：它按 `max_backoff_sec` 低频探测，输入恢复后自动
经过 `PROBING`、`STARTING` 回到 `RUNNING`。只有一次性 `--exit-when-idle` 模式会在重试
耗尽后结束。

从 PC 摄像头推流、板卡编译和前台运行，到 RTSP/WebRTC 播放、HTTP 控制、录像和故障
恢复的完整流程见 [从零跑通与演示完整视频链路](docs/demo.md)。

## MediaMTX 录像配置

`mediamtx.recording` 配置录像目录、格式、分段、自动删除周期和回放监听地址。根据网关
通道生成可直接交给 MediaMTX 的配置：

```bash
export CAM01_RTSP_URL='rtsp://user:password@camera.example/live'
./build/gatewayd --config config/gateway.example.yaml \
  --print-mediamtx-config > mediamtx.generated.yml
mediamtx mediamtx.generated.yml
```

生成内容不会包含输入 URL 或密码。示例 [config/mediamtx.example.yml](config/mediamtx.example.yml)
启用 fMP4 录像、按通道分目录、7 天自动删除和仅回环回放。修改录像参数后需要重新生成
MediaMTX 配置并重载或重启 MediaMTX；向 `gatewayd` 发送 SIGHUP 不会修改外部服务。
`GET /v1/recording` 返回录像文件系统总量、当前用户可用量、阈值和
`ok`/`low_space`/`unavailable` 状态；目录本身不会暴露在 API 中。空间不足时健康检查
变为 `degraded`，自动删除仍由 MediaMTX 的 `recordDeleteAfter` 执行。

## systemd 部署

安装目标包含 `rk-media-gateway.service`、独立的 `mediamtx.service`、tmpfiles 目录规则
和环境文件示例。基本入口：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
sudo cmake --install build
```

不要直接启动示例配置；先创建非 root 服务账号、安装真实配置并以 0600 权限保存凭据。
完整步骤、重载边界、优雅停止和安全说明见
[docs/deployment.md](docs/deployment.md)。

## 板卡环境检查

在 RK3588 板卡上执行：

```bash
./scripts/check_media_env.sh
```

脚本会检查架构、MPP/RGA/DRM 设备权限、Rockchip 编解码器、RGA 滤镜和
MediaMTX。完整设计见 [ARCHITECTURE.md](ARCHITECTURE.md)，可重复执行的分阶段
测试步骤和验收记录见 [docs/test-plan.md](docs/test-plan.md)。

## 运行指标采样

`gateway-metrics` 可同时采样多个 Linux 进程并将 CPU、RSS 和文件描述符数输出为 CSV：

```bash
gateway-metrics \
  --target gateway=1234 \
  --target ffmpeg=1235 \
  --target mediamtx=1236 \
  --duration-sec 5 \
  --interval-ms 1000 \
  --summary-output metrics-summary.csv > metrics.csv
```

PID 需替换为当前实际进程。样本 CSV 写入标准输出，`--summary-output`
另生成 CPU、RSS、FD 和样本可用性汇总；已有汇总文件不会被覆盖。详细口径、
FPS/丢帧配套采集方式和性能结果见
[docs/benchmark-results.md](docs/benchmark-results.md)。

## 固定样本转码测试

安装后可使用同一个本地视频样本比较软件、MPP 和 MPP+RGA 路径：

```bash
/usr/local/share/rk-media-gateway/scripts/run_transcode_benchmark.sh \
  --mode mpp-rga \
  --input /absolute/path/input.mkv \
  --output-dir /absolute/path/results \
  --output-width 1280 --output-height 720 --bitrate-kbps 3000
```

`--mode` 可为 `software`、`mpp` 或 `mpp-rga`。脚本默认预热 2 秒、采样 10 秒，
保存原始指标与汇总，并实际生成 3 秒输出做 ffprobe 和完整软件解码校验。
它只接受本地固定文件，
且不覆盖已有结果。开发者验收步骤见 [docs/test-plan.md](docs/test-plan.md)。

使用同一固定样本进行多通道容量短测：

```bash
/usr/local/share/rk-media-gateway/scripts/run_capacity_benchmark.sh \
  --channels 4 --mode mpp-rga \
  --input /absolute/path/input.mkv \
  --output-dir /absolute/path/capacity-4
```

该命令会并发运行多条转码路径，生成每路证据和 `capacity-total.csv`。
它测量的是重复固定样本的板卡容量，不等价于多个真实摄像头验收。
