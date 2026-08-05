# RK3588 Media Gateway

运行在 RK3588 嵌入式 Linux 上的硬件加速视频转码与流媒体网关。自行开发的
`gatewayd` 使用 C17；FFmpeg-Rockchip 负责 MPP/RGA 媒体处理，MediaMTX 负责
RTSP、WebRTC 和 HLS 分发。

当前处于初始开发阶段，已经具备：

- YAML 配置读取、环境变量展开及边界校验。
- RTSP URL 密码脱敏。
- 不经过 shell 的 FFmpeg `argv` 参数构造。
- FFmpeg `-progress pipe:1` 增量解析。
- 配置检查与脱敏 dry-run 命令输出。
- 单通道 FFmpeg 工作进程创建、输出管道读取和退出回收。
- 接收 `SIGINT`/`SIGTERM` 后先正常停止，超时再强制清理工作进程组。
- RK3588 媒体环境检查脚本。

多通道状态机、自动重试、配置重载和 HTTP API 尚未实现，不能将当前版本作为
完整网关服务部署。

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
`posix_spawnp()`，不会交给 shell 执行。不带检查选项时，当前版本会启动
唯一一个启用的通道；需要使用包含 Rockchip MPP/RGA 支持的 FFmpeg。

## 板卡环境检查

在 RK3588 板卡上执行：

```bash
./scripts/check_media_env.sh
```

脚本会检查架构、MPP/RGA/DRM 设备权限、Rockchip 编解码器、RGA 滤镜和
MediaMTX。完整设计见 [ARCHITECTURE.md](ARCHITECTURE.md)，可重复执行的分阶段
测试步骤和验收记录见 [docs/test-plan.md](docs/test-plan.md)。
