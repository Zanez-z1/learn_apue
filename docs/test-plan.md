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

- 独立的通道状态查询接口。
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
结果：PASS（独立 supervisor 模块增量，常规与 ASan/UBSan 均为 14/14 通过）
覆盖：创建、双管道、退出检测、SIGTERM、SIGKILL、回收、日志密码脱敏、
      ffprobe 成功/失败/超时/编码不匹配、状态机、启动超时、progress 超时、
      稳定窗口、探测及工作进程退避重试、重试耗尽、supervisor 参数契约和
      探测阶段外部停止
限制：开发机使用 ffprobe/FFmpeg 夹具，尚未连接真实 RTSP、Rockchip FFmpeg 或
      MediaMTX，不代表完整 Phase 2 通过
```

通过标准：不手工执行 FFmpeg，通过 `gatewayd` 启停一路真实转码。

## 6. Phase 3：多通道与异常恢复

状态：`PENDING`

功能完成后，本节需要覆盖：

- 多个通道互不影响。
- 输入流断开与恢复。
- FFmpeg 异常退出。
- MediaMTX 停止与恢复。
- 退避时间序列。
- 最大重试次数与 `FAILED` 状态。
- SIGHUP 配置重载和差异化重启。

通过标准：故障解除后通道自动恢复，其他通道不被重启或中断。

## 7. Phase 4：控制接口、录像和服务化

状态：`PENDING`

功能完成后，本节需要覆盖：

- HTTP 健康检查和通道查询。
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
