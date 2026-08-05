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

当前阶段：Phase 2——单通道 `gatewayd`，状态为 `IN PROGRESS`。

当前开发机测试基线：

```text
日期：2026-08-05
平台：x86_64 Arch Linux
编译器：GCC 16.1.1
常规 CTest：15/15 PASS
ASan/UBSan：15/15 PASS
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

### 本次增量：单通道状态快照

- 新增 `gw_channel_snapshot`，保存通道状态、最近事件、活动进程类型/PID、退出码、
  探测信息、最近 progress、连续失败数、退避和总重启数。
- supervisor 在探测、启动、progress、稳定、失败、退避和停止等关键点同步发布
  只读快照。
- 观察者回调只在 supervisor 线程内同步调用；未来多通道管理器负责复制并加锁保存，
  当前没有宣称具备线程安全的 HTTP 查询能力。
- 新增纯快照单元测试，并扩展 supervisor 测试验证
  `PROBING -> STARTING -> RUNNING -> STOPPED` 和最终指标。

## 5. 当前能力边界

已经具备：

- 单个启用通道的工作进程监督。
- 每次工作进程启动前执行 ffprobe，校验首个视频流的编码和尺寸。
- 可通过同步观察回调取得单通道生命周期快照。
- 结构化进度、最终指标和受控 stderr 日志。
- 信号停止、超时清理、自动重试和失败终态。
- 开发机假工作进程端到端验证。

尚未具备：

- RK3588 板卡上的真实 RTSP/ffprobe 与 MPP/RGA 联调。
- 多通道并行监督。
- SIGHUP 配置重载。
- 通道状态查询和 HTTP 控制 API。
- MediaMTX 录像、systemd 部署和磁盘监控。
- RK3588 真实硬件转码与稳定性/性能数据。

## 6. 下一步队列

按顺序执行：

1. 实现多通道管理器，在线程安全的注册表中保存各 supervisor 快照。
2. 支持多个启用通道独立启动、停止和回收。
3. 增加 SIGHUP 配置重载与差异化重启。

## 7. 文档职责

- `README.md`：用户安装、配置和运行方法。
- `ARCHITECTURE.md`：架构、模块职责、阶段与最终验收标准。
- `docs/test-plan.md`：开发者可重复执行的测试和实际验收记录。
- `docs/development-status.md`：当前进度、关键约定、提交和下一步。
- `docs/benchmark-results.md`：后续记录可复现的性能数据。
