# 开发状态与上下文恢复记录

本文档是开发者恢复项目上下文的首要入口。每完成一个可独立验证的功能增量，都必须
在提交代码前更新本文件和 `docs/test-plan.md`，不能只在对话中记录进度。

## 1. 固定开发方法

每个增量严格按以下顺序进行：

1. 从本文档确认当前阶段、约束和下一项任务。
2. 实现一个职责明确、可以独立测试的最小增量。
3. 增加或更新自动化测试。
4. 更新 `docs/test-plan.md` 中的测试步骤、预期结果和实际记录。
5. 更新本文档中的完成项、遗留项和下一步。
6. 运行常规测试；涉及内存、进程或缓冲区时同时运行 Sanitizer。
7. 执行 `git diff --check`，确认没有显式 `(void)函数调用` 写法。
8. 创建只包含该增量的独立 Git 提交。
9. 确认 Git 工作区干净后再开始下一个增量。

如果对话被自动 compact 或由新的开发会话接手，先读取：

```text
docs/development-status.md
docs/test-plan.md
ARCHITECTURE.md
git log --oneline --decorate -10
git status --short --branch
```

## 2. 固定技术约定

- 实现语言：C17。
- 构建系统：CMake。
- YAML 库：libyaml。
- 进程创建：`posix_spawnp()`，禁止通过 shell 拼接配置参数。
- 每个媒体通道使用独立 FFmpeg 进程组。
- stdout 只解析 FFmpeg `-progress` 结构化输出。
- stderr 必须在输出前隐藏 URL 密码。
- HTTP API 默认只允许监听回环地址。
- 不使用 `(void)printf(...)`、`(void)memcpy(...)` 等显式丢弃返回值写法。
  `function(void)` 表示 C 函数无参数，不属于这一限制。
- `README.md` 面向使用者；详细开发测试只写入 `docs/`。
- 未在 RK3588 实机验证的数据不得写成已完成结果。

## 3. 当前阶段

当前阶段：Phase 4——控制接口、录像和服务化。开发机软件路径状态为 `COMPLETE`；
完整 Phase 4 仍为 `IN PROGRESS`，等待 RK3588、真实 RTSP、MediaMTX 和 systemd 实机验收。

当前开发机测试基线：

```text
日期：2026-08-06
平台：x86_64 Arch Linux
编译器：GCC 16.1.1
常规 CTest：26/26 PASS
ASan/UBSan：26/26 PASS
TSan（录像状态/HTTP/通道管理器/重载相关）：5/5 PASS
GCC -fanalyzer：构建完成；9 条跨函数资源所有权/单字符缓冲区路径告警已人工复核
LeakSanitizer：当前 ptrace 环境不支持，尚未完成
RK3588 MPP/RGA：当前开发机不具备，等待板卡验收
```

当前 RK3588 基线：

```text
日期：2026-08-06
平台：LubanCat aarch64，Debian 11，Linux 5.10.160
源码：板卡无 .git；关键源文件校验值与本机 10e3432 一致
CMake：3.31.10
FFmpeg-Rockchip：388741a，rkmpp/rkrga 检查 PASS
MediaMTX：v1.20.0 linux arm64
设备访问：MPP、RGA、DRM PASS
环境检查：0 failures，0 warnings
板卡完整 CTest：26/26 PASS
Phase 0：PASS
```

## 4. 已完成增量

### 530a338：C17 单通道基础基线

- YAML 配置加载、环境变量展开与校验。
- URL 密码脱敏。
- FFmpeg 参数数组构造。
- progress 增量解析。
- 单工作进程创建、双管道、停止与回收。

### 543a925：状态机、超时与退避重试

- `STOPPED`、`PROBING`、`STARTING`、`RUNNING`、`BACKOFF`、`FAILED`。
- 启动超时和 progress 停滞超时。
- 有上限的指数退避、最大重试次数和总重启计数。
- 正常退出、超时、重试耗尽和密码脱敏端到端测试。

