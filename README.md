# RK3588 Media Gateway

运行在 RK3588 嵌入式 Linux 上的硬件加速视频转码与流媒体网关。自行开发的
`gatewayd` 使用 C17；FFmpeg-Rockchip 负责 MPP/RGA 媒体处理，MediaMTX 负责
RTSP、WebRTC 和 HLS 分发。

当前处于开发阶段，已经具备：

- YAML 配置读取、环境变量展开及边界校验。
- RTSP URL 密码脱敏。
- 不经过 shell 的 FFmpeg `argv` 参数构造。
- FFmpeg `-progress pipe:1` 增量解析。
- 配置检查与脱敏 dry-run 命令输出。
- 每通道独立的 FFmpeg 工作进程创建、输出管道读取和退出回收。
- 接收 `SIGINT`/`SIGTERM` 后先正常停止，超时再强制清理工作进程组。
- 每通道独立的状态转换、启动/progress 超时、稳定窗口和有上限的退避重试。
- 启动 FFmpeg 前执行 ffprobe 输入探测，并检查视频编码与硬件解码器是否匹配。
- 多个启用通道并行运行，一个通道失败不会停止其他通道。
- 支持通过 SIGHUP 重新读取配置，只新增、删除或重启发生变化的通道。
- 提供本地 HTTP 健康检查、通道状态查询和启动/停止/重启控制。
- 提供录像文件系统容量、最低空闲阈值和降级状态查询。
- RK3588 媒体环境检查脚本。

Phase 4 已完成开发机测试和 RK3588 单路实机验收：PC 摄像头可经 RTSP 输入、MPP/RGA
硬件转码、MediaMTX 录像与回放，并由 systemd 以非 root 账号开机启动和异常恢复。
当前媒体链路只有视频，不采集或输出音频；长时间压力、多路真实输入和性能对比留待后续阶段。

## 构建

依赖 CMake、支持 C17 的 C 编译器以及 libyaml 开发包。

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## 配置检查与 dry-run

示例配置通过环境变量注入源地址：

```bash
export CAM01_RTSP_URL='rtsp://user:password@camera.example/live'
./build/gatewayd --config config/gateway.example.yaml --check-config
./build/gatewayd --config config/gateway.example.yaml --dry-run
./build/gatewayd --config config/gateway.example.yaml
```

dry-run 输出会隐藏 URL 密码。输出内容只用于诊断；程序会将参数数组直接交给
`posix_spawnp()`，不会交给 shell 执行。不带检查选项时，当前版本会并行启动
所有启用的通道；需要使用包含 Rockchip MPP/RGA 支持的 FFmpeg。

## 配置重载

修改正在使用的 YAML 文件后，向 `gatewayd` 发送 SIGHUP：

```bash
kill -HUP <gatewayd-pid>
```

程序会先完整加载并校验候选配置。配置无效时保留现有通道并记录拒绝原因；配置有效
时，未变化通道继续运行，只对新增、删除或内容变化的通道执行对应操作。SIGINT 和
SIGTERM 仍用于停止全部通道。

## HTTP 状态与控制

示例配置默认在 `127.0.0.1:9080` 启用 HTTP。`server.listen` 必须是数值 IPv4 或
IPv6 地址；如不需要控制面，可设置 `server.enabled: false`。

```bash
curl http://127.0.0.1:9080/v1/health
curl http://127.0.0.1:9080/v1/channels
curl http://127.0.0.1:9080/v1/channels/cam01
curl http://127.0.0.1:9080/v1/recording
curl -X POST http://127.0.0.1:9080/v1/channels/cam01/stop
curl -X POST http://127.0.0.1:9080/v1/channels/cam01/start
curl -X POST http://127.0.0.1:9080/v1/channels/cam01/restart
```

查询接口只接受 GET，控制接口只接受 POST。成功接受控制命令时返回 202；通道不存在
返回 404；命令与当前状态冲突返回 409。接口不返回输入 URL 或密码。

当前 HTTP 服务没有身份认证，默认回环监听是安全边界，不应直接暴露到不受信任的网络。
监听地址、端口和启用状态的修改需要重启 `gatewayd`，不会通过 SIGHUP 生效。
`gatewayd` 默认保持常驻，即使所有通道均已停止也可通过接口重新启动；仅批处理场景可
使用 `--exit-when-idle` 让程序在全部通道结束后退出。

## MediaMTX 录像配置

`mediamtx.recording` 配置录像目录、格式、分段、自动删除周期和回放监听地址。根据网关
通道生成可直接交给 MediaMTX 的配置：

```bash
export CAM01_RTSP_URL='rtsp://user:password@camera.example/live'
./build/gatewayd --config config/gateway.example.yaml \
  --print-mediamtx-config > mediamtx.generated.yml
mediamtx mediamtx.generated.yml
```

生成内容不会包含输入 URL 或密码。示例 [config/mediamtx.example.yml](config/mediamtx.example.yml)
启用 fMP4 录像、按通道分目录、7 天自动删除和仅回环回放。修改录像参数后需要重新生成
MediaMTX 配置并重载或重启 MediaMTX；向 `gatewayd` 发送 SIGHUP 不会修改外部服务。
`GET /v1/recording` 返回录像文件系统总量、当前用户可用量、阈值和
`ok`/`low_space`/`unavailable` 状态；目录本身不会暴露在 API 中。空间不足时健康检查
变为 `degraded`，自动删除仍由 MediaMTX 的 `recordDeleteAfter` 执行。

## systemd 部署

安装目标包含 `rk-media-gateway.service`、独立的 `mediamtx.service`、tmpfiles 目录规则
和环境文件示例。基本入口：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
sudo cmake --install build
```

不要直接启动示例配置；先创建非 root 服务账号、安装真实配置并以 0600 权限保存凭据。
完整步骤、重载边界、优雅停止和安全说明见
[docs/deployment.md](docs/deployment.md)。

## 板卡环境检查

在 RK3588 板卡上执行：

```bash
./scripts/check_media_env.sh
```

脚本会检查架构、MPP/RGA/DRM 设备权限、Rockchip 编解码器、RGA 滤镜和
MediaMTX。完整设计见 [ARCHITECTURE.md](ARCHITECTURE.md)，可重复执行的分阶段
测试步骤和验收记录见 [docs/test-plan.md](docs/test-plan.md)。
