# RK3588 Media Gateway 面试速记

这份文档用于在较短时间内熟悉项目并准备面试。它不替代用户手册、架构文档或测试记录，
重点是：项目解决什么问题、自己实现了什么、关键技术取舍是什么，以及哪些能力不能夸大。

## 1. 一句话介绍

这是一个运行在 RK3588 嵌入式 Linux 上的 C17 视频网关：它接收 RTSP 视频，监督独立的
FFmpeg-Rockchip 工作进程，利用 MPP/RGA 完成硬件解码、缩放和编码，再交给 MediaMTX
提供 RTSP、WebRTC、录像和回放；同时实现配置、状态机、故障恢复、指标和 systemd 部署。

## 2. 30 秒面试介绍

> 我做了一个 RK3588 媒体网关。数据面复用 FFmpeg-Rockchip、MPP/RGA 和 MediaMTX，
> 我自己用 C17 实现的是控制面：YAML 配置、RTSP 输入探测、FFmpeg 参数构造、子进程创建
> 与回收、每通道状态机、断流退避恢复、SIGHUP 差异化重载、HTTP 状态控制以及 `/proc`
> 资源采样。项目已经在真实 RK3588 板卡上跑通 H.264 1080p25、浏览器 WebRTC、OSD、录像
> 和长期断流后无人值守恢复。当前没有音频和 AI，双真实摄像头仍待验收。

## 3. 项目为什么有价值

摄像头、FFmpeg 和流媒体服务器都已经存在，这个项目的价值不是重新实现它们，而是解决
嵌入式设备上的工程管理问题：

- 多路视频任务如何独立启动、停止和恢复；
- 输入断流、FFmpeg 崩溃或发布端故障后如何避免整机人工重启；
- 如何安全创建外部进程、读取进度、处理超时并清理僵尸进程；
- 如何利用 RK3588 硬件媒体能力降低 CPU 和内存开销；
- 如何把配置、日志、指标、录像和 systemd 服务组合成可部署程序；
- 如何区分控制平面和媒体数据平面，避免在一个进程里重复造协议栈。

它更接近嵌入式视频平台中的媒体任务守护服务，而不是消费级摄像头 App。

## 4. 总体架构

```text
PC USB 摄像头或真实 RTSP 摄像头
              |
              | RTSP/TCP
              v
+------------------------------------------------------+
| RK3588                                                |
|                                                      |
| gatewayd 控制平面                                    |
|   配置 -> 通道管理 -> supervisor -> 状态/指标/API    |
|                         |                            |
|                         | posix_spawnp               |
|                         v                            |
| FFmpeg-Rockchip 数据面                               |
|   MPP 硬件解码 -> RGA 缩放/格式转换 -> MPP 硬件编码  |
|                         |                            |
|                         | RTSP publish               |
|                         v                            |
| MediaMTX -> RTSP / WebRTC / 录像 / 回放              |
+------------------------------------------------------+
              |
              v
         浏览器 / ffplay / VLC
```

项目采用控制平面和数据平面分离：

- `gatewayd` 决定运行什么、何时重启、当前状态是什么；
- FFmpeg-Rockchip 处理视频帧；
- MediaMTX 负责流媒体协议、播放器接入和录像。

## 5. 哪些是自己写的，哪些是外部组件

| 部分 | 来源 | 作用 |
| --- | --- | --- |
| `gatewayd` | 本项目 | 配置、通道、进程、状态机、恢复、HTTP、指标 |
| `gateway-metrics` 和脚本 | 本项目 | `/proc` 采样与可复现验收 |
| 浏览器 OSD | 本项目 | 将 gatewayd 指标覆盖在 WebRTC 画面上 |
| libyaml | 外部库 | 将 YAML 解析成 document/node 结构 |
| FFmpeg-Rockchip | 外部项目 | 调用 MPP/RGA 处理媒体数据 |
| Rockchip MPP | 芯片媒体库 | 硬件视频解码和编码 |
| Rockchip RGA | 芯片图像加速库 | 缩放、像素格式转换等二维处理 |
| MediaMTX | 外部项目 | RTSP/WebRTC、录像和回放 |
| systemd | Linux 基础设施 | 服务启动、权限隔离和异常重启 |

