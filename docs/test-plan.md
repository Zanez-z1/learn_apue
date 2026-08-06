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

状态：`PASS`

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

### 3.3 RK3588 实机验收记录

```text
日期：2026-08-06
本机提交：10e3432
板卡源码：无 .git 目录；CMakeLists.txt、src/main.c 和 docs/test-plan.md 的 SHA-256
          与本机 10e3432 完全一致
板卡：LubanCat，aarch64
系统：Debian GNU/Linux 11 (bullseye)
内核：Linux 5.10.160 #13 SMP Thu Oct 16 13:39:40 CST 2025
CMake：3.31.10（/home/cat/.local/bin/cmake）
FFmpeg-Rockchip：388741a，启用 rkmpp/rkrga
MediaMTX：v1.20.0 linux arm64
MPP /dev/mpp_service：PASS（cat 用户可读写）
RGA /dev/rga：PASS（cat 用户可读写）
DRM：PASS（至少一个节点可读写）
h264/hevc RKMpp 编解码器：PASS
scale_rkrga：PASS
环境检查：0 failures，0 warnings
板卡 Release 配置与构建：PASS
板卡完整 CTest：26/26 PASS，24.28 秒
结果：PASS
```

可复核日志保存在板卡仓库外，避免将运行产物或凭据加入源码：

```text
/home/cat/rk3588-acceptance/2026-08-06/phase0-media-env.log
/home/cat/rk3588-acceptance/2026-08-06/phase0-configure.log
/home/cat/rk3588-acceptance/2026-08-06/phase0-build.log
/home/cat/rk3588-acceptance/2026-08-06/phase0-ctest.log
```

补充边界：系统整体 `degraded` 来自项目外的 `rkwifibt.service` 失败；Phase 0 未把它
归因于 gateway。此记录只证明环境、设备访问和项目测试基线，真实媒体链路在 Phase 1
单独验收。

## 4. Phase 1：手工跑通单路媒体链路

状态：`COMPLETE`（按用户批准的 5 分钟以上实机窗口）

功能开发和板卡环境准备完成后，本节需要补充：

- 测试源的生成或推送方法。
- 本地文件硬件转码命令。
- RTSP 输入和 MediaMTX 发布命令。
- RTSP、WebRTC 播放验证方法。
- 连续运行至少 5 分钟的日志和结果；30 分钟长稳留到后续压力测试。
- CPU、RSS、FPS、丢帧和错误记录。

当前通过标准：单路 1080p 视频连续硬件转码和播放至少 5 分钟。原 30 分钟标准由用户在
2026-08-06 明确取消本轮执行，记录为 `SKIPPED`，不得写成 PASS。

### 4.1 真实 PC 摄像头 5 分钟冒烟验收

本增量验证至少 5 分钟。PC 集成摄像头原生最高支持 MJPEG 1280×720@30fps，不得
写成原生 1080p；PC FFmpeg 将真实画面上采样并编码为 H.264 1920×1080@25fps，再通过
同网段临时 MediaMTX 提供真实 RTSP 输入：

```text
/dev/video0 (MJPEG 1280x720@30)
  -> PC FFmpeg (H.264 1920x1080@25, 8 Mbit/s)
  -> rtsp://192.168.1.16:8554/source
  -> RK3588 gatewayd: h264_rkmpp + scale_rkrga + h264_rkmpp
  -> RK3588 MediaMTX cam01
  -> PC RTSP / WebRTC reader
```

验收记录：

```text
日期：2026-08-06
本机提交：f672c7d 的父功能版本 10e3432（本次仅追加验收文档）
输入：PC Integrated Camera，原生 MJPEG 1280x720@30，PC 上采样 H.264 1920x1080@25
板卡输入探测：H.264，1920x1080，25/1 fps
板卡处理链：h264_rkmpp -> scale_rkrga 1920x1080 NV12 -> h264_rkmpp 6000 kbit/s
连续运行：至少 10 分 44 秒
最终状态：RUNNING，frame=16106，fps=25.04，speed=1.00，drop_frames=0
重启：0；连续失败：0；工作进程 PID 在采样期间不变
工作进程 CPU：约 19.8%～21.7%；RSS：约 19.2 MiB；采样温度：约 39.8°C
RTSP：PASS；PC FFmpeg 实际读取板卡 H.264 1920x1080@25 输出并正常退出
WebRTC：PASS；MediaMTX 记录 peer connection established 和读取 cam01；用户确认可见画面
主观 WebRTC 延迟：约 1～2 秒，尚未进行低延迟专项优化
音频：不在当前链路中；gateway FFmpeg 使用 -an，输出与录像均只有视频
结果：PASS（至少 5 分钟摄像头/RTSP/WebRTC）；SKIPPED（30 分钟长稳，用户取消本轮执行）
```

运行期间 FFmpeg 发布端先出现一次 `461 Unsupported Transport`，随后自动使用可接受的
传输方式成功发布并稳定运行，未触发通道重启。该现象保留为后续明确发布传输配置的检查项。

板卡仓库外证据：

```text
/home/cat/rk3588-acceptance/2026-08-06/phase1-camera-gateway.log
/home/cat/rk3588-acceptance/2026-08-06/phase1-camera-source-probe.log
/home/cat/rk3588-acceptance/2026-08-06/phase1-camera-final-status.json
/home/cat/rk3588-acceptance/2026-08-06/phase1-mediamtx.log
```

## 5. Phase 2：单通道 gatewayd

状态：`COMPLETE`

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

后续 Phase 4 增量已补齐：

- 面向外部用户的通道状态查询接口：PASS。
- RK3588 板卡上的真实 RTSP 输入探测和单路硬件媒体链路验收：PASS。

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

