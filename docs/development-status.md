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
常规 CTest：9/9 PASS
ASan/UBSan：9/9 PASS
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

### 当前增量：ffprobe 参数与结果解析

- 使用独立参数数组构造 ffprobe 命令，不经过 shell。
- 固定读取首个视频流的 `codec_name`、`width` 和 `height`。
- 支持 LF 与 CRLF 输出，拒绝缺失或非法字段。
- 校验 H.264/H.265 探测结果与配置的 RKMpp 解码器是否匹配。
- 当前只完成纯逻辑和单元测试，尚未启动 ffprobe 子进程。

## 5. 当前能力边界

已经具备：

- 单个启用通道的工作进程监督。
- 结构化进度、最终指标和受控 stderr 日志。
- 信号停止、超时清理、自动重试和失败终态。
- 开发机假工作进程端到端验证。

尚未具备：

- 真实 `ffprobe` 输入探测；目前 `PROBING` 只是状态占位。
- 多通道并行监督。
- SIGHUP 配置重载。
- 通道状态查询和 HTTP 控制 API。
- MediaMTX 录像、systemd 部署和磁盘监控。
- RK3588 真实硬件转码与稳定性/性能数据。

## 6. 下一步队列

按顺序执行：

1. 使用进程管理器执行 ffprobe，并接入 `PROBING`，区分探测失败、探测超时和
   工作进程失败。
2. 将 supervisor 从 `main.c` 拆分为独立通道模块。
3. 完成单通道状态快照，为 HTTP 查询接口准备稳定数据模型。
4. 进入多通道管理与配置重载。

## 7. 文档职责

- `README.md`：用户安装、配置和运行方法。
- `ARCHITECTURE.md`：架构、模块职责、阶段与最终验收标准。
- `docs/test-plan.md`：开发者可重复执行的测试和实际验收记录。
- `docs/development-status.md`：当前进度、关键约定、提交和下一步。
- `docs/benchmark-results.md`：后续记录可复现的性能数据。
