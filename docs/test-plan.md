# 分阶段测试与验收指南

本文档用于记录 `rk3588-media-gateway` 每个开发阶段的测试方法、预期结果和实际
验收记录。功能完成时必须同步更新本文件，未经实际执行的项目不得标记为通过。

## 1. 使用约定

所有命令默认从项目根目录执行：

```bash
cd /home/zane/repo/rk3588-media-gateway
```

结果标记：

- `PASS`：已经在注明的环境中实际执行并通过。
- `FAIL`：已经执行，但结果不满足验收标准。
- `IN PROGRESS`：阶段已有部分实现和测试通过，但尚未达到阶段最终标准。
- `PENDING`：尚未执行，或者对应功能尚未实现。
- `N/A`：当前环境不适用，必须注明原因。

每次阶段验收至少记录：

```text
日期：
提交或版本：
测试机器：
系统与内核：
编译器：
FFmpeg：
MediaMTX：
测试结果：
遗留问题：
```

配置和日志中不得记录真实 RTSP 密码。示例凭据必须使用虚构内容。

## 2. 开发机基础里程碑：C17 核心模块

这一里程碑不是架构中的完整 Phase 0。它用于确认项目能够在开发机编译，并验证
不依赖 RK3588 硬件的配置、参数构造和进度解析逻辑。

### 2.1 测试范围

- CMake 配置与 C17 编译。
- libyaml 依赖检测。
- YAML 配置加载。
- 环境变量展开及错误处理。
- 配置字段校验。
- RTSP URL 密码脱敏。
- FFmpeg 参数数组构造。
- FFmpeg `-progress` 增量解析。

不包含：

- 创建和管理 FFmpeg 子进程。
- 实际访问 RTSP 视频源。
- MPP/RGA 硬件编解码。
- 通道状态机、自动重试和 HTTP API。

### 2.2 检查构建依赖

执行：

```bash
cmake --version
cc --version
pkg-config --modversion yaml-0.1
```

预期：三个命令均成功，并能输出版本信息。

### 2.3 配置与编译

执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

预期生成：

```text
build/gatewayd
build/gateway_unit_tests
build/libgateway_core.a
```

通过标准：

- CMake 成功找到 `yaml-0.1`。
- 编译退出码为 `0`。
- 没有编译警告。

### 2.4 单元测试

执行：

```bash
ctest --test-dir build --output-on-failure
```

预期：

```text
100% tests passed, 0 tests failed
```

也可以直接执行：

```bash
./build/gateway_unit_tests
```

预期：

```text
All gateway unit tests passed.
```

### 2.5 有效配置测试

执行：

```bash
export CAM01_RTSP_URL='rtsp://alice:super-secret@camera.local/live'
./build/gatewayd --config config/gateway.example.yaml --check-config
```

预期：

```text
Configuration valid: 1 channel(s)
```

退出码应为 `0`：

```bash
echo $?
```

### 2.6 缺少环境变量测试

执行：

```bash
unset CAM01_RTSP_URL
./build/gatewayd --config config/gateway.example.yaml --check-config
```

预期：

- 程序拒绝加载配置。
- 错误信息包含 `CAM01_RTSP_URL` 未设置。
- 退出码为 `1`。

测试结束后恢复变量：

```bash
export CAM01_RTSP_URL='rtsp://alice:super-secret@camera.local/live'
```

### 2.7 dry-run 与密码脱敏测试

执行：

```bash
./build/gatewayd --config config/gateway.example.yaml --dry-run
```

输出应包含：

```text
-hwaccel rkmpp
-hwaccel_output_format drm_prime
scale_rkrga=w=1280:h=720:format=nv12
-c:v h264_rkmpp
-b:v 4000k
rtsp://127.0.0.1:8554/cam01
```

输入 URL 应显示为：

```text
rtsp://alice:***@camera.local/live
```

输出中不得出现：

```text
super-secret
```

### 2.8 内存和未定义行为检查

在支持 AddressSanitizer 和 UndefinedBehaviorSanitizer 的开发机上执行：

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

