# 单路摄像头用户手册（零基础版）

这份手册只讲当前已经实测通过的单路用法：使用一台 Linux PC 的 USB 摄像头作为输入，
RK3588 板卡负责硬件转码和运行管理，最后在 PC 浏览器中查看实时画面与 OSD 状态。

不需要先理解 C、FFmpeg、RTSP 或 WebRTC。请按顺序操作，不要跳步。

## 1. 使用前确认

需要准备：

- 一块已经部署本项目的 RK3588 板卡；
- 一台带普通 USB/UVC 摄像头的 Linux PC；
- PC 和板卡连接到同一个可信局域网；
- PC 上有本项目源码；
- 两个 PC 终端和一个浏览器窗口。

当前实测地址是：

```text
板卡：192.168.1.45
通道：cam01
PC 摄像头：/dev/video0
```

换网络后，板卡地址可能变化。在板卡终端执行下面的命令查看地址：

```bash
ip -brief address
```

找到类似 `192.168.1.45/24` 的局域网地址。本文后续出现的 `192.168.1.45` 都要替换为
你的实际板卡地址。

> 本手册假定板端已经部署完成。若两个 systemd 服务尚未安装，请先阅读
> [systemd 部署指南](deployment.md)，部署成功后再回到这里。

## 2. 先理解每台机器在做什么

```text
PC 摄像头
  -> PC FFmpeg 采集并编码
  -> PC MediaMTX 提供 RTSP 输入
  -> RK3588 gatewayd 自动拉流并监督板端 FFmpeg
  -> RK3588 MPP/RGA 硬件处理
  -> 板端 MediaMTX 提供浏览器画面和录像
  -> PC 浏览器显示画面与 OSD
```

PC 上的 MediaMTX 只是把 USB 摄像头临时模拟成 RTSP 网络摄像头。以后换成真正支持 RTSP
的网络摄像头，就不再需要 PC FFmpeg 和 PC MediaMTX。板卡上的 MediaMTX 仍然需要，它负责
分发处理后的画面、WebRTC 播放和录像。

## 3. PC 第一次准备

### 3.1 安装基本命令

Ubuntu 或 Debian 使用：

```bash
sudo apt update
sudo apt install -y ffmpeg v4l-utils curl
```

Arch Linux 使用：

```bash
sudo pacman -S --needed ffmpeg v4l-utils curl
```

确认命令都存在：

```bash
command -v ffmpeg
command -v ffprobe
command -v v4l2-ctl
command -v curl
```

每条命令都应输出一个路径。

### 3.2 将 MediaMTX 安装到持久目录

不要放在 `/tmp`，因为重启 PC 后 `/tmp` 可能被清空。以下命令只需执行一次：

```bash
mkdir -p "$HOME/mediamtx"
cd "$HOME/mediamtx"

curl -fLO \
  https://github.com/bluenviron/mediamtx/releases/download/v1.20.0/mediamtx_v1.20.0_linux_amd64.tar.gz
curl -fLO \
  https://github.com/bluenviron/mediamtx/releases/download/v1.20.0/checksums.sha256

grep '  mediamtx_v1.20.0_linux_amd64.tar.gz$' checksums.sha256 \
  | sha256sum --check -
tar -xzf mediamtx_v1.20.0_linux_amd64.tar.gz
./mediamtx --version
```

校验应显示 `OK`，最后应显示 `v1.20.0`。以后不需要再次下载。

### 3.3 检查摄像头

```bash
ls -l /dev/video*
v4l2-ctl --device=/dev/video0 --list-formats-ext
```

当前演示脚本要求 `/dev/video0` 支持：

```text
MJPG / Motion-JPEG
1280x720
30 fps
```

如果摄像头实际节点不是 `/dev/video0`，记住正确节点，后续启动脚本时增加
`--device /dev/videoN`。

## 4. 每次启动的完整步骤

只需要两个 PC 终端。每个终端里的命令会一直运行，这是正常现象，不是卡死。

### 第一步：确认板端服务

在 PC 的第一个终端执行：

```bash
ssh cat@192.168.1.45 \
  'systemctl is-active rk-media-gateway.service mediamtx.service'
```

第一次 SSH 连接可能询问是否信任主机，输入 `yes`，再输入板卡用户 `cat` 的密码。

正确结果是两行：

```text
active
active
```

如果不是 `active`，先在板卡执行：