当前结论：后续 RK3588 实机记录证明 `gatewayd` 已完成单路真实转码启停，Phase 2 为
`COMPLETE`；上面的“限制”只描述 2026-08-05 当次开发机增量，不是当前限制。

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
| 真实 RTSP/MediaMTX/RK3588 链路 | RK3588 单路故障记录 | `PASS`（单路） |

结论：Phase 3 开发机软件路径和 RK3588 单路真实故障恢复通过，但完整 Phase 3 仍为
`IN PROGRESS`。双通道隔离由夹具证明，尚未用两个真实输入完成板卡验证和性能测试。

### 6.4 RK3588 实机验收清单

以下清单用于双真实输入最终验收。单路输入断流、工作进程崩溃和 MediaMTX 恢复已经在
板卡执行并记录；双通道互不影响仍为 `PENDING`。

前置条件：

- 已通过本文档 Phase 0 环境检查和 Phase 1 单路 5 分钟以上媒体链路验收。
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

常驻进程还必须覆盖“上游 EOF 导致 FFmpeg 退出码为 0”的情况。退出码 0 只说明 FFmpeg
自身没有报告命令错误，并不表示常驻媒体通道收到停止请求；此时仍应记录
`worker_failure`、进入 `BACKOFF` 并重新探测。`clean_exit -> STOPPED` 只允许出现在显式
`--exit-when-idle` 的一次性测试模式或真实 stop 请求之外的既定测试路径。

开发机回归执行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'gateway_(supervisor|channel_manager|input_recovery|publish_recovery)_tests'
```

2026-08-06 首次 RK3588 单路实测记录：PC 摄像头源停止后，修复前二进制的 FFmpeg 在
87.44 秒进度处以 0 退出，通道错误进入 `STOPPED/clean_exit`。该次结果为 `FAIL`，是修复
触发证据，不得作为恢复通过记录。

修复后复测：`PASS`。第二次停止 PC 摄像头源后，旧工作 PID 755974 的 FFmpeg 同样因
EOF 以 0 退出，通道记录 `worker_failure`，随后依次使用 1、2、4、8、16、30 秒退避并
探测输入。恢复源后通道以新 PID 762652 回到 `RUNNING`，输出重新可读；60 秒稳定窗口后
`consecutive_failures` 从 7 清零，`total_restarts` 保留为 7。

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

2026-08-06 RK3588 单路真实故障记录：

```text
提交：25e1a3f、d4a8b77
板卡：aarch64 LubanCat，Debian 11，Linux 5.10.160
输入：PC 集成摄像头，经 PC MediaMTX 提供 H.264 1920x1080 RTSP
基线播放：PASS；板卡本地与 PC 跨主机均可读取，平均 25fps
输入 EOF/退出码 0：PASS；BACKOFF 后恢复为新 PID 762652，稳定窗口后失败计数清零
工作进程 SIGKILL：PASS；PID 762652 -> exit 137/BACKOFF -> PID 767868/RUNNING
MediaMTX 停止：PASS；PID 692980 消失期间 gatewayd 存活且没有代为启动 MediaMTX
MediaMTX 恢复：PASS；新 PID 770462，gateway 工作 PID 770861 恢复发布
录像恢复：PASS；继续产生非空 MP4，回放 API 返回恢复后的新时间段
WebRTC 恢复：PASS；MediaMTX 记录来自 PC 的会话建立并读取 cam01 H.264
音轨：无；当前链路使用 -an，本项只验收视频恢复
多通道互不影响：开发机夹具 PASS；本次实机只有一个真实输入，未伪造双路结果
SIGHUP 差异化重载：本轮未重复，PENDING
SIGTERM 与 systemd 资源清理：PASS，证据见 Phase 4 systemd 生命周期记录
原始日志：/home/cat/rk3588-acceptance/2026-08-06/phase3-*.log
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

状态：`COMPLETE`

当前已经实现：

- 可配置启停的本地 HTTP 服务，默认监听 `127.0.0.1:9080`。
- `GET /v1/health`、`GET /v1/channels` 和 `GET /v1/channels/{id}`。
- 受锁保护的批量快照复制和有界 JSON 响应。
- 404、405、431 错误响应以及 URL/密码不进入 API 响应的安全边界。
- `POST /v1/channels/{id}/start`、`stop` 和 `restart` 生命周期控制。
- 控制命令与 SIGHUP 重载串行化，以及重复/并发命令的 409 冲突响应。
- MediaMTX 录像配置生成、回放监听、自动删除参数和录像磁盘状态查询。
- gatewayd/MediaMTX systemd 单元、非 root 权限边界、同步信号处理和优雅退出。

开发机软件路径与目标板卡单路真实媒体、录像、故障恢复和 systemd 生命周期均已验收。
30 分钟长稳、双真实输入和音频不在本轮完成范围内，限制在各自记录中明确保留。

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

### 7.2 HTTP 通道控制开发机验收