如果执行环境通过 `ptrace` 管理进程，LeakSanitizer 可能无法启动。此时可以仅关闭
泄漏检测，继续检查越界访问和未定义行为：

```bash
ASAN_OPTIONS=detect_leaks=0 \
  ctest --test-dir build-sanitize --output-on-failure
```

关闭泄漏检测必须记录在验收结果中，不能将其表述为已完成泄漏检查。

### 2.9 当前验收记录

```text
日期：2026-08-04
提交或版本：工作区初始版本（尚未建立 Git 仓库）
测试机器：x86_64 开发机
系统与内核：Arch Linux，7.1.5-zen1-2-zen
编译器：GCC 16.1.1
FFmpeg：8.1.2，非 Rockchip 构建
MediaMTX：未安装
结果：PASS（开发机基础里程碑）
补充：常规单元测试通过；ASan/UBSan 在关闭 LeakSanitizer 后通过
遗留问题：需要在非 ptrace 环境补做泄漏检测；需要进入 RK3588 Phase 0 验收
```

## 3. Phase 0：RK3588 运行环境确认

状态：`PENDING`

### 3.1 自动检查

在 RK3588 板卡执行：

```bash
./scripts/check_media_env.sh
```

脚本检查：

- CPU 架构为 `aarch64`。
- `/dev/mpp_service` 可读写。
- `/dev/rga` 可读写。
- 至少一个 DRM 设备可读写。
- FFmpeg 包含 `h264_rkmpp`、`hevc_rkmpp` 编解码器。
- FFmpeg 包含 `scale_rkrga` 滤镜。
- MediaMTX 已安装并可执行。

### 3.2 手工记录

执行并保存结果：

```bash
uname -a
ffmpeg -version
ffmpeg -hide_banner -decoders
ffmpeg -hide_banner -encoders
ffmpeg -hide_banner -filters
ls -l /dev/mpp_service /dev/rga /dev/dri
mediamtx --version
```

通过标准：环境检查脚本无关键失败，并保存系统、FFmpeg、MediaMTX 和设备权限
信息。没有摄像头时允许缺少 V4L2 设备，但必须能够使用文件或 RTSP 输入。

## 4. Phase 1：手工跑通单路媒体链路

状态：`PENDING`

功能开发和板卡环境准备完成后，本节需要补充：

- 测试源的生成或推送方法。
- 本地文件硬件转码命令。
- RTSP 输入和 MediaMTX 发布命令。
- RTSP、WebRTC 播放验证方法。
- 连续运行 30 分钟的日志和结果。
- CPU、RSS、FPS、丢帧和错误记录。

通过标准：单路 1080p 视频连续硬件转码和播放 30 分钟。

## 5. Phase 2：单通道 gatewayd

状态：`IN PROGRESS`

当前已经实现：

- 由 `gatewayd` 创建 FFmpeg 工作进程。
- stdout progress 与 stderr 日志收集。
- 启动、正常停止和强制停止。
- 子进程回收及僵尸进程检查。
- 发送 `SIGTERM` 后的资源清理。
- `STOPPED`、`PROBING`、`STARTING`、`RUNNING`、`BACKOFF` 和 `FAILED`
  状态转换。
- 启动超时和 progress 停滞超时。
- 指数退避、退避上限、最大重试次数和重试计数。
- 连续运行达到 `stable_run_sec` 后清零连续失败次数。
- 启动 FFmpeg 前执行 ffprobe，并校验视频编码、宽度和高度。
- 独立的探测超时，以及探测失败、编码不匹配和探测重试事件。

尚未实现：

- 面向外部用户的通道状态查询接口。
- RK3588 板卡上的真实 RTSP 输入探测和硬件媒体链路验收。

### 5.1 开发机进程管理集成测试

这组测试使用仓库内的假工作进程，不要求 RK3588、Rockchip FFmpeg 或 MediaMTX。

执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CTest 中与 Phase 2 相关的测试：