### eac43d1：模块契约与不变量注释

- 记录公共接口职责、资源所有权、管道行为和状态机约束。

### f305c16：稳定运行窗口

- 新增 `stable_run_sec` 配置，默认 60 秒。
- 达到稳定窗口后清零连续失败次数，保留总重启次数。
- 增加持续 progress 端到端测试。

### 6373881：ffprobe 参数与结果解析

- 使用独立参数数组构造 ffprobe 命令，不经过 shell。
- 固定读取首个视频流的 `codec_name`、`width` 和 `height`。
- 支持 LF 与 CRLF 输出，拒绝缺失或非法字段。
- 校验 H.264/H.265 探测结果与配置的 RKMpp 解码器是否匹配。

### 4034726：真实输入探测执行链路

- supervisor 在每次启动或重试 FFmpeg 前执行独立 ffprobe 子进程。
- 新增 `probe_timeout_sec`，与 FFmpeg 的 `startup_timeout_sec` 分开计时。
- 限制并解析探测 stdout，探测 stderr 与工作进程日志使用相同的 URL 密码脱敏。
- 区分 `probe_failure`、`probe_timeout`、`probe_mismatch` 和工作进程失败。
- 探测失败使用现有退避及最大重试预算；探测成功后才从 `PROBING` 进入
  `STARTING`。
- 假 ffprobe 端到端覆盖成功、失败、超时、编码不匹配和重试耗尽。

### 997c096：独立 supervisor 模块

- 将探测、工作进程监督、progress 处理、超时、退避和重试从 `main.c` 迁入
  `src/channel/supervisor.c`。
- 新增 `include/gateway/supervisor.h`，由 CLI 注入 ffprobe/FFmpeg 路径和只读停止
  信号；supervisor 不再安装或拥有进程级信号处理器。
- `main.c` 只负责参数解析、配置加载、dry-run、单通道选择和发布停止信号。
- 新增 supervisor 公共接口测试，覆盖默认选项、非法参数和探测阶段停止/回收。
- 原有 13 项端到端与模块测试保持行为不变，当前总计 14 项。

### 773290c：单通道状态快照

- 新增 `gw_channel_snapshot`，保存通道状态、最近事件、活动进程类型/PID、退出码、
  探测信息、最近 progress、连续失败数、退避和总重启数。
- supervisor 在探测、启动、progress、稳定、失败、退避和停止等关键点同步发布
  只读快照。
- 观察者回调只在 supervisor 线程内同步调用；未来多通道管理器负责复制并加锁保存，
  当前没有宣称具备线程安全的 HTTP 查询能力。
- 新增纯快照单元测试，并扩展 supervisor 测试验证
  `PROBING -> STARTING -> RUNNING -> STOPPED` 和最终指标。

### 4f66f7f：多通道管理器

- 新增 POSIX 线程多通道管理器，每个启用通道拥有独立 supervisor 线程。
- 使用读写锁保存每个通道的最新快照，查询时复制数据，不暴露 supervisor 临时内存。
- 使用原子停止状态协调管理器内部停止，并继续响应 CLI 发布的 SIGINT/SIGTERM。
- `gatewayd` 不再限制只能启用一个通道，会并行启动配置中的所有启用通道。
- 双通道测试覆盖全部成功、统一停止，以及一个通道工作进程失败而另一个正常完成。
- 多通道管理器和 CLI 测试通过 ThreadSanitizer；尚未进行 RK3588 多路性能测试。

### a5542c7：SIGHUP 差异化配置重载

- 主线程接收 SIGHUP 后重新读取同一 YAML 路径，候选配置完整解析和校验通过前不修改
  任何运行通道。
- 管理器按通道 ID 比较配置；未变化通道保持原线程和配置代次，变化通道停止后重启，
  并支持新增、禁用或删除通道。
- MediaMTX 发布地址或重试默认值变化时重启所有受其影响的通道；仅 server 字段变化
  不重启媒体工作进程。