面试时不要说“我实现了 FFmpeg、RTSP 或 WebRTC”。准确说法是：我将这些成熟组件集成到
自己实现的嵌入式媒体任务管理服务中。

## 6. 一路视频怎样启动

1. `main()` 解析 `--config`、`--check-config` 等参数。
2. `gw_config_load_file()` 使用 libyaml 加载配置，展开环境变量并完成语义校验。
3. Channel Manager 为每个启用通道创建独立 supervisor 线程。
4. Pipeline Builder 根据配置构造 FFmpeg `argv[]`。
5. Process Manager 使用 `posix_spawnp()` 创建新进程组并连接 stdout/stderr 管道。
6. FFmpeg 使用 `h264_rkmpp` 解码、`scale_rkrga` 处理、`h264_rkmpp` 编码。
7. FFmpeg 将结果发布到板端 MediaMTX 的 `cam01`。
8. supervisor 解析 `-progress pipe:1`，将 FPS、帧数和丢帧写入通道快照。
9. 独立指标线程读取 `/proc/<pid>`，HTTP 线程只复制已经发布的快照。

## 7. 最值得讲的技术点

### 7.1 每通道独立进程和独立 supervisor

每个通道对应一个 FFmpeg 进程和一个 supervisor 线程。这样单通道崩溃可以独立回收和
重建，不会把所有媒体任务放进同一个故障域。代价是多了一些进程和线程开销，但换来了
隔离、可观测性和更简单的故障处理。

### 7.2 不通过 shell 启动 FFmpeg

Pipeline Builder 构造独立的参数数组，Process Manager 直接调用 `posix_spawnp()`，不使用：

```text
/bin/sh -c "ffmpeg ..."
```

这样避免 URL 或配置内容被 shell 再解释，降低命令注入风险，也能明确管理文件描述符、
信号掩码和进程组。

### 7.3 子进程生命周期

父进程为子进程创建 stdout/stderr 管道和独立进程组。正常停止先发送 `SIGTERM`，在
`stop_timeout_sec` 内未退出再发送 `SIGKILL`，最后 `waitpid()` 回收，避免僵尸进程。
程序还确保监听 socket 不被 FFmpeg 子进程继承。

### 7.4 可恢复状态机

```text
STOPPED -> STARTING -> RUNNING
              |          |
              + failure -+
                  |
                  v
               BACKOFF
                  |
                  v
               STARTING
```

快速重试采用 1、2、4、8、16、30 秒的有界指数退避。超过 `max_retries` 后显示
`FAILED`，但默认守护模式不会退出 supervisor，而是每隔 `max_backoff_sec` 继续低频重试。
输入恢复后自动回到 `RUNNING`。

这是项目中一次重要的设计修正：旧版本把 `FAILED` 当成终态，长时间断流后必须人工重启。
真实演示暴露问题后，将它改成了可恢复状态。面试时可以用这个例子说明测试通过不等于
需求正确，以及如何根据真实运行场景修正状态机语义。

### 7.5 进度和健康判断

项目不依赖 FFmpeg 面向人的普通日志统计 FPS，而是使用稳定的：

```text
-progress pipe:1
```

解析 `frame`、`fps`、`bitrate`、`drop_frames`、`speed` 和 `progress`。如果启动后迟迟没有
有效进度，或运行中超过 `progress_timeout_sec` 不更新，就终止旧进程并进入恢复流程。

### 7.6 线程与锁

- 主线程使用 `signalfd` 同步处理 SIGHUP、SIGINT 和 SIGTERM；
- 每通道 supervisor 线程处理媒体进程生命周期；
- 指标线程周期采样 `/proc`；
- HTTP 线程处理查询和控制。

通道快照使用读写锁保护短时间内存复制；启动、停止、重启和重载使用单独的生命周期互斥
锁串行化。耗时的进程等待不能占用快照读写锁，否则 HTTP 查询会被长期阻塞。