```text
gateway_channel_state_tests
gateway_channel_snapshot_tests
gateway_probe_tests
gateway_supervisor_tests
gateway_process_tests
gateway_single_channel_test
gateway_stable_run_test
gateway_startup_timeout_test
gateway_progress_timeout_test
gateway_retry_exhaustion_test
gateway_probe_failure_test
gateway_probe_timeout_test
gateway_probe_mismatch_test
gateway_probe_retry_exhaustion_test
```

`gateway_channel_state_tests` 验证：

- 正常状态转换和非法事件拒绝。
- 首次失败从 1 秒开始指数退避。
- 退避时间不超过配置上限。
- 重试耗尽进入 `FAILED`。
- 稳定事件和人工重新开始能够清零连续失败次数。

`gateway_channel_snapshot_tests` 验证：

- 初始化状态、通道 ID 和空的探测/progress/退出信息。
- 状态、失败次数、退避时间和重启数同步更新。
- 活动进程类型/PID、退出码、探测结果和 progress 的保存与覆盖。
- 进程类型使用稳定字符串表示。

`gateway_probe_tests` 验证：

- ffprobe 参数以独立 argv 构造，不经过 shell。
- RTSP transport 和源 URL 位于预期参数位置。
- 解析 `codec_name`、`width` 和 `height`。
- 同时接受 LF 和 CRLF 行结束符。
- 缺失字段、非数字尺寸和解码器不匹配能够被识别。

该单元测试不创建进程；后面的单通道及探测故障测试通过假 ffprobe 覆盖完整的
`PROBING` 子进程执行链路。

`gateway_process_tests` 验证：

- 使用 `posix_spawnp()` 启动工作进程。
- stdout 和 stderr 使用独立非阻塞管道。
- 正常退出状态和退出码。
- `SIGTERM` 停止整个工作进程组。
- 超时后使用 `SIGKILL` 并回收进程。
- 不存在的可执行文件能够明确报错。

`gateway_supervisor_tests` 直接链接 `gateway_core` 中的 supervisor 模块，验证：

- ffprobe 和 FFmpeg 默认可执行文件名称。
- 空配置、空通道和空可执行文件名称被公共接口拒绝。
- CLI 注入的停止信号能够在 `PROBING` 阶段终止并回收探测进程，最终正常停止。
- 同步观察者能够看到 `PROBING`、`STARTING`、`RUNNING` 和 `STOPPED`。
- 最终快照保留探测编码、最后一条 progress、干净退出码，并清除活动 PID。

`gateway_single_channel_test` 验证：

- 配置加载、ffprobe 探测、FFmpeg 参数构造和进程创建的完整调用链。
- 探测成功后输出编码和源视频尺寸，再进入 `STARTING`。
- progress 输出能够进入 `RUNNING` 状态并生成最终指标。
- 工作进程 stderr 中出现源 URL 时，密码不会出现在网关日志中。
- 工作进程退出后进入 `STOPPED`，退出码为 `0`。

`gateway_stable_run_test` 持续发送 progress 超过 `stable_run_sec`，验证 supervisor
产生 `event=stable`；状态机单元测试同时验证该事件会把连续失败次数从非零值清零。

其余故障测试验证：

- ffprobe 非零退出时记录 `probe_failure`，且其 stderr 源 URL 密码被隐藏。
- ffprobe 超过 `probe_timeout_sec` 时被终止并记录 `probe_timeout`。
- 探测编码与配置解码器不匹配时记录 `probe_mismatch`，不启动 FFmpeg。
- 探测连续失败时同样进入 `BACKOFF`，重试耗尽后进入 `FAILED`。
- 未在 `startup_timeout_sec` 内收到 progress 时终止工作进程并记录
  `startup_timeout`。
- 已进入 `RUNNING` 后 progress 超过 `progress_timeout_sec` 未更新时记录
  `progress_timeout`。
- 工作进程连续失败时进入 `BACKOFF` 并自动重启。
- 超过 `max_retries` 后进入 `FAILED`，不再快速重启。
- 所有失败路径都继续检查日志中不得出现明文密码。

预期：全部测试通过，CTest 输出中不得出现 `fixture-password`。