执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'gateway_(http_server|http|channel_manager|reload)_tests'
```

`gateway_channel_manager_tests` 验证：

- 管理器启动前和全局退出期间拒绝控制命令。
- 运行通道可以停止、停止通道可以重新启动，重启会更换工作进程。
- 重复启动和重复停止返回 `GW_ERR_CONFLICT`，未知通道返回
  `GW_ERR_NOT_FOUND`。
- 两个 POSIX 线程同时启动同一停止通道时，生命周期锁保证恰好一个成功，另一个得到
  状态冲突。
- 生命周期控制完成后仍可执行差异化配置重载，最终统一停止并回收全部工作进程。

`gateway_http_server_tests` 和 `gateway_http_tests` 验证：

- 三个动作路由只接受 POST，并返回包含通道 ID 与动作的 202 JSON。
- GET 动作路由返回 405 和 `Allow: POST`；未知通道返回 404；重复命令返回 409。
- 停止后状态变为 `STOPPED`，随后启动恢复为 `RUNNING`；启动和重启后的工作进程 PID
  与先前不同。
- 停止最后一个通道后 `gatewayd` 仍保持 HTTP 可用，并能再次启动该通道。
- 所有响应和日志继续检查测试 URL 密码不得泄露。

手工调用示例：

```bash
curl -i -X POST http://127.0.0.1:9080/v1/channels/cam01/stop
curl -i -X POST http://127.0.0.1:9080/v1/channels/cam01/start
curl -i -X POST http://127.0.0.1:9080/v1/channels/cam01/restart
```

预期：合法命令返回 202；重复命令返回 409；不存在通道返回 404。HTTP 服务目前没有
身份认证，只能在回环或等价的受信任网络边界内使用。

本增量验收记录：

```text
日期：2026-08-06
测试机器：x86_64 开发机
相关常规 CTest：4/4 PASS
完整常规 CTest：22/22 PASS
ASan/UBSan：22/22 PASS（LeakSanitizer 因 ptrace 环境关闭）
TSan：HTTP/通道管理器/重载相关 4/4 PASS
覆盖：HTTP 路由、通道管理器并发控制、SIGHUP 互斥、真实回环端到端控制
限制：使用假媒体进程；未连接真实 RTSP、MediaMTX 或 RK3588
```

RK3588 真实摄像头验收记录：

```text
日期：2026-08-06
输入：PC 真实摄像头，经 PC RTSP、RK3588 RKMpp/RGA 转码和板卡 MediaMTX
查询：health、channels、单通道和 recording 均为 HTTP 200
停止：202；状态进入 STOPPED；PID 628358 消失；RTSP 输出不可读
重复停止：409
启动：202；并发窗口内再次启动为 409；新工作进程 PID 676117
重启：202；新工作进程 PID 677115
恢复：启动和重启后 ffprobe 均得到 H.264 1920x1080@25；PC 跨主机读取 PASS
错误接口：未知通道 404；GET 控制动作 405
最终健康：HTTP 200，status=ok，running=1，failed=0
敏感信息：验收日志不包含输入 URL
结果：PASS
```

板卡仓库外证据：

```text
/home/cat/rk3588-acceptance/2026-08-06/phase4-http-control.log
/home/cat/rk3588-acceptance/2026-08-06/phase1-camera-gateway.log
```

### 7.3 MediaMTX 录像配置开发机验收

执行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'gateway_(unit|mediamtx_config|mediamtx_cli)_test'
```

覆盖：

- YAML 加载录像启用、目录、格式、part/segment 周期、part 大小、删除周期、最低空闲
  空间和回放监听字段。
- 拒绝相对目录、不安全字符、未知格式、越界周期/大小、非数值监听地址、非法端口和
  重复输出路径。
- 生成 MediaMTX playback、`pathDefaults` 和每通道录像配置；禁用通道不录像。
- 小输出缓冲区明确返回 `GW_ERR_OVERFLOW`。
- CLI 输出不混入 gateway 状态行、输入 RTSP URL 或测试密码，可以安全重定向为 YAML。

手工生成：

```bash
export CAM01_RTSP_URL='rtsp://user:example-password@camera/live'
./build/gatewayd --config config/gateway.example.yaml \
  --print-mediamtx-config > /tmp/mediamtx.generated.yml
sed -n '1,120p' /tmp/mediamtx.generated.yml
```

开发机只验证生成内容，不得标记真实录像通过。有 MediaMTX 的环境还应执行其配置检查或
前台启动，确认版本支持 `recordMaxPartSize` 等字段；真实流、回放和删除周期留给板卡验收。

本增量验收记录：

```text
日期：2026-08-06
测试机器：x86_64 开发机
MediaMTX：未安装
常规 CTest：24/24 PASS
ASan/UBSan：24/24 PASS（LeakSanitizer 因 ptrace 环境关闭）
结果：PASS（配置模型、渲染单元测试和 CLI 敏感信息边界）
限制：未启动 MediaMTX，未生成真实录像分段，未验证真实回放或自动删除
```

### 7.4 录像磁盘状态开发机验收