### 7.7 配置热重载

收到 SIGHUP 后先完整加载候选 YAML。候选配置无效时保留现有通道；有效时比较新旧配置，
只新增、删除或重启发生变化的通道，未变化通道继续运行。HTTP 监听地址等进程级配置不在
SIGHUP 中偷偷切换，而是明确要求重启服务。

### 7.8 指标采样

每秒从 `/proc/<pid>/stat` 和 `/proc/<pid>/status` 读取 CPU tick 与 RSS。CPU 百分比根据
同一 PID 相邻两次 tick 和单调时钟差计算。PID 变化时必须清空旧基线，否则会把两个不同
进程的数据错误拼接。

### 7.9 WebRTC 与 OSD 网络边界

gatewayd 的 9080 HTTP 接口包含控制能力但没有认证，因此只监听 `127.0.0.1`，演示时通过
SSH 转发。浏览器使用板卡真实 IP 直接连接 MediaMTX 的 8889/TCP 信令和 8189/UDP ICE
媒体。OSD 是 HTML/CSS/JavaScript 覆盖层，不修改视频帧，也不会写进录像。

## 8. MPP、RGA 和 H.264 应该怎样回答

### MPP

MPP 是 Rockchip 的媒体处理平台。FFmpeg-Rockchip 通过 `h264_rkmpp` 等组件将 H.264
解码和编码任务提交给 RK3588 的硬件视频单元，避免完全依赖 CPU 软件编解码。

### RGA

RGA 是 Rockchip 的二维图像加速单元，适合缩放、裁剪、旋转和像素格式转换。项目使用
`scale_rkrga` 将这些图像处理步骤从 CPU 软件路径转移到硬件路径。

### H.264

H.264 是视频压缩标准。项目的默认输出是 H.264、25 FPS，GOP 为 `2 * fps`，即约两秒
一个关键帧。这是在压缩效率和新播放器等待关键帧时间之间的折中，不代表端到端延迟必然
低于两秒。

不要声称项目实现了 H.264 算法；项目只是配置并监督硬件编解码链路。

## 9. 实测结果怎样讲

必须区分“最大吞吐微基准”和“实时摄像头运行”。

### 固定文件、无实时限速微基准

| 1080p25 转码方案 | CPU 平均 | RSS 平均 | 处理能力 |
| --- | ---: | ---: | ---: |
| 软件解码 + libx264 | 306.7% | 158.7 MiB | 12.46 FPS / 0.498x |
| MPP 解码 + MPP 编码 | 75.4% | 17.2 MiB | 492.93 FPS / 19.7x |
| MPP 解码 + RGA + MPP 编码 | 75.2% | 18.1 MiB | 497.70 FPS / 19.9x |

这些数据用于比较同一固定样本的最大处理能力，不能说真实摄像头在以 497 FPS 播放。

### 真实在线单路

最后一次实板恢复后记录为 H.264 1920×1080、约 25.11 FPS、丢帧 0、板端 FFmpeg CPU
约 20.97%、RSS 约 15.58 MiB。短时 RTSP 画面年龄测试约为 `1.25 ± 0.5` 秒，但没有完成
浏览器亚秒延迟专项调优。

性能结论应说“硬件路径显著降低 CPU/RSS 并满足实时处理”，不要把短时测试外推为长期
稳定性或生产容量保证。

## 10. 高频面试问题

### 为什么不直接在 C 程序里链接 FFmpeg 库？

独立进程更容易先验证命令行硬件链路，并提供通道级故障隔离、独立 PID、资源统计和重启。
代价是进程创建和管道管理开销；如果以后追求更低延迟或更细粒度帧控制，可以评估
libav*，但复杂度和故障影响面也会增加。

### 为什么使用 MediaMTX？

RTSP、WebRTC、录像和回放都是成熟而复杂的协议能力。使用专门服务器能让本项目聚焦于
嵌入式进程监督和硬件媒体链路，避免重复造协议栈。

### 为什么使用 C？