### 5.2 当前增量验收记录

```text
日期：2026-08-05
测试机器：x86_64 开发机
测试方式：假工作进程集成测试
结果：PASS（单通道状态快照增量，常规与 ASan/UBSan 均为 15/15 通过）
覆盖：创建、双管道、退出检测、SIGTERM、SIGKILL、回收、日志密码脱敏、
      ffprobe 成功/失败/超时/编码不匹配、状态机、启动超时、progress 超时、
      稳定窗口、探测及工作进程退避重试、重试耗尽、supervisor 参数契约、
      探测阶段外部停止、完整状态序列和最终快照
限制：开发机使用 ffprobe/FFmpeg 夹具，尚未连接真实 RTSP、Rockchip FFmpeg 或
      MediaMTX，不代表完整 Phase 2 通过
```

通过标准：不手工执行 FFmpeg，通过 `gatewayd` 启停一路真实转码。

## 6. Phase 3：多通道与异常恢复

状态：`IN PROGRESS`

当前已经实现：

- 每个启用通道运行在独立 supervisor 线程。
- 读写锁保护的通道快照注册表。
- 全部通道统一等待、停止和回收。
- 单通道工作进程失败不会改变其他通道的状态或退出结果。
- SIGHUP 候选配置校验和按通道差异化重载。
- 开发机一次性输入断流后的独立退避和恢复。
- 开发机一次性输出发布失败后的独立退避和恢复。

### 6.1 开发机多通道测试

执行全部常规测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

与本增量直接相关：

```text
gateway_channel_manager_tests
gateway_reload_tests
gateway_input_recovery_tests
gateway_publish_recovery_tests
gateway_multi_channel_test
```

`gateway_channel_manager_tests` 验证：

- 两个正常通道并发完成，分别保存最终 progress 和 `clean_exit`。
- 一个工作进程失败时，该通道进入 `FAILED`，健康通道保持 `STOPPED/clean_exit`。
- 外部停止请求传递给全部 supervisor，两个通道均停止并清除活动进程。
- 不存在的通道 ID 被快照查询接口拒绝。
- 合法候选只重启配置变化通道，未变通道保持配置代次和 `RUNNING`。
- 禁用旧通道并新增通道时更新注册表，复用槽位不会继承旧通道配置代次。
- 非法候选在修改任何通道前返回，现有通道状态和配置代次保持不变。

`gateway_reload_tests` 启动真实 `gatewayd` CLI 进程并验证：

- 修改同一路径 YAML 并发送 SIGHUP 后得到差异摘要。
- `cam01` 未变化且不重启，`cam02` 参数变化只重启一次，`cam03` 被新增。
- 第二次写入非法 YAML 配置后重载被拒绝，服务继续运行。
- 最终 SIGTERM 停止全部新旧通道，日志中不出现明文密码。

`gateway_input_recovery_tests` 使用双通道管理器和假媒体进程验证：

- `cam02` 第一次工作进程已进入 `RUNNING` 后以退出码 9 模拟输入断开。
- supervisor 记录 `worker_failure` 并退避 1 秒，重新执行探测和工作进程后恢复到
  `RUNNING`，总重启数精确为 1。
- 故障期间 `cam01` 保持 `RUNNING`，总重启数为 0，配置代次保持为 1。
- 观察回调使用原子标志保存故障和恢复事件，测试结束统一停止、等待并回收两个通道。
- 日志不得出现 `fixture-password`；临时恢复标记在测试结束后删除。

`gateway_publish_recovery_tests` 复用相同的双通道恢复断言，但故障由 `cam02` 的
FFmpeg 输出发布 URL 触发：

- 输入探测始终成功，第一次发布工作进程进入 `RUNNING` 后以退出码 9 失败。
- 退避后第二个发布工作进程恢复到 `RUNNING`，总重启数为 1。
- `cam01` 保持 `RUNNING`、总重启数为 0，证明发布故障不会重启健康通道。
- 该模式不连接真实 MediaMTX；它只验证 FFmpeg 因发布端故障退出时的监督恢复路径。

