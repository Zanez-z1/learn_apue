# 基于 RK3588 的硬件加速视频转码与流媒体网关

## 1. 文档目的

本文档定义项目的目标、系统边界、核心架构、模块职责、运行流程、异常恢复策略和验收标准。

项目部署在实际使用 RK3588S2 的开发板上，但对外统一描述为 RK3588 平台。板卡品牌和型号只作为测试环境记录，不进入项目名称和核心架构。

## 2. 项目定位

本项目实现一个运行在嵌入式 Linux 上的视频转码与流媒体网关。当前 `gatewayd` 接入
RTSP 网络视频流，利用 RK3588 的 MPP 和 RGA 完成硬件解码、图像缩放、格式转换和硬件
编码，再通过 MediaMTX 提供 RTSP、WebRTC、录像和回放。USB 摄像头只作为开发机测试源，
先由外部 FFmpeg/MediaMTX 转成 RTSP；本地文件和 V4L2 不是当前 `gatewayd` 输入类型。

项目的主体不是重新实现编解码器或流媒体协议，而是自行开发一个 C17 网关管理服务 `gatewayd`，负责：

- 解析配置并维护视频通道。
- 创建、停止和重启 FFmpeg 转码工作进程。
- 管理通道状态机和断流重连。
- 采集帧率、码率、进程资源和重启次数等运行指标。
- 提供通道查询和控制接口。
- 输出结构化日志并支持 systemd 服务化部署。

## 3. 项目目标

### 3.1 核心目标

1. 支持至少一种真实视频源接入，第一阶段优先使用 RTSP。
2. 在 RK3588 上跑通 MPP 硬件解码、RGA 图像处理和 MPP 硬件编码链路。
3. 将转码结果发布到 MediaMTX，并提供 RTSP 和 WebRTC 播放。
4. 自行实现通道管理、进程管理、状态监控和异常恢复逻辑。
5. 支持配置文件、分级日志、运行指标和 systemd 部署。
6. 完成软硬件转码性能对比，并保留可复现的测试记录。

### 3.2 非目标

第一版不包含以下内容：

- 不自行实现 H.264/H.265 编解码器。
- 不自行实现完整 RTSP、WebRTC 或 HLS 协议栈。
- 不开发复杂的 NVR 管理平台。
- 不加入目标检测、人脸识别等 AI 功能。
- 不承诺生产级集群、高可用和公网安全能力。
- 不以修改 FFmpeg 或 MediaMTX 核心源码作为完成条件。

## 4. 总体架构

```text
                               Control Plane
                    +--------------------------------+
                    |            gatewayd            |
                    |                                |
配置文件 ---------->| Config Manager                 |
本地 HTTP API ----->| Channel Manager                |
systemd/信号 ------>| Process Manager                |
                    | State Machine / Health Monitor |
                    | Metrics / Logging              |
                    +---------------+----------------+
                                    |
                         创建、监控、重启工作进程
                                    |
                                    v
                               Data Plane
+----------------+       +-------------------------+
| RTSP 摄像机     |------>|                         |
| PC RTSP 测试源  |------>| FFmpeg-Rockchip Worker |
| 其他 RTSP 服务  |------>|                         |
| NVR/视频平台    |------>| MPP Decode              |
+----------------+       | RGA Scale/Convert       |
                         | MPP Encode              |
                         +------------+------------+
                                      |
                               RTSP Publish
                                      |
                                      v
                         +------------+------------+
                         |        MediaMTX         |
                         | RTSP / WebRTC           |
                         | Record / Playback       |
                         +------------+------------+
                                      |
                         +------------+------------+
                         | VLC / ffplay / Browser  |
                         +-------------------------+
```

系统分为两个平面：

- 控制平面：由 `gatewayd` 负责配置、任务生命周期、状态、日志和控制接口。
- 数据平面：由 FFmpeg-Rockchip 工作进程和 MediaMTX 负责实际视频处理与分发。

## 5. 核心架构决策

### 5.1 每个通道使用独立 FFmpeg 进程

每个视频通道对应一个 FFmpeg 工作进程，而不是把所有通道放进同一个进程。