- 每个通道使用独立原子停止源，配置代次按通道而非槽位递增。
- SIGINT/SIGTERM 只由主线程读取信号标志，再通过原子状态广播，消除工作线程直接读取
  `sig_atomic_t` 造成的数据竞争。
- CLI 测试覆盖合法重载、非法候选回滚、未变通道不中断和最终统一停止。

### e2f7e01：输入断开后自动恢复

- 为假 FFmpeg 增加确定性一次性断流模式：同一输入第一次启动在进入 `RUNNING` 后
  非零退出，退避后的第二次启动保持运行，用独占创建临时标记文件协调两个进程。
- 新增独立 `gateway_input_recovery_tests`，双通道运行时验证故障通道经历
  `worker_failure -> BACKOFF -> PROBING -> RUNNING`，总重启数为 1。
- 同一测试验证健康通道保持 `RUNNING`、总重启数为 0、配置代次不变，并继续检查日志
  密码脱敏。
- 测试使用原子观察标志记录并发回调事件，并通过 ThreadSanitizer 验证。
- 这是开发机故障注入，不代表真实 RTSP 恢复或重新发布到 MediaMTX 已通过。

### 3815eab：输出发布失败后自动恢复

- 将恢复夹具扩展为输入和发布两种模式；发布模式只根据 FFmpeg 输出 URL 触发首次
  非零退出，不改变输入探测结果。
- 新增 `gateway_publish_recovery_tests`，验证发布工作进程失败后通道独立退避、重新
  探测并恢复到 `RUNNING`，健康通道不重启。
- 输入和发布两个场景共用相同的生命周期断言，并分别使用独立临时标记文件，避免
  测试间共享状态。
- 该测试未连接 MediaMTX，只证明 gateway 对“FFmpeg 因发布端故障退出”的恢复路径；
  真实 RTSP 发布和 MediaMTX 停止/启动仍属于实机验收。

### f30682d：Phase 3 开发机审计与实机清单

- Phase 3 的开发机软件路径已覆盖多通道隔离、有限退避、工作进程失败、输入/发布
  故障恢复和差异化重载，测试证据逐项记录在 `docs/test-plan.md`。
- 完整 Phase 3 仍为 `IN PROGRESS`，因为当前环境没有 RK3588、真实 RTSP 源或
  MediaMTX，未将夹具结果替代为实机结果。
- `docs/test-plan.md` 已增加可执行的板卡验收清单和记录模板，包含基线播放、输入断流、
  工作进程崩溃、MediaMTX 停启、SIGHUP、密码脱敏和资源清理。

### bb0c0a9：只读 HTTP 服务

- 新增独立 `src/api/http_server.c` 模块，提供有界 HTTP/1.0/1.1 请求读取、JSON
  序列化和回环 socket 服务。
- 提供 `GET /v1/health`、`GET /v1/channels` 和
  `GET /v1/channels/{id}`，通道快照在管理器读锁下批量复制。
- JSON 只暴露状态、进程、探测和 progress 指标，不包含输入 URL；字符串统一转义，
  非有限浮点数输出为 `null`。
- 非 GET 返回 405，未知资源返回 404，超过 8192 字节的请求头返回 431；响应包含
  `nosniff` 和 `Connection: close`。
- 新增 `server.enabled`；默认启用并监听 `127.0.0.1`，测试夹具可显式关闭。监听地址
  只接受数值 IPv4/IPv6，监听配置变化通过 SIGHUP 明确拒绝并要求进程重启。
- 单元测试覆盖路由、列表容量和敏感信息边界；CLI 端到端测试覆盖真实回环连接、三类
  查询、404、405、431、SIGTERM 和密码脱敏。

### 25f658f：HTTP 通道生命周期控制

- 新增 `POST /v1/channels/{id}/start`、`stop` 和 `restart`；成功返回 202，未知通道
  返回 404，重复启动、重复停止或管理器退出期间的命令返回 409。