执行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'gateway_(recording_status|http_server|http)_tests'
```

覆盖：

- 录像关闭时返回 `disabled`，不访问目录。
- 对 `/tmp` 使用 `statvfs()` 取得非零总容量和当前用户可用容量。
- 将 `min_free_mb` 提高到开发机容量以上时稳定得到 `low_space`。
- 不存在的目录返回 `unavailable` 快照，不把操作系统路径或错误内容写入 HTTP。
- `GET /v1/recording` 返回容量和阈值；POST 返回 405 与 `Allow: GET`。
- 低空间时 `/v1/health` 为 `degraded` 且 `recording` 为 `low_space`。
- API 和日志继续检查不得出现输入 URL 或测试密码。

本增量验收记录：

```text
日期：2026-08-06
测试机器：x86_64 开发机
相关常规 CTest：3/3 PASS
完整常规 CTest：25/25 PASS
ASan/UBSan：25/25 PASS（LeakSanitizer 因 ptrace 环境关闭）
TSan：录像状态/HTTP/通道管理器/重载相关 5/5 PASS
测试目录：/tmp（仅 statvfs 查询，无文件创建或删除）
结果：PASS（开发机容量状态与 HTTP 降级路径）
限制：未制造真实磁盘写满，未启动 MediaMTX，未验证自动删除是否释放空间
```

RK3588 真实录像、回放、保留和低空间验收记录：

```text
日期：2026-08-06
输入：PC 真实摄像头 -> PC RTSP -> RK3588 RKMpp/RGA -> MediaMTX cam01
MediaMTX：v1.20.0 linux arm64
配置来源：板卡 gatewayd --print-mediamtx-config
录像目录：/home/cat/rk3588-acceptance/recordings（板卡本地，不是 PC）
格式：fMP4；part=1s；segment=5s；delete-after=15s
录像：PASS；MediaMTX recorder 报告 1 track (H264)，持续产生非空 MP4 分段
回放列表：PASS；返回 cam01 实际时间段和 get URL
回放下载：PASS；5 秒 MP4，3747338 bytes
回放探测：H.264，1920x1080，25/1 fps，duration=5.000000
自动删除：PASS；记录的最旧分段在 20 秒观察窗口内消失，新分段继续生成
常规磁盘状态：ok；目录存在，总量和当前用户可用量均非零
低空间模拟：min_free=40000 MiB，大于实际文件系统总量，不创建填充文件
低空间结果：recording=low_space，health=degraded，媒体通道保持 RUNNING
恢复：恢复 1 MiB 阈值后 recording=ok、health=ok、通道 RUNNING
音轨：无；当前 gateway 命令使用 -an，录像只含 H.264 视频
结果：PASS
```

此验收没有主动删除录像分段；删除动作由 MediaMTX 的 `recordDeleteAfter` 完成。低空间
测试只改变只读比较阈值，不写满系统盘。API 响应没有暴露录像目录。

板卡仓库外证据：

```text
/home/cat/rk3588-acceptance/2026-08-06/phase4-mediamtx-generated.yml
/home/cat/rk3588-acceptance/2026-08-06/phase4-recording-mediamtx.log
/home/cat/rk3588-acceptance/2026-08-06/phase4-recording-gateway.log
/home/cat/rk3588-acceptance/2026-08-06/phase4-recording-playback-retention.log
/home/cat/rk3588-acceptance/2026-08-06/phase4-playback-sample.mp4
/home/cat/rk3588-acceptance/2026-08-06/phase4-lowspace-recording.json
/home/cat/rk3588-acceptance/2026-08-06/phase4-lowspace-health.json
```

### 7.5 systemd 与优雅退出开发机验收

执行静态部署检查和 SIGTERM 端到端测试：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'gateway_(deployment|http)_tests'
cmake --install build --prefix /tmp/rk-media-gateway-install-test
```

`gateway_deployment_tests` 检查：

- gatewayd 使用非 root 用户、受保护 EnvironmentFile、SIGHUP ExecReload、
  `Restart=on-failure`、SIGTERM、control-group 清理和 20 秒停止上限。
- gatewayd 与 MediaMTX 都启用 NoNewPrivileges、只读系统目录和录像目录写白名单。
- gatewayd 在 ProtectClock 封闭设备策略下仅放行 RK3588 的 MPP、RGA、DMA heap 和 DRM
  节点；不得通过改成 root 或关闭全部设备隔离来绕过硬件权限。
- MediaMTX 生成配置显式关闭 RTMP、HLS、SRT 和 MoQ，RTSP 仅使用 TCP 并监听回环地址；
  防止 v1.20 默认 MoQ 自动证书写入与只读文件系统冲突。
- MediaMTX 独立服务运行，gatewayd 仅通过 `Wants`/`After` 表达启动顺序。
- tmpfiles 创建录像目录，部署文件不包含测试密码或内嵌 Password 字段。
- CMake 临时安装树包含二进制、两个服务单元、tmpfiles、环境示例、YAML 和部署文档。

`gateway_http_tests` 的停止路径检查：

- 向常驻 gatewayd 发送 SIGTERM 后退出码为 0。
- 主线程通过 signalfd 消费控制信号，TSan 不再报告处理器落入工作线程的数据竞争。
- 子进程的 posix_spawn 属性恢复默认 SIGINT/SIGTERM/SIGHUP 和空信号掩码。
- 最后一次 restart 后的 FFmpeg PID 已不存在，不遗留工作进程。
- 最后一次 restart 发生在 HTTP listener 已建立之后；测试检查假 FFmpeg 的 `/proc/PID/fd`
  不含 socket，防止监听或客户端描述符跨 exec 泄漏到工作进程。
- HTTP 端口已经关闭，不能继续接受健康检查。
- 日志不包含测试 URL 密码。

本增量验收记录：

```text
日期：2026-08-06
测试机器：x86_64 开发机，非 systemd 测试容器
相关常规 CTest：2/2 PASS
完整常规 CTest：26/26 PASS
ASan/UBSan：26/26 PASS（LeakSanitizer 因 ptrace 环境关闭）
TSan：录像状态/HTTP/通道管理器/重载相关 5/5 PASS
CMake 临时安装：PASS
systemd-analyze verify：单元被解析；因 /usr/local/bin/gatewayd 和 mediamtx 未安装而退出 1
结果：PASS（部署文件静态契约与进程优雅退出）；PENDING（真实 systemd 生命周期）
限制：未执行 enable、开机启动、异常自动恢复、真实 MediaMTX 停止或板卡设备权限验收
```

RK3588 首次启动与修复前记录：