原因：

- 单通道崩溃不会影响其他通道。
- 可以独立重启、统计资源和收集日志。
- FFmpeg 命令行便于先验证硬件链路，再接入管理服务。
- 能清楚区分网关管理代码和媒体处理组件的职责。

`gatewayd` 使用 `posix_spawnp()` 或等价的安全进程创建接口构造参数数组，不通过 `/bin/sh -c` 拼接命令，避免配置内容造成命令注入。

### 5.2 MediaMTX 独立运行

MediaMTX 作为单独的 systemd 服务运行，负责流协议转换、播放端接入和录像。`gatewayd` 只向固定的本地路径发布视频流，不在自身内部重复实现流媒体服务器。

### 5.3 配置文件是通道定义的唯一来源

所有通道 ID、输入地址、转码参数、输出路径和重试策略均来自配置文件。启动时完成完整校验，配置不合法时拒绝创建对应通道，并输出明确错误。

### 5.4 先使用结构化进度输出，再解析普通日志

FFmpeg 工作进程启用 `-progress pipe:1`，由 `gatewayd` 解析 `frame`、`fps`、`bitrate`、`out_time`、`drop_frames` 和 `speed` 等键值。

标准错误流只用于错误诊断，不依赖不稳定的人类可读日志格式计算运行指标。

## 6. 组件职责

### 6.1 gatewayd

项目自行实现的核心程序使用 C17。模块通过显式结构体、返回码和清晰的资源所有权约定协作。

#### Config Manager

- 加载 YAML 配置。
- 校验通道 ID、URL、编解码格式、分辨率、码率和输出路径。
- 对敏感字段进行脱敏后再写入日志。
- 支持通过 `SIGHUP` 重新加载配置。
- 比较新旧配置，只重启发生变化的通道。
- 重载时先完成候选配置解析和校验；无效候选不得改变当前注册表或停止运行通道。
- HTTP 监听地址、端口或启用状态变化需要重启进程，SIGHUP 不切换监听 socket。

#### Channel Manager

- 维护所有通道对象。
- 接收启动、停止和重启命令。
- 管理通道状态机。
- 保存最近一次错误、重启次数和最近进度时间。
- 从各 supervisor 的同步观察回调复制状态快照；多通道阶段由管理器负责加锁，
  不能让 HTTP 线程直接读取 supervisor 的临时快照。
- 使用独立生命周期互斥锁串行化单通道启动、停止、重启与 SIGHUP 差异化重载；快照
  读写锁只保护短时间内存复制，不覆盖进程停止或线程回收。

#### Pipeline Builder

- 根据输入探测结果和转码配置生成 FFmpeg 参数数组。
- 区分 H.264/H.265 解码器和编码器。
- 选择是否启用 RGA 缩放与格式转换。
- 拒绝不受支持的配置组合。
- 提供 dry-run 模式，输出脱敏后的等价命令用于调试。

#### Process Manager

- 创建 FFmpeg 工作进程并记录 PID、进程组和启动时间。
- 读取 stdout 进度管道和 stderr 日志管道。
- 使用 `SIGTERM` 请求正常退出，超时后使用 `SIGKILL` 清理。
- 回收子进程，避免僵尸进程。
- 程序退出时按顺序关闭全部通道。

#### Health Monitor

- 检查工作进程是否存活。
- 检查 FFmpeg 进度是否持续更新。
- 判断输入流停滞、发布失败和异常退出。
- 按错误类型决定立即重启、延迟重试或进入失败状态。

#### Metrics Collector

- 采集 FFmpeg 输出帧率、码率、处理速度和丢帧数。
- 从 `/proc/<pid>/stat` 和 `/proc/<pid>/status` 采集 CPU 时间与 RSS。
- 记录运行时长、连续失败次数和总重启次数。
- 第一版通过 HTTP JSON 输出，后续可增加 Prometheus 格式。

#### HTTP Control API

默认只监听 `127.0.0.1`，第一版提供：

```text
GET  /v1/health
GET  /v1/channels
GET  /v1/channels/{id}
POST /v1/channels/{id}/start
POST /v1/channels/{id}/stop
POST /v1/channels/{id}/restart
```

