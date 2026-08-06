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

当前阶段：Phase 5——性能、稳定性与最终交付，状态为 `IN PROGRESS`。用户已明确要求
继续推进；先建立短时可重复基线，不执行 30 分钟或更长的稳定性测试。

当前开发机测试基线：

```text
日期：2026-08-06
平台：x86_64 Arch Linux
编译器：GCC 16.1.1
常规 CTest：28/28 PASS
ASan/UBSan：28/28 PASS
TSan（适用的录像/HTTP/通道管理器/重载/故障恢复测试）：7/7 PASS
GCC -fanalyzer：构建完成；9 条跨函数资源所有权/单字符缓冲区路径告警已人工复核
LeakSanitizer：当前 ptrace 环境不支持，尚未完成
RK3588 MPP/RGA：开发机不具备；已在下述目标板卡单独验收
```

当前 RK3588 基线：

```text
日期：2026-08-06
平台：LubanCat aarch64，Debian 11，Linux 5.10.160
源码：板卡无 .git；新增文件校验值与本机功能提交 4ffa28d 一致
CMake：3.31.10
FFmpeg-Rockchip：388741a，rkmpp/rkrga 检查 PASS
MediaMTX：v1.20.0 linux arm64
设备访问：MPP、RGA、DRM PASS
环境检查：0 failures，0 warnings
板卡完整 CTest：28/28 PASS
Phase 0、1、2、4：按当前单路和 5 分钟以上实机范围 PASS
Phase 3：开发机双通道与实板单路故障恢复 PASS；双真实输入板卡验收未执行
限制：无音频；未执行 30 分钟长稳
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

### ca25dd1：RK3588 HTTP 生命周期控制验收

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

### 本次增量：RK3588 MediaMTX 录像与磁盘状态验收

- 在板卡使用真实 `gatewayd --print-mediamtx-config` 生成录像配置，MediaMTX v1.20.0
  将 PC 摄像头转码输出录制到板卡本地
  `/home/cat/rk3588-acceptance/recordings`；录像不是存放在 PC。
- 使用 1 秒 part、5 秒 segment 和 15 秒 delete-after 进行可观察验收。MediaMTX 持续
  生成非空 fMP4 分段；回放服务列出实际时间段，下载的 5 秒 MP4 为 3,747,338 bytes，
  ffprobe 确认为 H.264 1920×1080@25fps。
- 记录一个具体最旧分段后等待 20 秒，该文件由 MediaMTX 自动删除且新分段继续生成；
  gatewayd 没有主动删除文件，也没有与 recorder 竞争。
- 正常阈值下录像状态与健康状态均为 ok。将 `min_free_mb` 临时提高到 40000，仅通过
  比较模拟低空间，不写入填充文件；录像状态变为 low_space、健康状态 degraded，实时
  通道仍为 RUNNING。恢复 1 MiB 阈值后两个状态回到 ok。
- 当前录像只有 H.264 视频轨，音频因 pipeline 的 `-an` 未进入发布流。真实证据保存在
  板卡仓库外；该录像增量完成时仍待故障恢复和 systemd 验收，后续增量已经补齐。

### 25e1a3f / d4a8b77：常驻工作进程零退出恢复修复

- 在真实 PC 摄像头断流验收中，FFmpeg 读取上游 EOF 后以退出码 0 结束；旧逻辑把它记为
  `clean_exit` 并将通道停在 `STOPPED`，因此没有执行既定的退避和重新探测。这是实机发现
  的恢复缺陷，不能写成断流验收通过。
- 常驻模式现将所有未经 stop 请求的工作进程退出都归入 `worker_failure`，即使退出码为
  0 也会消耗重试预算并进入 `BACKOFF`。显式 `--exit-when-idle` 模式仍允许测试夹具以 0
  完成，保留一次性端到端测试语义。
- supervisor 单元测试新增退出码 0 的失败终态断言；原 clean-exit、通道管理、输入恢复和
  发布恢复测试继续通过。HTTP 端到端测试还会在端口启动失败时打印 gateway 子进程日志，
  便于区分应用错误和执行环境限制。
- 本增量完整常规 CTest 26/26、ASan/UBSan 26/26、适用 TSan 7/7 PASS。TSan HTTP 首次
  在受限沙箱中因禁止绑定回环临时端口失败，取得明确 `Operation not permitted` 日志后在
  允许回环监听的同机环境复跑通过；没有把沙箱失败算作功能通过。
- 修复源码已同步到板卡并完成 Release 重建；板卡完整 CTest 26/26 PASS。真实断流复测结果
  记录在下一增量，不以开发机测试替代实机结果。
- 修复首次在板卡 GCC 上构建时，编译器对受 `received_progress` 短路保护的
  `running_since` 给出可能未初始化警告；显式零初始化消除该跨编译器告警，不改变稳定窗口
  语义。板卡用户级 CTest 在修复源码上为 26/26 PASS。

### 本次增量：RK3588 故障恢复验收

- 修复后二次停止 PC 摄像头推流，板卡 FFmpeg 在上游 EOF 后仍以 0 退出，但通道正确记录
  `worker_failure -> BACKOFF`；输入离线期间探测失败，退避按 1、2、4、8、16、30 秒封顶。
  恢复 PC 摄像头后重新探测成功，新工作 PID 762652 回到 `RUNNING`；60 秒稳定窗口后连续
  失败计数从 7 清零，累计重启数保留。
- 精确向 PID 762652 发送 SIGKILL 后，通道记录退出码 137 和 1 秒退避，经探测后以新 PID
  767868 恢复 `RUNNING`。没有使用宽泛的 `pkill`，gatewayd 与 MediaMTX 未被误停。
- 精确停止 MediaMTX PID 692980 后，gatewayd 保持运行且没有代为启动 MediaMTX；FFmpeg
  因 broken pipe 以 224 退出，并在 MediaMTX 离线期间持续有限退避。使用同一录像配置恢复
  MediaMTX 为 PID 770462 后，gatewayd 以新工作 PID 770861 重新发布。
- 恢复后板卡本地探测为 H.264 1920×1080@25，PC 跨主机探测的平均帧率为 25fps；录像目录
  继续生成非空 MP4，回放 API 返回新的时间段。MediaMTX 日志还记录来自 PC 的 WebRTC 会话
  重新建立并读取单路 H.264。
- 原始证据位于板卡仓库外的 `phase3-fault-recovery.log`、
  `phase3-input-disconnect-fixed.log`、`phase3-input-recovery-fixed.log`、
  `phase3-worker-crash-recovery.log` 和 `phase3-mediamtx-stop-recovery.log`。该增量之后执行的
  systemd 非 root 服务生命周期验收见后续记录。

### 244f5b9：systemd 实机权限与 MediaMTX v1.20 兼容修复

- 首次 systemd 启动没有伪装为通过：MediaMTX 因 v1.20 默认启用 MoQ、尝试在只读工作目录
  生成 `auto.crt` 而反复退出；gatewayd 服务账号虽属于 `video,render`，但
  `ProtectClock=yes` 间接启用封闭设备策略，RKMPP 无法取得 allocator。
- MediaMTX 生成器现显式关闭未使用的 RTMP、HLS、SRT 和 MoQ，仅保留 TCP RTSP、WebRTC
  与按需 playback；RTSP 发布监听限制到回环地址。测试固定这些字段，避免后续 MediaMTX
  版本新增协议默认开启。
- gatewayd 单元保留 `ProtectClock=yes`，并以 `DeviceAllow` 最小放行 MPP、RGA、板卡现有
  DMA heap、DRM card0 与 renderD128。临时 systemd 单元已证明单独启用 ProtectClock 会
  复现失败，加入精确设备白名单后同一非 root 账号完成 3 秒硬件解码、RGA 与编码。
- 下一步在开发机完成常规和 sanitizer 回归，重新安装单元与配置后再进行正式 systemd
  启停、异常恢复和开机启动验收。

### 54f5e91：HTTP 描述符跨 exec 泄漏修复

- systemd 监听审计发现 FFmpeg 持有 gatewayd 的 9080 监听 socket。原因是 HTTP listener
  和 accept socket 创建时未原子设置 close-on-exec，后续通道重启的 posix_spawn 会继承
  当时打开的网络描述符。
- listener 改用 `SOCK_CLOEXEC` 创建，客户端改用 `accept4(..., SOCK_CLOEXEC)`，避免多线程
  环境中先 accept 再 fcntl 的竞态窗口。
- HTTP 端到端测试在 HTTP 已监听后执行通道 restart，并扫描最终假 FFmpeg 的 `/proc/PID/fd`；
  工作进程若继承任何 socket 会直接失败。下一步完成三套回归并再次部署板卡。
- 修复后完整常规 CTest 26/26、ASan/UBSan 26/26 和适用 TSan 7/7 PASS；下一步同步板卡
  并用真实 FFmpeg `/proc/PID/fd` 复核 9080 socket 不再被继承。

### 94d7e47：RK3588 systemd 生命周期验收

- CMake 正式安装后创建无登录 `rk-media-gateway` 账号，UID/GID 997，补充组为 video(44)
  与 render(107)。配置目录 0750、YAML 0640、root 环境文件 0600、录像目录 0750；两个
  服务进程和 FFmpeg 均以 UID/GID 997 运行，没有改成 root。
- 修复后的 MediaMTX 与 gatewayd 单元均 enabled/active。监听审计确认 gateway HTTP 9080、
  RTSP 8554、playback 9996 只在回环地址；外部仅保留验收所需 WebRTC 8889/8189，未监听
  RTMP、HLS、SRT 或 MoQ 端口。
- 板卡重新构建后完整 CTest 26/26 PASS。真实 FFmpeg FD 包含自身 RTSP socket、MPP、RGA、
  DMA heap 和 dmabuf，但不包含 gatewayd 的 9080 listener inode，close-on-exec 修复通过。
- `systemctl stop` 使 gatewayd 以 ExecMainStatus=0 退出，旧 gateway/FFmpeg PID 均消失，
  MediaMTX 保持运行；再次 start 后以新 PID 恢复真实硬件通道。
- 精确 SIGKILL gatewayd 后，systemd 的 NRestarts 变为 1，旧 control group 工作进程被清理，
  新 gatewayd/FFmpeg 回到 `RUNNING`。精确 SIGKILL MediaMTX 后其 NRestarts 也变为 1，
  gatewayd 主 PID 不变，工作进程经历 broken pipe 和退避后重新发布。
- 故障恢复后 RTSP 为 H.264 1920×1080、平均 25fps，WebRTC 从 PC 自动重连，playback 返回
  新时间段且 `/var/lib/rk-media-gateway/recordings` 继续生成非空 MP4。原始证据保存在板卡
  仓库外的 `phase4-systemd-lifecycle.log` 和 `phase4-systemd-journal.log`。
- 该提交先记录停启和崩溃恢复；后续整机重启与缩短后的最终稳定窗口记录见下一增量。

### Phase 4 最终重启与五分钟稳定验收

- 整机重启前 boot ID 为 `761393ed-c372-439d-b7a8-afa94292ec7c`，重启后为
  `7ecb42c7-e1e2-45b6-b0b6-f8cfd51ef9a6`。两个已启用服务在新系统启动约 4 秒后自动
  进入 active；启动初期网络未就绪时通道按 1/2/4/8 秒退避，网络恢复后自动进入 RUNNING。
- 重启后的最终连续稳定窗口为 6 分 12 秒，超过用户要求的 5 分钟：H.264
  1920×1080@25，frame=9423，fps=25.19，speed=1.01，drop=0。gatewayd、MediaMTX 和
  FFmpeg 各 1 个实例，父子关系正确，僵尸进程为 0。
- 最终样本中 MediaMTX/gatewayd/FFmpeg 的 CPU 分别约 15.2%/0.1%/15.7%，RSS 分别约
  55.1/2.2/20.8 MiB，温度约 41.6°C；这些是单点验收数据，不替代 Phase 5 性能报告。
- 最小监听、配置权限、无凭据 URL、录像容量状态、RTSP 输出和 unit 静态验证均通过；
  `systemd-analyze security` 对两个服务均给出 6.5 MEDIUM，作为后续加固基线而非零风险声明。
- 重启后浏览器慢读曾触发 WebRTC 丢帧告警，录像器曾因绝对时间漂移重置一次；12:48 后
  未再出现应用告警，录像和媒体输出持续正常。这与用户观察到约 1～2 秒延迟一致，尚未做
  低延迟专项优化。
- 用户明确取消本轮 30 分钟测试，因此 30 分钟长稳记录为 `SKIPPED`，没有伪造为 PASS；
  Phase 4 按 5 分钟以上的修订验收范围完成。证据位于板卡仓库外的
  `phase4-systemd-boot*.log` 和 `phase4-five-minute-final.log`。
- 文档收尾后的最终顺序回归为常规 26/26、ASan/UBSan 26/26、适用 TSan 7/7 PASS。
  ASan 首次在受限沙箱运行时只有 HTTP 测试因回环 bind 被拒绝而失败；允许回环后完整
  复跑通过，错误证据为 `Operation not permitted`，未把该环境失败算成功能失败或通过。
- 验收结束后优雅停止板卡两个服务，均保持 enabled、状态为 inactive 且 ExecMainStatus=0；
  板卡 gatewayd/FFmpeg/MediaMTX 和僵尸进程计数均为 0。PC 临时摄像头 FFmpeg 与临时
  MediaMTX 也已停止，录像、配置和板卡证据未删除。

### Phase 5 增量：C17 运行指标采样工具

- 新增 `gateway-metrics`，可在同一单调时钟窗口内采样最多 16 个 Linux PID，输出
  elapsed、标签、PID、状态、CPU、RSS 和文件描述符数 CSV。100% CPU 明确定义为一个
  逻辑 CPU，首样本因没有前一 tick 基线而留空。
- `/proc/<pid>/stat` 解析从进程名最后一个右括号定位字段，兼容名称中的空格/右括号和
  前置有符号终端字段；`VmRSS` 必须以 kB 表示。任何目标消失都会输出 unavailable 并使
  工具非零退出，避免采样期间重启被静默忽略。
- 单元测试覆盖 stat/status 正常与非法输入、负终端字段和当前进程读取；CLI 端到端使用
  `self=self` 运行 1 秒并验证 CSV 中没有 unavailable。详细指标口径与待执行矩阵写入新的
  `docs/benchmark-results.md`，未产生的实板数据保持 PENDING。
- 完整常规 CTest 28/28、ASan/UBSan 28/28、既有适用 TSan 7/7 PASS；CMake 临时安装树
  同时包含 `gatewayd` 和 `gateway-metrics`。`git diff --check`、显式 `(void)` 调用和
  `system()`/`popen()` 扫描通过。

### Phase 5 增量：RK3588 短时软硬件性能基线

- `gateway-metrics` 已安装到板卡并通过 1 秒自采样；板卡 Release 构建和完整 CTest 为
  28/28 PASS。PC 摄像头 RTSP 抓取成 15 秒、14,972,221 bytes 的 H.264 1080p25 固定样本，
  SHA-256 已记录，且由软件解码器完整校验无错误。
- 第一轮直接读取在线 RTSP 的对比判为无效：软件解码出现宏块错误，MediaMTX 报告慢读并
  丢弃帧，两条硬件命令也产生重复帧。原始日志保留用于解释方法修订，未纳入性能表。
- 固定样本正式微基准在相同 12 秒墙钟窗口运行，2 秒预热后采样 10 秒。软件路径为
  306.7% CPU、158.7 MiB RSS、12.46fps/0.498x；MPP 为 75.4%、17.2 MiB、
  492.93fps/19.7x；MPP+RGA 为 75.2%、18.1 MiB、497.70fps/19.9x。三组均 0 丢帧且无告警。
- 三条路径各自生成的 3 秒 H.264 1080p25 Matroska 均通过 ffprobe 和完整软件解码，避免
  把 null muxer 的进度当成输出正确性证明。软件使用 Debian FFmpeg/libx264，硬件使用
  FFmpeg-Rockchip，报告明确限制结论为完整实现路径对比。
- 正式服务另做 10 秒同步基线：gatewayd/FFmpeg/MediaMTX CPU 平均值为
  0.2%/18.6%/9.1%，RSS 为 2.14/18.72/45.10 MiB；通道保持 25.35fps、1.02x、0 丢帧、
  0 重启，输出 H.264 1080p25。普通用户受 `/proc` 权限限制，最终以同一非 root 服务账号
  采样，未通过 root 绕过服务边界。
- 验收结束后板卡服务保持 enabled 但已优雅停止，板卡和 PC 临时进程均清理；未执行
  30 分钟或更长测试。完整表格、限制和证据路径见 `docs/benchmark-results.md`。

### Phase 5 增量：固定样本基准运行器

- 新增可安装的 `run_transcode_benchmark.sh`，以同一本地固定文件运行
  `software`、`mpp` 或 `mpp-rga` 单一路径。默认预热 2 秒、采样 10 秒、
  1 秒间隔，并为每条路径产生指标、progress、stderr、温度和命令记录。
- 运行器使用 Bash 数组执行 FFmpeg，不使用 `eval` 或 shell 命令字符串；只接受
  可读本地普通文件，要求绝对且非根输出目录，拒绝覆盖任何同模式结果。
  `mpp` 不允许隐式缩放，改变分辨率必须选择 `mpp-rga`。
- 性能采样结束后只向刚刚启动的确切 FFmpeg PID 发送 SIGINT。脚本另生成
  短 Matroska 输出，校验 H.264/分辨率并用软件 FFmpeg 完整解码，不把 null
  muxer 的进度当作媒体正确性证明。
- CTest 行为测试使用可控 C 夹具和伪 ffprobe/metrics，覆盖软件缩放、
  RGA 缩放、MPP 非法缩放、防覆盖与无 `eval`。ASan 下暴露的零预热启动
  信号竞态已通过测试使用 1 秒预热修正；生产默认值仍为 2 秒。
- 完整常规 CTest 29/29、ASan/UBSan 29/29、适用 TSan 7/7 PASS；CMake 临时
  安装树包含可执行运行器。Bash 语法、`git diff --check`、无 `eval`、
  无 `system()`/`popen()` 及无显式 `(void)` 弃值调用扫描均通过。

### Phase 5 增量：RK3588 720p 真实缩放短测

- 板卡同步提交 `ef5e8b5` 的运行器后，Release 构建和完整 CTest 29/29 PASS。
  系统 `/usr/bin/cmake` 仅 3.18.4，不满足项目 3.20 最低版本；构建明确使用已有的
  `/home/cat/.local/bin/cmake` 3.31.10。用户级 Python CMake 在 `sudo` 下丢失模块路径，
  因此只用 `sudo install` 安装已验证的单个运行器，源码与安装副本 SHA-256 一致。
- 两条路径共用已验证的 PC 摄像头 H.264 1080p25 固定样本，输出统一为
  H.264 1280×720@25、3000 kbit/s。每条仅预热 2 秒、采样 10 秒，未重新
  占用 PC 摄像头，未执行 30 分钟测试。
- 软件解码+swscale+libx264 为 CPU 282.2%/300.0%、RSS 137.5/138.4 MiB、
  19.20fps/0.768x；MPP 解码+RGA 缩放+MPP 编码为 137.5%/146.0%、
  18.3/18.4 MiB、1038.54fps/41.5x。两者均 drop=0，CSV 无 unavailable。
- 两条路径各产生 3 秒 H.264 720p25 Matroska，ffprobe 和独立完整软件解码
  均 PASS。软件路径仅有像素范围弃用警告，无解码/编码错误；硬件路径
  stderr 为空。原始证据位于板卡仓库外的 `phase5-720p/`。
- 验收后板卡 gatewayd/FFmpeg/MediaMTX 和僵尸进程均为 0；两个 systemd 服务
  保持 enabled/inactive，没有因微基准改变服务状态。

### Phase 5 增量：C17 指标汇总

- `gateway-metrics --summary-output FILE` 在采样的同一进程内为每个目标统计
  available/unavailable 样本、CPU 样本数与平均/峰值、RSS 平均/峰值及
  FD 最小/最大值，避免后续码率矩阵人工拷贝和计算 CSV。
- 汇总路径使用 C17 `fopen(..., "wx")` 排他创建；文件已存在时在采样前失败，
  不覆盖旧证据。目标不可用时仍在汇总中保留计数，且 CLI 继续非零退出。
- 固定样本运行器现在自动产生 `metrics-summary.csv` 并将其纳入防覆盖检查。
  开发机集成测试覆盖真实 self 采样、汇总字段边界、防覆盖和运行器传参。
- 完整常规 CTest 30/30、ASan/UBSan 30/30、适用 TSan 7/7 及 CMake 临时安装
  均 PASS。Bash 语法、`git diff --check`、无 `eval`、无 `system()`/`popen()` 及
  无显式 `(void)` 弃值调用扫描通过。

### Phase 5 增量：RK3588 720p 码率矩阵

- 提交 `7fd1f3d` 同步板卡后，Release 构建和完整 CTest 30/30 PASS；安装后
  `gateway-metrics` 的 1 秒 self 汇总为 6 个 available、0 个 unavailable 样本。
- 在已有 3000 kbit/s 基线上，以相同 PC 摄像头固定样本补充 1500 和
  6000 kbit/s；每个码率分别运行软件+swscale 和 MPP+RGA，输出统一为
  H.264 1280×720@25。四个新增模式均仅预热 2 秒、采样 10 秒。
- 软件 1500k/6000k 的 CPU 平均为 261.3%/283.0%，RSS 为 138.0/138.1 MiB，
  吞吐为 20.16/17.97fps；MPP+RGA 为 135.6%/158.1%、18.3/18.0 MiB、
  1031.72/1011.65fps。四组均 drop=0，汇总均为 11 available、0 unavailable。
- 四份 3 秒输出均经 ffprobe 确认为 H.264 720p25，并通过独立完整软件解码。
  无解码/编码错误；板卡清理后无媒体或僵尸进程，systemd 服务保持
  enabled/inactive。原始证据分别位于 `phase5-bitrate-1500/` 和 `phase5-bitrate-6000/`。
- 短测显示软件路径在更高码率下吞吐降低，硬件路径保持约 1kfps；但每组
  仅 10 个 CPU 有效样本且起始温度不同，不将小幅差异伪装成长时趋势。

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
- RK3588 上的真实 PC 摄像头 RTSP、MPP/RGA 转码、RTSP/WebRTC 播放与 HTTP 控制。
- MediaMTX 真实录像、回放、自动删除、低空间状态和停止后重新发布恢复。
- 输入 EOF、FFmpeg SIGKILL 和 MediaMTX 停止后的有限退避及自动恢复。
- systemd 非 root 设备权限、开机启动、优雅停止以及 gatewayd/MediaMTX 崩溃恢复。
- 可安装的 C17 进程指标采样工具及原始/汇总 CSV 输出。

当前限制：

- 音频采集、编码、发布和录像；当前链路固定使用 `-an`。
- RK3588 30 分钟长稳、双真实输入板卡验收和 Phase 5 完整性能数据。
- WebRTC 低延迟专项调优；当前用户实测主观延迟约 1～2 秒。

## 6. 下一步队列

1. 设计端到端延迟测量，区分 PC 采集、RTSP、转码和播放端缓冲。
2. 在只有一个真实摄像头的边界下设计可明确标记为“重复固定样本”的多通道资源容量测试。
3. 双真实输入、长时间和音频仍需用户另行安排。

## 7. 文档职责

- `README.md`：用户安装、配置和运行方法。
- `ARCHITECTURE.md`：架构、模块职责、阶段与最终验收标准。
- `docs/test-plan.md`：开发者可重复执行的测试和实际验收记录。
- `docs/development-status.md`：当前进度、关键约定、提交和下一步。
- `docs/benchmark-results.md`：后续记录可复现的性能数据。
