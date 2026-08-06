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
- 提供本地只读 HTTP 健康检查和通道状态查询。
- RK3588 媒体环境检查脚本。

HTTP 写控制、录像和 systemd 部署尚未实现，也尚未通过 RK3588 真实媒体链路验收，
不能将当前版本作为完整网关服务部署。

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

## 只读 HTTP 状态

示例配置默认在 `127.0.0.1:9080` 启用 HTTP。`server.listen` 必须是数值 IPv4 或
IPv6 地址；如不需要控制面，可设置 `server.enabled: false`。

```bash
curl http://127.0.0.1:9080/v1/health
curl http://127.0.0.1:9080/v1/channels
curl http://127.0.0.1:9080/v1/channels/cam01
```

当前接口只接受 GET，不返回输入 URL 或密码。监听地址、端口和启用状态的修改需要
重启 `gatewayd`，不会通过 SIGHUP 生效。

## 板卡环境检查

在 RK3588 板卡上执行：

```bash
./scripts/check_media_env.sh
```

脚本会检查架构、MPP/RGA/DRM 设备权限、Rockchip 编解码器、RGA 滤镜和
MediaMTX。完整设计见 [ARCHITECTURE.md](ARCHITECTURE.md)，可重复执行的分阶段
测试步骤和验收记录见 [docs/test-plan.md](docs/test-plan.md)。