动态新增和删除通道不是第一版必须功能，优先通过配置文件管理。
当前接口不提供身份认证，回环监听是第一版的安全边界；若需要跨主机访问，必须在
gatewayd 外部增加认证、加密和网络访问控制。

### 6.2 FFmpeg-Rockchip Worker

负责单个通道的数据处理：

1. 连接 RTSP、V4L2 或文件输入。
2. 使用 `h264_rkmpp` 或 `hevc_rkmpp` 硬件解码。
3. 按配置使用 `scale_rkrga` 等 RGA 滤镜。
4. 使用 `h264_rkmpp` 或 `hevc_rkmpp` 硬件编码。
5. 将编码流发布到本机 MediaMTX。
6. 通过进度管道向 `gatewayd` 输出运行指标。

示意命令如下，最终参数必须以板卡上 `ffmpeg -h` 的实际结果为准：

```bash
ffmpeg \
  -nostdin \
  -hide_banner \
  -loglevel warning \
  -progress pipe:1 \
  -stats_period 1 \
  -hwaccel rkmpp \
  -hwaccel_output_format drm_prime \
  -c:v h264_rkmpp \
  -i rtsp://source/live \
  -vf "scale_rkrga=w=1280:h=720:format=nv12" \
  -c:v h264_rkmpp \
  -b:v 4000k \
  -r 25 \
  -g 50 \
  -an \
  -f rtsp \
  -rtsp_transport tcp \
  rtsp://127.0.0.1:8554/cam01
```

是否实现完整零拷贝不能仅根据命令推断，需要结合 FFmpeg 日志、CPU 占用和实际数据格式验证后再写入项目说明。
输出端显式使用 RTSP/TCP，避免 FFmpeg 先尝试不受发布端支持的传输方式。GOP 固定为
`2 * fps`，将新连接等待关键帧的上界控制在约 2 秒；这是启动延迟与压缩效率之间的
工程折中，不代表播放器端的端到端延迟一定低于 2 秒。

### 6.3 MediaMTX

- 接收 FFmpeg 发布的本地 RTSP 流。
- 为客户端提供 RTSP 和 WebRTC 输出；当前生成配置显式关闭未使用的 HLS。
- 按配置进行分段录像和历史回放。
- 提供路径状态和运行指标，供后续健康检查使用。

MediaMTX 不由 `gatewayd` 作为子进程启动，二者分别由 systemd 管理。

## 7. 通道状态机

```text
DISABLED
   |
   | enable
   v
STOPPED ---- start ----> PROBING ---- success ----> STARTING
   ^                         |                         |
   |                         | failure                 | progress received
   |                         v                         v
   +------ manual stop ---- BACKOFF <------------- RUNNING
                              ^                       |
                              |                       | process exit /
                              +-----------------------+ progress timeout
                              |
                              | retries exhausted
                              v
                            FAILED
```

状态含义：

- `DISABLED`：配置中明确禁用。
- `STOPPED`：通道可用但当前未运行。
- `PROBING`：正在探测输入和编解码信息。
- `STARTING`：工作进程已创建，等待有效进度。
- `RUNNING`：持续收到视频处理进度。
- `BACKOFF`：发生可恢复错误，等待下一次重试。
- `FAILED`：连续失败达到上限，需要人工处理或配置变更。

默认退避时间建议为 1、2、4、8、16、30 秒，后续重试保持 30 秒上限。稳定运行超过指定时间后，将连续失败计数清零。

## 8. 线程与事件模型

`gatewayd` 采用少量线程和事件驱动结合的模型：

- 主线程：初始化、配置加载，通过 `signalfd` 同步消费 SIGHUP/SIGINT/SIGTERM，并协调
  服务退出。
- 每通道 supervisor 线程：监督 ffprobe/FFmpeg 进程及 stdout/stderr 管道。
- HTTP 线程：处理本地控制和查询请求。

进程控制信号在创建线程前统一屏蔽，因此不会在 supervisor 或 HTTP 线程执行异步信号
处理器。创建 ffprobe/FFmpeg 时通过 `posix_spawn` 属性恢复空信号掩码和默认处理方式，
保证子进程仍能收到正常停止信号。