```bash
sudo systemctl start mediamtx.service rk-media-gateway.service
```

### 第二步：检查 PC 摄像头源

在 PC 第一个终端进入项目目录。下面路径只是示例，请换成项目实际位置：

```bash
cd "$HOME/repo/rk3588-media-gateway"
```

先只检查，不启动：

```bash
./scripts/run_pc_camera_source.sh \
  --board-ip 192.168.1.45 \
  --mediamtx "$HOME/mediamtx/mediamtx" \
  --check-only
```

看到最后一行：

```text
[PASS] PC source preflight completed
```

才继续下一步。检查失败时直接阅读 `[FAIL]` 后面的原因，不要先重启板卡。

### 第三步：启动 PC 摄像头源

仍在 PC 第一个终端执行：

```bash
./scripts/run_pc_camera_source.sh \
  --board-ip 192.168.1.45 \
  --mediamtx "$HOME/mediamtx/mediamtx"
```

脚本会同时启动：

- PC MediaMTX；
- 采集 `/dev/video0` 的 PC FFmpeg。

看到下面这类输出表示 PC 已经开始提供 RTSP：

```text
[PASS] PC camera source is ready: rtsp://PC_IP:8554/source ...
[READY] Type s, r, or q, then press Enter ...
```

保持这个终端运行，不要按 `Ctrl+C`。

如果使用其他摄像头节点，例如 `/dev/video2`：

```bash
./scripts/run_pc_camera_source.sh \
  --board-ip 192.168.1.45 \
  --device /dev/video2 \
  --mediamtx "$HOME/mediamtx/mediamtx"
```

### 第四步：建立诊断页面的 SSH 通道

新开 PC 第二个终端，执行：

```bash
ssh -N \
  -o ExitOnForwardFailure=yes \
  -L 9080:127.0.0.1:9080 \
  cat@192.168.1.45
```

输入密码后终端没有任何输出是正常的。它只负责建立下面这条安全通道：

```text
PC 127.0.0.1:9080 -> SSH -> 板端 127.0.0.1:9080
```

这里只转发 `9080`，不要增加 `-L 8889`。浏览器视频需要直接连接板卡的 WebRTC 端口。

### 第五步：打开唯一推荐页面

浏览器打开：

```text
http://127.0.0.1:9080/view/cam01?media_host=192.168.1.45
```

正常情况下会在同一页面看到实时摄像头画面和左上角 OSD。板端之前离线时间较长时，
gatewayd 最多可能等待 30 秒才进行下一次探测；不要因此重启服务。

## 5. 怎样判断运行正常

OSD 中重点看这些字段：

| 字段 | 正常表现 | 含义 |
| --- | --- | --- |
| `state` | `RUNNING` | 当前通道正在处理视频 |
| `FFmpeg PID` | 一个正整数 | 板端工作进程编号 |
| `input` | `h264 1920x1080` | PC 输入编码和分辨率 |
| `FPS` | 接近 25 | 每秒处理帧数 |
| `frames / dropped` | 前者增长、后者通常为 0 | 已处理帧数和丢帧数 |
| `FFmpeg CPU` | 有百分比 | 板端工作进程 CPU 使用率 |
| `FFmpeg RSS` | 有数值 | 板端工作进程占用内存，单位 KiB |

`bitrate` 显示 `N/A` 不一定是故障。刚恢复时 `failures` 也可能不是 0；稳定运行达到
`stable_run_sec` 后，连续失败次数才会清零，累计重启次数会保留。

## 6. 演示断流自动恢复

不要关闭 PC MediaMTX，也不要重启 gatewayd。在 PC 第一个终端输入：

```text
s
```

然后按回车。脚本只停止 PC FFmpeg，OSD 应变为 `BACKOFF` 或可恢复的 `FAILED`，实时字段
显示 `unavailable`。

需要恢复时输入：

```text
r
```

然后按回车。gatewayd 会自动重新探测输入并创建新的板端 FFmpeg。最多等待当前退避周期
结束，页面会自动恢复画面，PID 会变化。整个过程不需要：

- 重启 `rk-media-gateway.service`；
- 重启板卡；
- 调用 HTTP `start` 或 `restart`；
- 手动刷新浏览器。

## 7. 查看状态和录像

### 7.1 查询通道状态

在 PC 另开临时终端执行：