`gateway_multi_channel_test` 从 CLI 加载双通道 YAML，验证 `gatewayd` 同时运行两个
通道、两者均干净退出，并继续禁止明文密码日志。

并发专项复测：

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure \
  -R 'gateway_(channel_manager|reload|input_recovery|publish_recovery|multi_channel)_test'
```

### 6.2 当前增量验收记录

```text
日期：2026-08-06
测试机器：x86_64 开发机
测试方式：双通道假 ffprobe/FFmpeg 进程
结果：PASS（常规与 ASan/UBSan 20/20；多通道/重载/故障恢复 TSan 5/5）
覆盖：并发启动、独立快照、健康/失败隔离、统一停止、线程回收、密码脱敏、
      合法/非法 SIGHUP、按通道新增/禁用/重启、未变通道不中断、一次性输入断流、
      一次性发布失败、退避重试、故障通道恢复且健康通道不中断
限制：未连接真实 RTSP、MediaMTX 或 RK3588 MPP/RGA，不代表多路硬件性能通过
```

尚未实现：

- 真实 RTSP 输入停止、重新供流及向 MediaMTX 恢复发布的实机验证。
- 真实 MediaMTX 服务停止、启动及通道重新发布验证。
- RK3588 多通道性能与稳定性验收。

### 6.3 开发机完成项审计

| Phase 3 要求 | 开发机证据 | 状态 |
| --- | --- | --- |
| 多通道互不影响 | `gateway_channel_manager_tests`、`gateway_multi_channel_test` | `PASS` |
| 状态机、退避序列和重试耗尽 | `gateway_channel_state_tests`、各 timeout/retry 测试 | `PASS` |
| 工作进程异常退出 | worker failure、retry exhaustion 和两个恢复测试 | `PASS` |
| 输入断开后独立恢复 | `gateway_input_recovery_tests`（夹具模拟） | `PASS` |
| 发布端失败后独立恢复 | `gateway_publish_recovery_tests`（夹具模拟） | `PASS` |
| SIGHUP 差异化重载 | `gateway_reload_tests`、`gateway_channel_manager_tests` | `PASS` |
| 真实 RTSP/MediaMTX/RK3588 链路 | 当前开发机无对应环境 | `PENDING` |

结论：Phase 3 开发机软件路径通过，但完整 Phase 3 仍为 `IN PROGRESS`。夹具测试证明
状态机和隔离逻辑，不证明真实网络恢复、MediaMTX 重新发布或 RK3588 硬件链路。

### 6.4 RK3588 实机验收清单

以下项目当前均为 `PENDING`，只有在板卡上实际执行并保存日志后才能改为 `PASS`。

前置条件：

- 已通过本文档 Phase 0 环境检查和 Phase 1 单路 30 分钟媒体链路验收。
- MediaMTX 作为独立服务运行，并确认实际 service 名称；以下示例使用 `mediamtx`。
- 准备两个真实 RTSP 输入和两个不同输出路径 `cam01`、`cam02`。
- 测试配置的 `max_retries` 和 `max_backoff_sec` 应允许在重试耗尽前人工恢复故障。
- 配置文件放在 Git 仓库外，真实用户名和密码不得写入测试记录。

构建并保存环境基线：

```bash
./scripts/check_media_env.sh | tee /tmp/phase3-media-env.log
cmake -S . -B build-board -DCMAKE_BUILD_TYPE=Release
cmake --build build-board --parallel
ctest --test-dir build-board --output-on-failure
```

准备双通道配置后，避免把 URL 留在 shell 历史中：

```bash
read -rsp 'CAM01 RTSP URL: ' CAM01_RTSP_URL; echo
read -rsp 'CAM02 RTSP URL: ' CAM02_RTSP_URL; echo
export CAM01_RTSP_URL CAM02_RTSP_URL
export PHASE3_CONFIG=/absolute/path/outside/repository/phase3.yaml
export PHASE3_LOG=/tmp/gateway-phase3.log
./build-board/gatewayd --config "$PHASE3_CONFIG" >"$PHASE3_LOG" 2>&1 &
GATEWAY_PID=$!
```

确认日志中两个通道均进入 `RUNNING`，并从另一台机器连续播放 `cam01` 和 `cam02`。
记录 RTSP 播放命令以及 WebRTC 播放页面，但不得记录输入密码。

#### 6.4.1 单输入断流与恢复

1. 保持 `cam01` 播放，停止 `cam02` 的上游 RTSP 源。
2. 确认 `cam02` 进入 `BACKOFF`，退避值没有超过配置上限。
3. 故障期间确认 `cam01` 持续播放，日志中没有新的 `cam01 restart_count`。
4. 在重试耗尽前恢复 `cam02` 上游源。
5. 确认 `cam02` 重新经历 `PROBING`、`STARTING`、`RUNNING`，输出重新可播放。

通过标准：`cam02` 自动恢复，`cam01` 不重启且播放不中断。

#### 6.4.2 工作进程崩溃与恢复

从 `channel=cam02 pid=... state=STARTING` 日志取得当前 FFmpeg PID，只终止该明确
PID，不要使用宽泛的 `pkill ffmpeg`：

```bash
kill -KILL <cam02-ffmpeg-pid>
```

通过标准：旧 PID 被回收，`cam02` 退避后使用新 PID 恢复，`cam01` 不重启且播放
不中断。

#### 6.4.3 MediaMTX 停止与恢复

```bash
sudo systemctl stop mediamtx
sudo systemctl status mediamtx --no-pager
sudo systemctl start mediamtx
sudo systemctl status mediamtx --no-pager
```

停止期间两个发布通道均可能失败；`gatewayd` 不应尝试启动 MediaMTX。服务恢复后，
两个通道应通过各自 supervisor 重新发布，RTSP 和 WebRTC 输出重新可播放。

#### 6.4.4 SIGHUP 差异化重载

只修改 `cam02` 的码率并保持 `cam01` 配置不变，然后执行：

```bash
kill -HUP "$GATEWAY_PID"
```

通过标准：日志摘要为一个未变化通道和一个重启通道；`cam01` PID 与播放保持不变，
`cam02` 使用新 PID 和新码率恢复发布。随后写入一份非法候选配置再次发送 SIGHUP，
确认配置被拒绝且两个现有通道继续运行。

#### 6.4.5 停止与资源清理

先从日志记录两个通道最后一次启动的工作进程 PID：

```bash
FINAL_CAM01_PID=$(sed -n 's/^channel=cam01 pid=\([0-9]*\) state=STARTING.*/\1/p' \
  "$PHASE3_LOG" | tail -n 1)