```text
日期：2026-08-06
专用账号：rk-media-gateway，非 root，SupplementaryGroups=video,render
安装与配置权限：PASS；YAML 0640，环境文件 root:root 0600，录像目录服务账号 0750
首次 MediaMTX：FAIL；v1.20 默认 MoQ 写 auto.crt，被 ProtectSystem=strict 拒绝
首次 RKMPP：FAIL；ProtectClock 的封闭设备策略未放行 MPP/RGA/DMA heap/DRM
直接服务账号硬件测试：PASS；证明 Unix 组权限正确
临时 systemd + ProtectClock：可重复 FAIL
临时 systemd + ProtectClock + 精确 DeviceAllow：3 秒硬件解码/RGA/编码 PASS
修复后开发机常规 CTest：26/26 PASS
修复后 ASan/UBSan：首次 25/26（输入恢复观察窗口抖动），单项复跑 PASS，完整复跑 26/26 PASS
修复后适用 TSan：7/7 PASS
结果：PENDING；必须安装已提交修复并重新执行真实服务生命周期，不能用临时单元代替
```

RK3588 修复后 systemd 生命周期记录：

```text
日期：2026-08-06
提交：244f5b9、54f5e91
服务账号：UID/GID 997，非 root，补充组 video(44),render(107)
板卡完整 CTest：26/26 PASS
服务启用：mediamtx、rk-media-gateway 均 enabled/active
最小监听：HTTP 9080、RTSP 8554、playback 9996 为回环；WebRTC 8889/8189 对测试网开放
禁用协议：RTMP/HLS/SRT/MoQ 无监听
硬件设备：MPP、RGA、DMA heap、DRM 白名单下 H.264 1080p 硬件链路 PASS
描述符：真实 FFmpeg 不包含 gatewayd 9080 listener inode，PASS
优雅停止：ExecMainStatus=0，旧 gatewayd/FFmpeg PID 消失，MediaMTX 保持运行，PASS
重新启动：新 gatewayd PID 839668、FFmpeg PID 839784，RUNNING，PASS
gatewayd SIGKILL：旧 control group 清理，NRestarts=1，新 PID 841273/841415，PASS
MediaMTX SIGKILL：NRestarts=1，新 PID 843359；gatewayd PID 841273 不变，PASS
重新发布：工作 PID 843273，H.264 1920x1080，avg 25fps，PASS
WebRTC：PC 会话在 MediaMTX 恢复后重新建立并读取 cam01，PASS
录像/回放：恢复后继续生成非空 MP4，playback 返回新时间段，PASS
整机重启：PASS；boot ID 从 761393ed-c372-439d-b7a8-afa94292ec7c 变为
           7ecb42c7-e1e2-45b6-b0b6-f8cfd51ef9a6
开机启动：PASS；两个 enabled 服务在新系统启动约 4 秒后自动 active
网络就绪：启动初期 probe 按 1/2/4/8 秒退避；网络恢复后自动 RUNNING
重启后稳定窗口：6 分 12 秒，frame=9423，fps=25.19，speed=1.01，drop=0
进程：gatewayd/MediaMTX/FFmpeg 各 1 个，父子关系正确，zombie=0
最终资源样本：CPU 约 0.1%/15.2%/15.7%，RSS 约 2.2/55.1/20.8 MiB，41.6°C
最终输出：H.264 1920x1080，25/1 fps；录像状态 ok
安全：未发现凭据 URL；配置为 0640、环境文件 0600；unit verify 无项目告警
systemd security：两个服务均 6.5 MEDIUM
日志说明：浏览器慢读出现 WebRTC 丢帧告警，录像器发生一次时钟漂移重置；
          12:48 后无新的应用告警，媒体和录像持续正常
30 分钟长稳：SKIPPED（用户明确取消本轮执行，未标记 PASS）
验收清理：两个服务保持 enabled，优雅停止后均 inactive/ExecMainStatus=0；
          板卡 gatewayd/FFmpeg/MediaMTX/zombie 均为 0；PC 临时推流和 MediaMTX 已停止
结果：PASS（Phase 4 修订后的 5 分钟以上单路实机范围）
原始日志：/home/cat/rk3588-acceptance/2026-08-06/phase4-systemd-*.log
          /home/cat/rk3588-acceptance/2026-08-06/phase4-five-minute-final.log
```

### 7.6 Phase 4 开发机最终审计

三套动态测试必须依次执行，不要并行运行。每套都包含带严格时间阈值的进程监督测试，
并行运行多个 sanitizer 构建会互相争用 CPU，造成与功能无关的 progress 超时。

执行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake --build build-sanitize --parallel
ASAN_OPTIONS=detect_leaks=0 \
  ctest --test-dir build-sanitize --output-on-failure

cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure \
  -R 'gateway_(recording_status|http_server|http|channel_manager|reload|input_recovery|publish_recovery)_tests'

cmake -S . -B build-analyzer -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS=-fanalyzer
cmake --build build-analyzer --clean-first --parallel

git diff --check
rg -n '\(void\)[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' \
  --glob '*.[ch]' .
rg -n 'system[[:space:]]*\(|popen[[:space:]]*\(' src include
git grep -n -E '://[^/@[:space:]]+:[^/@[:space:]]+@' -- \
  ':!tests/**' ':!docs/**' ':!README.md' ':!ARCHITECTURE.md' \
  ':!deploy/systemd/gateway.env.example'