共享的通道快照注册表使用 POSIX 读写锁保护，耗时操作不能持有该读写锁。生命周期
命令使用另一把互斥锁串行化；该锁可以覆盖进程停止和线程回收，以保证同一通道不会
被并发启动两次，但不阻塞只读快照查询。

## 9. 配置设计

建议配置文件路径：

```text
/etc/rk-media-gateway/gateway.yaml
```

示例：

```yaml
server:
  listen: 127.0.0.1
  port: 9080

mediamtx:
  publish_base_url: rtsp://127.0.0.1:8554
  recording:
    enabled: true
    directory: /var/lib/rk-media-gateway/recordings
    format: fmp4
    part_duration_sec: 1
    max_part_size_mb: 50
    segment_duration_sec: 3600
    delete_after_sec: 604800
    min_free_mb: 1024
    playback_listen: 127.0.0.1
    playback_port: 9996

defaults:
  probe_timeout_sec: 10
  startup_timeout_sec: 15
  progress_timeout_sec: 10
  stable_run_sec: 60
  stop_timeout_sec: 5
  max_retries: 10
  max_backoff_sec: 30

channels:
  - id: cam01
    enabled: true
    input:
      type: rtsp
      url: ${CAM01_RTSP_URL}
      transport: tcp
    video:
      decoder: h264_rkmpp
      width: 1280
      height: 720
      encoder: h264_rkmpp
      bitrate_kbps: 4000
      fps: 25
    output:
      path: cam01
```

RTSP 用户名和密码通过 systemd `EnvironmentFile` 注入，不提交到 Git 仓库。

`gatewayd --print-mediamtx-config` 将已校验的录像字段和通道输出路径渲染为 MediaMTX
配置。gatewayd 不启动或重载 MediaMTX；生成文件的部署与服务重载由管理员或 systemd
完成。录像目录要求绝对安全路径，回放默认只监听回环地址。

`probe_timeout_sec` 限制每次 ffprobe 输入探测的最长时间；探测成功后才会进入
FFmpeg 启动阶段。

`stable_run_sec` 表示通道持续稳定收到 progress 多久后，将连续失败次数清零；总重启
次数不会因此清零。

## 10. 异常处理

### 10.1 输入流断开

判断条件：

- FFmpeg 进程异常退出。
- 超过 `progress_timeout_sec` 未收到新进度。
- stderr 出现连接超时或服务器返回错误。

处理：

1. 记录通道 ID、错误分类和退出码。
2. 终止残留进程并回收资源。
3. 进入 `BACKOFF`。
4. 按退避策略重新探测和拉流。

### 10.2 MediaMTX 不可用

- FFmpeg 发布失败后由通道状态机重试。
- `gatewayd` 不直接重启 MediaMTX，避免服务间循环控制。
- MediaMTX 自身由 systemd 的重启策略恢复。

### 10.3 硬件编码器初始化失败

- 不自动降级为软件编码，避免在未知情况下造成 CPU 过载。
- 将通道置为 `FAILED` 或按有限次数重试。
- 日志保留 MPP/RGA 相关错误以及最终 FFmpeg 参数。

### 10.4 磁盘空间不足

- 通过 HTTP 查询时使用 `statvfs()` 检查录像目录剩余空间，不扫描或打开录像分段。
- 达到警戒阈值或目录不可访问时将整体健康状态标记为降级。
- 最旧分段由 MediaMTX 的 `recordDeleteAfter` 自动删除；第一版 gatewayd 不越权删除
  MediaMTX 正在写入的文件。
- 转码与实时播放不应因录像目录写满而整体退出。

## 11. 日志设计

日志至少包含：

```text
timestamp
level
component
channel_id
worker_pid
state
event
message
```

示例：

```text
2026-07-27T20:15:30+08:00 WARN channel cam01 pid=1234 state=BACKOFF event=source_timeout retry_in=4s
```

日志规则：