```bash
ssh cat@192.168.1.45 \
  'curl -fsS http://127.0.0.1:9080/v1/health; echo; \
   curl -fsS http://127.0.0.1:9080/v1/channels/cam01/metrics; echo'
```

健康状态为 `ok` 且通道为 `RUNNING` 表示正常。输入尚未上线时，健康状态可能是
`degraded`，这表示服务仍在运行，但通道当前不可用。

### 7.2 查看录像文件

板端 MediaMTX 会自动分段录像。列出 `cam01` 录像：

```bash
ssh -t cat@192.168.1.45 \
  "sudo find /var/lib/rk-media-gateway/recordings/cam01 \
   -maxdepth 1 -type f -size +0c \
   -printf '%TY-%Tm-%Td %TH:%TM:%TS  %s bytes  %p\\n'"
```

录像由板端 MediaMTX 写入，不是 OSD 页面录制。浏览器 OSD 不会出现在 MP4 文件里。

## 8. 正确停止

结束演示时：

1. 在 PC 第一个终端输入 `q`，按回车；脚本会停止本轮 PC FFmpeg 和 PC MediaMTX。
2. 在 PC 第二个 SSH 终端按 `Ctrl+C`，关闭 9080 通道。
3. 不需要停止板端 systemd 服务；它可以保持后台运行并等待下次输入。

不要使用下面这些宽泛命令：

```text
pkill ffmpeg
killall mediamtx
```

它们可能误杀用户正在运行的其他媒体进程。

## 9. 常见问题

### 9.1 `TCP 8554 is already in use`

PC 上已经有旧 MediaMTX 或其他 RTSP 服务：

```bash
ss -ltnp 'sport = :8554'
```

找到对应的已知终端并用 `Ctrl+C` 正常停止，然后重新运行源脚本。不要直接 `pkill`。

### 9.2 找不到或打不开 `/dev/video0`

```bash
ls -l /dev/video*
groups
```

摄像头节点不同就使用 `--device /dev/videoN`。若用户不在 `video` 组，可执行：

```bash
sudo usermod -aG video "$USER"
```

然后注销并重新登录。Intel IPU3 等复杂内置摄像头不一定能作为普通 V4L2/UVC 摄像头直接
使用；当前推荐普通免驱 USB/UVC 摄像头。

### 9.3 OSD 页面打不开

确认 PC 第二个终端里的 SSH 命令仍在运行：

```bash
curl -I 'http://127.0.0.1:9080/view/cam01?media_host=192.168.1.45'
```

如果本地 9080 被占用：

```bash
ss -ltnp 'sport = :9080'
```

### 9.4 OSD 正常但没有视频

先直接打开板端 MediaMTX 原始播放器：

```text
http://192.168.1.45:8889/cam01/
```

- 原始播放器有画面：检查诊断 URL 的 `media_host` 是否为真实板卡 IP。
- 原始播放器也没画面：检查 OSD 的状态，以及 PC 源脚本是否仍显示 `[READY]`。
- 出现 `peer connection closed`：确认 PC 可以直接访问板卡 `8889/TCP` 和 `8189/UDP`，
  不要使用 SSH 转发 8889。

### 9.5 通道显示 `FAILED`

新版 `FAILED` 不是永久终止。gatewayd 会按 `max_backoff_sec`（当前 30 秒）继续低频探测。
确认 PC 源已经 `[READY]` 后等待下一次探测，不要重启 gatewayd。

可在板端查看最近日志：

```bash
journalctl -u rk-media-gateway.service -n 30 --no-pager
```

### 9.6 PC 源脚本异常退出

查看专用日志：

```bash
tail -n 50 "$HOME/.local/state/rk-media-gateway-demo/pc-mediamtx.log"
```

## 10. 以后再次使用，只记住这三步

1. PC 第一个终端：

```bash
cd "$HOME/repo/rk3588-media-gateway"
./scripts/run_pc_camera_source.sh \
  --board-ip 192.168.1.45 \
  --mediamtx "$HOME/mediamtx/mediamtx"
```

2. PC 第二个终端：

```bash
ssh -N -L 9080:127.0.0.1:9080 cat@192.168.1.45
```

3. 浏览器：

```text
http://127.0.0.1:9080/view/cam01?media_host=192.168.1.45
```

当前已实测范围是单路视频、OSD、录像和断流自动恢复；不包含音频、AI、复杂 Web 管理
前端或双真实摄像头验收。