FINAL_CAM02_PID=$(sed -n 's/^channel=cam02 pid=\([0-9]*\) state=STARTING.*/\1/p' \
  "$PHASE3_LOG" | tail -n 1)
```

再停止网关并检查这两个工作进程组：

```bash
kill -TERM "$GATEWAY_PID"
wait "$GATEWAY_PID"
GATEWAY_STATUS=$?
echo "$GATEWAY_STATUS"
if pgrep -a -g "$FINAL_CAM01_PID,$FINAL_CAM02_PID"; then
  echo 'FAIL: worker process group remains'
else
  echo 'PASS: worker process groups were removed'
fi
```

通过标准：`gatewayd` 退出码为 0，没有残留子进程或僵尸进程；日志不包含真实密码，
每个通道都有停止或最终状态记录。完成后执行：

```bash
unset CAM01_RTSP_URL CAM02_RTSP_URL PHASE3_CONFIG PHASE3_LOG GATEWAY_PID
unset FINAL_CAM01_PID FINAL_CAM02_PID GATEWAY_STATUS
```

实机记录至少填写：

```text
日期：
提交：
板卡与系统镜像：
内核：
FFmpeg-Rockchip：
MediaMTX：
输入源编码/分辨率/FPS：
基线双路播放：PENDING
cam02 输入断流恢复：PENDING
cam02 工作进程崩溃恢复：PENDING
MediaMTX 停止与恢复：PENDING
SIGHUP 未变化通道不中断：PENDING
SIGTERM 与资源清理：PENDING
原始日志路径：
遗留问题：
```

完整 Phase 3 需要覆盖：

- 多个通道互不影响。
- 输入流断开与恢复。
- FFmpeg 异常退出。
- MediaMTX 停止与恢复。
- 退避时间序列。
- 最大重试次数与 `FAILED` 状态。
- SIGHUP 配置重载和差异化重启。

通过标准：故障解除后通道自动恢复，其他通道不被重启或中断。

## 7. Phase 4：控制接口、录像和服务化

状态：`IN PROGRESS`

当前已经实现：

- 可配置启停的本地 HTTP 服务，默认监听 `127.0.0.1:9080`。
- `GET /v1/health`、`GET /v1/channels` 和 `GET /v1/channels/{id}`。
- 受锁保护的批量快照复制和有界 JSON 响应。
- 404、405、431 错误响应以及 URL/密码不进入 API 响应的安全边界。

### 7.1 只读 HTTP 开发机验收

执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'gateway_(http_server|http)_tests'
```