- 通道管理器使用生命周期互斥锁串行化 HTTP 控制与 SIGHUP 差异化重载；每个槽位增加
  原子线程运行标志，避免刚启动但快照尚未更新时把可回收线程与活动线程混淆。
- 网关默认作为常驻服务运行，全部通道停止或失败后仍能接受恢复命令；新增
  `--exit-when-idle` 保留批处理测试和一次性运行的退出方式。
- 管理器测试覆盖停止、启动、重启、未知通道、非法状态，以及两个线程同时启动同一
  通道时恰好一个成功。真实 HTTP 端到端测试覆盖状态轮询、PID 更换、404/405/409、
  SIGTERM 和敏感信息边界。
- HTTP 当前没有身份认证；默认回环监听是安全边界，不得直接暴露到不受信任网络。

### 1e4d881：MediaMTX 录像配置生成

- `mediamtx.recording` 新增录像启用、绝对目录、fMP4/MPEG-TS 格式、part 大小与周期、
  segment 周期、自动删除周期、最低空闲空间阈值和回放监听配置。
- 配置校验限制绝对目录字符、数值范围、回放数值 IP、端口和重复 MediaMTX 输出路径。
- `gatewayd --print-mediamtx-config` 生成每通道 MediaMTX `paths` 配置；启用通道录像，
  禁用通道保留路径但关闭录像，并开启仅回环 playback 服务。
- 生成器不复制输入 URL，因此不会把 RTSP 密码写入 MediaMTX 文件；容量不足和非法配置
  明确失败。提供与当前 MediaMTX v1.20 配置字段一致的示例文件。
- MediaMTX 仍是独立服务，gatewayd 不创建、重载或伪造其运行结果。录像参数变化需要
  重新生成配置并由部署流程重载 MediaMTX。

### 0396124：录像磁盘状态管理

- 新增纯 `statvfs()` 录像状态模块，按当前服务用户可用块计算总容量与可用容量，不扫描
  录像文件，也不接触 MediaMTX 正在写入的分段。
- 新增 `GET /v1/recording`，返回启用状态、`disabled`/`ok`/`low_space`/
  `unavailable`、容量和最低空闲阈值；不暴露服务器目录路径。
- 录像启用且空间低于 `min_free_mb`，或录像目录不可访问时，`GET /v1/health` 返回
  `degraded` 并给出录像状态；通道实时转码不会因此被停止。
- 空间回收由已生成的 MediaMTX `recordDeleteAfter` 策略负责。gatewayd 不直接删除录像
  文件，避免与 MediaMTX 写入竞争或产生路径穿越风险。
- 单元测试覆盖关闭、正常、低空间、目录不可用和参数错误；HTTP 单元与真实回环测试
  覆盖 JSON、健康降级、GET-only、目录隐藏和密码边界。

### 3cd9ffe：systemd 服务化与优雅退出

- 新增 gatewayd 与独立 MediaMTX 的 systemd 单元；使用同一非 root 服务账号共享录像
  目录，MediaMTX 仍不是 gatewayd 子进程。
- gatewayd 单元通过 EnvironmentFile 注入凭据，支持 `systemctl reload` 转换为 SIGHUP，
  异常退出按 `Restart=on-failure` 恢复，正常停止使用 SIGTERM。
- 两个服务均配置 control-group 清理、停止超时、写目录白名单、NoNewPrivileges、严格
  文件系统保护和多项内核/权限加固。
- tmpfiles 规则创建配置、状态与录像目录；CMake 安装规则包含二进制、示例配置、服务
  单元、环境示例和部署文档。
- 部署静态测试检查关键退出、重启、权限和路径配置；HTTP 端到端增强为 SIGTERM 后同时
  验证 gatewayd 正常退出、最终 FFmpeg PID 消失且 HTTP 监听关闭。
