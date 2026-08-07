# systemd 部署指南

本文说明如何把已验证的构建安装为长期运行的非 root systemd 服务。全新板卡先完成
第 1～3 节，再回到 [从零跑通与演示完整视频链路](demo.md) 做前台验证；验证通过后才
执行第 4 节启用服务。已经部署过 MediaMTX 的开发板，可以先按演示文档运行
`./build/gatewayd`，不需要每次重新安装。

## 1. 部署前提和外部组件

本仓库不会构建或安装 FFmpeg-Rockchip、Rockchip MPP/RGA 和 MediaMTX。开始前确认：

```bash
cmake --version
pkg-config --modversion yaml-0.1
command -v ffmpeg
command -v ffprobe
command -v mediamtx
ffmpeg -hide_banner -decoders | grep h264_rkmpp
ffmpeg -hide_banner -encoders | grep h264_rkmpp
ffmpeg -hide_banner -filters | grep scale_rkrga
```

项目要求 CMake 3.20 及以上、C17 编译器、libyaml 开发包、FFmpeg-Rockchip 和与板卡架构
匹配的 MediaMTX。Debian 标准 FFmpeg 不包含 `h264_rkmpp`/`scale_rkrga`，不能替代。
MediaMTX 示例配置按 v1.20 验证；其他版本应先检查配置字段兼容性。

外部组件就绪后执行：

```bash
./scripts/check_media_env.sh
```

环境脚本失败时先解决 MPP/RGA/DRM 设备权限或媒体软件问题，不要继续排查 `gatewayd`。

## 2. 安装本项目程序和服务文件

在目标板卡安装编译依赖并构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
sudo cmake --install build
```

创建无登录服务账号和所需目录：

```bash
sudo useradd --system --user-group --home-dir /var/lib/rk-media-gateway \
  --shell /usr/bin/nologin rk-media-gateway
sudo usermod -a -G video,render rk-media-gateway
sudo systemd-tmpfiles --create /usr/local/lib/tmpfiles.d/rk-media-gateway.conf
```

如果发行版使用不同的设备访问组，按 `/dev/mpp_service`、`/dev/rga` 和 `/dev/dri/*`
的实际属组调整补充组，不能为方便而把服务改成 root。服务单元还在 `ProtectClock` 带来的
封闭设备策略中显式放行 RK3588 使用的 MPP、RGA、DMA heap、DRM card0 和 renderD128；
如果板卡节点名称不同，应按实际节点修改 `DeviceAllow`，不能直接关闭全部设备隔离。

`cmake --install` 安装的是 `gatewayd`、`gateway-metrics`、systemd 单元、文档和脚本；
不会替你下载外部媒体组件，也不会覆盖 `/etc/rk-media-gateway` 中的真实配置。

## 3. 安装配置与凭据

```bash
sudo install -d -m 0750 -o root -g rk-media-gateway /etc/rk-media-gateway
sudo install -m 0640 -o root -g rk-media-gateway \
  config/gateway.example.yaml /etc/rk-media-gateway/gateway.yaml
sudo install -m 0640 -o root -g rk-media-gateway \
  config/mediamtx.example.yml /etc/rk-media-gateway/mediamtx.yml
sudo install -m 0600 -o root -g root \
  deploy/systemd/gateway.env.example /etc/rk-media-gateway/gateway.env
sudoedit /etc/rk-media-gateway/gateway.env
```

把环境文件中的虚构地址替换为真实 RTSP URL。不要把环境文件、终端输出或 journal 中
的凭据提交到 Git。修改录像参数或通道输出路径后，在受控 shell 中加载环境变量并执行
`gatewayd --print-mediamtx-config`，检查输出后再以 0640 权限替换 `mediamtx.yml`。

## 4. 启动和重载

先检查配置：

```bash
sudo -u rk-media-gateway env \
  CAM01_RTSP_URL='rtsp://user:example-only@camera.example/live' \
  /usr/local/bin/gatewayd --config /etc/rk-media-gateway/gateway.yaml \
  --check-config
```

上面的 URL 仍是虚构示例；真实检查应从受保护的环境文件加载，避免进入 shell 历史。
确认后启动：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mediamtx.service rk-media-gateway.service
systemctl status mediamtx.service rk-media-gateway.service --no-pager
journalctl -u rk-media-gateway.service -n 100 --no-pager
```

仅修改通道输入、转码或重试参数时：

```bash
sudo systemctl reload rk-media-gateway.service
```

修改 HTTP 监听或 MediaMTX 录像字段时，gatewayd 会拒绝 SIGHUP 热更新。此时分别检查
并部署配置，再重启对应服务：

```bash
sudo systemctl restart mediamtx.service
sudo systemctl restart rk-media-gateway.service
```

## 5. 优雅停止与故障恢复

```bash
sudo systemctl stop rk-media-gateway.service
systemctl show rk-media-gateway.service \
  -p ActiveState -p SubState -p Result -p ExecMainStatus
pgrep -a gatewayd
pgrep -a ffmpeg
```

systemd 向 gatewayd 发送 SIGTERM。gatewayd 先通知并回收每个 supervisor/FFmpeg 进程，
再停止 HTTP 线程并退出；`KillMode=control-group` 和 `TimeoutStopSec=20s` 是异常路径的
兜底。正常退出不会触发 `Restart=on-failure`，异常退出会在 5 秒后恢复。

## 6. 服务安全边界

- gatewayd、MediaMTX 使用同一非 root 账号，以便共享录像目录。
- `ProtectSystem=strict`，只有 `/var/lib/rk-media-gateway` 可写。
- HTTP、playback 和示例 RTSP 都只监听回环地址；跨主机访问必须另加认证、TLS、防火墙
  或受控反向代理。
- 生成的 MediaMTX 配置只启用 TCP RTSP、WebRTC 和按需 playback；未使用的 RTMP、HLS、
  SRT 和 MoQ 显式关闭，避免版本新增协议默认开启或 MoQ 自动证书写入与只读服务冲突。
- 环境文件建议 0600，YAML 建议 0640；真实 URL 密码不得进入仓库或测试记录。
- 设备节点和媒体软件版本依赖板卡系统，服务文件不能证明 MPP/RGA 或真实录像已通过。
