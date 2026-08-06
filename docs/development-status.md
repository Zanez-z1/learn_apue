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

当前阶段：Phase 3——多通道与异常恢复，状态为 `IN PROGRESS`。Phase 2 的开发机
软件路径已完成，RK3588 真实媒体链路验收仍为 `PENDING`。

当前开发机测试基线：

```text
日期：2026-08-06
平台：x86_64 Arch Linux
编译器：GCC 16.1.1
常规 CTest：20/20 PASS
ASan/UBSan：20/20 PASS
TSan（多通道/重载/故障恢复相关）：5/5 PASS
LeakSanitizer：当前 ptrace 环境不支持，尚未完成
RK3588 MPP/RGA：当前开发机不具备，等待板卡验收
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

### 本次增量：Phase 3 开发机审计与实机清单

- Phase 3 的开发机软件路径已覆盖多通道隔离、有限退避、工作进程失败、输入/发布
  故障恢复和差异化重载，测试证据逐项记录在 `docs/test-plan.md`。
- 完整 Phase 3 仍为 `IN PROGRESS`，因为当前环境没有 RK3588、真实 RTSP 源或
  MediaMTX，未将夹具结果替代为实机结果。
- `docs/test-plan.md` 已增加可执行的板卡验收清单和记录模板，包含基线播放、输入断流、
  工作进程崩溃、MediaMTX 停启、SIGHUP、密码脱敏和资源清理。

## 5. 当前能力边界

已经具备：

- 多个启用通道的独立线程和工作进程监督。
- 每次工作进程启动前执行 ffprobe，校验首个视频流的编码和尺寸。
- 可通过同步观察回调取得单通道生命周期快照。
- 结构化进度、最终指标和受控 stderr 日志。
- 信号停止、超时清理、自动重试和失败终态。
- SIGHUP 候选配置校验与按通道差异化重载。
- 开发机夹具模拟输入或发布端中断后，故障通道可独立退避并恢复运行。
- 开发机假工作进程端到端验证。

尚未具备：

- RK3588 板卡上的真实 RTSP/ffprobe 与 MPP/RGA 联调。
- 真实 MediaMTX 发布端停止、恢复与重新发布联调。
- 通道状态查询和 HTTP 控制 API。
- MediaMTX 录像、systemd 部署和磁盘监控。
- RK3588 真实硬件转码与稳定性/性能数据。

## 6. 下一步队列

按顺序执行：

1. 在 RK3588 上执行 `docs/test-plan.md` 的 Phase 3 实机验收清单并保存原始日志。
2. 在安装 MediaMTX 的环境执行服务停止、恢复和通道重新发布验收。
3. 在等待板卡验收期间开始 Phase 4 的只读 HTTP 健康检查和通道查询设计。

## 7. 文档职责

- `README.md`：用户安装、配置和运行方法。
- `ARCHITECTURE.md`：架构、模块职责、阶段与最终验收标准。
- `docs/test-plan.md`：开发者可重复执行的测试和实际验收记录。
- `docs/development-status.md`：当前进度、关键约定、提交和下一步。
- `docs/benchmark-results.md`：后续记录可复现的性能数据。