- TSan 在增强测试中发现异步处理器可能落到任意工作线程。现改为主线程通过 Linux
  `signalfd` 同步消费 SIGHUP/SIGINT/SIGTERM，所有工作线程继承屏蔽掩码；
  `posix_spawn` 明确为 ffprobe/FFmpeg 恢复空掩码与默认信号动作，保留优雅停止能力。
- 开发机没有以 PID 1 运行测试单元。`systemd-analyze verify` 已解析单元，仅因开发机
  `/usr/local/bin` 尚未安装目标二进制而返回缺失命令；真实 enable/start/restart 仍必须
  在板卡执行，未伪造为通过。

### 本次增量：Phase 4 开发机收尾审计

- 依次执行完整常规 CTest、ASan/UBSan 和适用的 TSan，最终分别为 26/26、26/26 和
  5/5 PASS；LeakSanitizer 因当前 ptrace 环境关闭，未写成已通过。
- 三套测试首次并行执行时，常规套件的 1 秒 progress 超时因 CPU 饥饿出现一次失败；
  单独重跑完整常规套件 26/26 PASS。最终验收固定为依次执行，避免测试程序互相争用。
- GCC `-fanalyzer` 完成全量构建。剩余 9 条告警均已对照控制流复核：HTTP 客户端由
  `run_server()` 关闭、监听描述符由 stop/destroy 关闭、管道失败路径逐项关闭；另两条
  来自已初始化的两字节单字符缓冲区。常规与 Sanitizer 测试未发现对应资源泄漏或
  未初始化读取，未以“静态分析零告警”表述结果。
- `git diff --check` 通过；源码未发现显式 `(void)函数调用`、`system()`/`popen()`，受管
  源码和部署文件中未发现带内嵌凭据的 URL。夹具密码只存在于测试输入并持续验证脱敏。
- README、开发状态、测试计划和部署文档的职责与边界已复核。开发机软件路径完成不代表
  真实 MediaMTX 录像、RK3588 硬件转码或 systemd 生命周期已经通过。

### f672c7d：Phase 0 RK3588 环境基线

- 通过 SSH 复用连接核验目标为 `cat@192.168.1.45`、aarch64，并在任何板卡操作前确认
  `/home/cat/rk3588-media-gateway`；板卡目录不是 Git 仓库，因此使用关键源文件 SHA-256
  与本机 `10e3432` 对照，结果一致。
- 项目环境检查在 Debian 11 / Linux 5.10.160 上为 0 failures、0 warnings；`cat` 用户可
  访问 MPP、RGA 和至少一个 DRM 节点，FFmpeg-Rockchip 提供 h264/hevc RKMpp 编解码器
  和 `scale_rkrga`，MediaMTX 版本为 v1.20.0。
- 使用板卡用户级 CMake 3.31.10 重新配置并构建 Release，随后完整 CTest 26/26 PASS，
  总用时 24.28 秒。日志保存在板卡仓库外的
  `/home/cat/rk3588-acceptance/2026-08-06/`。
- 板卡 systemd 的 `degraded` 来自项目外 `rkwifibt.service`，没有误写成 gateway 故障；
  Phase 0 只标记环境基线通过，不替代真实 RTSP、录像或服务生命周期验收。

### 531f3a2：真实 PC 摄像头 RK3588 媒体冒烟

- PC 集成摄像头原生上限为 MJPEG 1280×720@30fps；PC FFmpeg 实时上采样并编码为
  H.264 1920×1080@25fps，通过 PC 临时 MediaMTX 向板卡提供真实网络 RTSP 输入。
  记录中没有把上采样流描述成摄像头原生 1080p。
- gatewayd 在 RK3588 上探测到 H.264 1920×1080，实际启动 `h264_rkmpp` 解码、
  `scale_rkrga` 和 `h264_rkmpp` 编码并发布到板卡 MediaMTX。至少连续运行 10 分 44 秒，
  最终为 25.04fps、speed 1.00、0 丢帧、0 重启。