- 不输出 RTSP URL 中的明文密码。
- FFmpeg stderr 按通道分别记录，并限制单条长度。
- 高频进度数据不写入普通日志，只更新指标。
- systemd 环境下输出到 stdout/stderr，由 journald 统一收集。

## 12. 安全边界

- HTTP API 默认仅监听回环地址。
- 配置文件权限建议为 `0640`，凭据文件建议为 `0600`。
- 通道 ID 只允许字母、数字、下划线和短横线。
- 不允许用户配置直接形成 shell 命令。
- 对输入 URL、分辨率、码率、FPS 和输出路径设置合法范围。
- `gatewayd` 和 MediaMTX 使用普通用户运行，仅授予访问 `/dev/mpp_service`、`/dev/rga`、`/dev/dri` 和摄像头设备所需权限。

## 13. 代码目录规划

```text
rk3588-media-gateway/
├── CMakeLists.txt
├── README.md
├── ARCHITECTURE.md
├── config/
│   ├── gateway.example.yaml
│   └── mediamtx.example.yml
├── include/
│   └── gateway/
├── src/
│   ├── main.c                 # CLI、配置入口和进程级信号处理
│   ├── config/                # YAML 加载和 MediaMTX 配置生成
│   ├── channel/               # 状态机、supervisor、快照和多通道管理器
│   ├── pipeline/              # FFmpeg argv 构造
│   ├── process/               # 子进程创建、轮询、停止和回收
│   ├── probe/                 # ffprobe 输入探测
│   ├── monitor/               # progress、录像状态和 /proc 指标
│   ├── api/                   # 本地 HTTP 状态与控制
│   └── tools/                 # gateway-metrics CLI
├── deploy/
│   └── systemd/              # gatewayd/MediaMTX 单元、环境示例和 tmpfiles
├── scripts/
│   ├── check_media_env.sh
│   ├── run_transcode_benchmark.sh
│   └── run_capacity_benchmark.sh
├── tests/
│   ├── unit/
│   └── integration/
└── docs/
    ├── demo.md
    ├── code-reading-guide.md
    ├── deployment.md
    ├── test-plan.md
    ├── benchmark-results.md
    └── development-status.md
```

## 14. 开发阶段

### Phase 0：运行环境确认

- 记录系统镜像、内核版本和 FFmpeg 版本。
- 检查 MPP、RGA、DRM 和 V4L2 设备节点权限。
- 确认 `h264_rkmpp`、`hevc_rkmpp` 和 `scale_rkrga` 可用。
- 安装并单独验证 MediaMTX。

完成标准：板卡能够识别硬件编解码器和 RGA 滤镜。

### Phase 1：手工跑通单路媒体链路

- 使用本地视频文件验证硬件解码和编码。
- 使用 PC 推送的 RTSP 流作为真实网络输入。
- 将转码结果发布到 MediaMTX。
- 使用 ffplay、VLC 和浏览器分别验证播放。

长期完成标准：单路 1080p 视频能够连续转码和播放 30 分钟。2026-08-06 的 Phase 0～4
收口按用户明确批准的 5 分钟以上窗口执行，30 分钟长稳记录为未执行并留到后续压力测试；
两者不得混写为同一验收结果。

### Phase 2：实现单通道 gatewayd

- 实现配置加载和校验。
- 实现 FFmpeg 参数构造。
- 实现工作进程创建、退出和日志收集。
- 解析 `-progress` 结构化输出。
- 实现单通道状态查询。

完成标准：不手工执行 FFmpeg 命令，通过 `gatewayd` 启动和停止单路转码。

### Phase 3：多通道与异常恢复

- 支持多个独立通道。
- 实现状态机、超时检测和退避重试。
- 实现配置重载和按通道重启。
- 注入输入断流、MediaMTX 停止和工作进程崩溃等故障。

完成标准：输入恢复后通道能够自动重新发布，其他通道不受影响。

### Phase 4：控制接口、录像和服务化

- 提供本地 HTTP 控制 API。
- 接入 MediaMTX 录像和回放。
- 增加磁盘空间监控。
- 编写 systemd service 和环境变量文件。

完成标准：设备重启后服务自动启动，异常退出后由 systemd 恢复。

### Phase 5：性能测试与项目文档