```

静态分析结果需人工沿资源所有权复核，不能只统计告警数。本次检查确认：HTTP 客户端由
服务器循环关闭；监听描述符由服务器停止/销毁路径关闭；进程启动失败路径关闭四个管道
端点；单字符转义缓冲区在读取前已初始化。GCC 仍报告 7 条描述符所有权告警和 2 条
单字符缓冲区告警，因此记录为“已复核”，不记录为“零告警”。

最终验收记录：

```text
日期：2026-08-06
测试机器：x86_64 Arch Linux 开发机
常规 CTest：26/26 PASS（单独完整运行）
ASan/UBSan：26/26 PASS（LeakSanitizer 因 ptrace 环境关闭）
TSan：适用的录像/HTTP/通道管理器/重载/故障恢复测试 7/7 PASS
GCC -fanalyzer：全量构建完成；9 条告警已按上述所有权和数据流人工复核
安全检查：git diff、显式 (void) 调用、shell 启动和受管文件凭据 URL 检查 PASS
实板补充：真实 MediaMTX、PC 摄像头 RTSP、RK3588 MPP/RGA 和 systemd 生命周期 PASS
限制：无音频；未执行双真实输入与 30 分钟长稳；LeakSanitizer 受 ptrace 环境限制
```

补充记录：首次把常规、ASan/UBSan、TSan 三套测试并行运行时，常规
`gateway_channel_manager_tests` 因 CPU 饥饿触发 1 秒 progress 超时；随后在正常的单独
完整运行中 26/26 PASS。该结果用于约束测试执行方式，不冒充三套并行压力测试通过。

2026-08-06 文档收尾后再次依次执行三套门禁：常规 26/26、ASan/UBSan 26/26、适用
TSan 7/7 PASS。ASan 首次在受限沙箱中为 25/26，唯一失败项因回环 bind 返回
`Operation not permitted`；在获准使用本机回环后完整复跑为 26/26。

目标板卡覆盖结果：

- API 默认只监听回环地址：PASS。
- MediaMTX 录像与回放：PASS。
- 磁盘空间不足处理：PASS（阈值模拟，不写满磁盘）。
- systemd 开机启动、停止和异常恢复：PASS。

通过标准：设备重启后服务自动启动，接口可用，异常退出后 systemd 能恢复服务。结果：PASS。

## 8. Phase 5：性能、稳定性与最终交付

状态：`IN PROGRESS`

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

### 8.1 C17 运行指标采样工具

构建并执行开发机测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'gateway_(process_metrics|metrics_cli|metrics_summary)_test'
```

`gateway_process_metrics_tests` 验证：

- 从 `/proc/<pid>/stat` 提取 user/system CPU tick，兼容进程名中的空格和右括号。
- 接受 stat 前置字段中的合法负终端进程组，拒绝缺字段、非法数字和负 CPU tick。
- 从 `/proc/<pid>/status` 读取以 kB 表示的 `VmRSS`，拒绝错误单位和缺失字段。
- 读取测试进程自身的 CPU tick、RSS 和文件描述符数。

`gateway_metrics_cli_test` 使用 `self=self` 采样 1 秒，验证 CSV 表头和全部样本可用。手工
短时采样示例：

```bash
./build/gateway-metrics \
  --target gateway=<gatewayd-pid> \
  --target ffmpeg=<ffmpeg-pid> \
  --target mediamtx=<mediamtx-pid> \
  --duration-sec 5 --summary-output /tmp/gateway-metrics-summary.csv \
  --interval-ms 1000 > /tmp/gateway-metrics.csv
```

`gateway_metrics_summary_test` 验证汇总中的可用样本、CPU 样本、CPU 平均/峰值、
RSS 平均/峰值和 FD 范围均有效，并验证第二次使用同一
`--summary-output` 会在采样前失败。汇总文件只包标签和进程指标，不接收 URL。

预期：首样本 `cpu_percent` 为空，后续样本为相邻 tick 的 CPU 百分比；100% 表示一个
逻辑 CPU。采样期间任何 PID 消失时相应行必须为 `unavailable` 且程序非零退出。CSV 不
接收 URL 或凭据参数。FPS、速度、丢帧和重启次数必须另存开始/结束 HTTP 快照；端到端
延迟必须使用时间戳画面，不能从 CPU 或 `speed` 推断。

开发机增量记录：

```text
日期：2026-08-06
语言：C17
首轮结果：FAIL；stat 前置负终端字段被错误当作无符号数，真实 self 采样 unavailable
修复：字段 4～13 按有符号数跳过，仅 CPU tick 字段 14～15 按无符号数读取
相关测试：2/2 PASS
完整常规 CTest：28/28 PASS
ASan/UBSan：28/28 PASS（LeakSanitizer 因 ptrace 环境关闭）
既有适用 TSan：7/7 PASS
CMake 临时安装：PASS；gatewayd 和 gateway-metrics 均存在
安全检查：git diff、显式 (void) 调用、system()/popen() 扫描 PASS
实板性能数据：PENDING
长时间测试：本增量不执行
```

汇总输出增量记录：

```text
日期：2026-08-06
相关测试：gateway_metrics_summary_test 与 gateway_benchmark_runner_tests 2/2 PASS
完整常规 CTest：30/30 PASS
完整 ASan/UBSan：30/30 PASS（LeakSanitizer 因 ptrace 环境关闭）
适用 TSan：7/7 PASS
CMake 临时安装树：PASS
失效目标：汇总保留 unavailable 计数，CLI 退出码 2
防覆盖：PASS；已有汇总路径在采样前失败
安全扫描：Bash 语法、git diff、无 eval/system()/popen()、无显式 (void) 弃值调用 PASS
长时间测试：本增量不执行
```

### 8.2 RK3588 固定样本短时性能对比

本节只执行短时吞吐微基准和 10 秒正式服务基线，不执行 30 分钟或更长稳定性测试。

先从真实 PC 摄像头 RTSP 抓取一次固定输入，后续三条路径不得分别读取在线流：

```bash
export PHASE5_DIR=/home/cat/rk3588-acceptance/2026-08-06/phase5
mkdir -p "$PHASE5_DIR"
/usr/local/bin/ffmpeg -nostdin -hide_banner -loglevel warning \
  -rtsp_transport tcp -i rtsp://PC-ADDRESS:8554/source \
  -t 15 -map 0:v:0 -an -c:v copy -y "$PHASE5_DIR/pc-camera-15s.mkv"
/usr/local/bin/ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate:format=duration,size \
  "$PHASE5_DIR/pc-camera-15s.mkv"
/usr/bin/ffmpeg -nostdin -v error -i "$PHASE5_DIR/pc-camera-15s.mkv" -an -f null -
```