- PC FFmpeg 通过局域网实际读取板卡 RTSP 输出；MediaMTX 记录 WebRTC peer connection
  established 和读取 `cam01`，用户在 PC 浏览器确认能够看到实时摄像头画面，主观延迟
  约 1～2 秒。
- 当前媒体链路显式使用 `-an`，没有音轨，MediaMTX 录像也只能得到视频。音频采集、
  透传或编码属于后续独立功能，不在本轮实机验收中伪装为已支持。
- 一次发布 `SETUP` 先返回 461，随后成功发布且稳定运行；该兼容性现象保留待后续明确
  输出 RTSP 传输方式。原 Phase 1 的 30 分钟标准尚未执行，当前只标记 5 分钟冒烟通过。

### 本次增量：RK3588 HTTP 生命周期控制验收

- 在 PC 真实摄像头、跨主机 RTSP、RK3588 RKMpp/RGA 和板卡 MediaMTX 均在线的实际
  链路上验证健康、通道列表、单通道与录像状态查询，四个接口均返回 HTTP 200。
- stop 返回 202 后通道进入 `STOPPED`，原工作进程 PID 628358 消失且板卡 RTSP 输出
  不可读；重复 stop 返回 409。start 返回 202，紧接的重复 start 返回 409，新 PID 为
  676117，H.264 1920×1080@25 输出恢复。
- restart 返回 202，工作进程再次更换为 PID 677115；板卡 ffprobe 与 PC 跨主机读取均
  确认输出恢复。未知通道返回 404，GET 控制动作返回 405，最终健康状态为 ok。
- 验收日志未出现输入 URL，保存在板卡仓库外
  `/home/cat/rk3588-acceptance/2026-08-06/phase4-http-control.log`。开发机并发单元测试的
  锁竞争覆盖仍作为并发证明，板卡测试补充真实进程和真实媒体恢复证据。

## 5. 当前能力边界

已经具备：

- 多个启用通道的独立线程和工作进程监督。
- 每次工作进程启动前执行 ffprobe，校验首个视频流的编码和尺寸。
- 可通过同步观察回调取得单通道生命周期快照。
- 结构化进度、最终指标和受控 stderr 日志。
- 信号停止、超时清理、自动重试和失败终态。
- SIGHUP 候选配置校验与按通道差异化重载。
- 开发机夹具模拟输入或发布端中断后，故障通道可独立退避并恢复运行。
- 本地 HTTP 健康检查、通道状态查询，以及并发安全的启动、停止和重启控制。
- MediaMTX 录像配置生成、自动保留参数和录像文件系统健康状态。
- gatewayd/MediaMTX systemd 单元、非 root 权限边界与部署文档。
- 开发机假工作进程端到端验证。

尚未具备：

- RK3588 板卡上的真实 RTSP/ffprobe 与 MPP/RGA 联调。
- 真实 MediaMTX 发布端停止、恢复与重新发布联调。
- 真实 MediaMTX 录像、回放、自动删除与磁盘阈值联调。
- systemd 开机启动、正常停止、崩溃恢复和设备权限验收。
- RK3588 真实硬件转码与稳定性/性能数据。

## 6. 下一步队列

按顺序执行：

1. 在 RK3588 板卡安装 MediaMTX 和生成配置，验证真实发布、录像与回放。
2. 验证自动删除、低磁盘空间状态和服务账号对设备/录像目录的权限。
3. 执行 systemd enable、重启、正常停止和崩溃自动恢复验收并保存日志。
4. 板卡验收通过后更新 Phase 4 状态，再进入 Phase 5 性能与稳定性测试。

## 7. 文档职责

- `README.md`：用户安装、配置和运行方法。
- `ARCHITECTURE.md`：架构、模块职责、阶段与最终验收标准。
- `docs/test-plan.md`：开发者可重复执行的测试和实际验收记录。
- `docs/development-status.md`：当前进度、关键约定、提交和下一步。
- `docs/benchmark-results.md`：后续记录可复现的性能数据。