`gateway_http_server_tests` 不创建 socket，直接验证：

- 健康检查返回通道总数、运行数和失败数。
- 列表及单通道 JSON 包含状态、进程、探测和 progress 字段。
- 不存在通道和路由返回 404，非 GET 返回 405 和 `Allow: GET`。
- 快照批量复制容量不足时返回 `GW_ERR_OVERFLOW`，不发布部分计数。
- JSON 中不出现输入 RTSP URL 或密码。

`gateway_http_tests` 启动真实 `gatewayd`，使用 `server.port: 0` 取得内核分配的临时
回环端口，并验证：

- 三个只读端点能够通过 HTTP/1.1 socket 访问。
- 响应包含正确状态行、`Content-Length`、JSON 类型、`nosniff` 和关闭连接语义。
- 未知通道返回 404，POST 返回 405，8192 字节无结束请求头返回 431。
- SIGTERM 后 HTTP 线程、通道和工作进程均正常退出。
- API 和 gateway 日志均不出现测试密码。

本增量验收记录：

```text
日期：2026-08-06
测试机器：x86_64 开发机
常规 CTest：22/22 PASS
ASan/UBSan：22/22 PASS（LeakSanitizer 因 ptrace 环境关闭）
TSan：HTTP/通道管理器/重载相关 4/4 PASS
限制：仅测试本机回环 HTTP 和假媒体进程，未连接真实 RTSP、MediaMTX 或 RK3588
```

功能完成后，本节需要覆盖：

- 通道启动、停止和重启接口。
- API 默认只监听回环地址。
- MediaMTX 录像与回放。
- 磁盘空间不足处理。
- systemd 开机启动、停止和异常恢复。

通过标准：设备重启后服务自动启动，接口可用，异常退出后 systemd 能恢复服务。

## 8. Phase 5：性能、稳定性与最终交付

状态：`PENDING`

测试矩阵至少包含：

1. 软件解码和软件编码。
2. MPP 解码和 MPP 编码。
3. MPP 解码、RGA 缩放和 MPP 编码。

统一记录：

- 输入、输出分辨率、编码格式、帧率和码率。
- 平均与峰值 CPU 占用。
- RSS 内存。
- 实际 FPS、处理速度和丢帧数。
- 端到端延迟。
- 2 小时及 24 小时稳定性结果。
- 重启次数、内存变化、文件描述符和僵尸进程检查。

通过标准：测试可以复现，所有性能结论均能对应到原始日志或报告。

## 9. 阶段验收记录模板

完成新阶段时复制以下模板：

```markdown
### Phase N 验收记录：YYYY-MM-DD

- 提交或版本：
- 测试人员：
- 测试机器：
- 系统与内核：
- 编译器：
- FFmpeg：
- MediaMTX：
- 使用配置：
- 开始时间：
- 结束时间：
- 结果：PASS / FAIL
- 失败项：
- 日志位置：
- 遗留问题：
- 下一步：
```