- 对比软件转码和 MPP/RGA 硬件转码。
- 测试不同分辨率、码率和通道数量。
- 记录 CPU、RSS、FPS、处理速度、丢帧和端到端延迟。
- 完成部署文档、故障记录和演示视频。

完成标准：测试过程可复现，简历中的每个结果都有对应日志或报告支撑。

## 15. 测试与验收

### 15.1 功能测试

- 固定本地文件可通过独立基准脚本完成软硬件转码对比；这不是 `gatewayd` 输入类型。
- RTSP 输入转码成功。
- RTSP、WebRTC 输出可播放。
- 启动、停止和重启接口有效。
- 配置错误能够被明确拒绝。
- 日志中不泄露密码。

### 15.2 故障注入

- 主动停止 RTSP 源，验证退避重连。
- 杀死 FFmpeg 工作进程，验证自动恢复。
- 停止并重启 MediaMTX，验证重新发布。
- 发送 `SIGTERM`，验证子进程能够被完整清理。
- 使用错误编码参数，验证通道进入 `FAILED` 而不是无限快速重启。

### 15.3 稳定性测试

- 第一阶段连续运行 2 小时。
- 项目完成前连续运行 24 小时。
- 记录进程重启次数、内存变化和丢帧情况。
- 检查是否存在僵尸进程、文件描述符泄漏和日志无限增长。

### 15.4 性能测试

对相同输入分别测试：

1. 软件解码 + 软件编码。
2. MPP 解码 + MPP 编码。
3. MPP 解码 + RGA 缩放 + MPP 编码。

统一记录：

- 输入分辨率、编码格式、帧率和码率。
- 输出分辨率、编码格式、帧率和码率。
- 平均与峰值 CPU 占用。
- RSS 内存。
- 实际 FPS、处理速度和丢帧数。
- 端到端延迟。

端到端延迟应通过画面时间戳或专用测试源测量，不能直接使用 FFmpeg 的处理速度代替。

## 16. 项目交付物

最终仓库至少应包含：

- 可编译的 `gatewayd` 源码。
- CMake 构建文件。
- 示例配置和配置字段说明。
- MediaMTX 示例配置。
- systemd service。
- 环境检查和性能测试脚本。
- 部署说明。
- 故障注入与稳定性测试记录。
- 软硬件转码性能对比报告。
- 一份可复现的完整演示指南；作为公开作品集时，再附一段短视频或至少一张真实画面截图。

## 17. 简历完成条件

只有满足以下条件后，项目才能按“已完成项目”写入简历：

1. 在实际 RK3588S2 板卡上跑通硬件转码。
2. `gatewayd` 是自行实现并能够管理至少一个真实视频通道。
3. RTSP 输入断开后能够自动恢复。
4. MediaMTX 能提供至少 RTSP 和 WebRTC 两种输出。
5. systemd 能完成开机启动和异常恢复。
6. 已完成一次软硬件转码性能对比。
7. GitHub README 能清楚区分开源依赖和自行实现的模块。

简历中可以使用的数据必须来自测试记录，不能提前填写未经验证的通道数、CPU 降幅、延迟或稳定运行时间。

截至 2026-08-06，上述 1～7 项已有代码、测试和实板记录支撑，因此项目在技术证据上可以
写入简历。但“技术完成”不等于“面试就绪”：项目所有者还应按 `docs/demo.md` 独立完成
一次五分钟演示，按 `docs/code-reading-guide.md` 讲清启动、断流恢复、热重载和停止四条
流程，并为公开仓库补充真实画面截图或短视频。在此之前，不应把无法解释的实现细节写成
个人熟练掌握的能力。

## 18. 上游项目

- FFmpeg-Rockchip: https://github.com/nyanmisaka/ffmpeg-rockchip
- MediaMTX: https://github.com/bluenviron/mediamtx
- Rockchip MPP: https://github.com/rockchip-linux/mpp
- 野火 RK3588 FFmpeg 文档: https://doc.embedfire.com/linux/rk3588/quick_start/zh/latest/lubancat_rk_software_hardware/software/ffmpeg/ffmpeg.html