预期：固定样本为 H.264 1920×1080@25、15 秒，完整软件解码无错误。记录 SHA-256，
禁止在报告里写入带凭据 URL。

三条路径分别使用 Debian FFmpeg 的 `h264 + libx264`、FFmpeg-Rockchip 的
`h264_rkmpp + h264_rkmpp`，以及增加
`scale_rkrga=w=1920:h=1080:format=nv12` 的硬件路径。共同约束：

- 循环读取同一个固定样本，输出 H.264 1920×1080@25、6000 kbit/s 到 null muxer。
- 进程启动 2 秒后运行 `gateway-metrics --duration-sec 10 --interval-ms 1000`。
- 指标采集完成后只向该明确 FFmpeg PID 发送 SIGINT；退出码 255 是本测试的受控停止。
- 保存 progress、stderr、CSV、开始/结束温度；stderr 有错误或 CSV 有 unavailable 即失败。
- 另为每条路径实际生成 3 秒 Matroska，使用 ffprobe 和完整软件解码验证。

三条微基准 FFmpeg argv 分别为：

```bash
/usr/bin/ffmpeg -nostdin -hide_banner -loglevel warning \
  -progress "$PHASE5_DIR/formal-software-progress.log" \
  -stream_loop -1 -c:v h264 -i "$PHASE5_DIR/pc-camera-15s.mkv" -an \
  -c:v libx264 -preset veryfast -tune zerolatency -profile:v high \
  -pix_fmt yuv420p -b:v 6000k -r 25 -g 50 -f null -

/usr/local/bin/ffmpeg -nostdin -hide_banner -loglevel warning \
  -progress "$PHASE5_DIR/formal-mpp-progress.log" -stats_period 1 \
  -stream_loop -1 -hwaccel rkmpp -hwaccel_output_format drm_prime \
  -c:v h264_rkmpp -i "$PHASE5_DIR/pc-camera-15s.mkv" -an \
  -c:v h264_rkmpp -b:v 6000k -r 25 -f null -

/usr/local/bin/ffmpeg -nostdin -hide_banner -loglevel warning \
  -progress "$PHASE5_DIR/formal-mpp-rga-progress.log" -stats_period 1 \
  -stream_loop -1 -hwaccel rkmpp -hwaccel_output_format drm_prime \
  -c:v h264_rkmpp -i "$PHASE5_DIR/pc-camera-15s.mkv" \
  -vf scale_rkrga=w=1920:h=1080:format=nv12 -an \
  -c:v h264_rkmpp -b:v 6000k -r 25 -f null -
```

每条命令均单独后台启动并记录 `$!`，不能同时运行三条路径；否则 CPU、温度和带宽竞争会
破坏比较。预热、采样和 SIGINT 步骤按上面的共同约束执行。

正式服务采样必须用服务账号读取同 UID 进程：

```bash
sudo -u rk-media-gateway /usr/local/bin/gateway-metrics \
  --target gateway=<gateway-pid> \
  --target ffmpeg=<ffmpeg-pid> \
  --target mediamtx=<mediamtx-pid> \
  --duration-sec 10 --interval-ms 1000 > "$PHASE5_DIR/service-metrics.csv"
```

普通管理用户可能因 `/proc/<pid>/fd` 权限得到 unavailable；不能因此放宽服务权限或把
unavailable CSV 写成通过。采样前后保存通道 HTTP 快照并确认 PID、重启数和丢帧数。

实板记录：

```text
日期：2026-08-06
提交：4ffa28d
板卡构建/CTest：28/28 PASS
固定输入：PC 摄像头 H.264 1920x1080@25，15.000 秒，14972221 bytes
样本 SHA-256：ea17e825205b953a0ba81c29293b5ac08f2d8120d97109cf23e5640fde2f5052
在线 RTSP 预跑：INVALID；软件宏块错误、MediaMTX 慢读丢帧、硬件路径重复帧
软件正式短测：CPU 306.7%/330.0%，RSS 158.7/159.7 MiB，12.46fps，0.498x，drop=0
MPP 正式短测：CPU 75.4%/83.0%，RSS 17.2/17.2 MiB，492.93fps，19.7x，drop=0
MPP+RGA 正式短测：CPU 75.2%/85.0%，RSS 18.1/18.1 MiB，497.70fps，19.9x，drop=0
实际输出：三个 3 秒 H.264 1080p25 文件均通过 ffprobe 和完整软件解码
正式服务 10 秒：gateway/FFmpeg/MediaMTX CPU 平均 0.2%/18.6%/9.1%
正式服务 RSS：2.14/18.72/45.10 MiB；FD：7/69/12，样本内不变
正式服务媒体：25.35fps，1.02x，drop=0，restart=0，H.264 1080p25 输出 PASS
温度：服务窗口约 42.5°C；无应用告警或僵尸进程
清理：板卡服务 enabled/inactive，ExecMainStatus=0；板卡和 PC 临时进程为 0
长时间测试：SKIPPED；按用户要求未执行
结果：PASS（仅固定样本微基准与正式服务短基线）
```

原始证据位于板卡仓库外的
`/home/cat/rk3588-acceptance/2026-08-06/phase5/`。微基准是最大吞吐测试，不代表
在线网关会以 493fps 输出；正式在线输出仍为 25fps。端到端延迟和其他矩阵项保持 PENDING。

### 8.3 固定样本基准运行器