核心问题涉及 POSIX 进程、信号、管道、线程、锁、`/proc` 和可预测资源管理，C 能直接
表达这些系统边界，也符合嵌入式 Linux 常驻服务场景。浏览器页面和部署脚本仍分别使用
HTML/JavaScript 和 Bash，不为了“全用 C”牺牲可读性。

### 摄像头断流后网关会控制摄像头重启吗？

不会。网关重启的是板端 FFmpeg 会话并重新连接 RTSP。远端摄像头完全死机时，需要摄像头
自身看门狗、ONVIF/厂商接口或可管理 PoE 供电设备。

### 为什么需要两套 MediaMTX？

只在 USB 摄像头演示中需要。PC MediaMTX 把 USB 摄像头模拟成 RTSP 输入；板端 MediaMTX
分发 RK3588 处理后的输出。真实 RTSP 网络摄像头可以直接连接板卡，不需要 PC MediaMTX。

### 如何避免密码出现在日志？

真实 URL 通过受保护环境文件注入；探测和 stderr 诊断输出密码前会使用专门的 URL
脱敏函数替换敏感部分。FFmpeg 参数不经过 shell。

### 如何保证一个通道故障不影响其他通道？

每通道拥有独立 supervisor、FFmpeg 进程、状态快照和恢复计数；Channel Manager 只对变化
或故障通道执行生命周期操作。开发机已验证隔离逻辑，但两个真实摄像头在板卡上的最终
验收仍是 `PENDING`，面试时必须如实说明。

### 是否实现了零拷贝？

不能只根据 `drm_prime`、MPP 和 RGA 参数就宣称完整零拷贝。当前可以确认使用了硬件媒体
路径并取得明显性能收益；完整零拷贝需要进一步跟踪帧格式、DMA buffer 和各组件映射行为。

### 项目安全吗？

第一版不是公网产品。HTTP 控制接口没有认证，因此只监听回环地址并通过 SSH 访问；WebRTC
仅用于可信测试局域网。公网部署还需要 TLS、认证、授权和网络访问控制。

## 11. 当前限制

- 当前只有视频，FFmpeg 参数使用 `-an`，没有音频采集、转码或录像；
- 单路真实摄像头已经通过，双真实输入板卡验收尚未完成；
- 没有 AI 检测；
- 没有复杂的用户、告警和录像管理前端；
- 没做 30 分钟及更长稳定性测试；
- 没有宣称完整零拷贝或亚秒 WebRTC 延迟；
- API 没有认证，不能直接暴露到不可信网络。

主动说明边界不会减分，反而能证明你理解证据范围和工程取舍。

## 12. 源码阅读顺序

```text
src/main.c
  -> src/config/config.c
  -> src/channel/channel_manager.c
  -> src/channel/supervisor.c
  -> src/pipeline/pipeline_builder.c
  -> src/process/process_manager.c
  -> src/monitor/progress_parser.c
  -> src/monitor/process_metrics.c
  -> src/api/http_server.c
```

阅读时始终追踪三个对象：

- 配置从 YAML 节点怎样进入 `gw_config`；
- 一个通道怎样从 `STARTING` 走到 `RUNNING` 或恢复状态；
- 一个 FFmpeg PID 怎样被创建、观察、终止和回收。

## 13. 三分钟演示讲解顺序

1. 用架构图说明控制平面与数据平面，以及自己实现的范围。
2. 打开带 OSD 的真实画面，指出状态、PID、FPS、CPU/RSS 和丢帧。
3. 停止 PC 推流，展示通道进入退避且实时字段清空。
4. 恢复推流，展示同一个 gatewayd 自动创建新 FFmpeg PID 并恢复画面。
5. 展示软件与 MPP/RGA 的性能对比，同时说明这是固定样本微基准。
6. 主动说明没有音频、AI和双真实输入最终验收。

面试时最重要的不是背函数，而是能清楚回答：为什么这样拆分、故障发生时系统怎样变化、
资源由谁拥有，以及你的结论由什么真实证据支持。