开发机行为测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R gateway_benchmark_runner_tests
```

`gateway_benchmark_runner_tests` 使用 C 实现的可控 FFmpeg 夹具，不需要真实媒体或
MPP 硬件。它验证：

- 软件路径改变尺寸时使用 swscale，MPP+RGA 使用 `scale_rkrga`。
- `mpp` 模式输出尺寸与输入不同时拒绝运行。
- 同模式、分辨率的任何结果文件已存在时拒绝覆盖。
- 指标 CSV、验证输出均生成，运行器源码不包含 `eval`。
- 指标汇总 CSV 由 `gateway-metrics` 直接生成，运行器将它纳入防覆盖列表。

安装树检查：

```bash
install_root=$(mktemp -d /tmp/rk-gateway-install.XXXXXX)
DESTDIR="$install_root" cmake --install build
test -x "$install_root/usr/local/share/rk-media-gateway/scripts/run_transcode_benchmark.sh"
```

RK3588 上的短测示例（一次只运行一条）：

```bash
sample=/home/cat/rk3588-acceptance/2026-08-06/phase5/pc-camera-15s.mkv
result=/home/cat/rk3588-acceptance/2026-08-06/phase5-720p
runner=/usr/local/share/rk-media-gateway/scripts/run_transcode_benchmark.sh

"$runner" --mode software --input "$sample" --output-dir "$result" \
  --output-width 1280 --output-height 720 --bitrate-kbps 3000
"$runner" --mode mpp-rga --input "$sample" --output-dir "$result" \
  --output-width 1280 --output-height 720 --bitrate-kbps 3000
```

通过标准：两次运行均返回 0，CSV 不含 `unavailable`，stderr 无解码/编码
错误，`output-probe.txt` 显示 H.264 1280×720，输出可完整软件解码。
`output-decode.log` 可能含硬件库版本信息，不能只以非空判定失败。每个模式
默认只运行约 15 秒，本节不是 30 分钟或长时间稳定性测试。

开发机增量记录：

```text
日期：2026-08-06
常规行为测试：PASS
ASan/UBSan 行为测试：PASS
完整常规 CTest：29/29 PASS
完整 ASan/UBSan：29/29 PASS（LeakSanitizer 因 ptrace 环境关闭）
适用 TSan：7/7 PASS
临时安装树：PASS；运行器为可执行安装文件
零预热诊断：ASan 启动较慢时夹具尚未安装信号处理器，改为 1 秒测试预热
安全边界：本地普通文件、非根绝对输出目录、防覆盖、无 eval
RK3588 构建：Release，29/29 PASS；使用用户级 CMake 3.31.10
RK3588 软件 720p：CPU 282.2%/300.0%，RSS 137.5/138.4 MiB，19.20fps/0.768x，drop=0
RK3588 MPP+RGA 720p：CPU 137.5%/146.0%，RSS 18.3/18.4 MiB，1038.54fps/41.5x，drop=0
输出验证：两份 3 秒 H.264 1280x720@25 均通过 ffprobe 和独立完整软件解码
安全清理：无 unavailable、无解码/编码错误、无残留媒体或僵尸进程
服务状态：rk-media-gateway/mediamtx 均 enabled/inactive
原始证据：/home/cat/rk3588-acceptance/2026-08-06/phase5-720p
长时间测试：本增量不执行
```

### 8.4 RK3588 720p 码率矩阵

在 8.3 的 3000 kbit/s 结果上补充 1500 和 6000 kbit/s；每个码率使用
独立目录，避免触发防覆盖机制：

```bash
sample=/home/cat/rk3588-acceptance/2026-08-06/phase5/pc-camera-15s.mkv
runner=/usr/local/share/rk-media-gateway/scripts/run_transcode_benchmark.sh

for rate in 1500 6000; do
  result=/home/cat/rk3588-acceptance/2026-08-06/phase5-bitrate-$rate
  "$runner" --mode software --input "$sample" --output-dir "$result" \
    --output-width 1280 --output-height 720 --bitrate-kbps "$rate"
  "$runner" --mode mpp-rga --input "$sample" --output-dir "$result" \
    --output-width 1280 --output-height 720 --bitrate-kbps "$rate"
done
```

一次只能运行一条路径。每组必须检查 `metrics-summary.csv` 中
`unavailable_samples=0`，progress 中 `drop_frames=0`，并复核 ffprobe、完整软件解码、
stderr、温度与进程清理。

实板记录：

```text
日期：2026-08-06
提交：7fd1f3d
板卡 Release 构建/CTest：30/30 PASS
软件 1500k：CPU 261.3%/294.0%，RSS 138.0/139.0 MiB，20.16fps/0.806x，drop=0
软件 6000k：CPU 283.0%/314.0%，RSS 138.1/139.3 MiB，17.97fps/0.719x，drop=0
MPP+RGA 1500k：CPU 135.6%/147.0%，RSS 18.3/18.5 MiB，1031.72fps/41.3x，drop=0
MPP+RGA 6000k：CPU 158.1%/164.0%，RSS 18.0/18.2 MiB，1011.65fps/40.5x，drop=0
汇总：四组均 11 available/0 unavailable，10 个 CPU 有效样本
实际输出：四份 3 秒 H.264 1280x720@25 均通过 ffprobe 和独立完整软件解码
日志：无解码/编码错误；软件路径仅有已知像素范围弃用警告
温度：1500k 软件 40.7->47.2°C，MPP+RGA 42.5->52.7°C
      6000k 软件 45.3->51.8°C，MPP+RGA 46.2->56.4°C
清理：gatewayd/FFmpeg/MediaMTX/zombie 均为 0，服务均 enabled/inactive
长时间测试：未执行
结果：PASS（仅码率短时矩阵）
```

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
