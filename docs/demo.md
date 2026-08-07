# 从零跑通与演示完整视频链路

本文只解决一件事：让第一次接触项目的人知道每台机器做什么，并从 PC 摄像头一路跑到
浏览器画面。完成本文后，再阅读 [systemd 部署指南](deployment.md) 做长期部署。

当前链路只有视频，不包含音频。当前 `gatewayd` 只接受 RTSP 输入；USB 摄像头先由 PC
上的 FFmpeg 转为 RTSP，并不是直接插到板卡后由 `gatewayd` 读取。

## 1. 最终会得到什么

```text
PC 摄像头 /dev/video0
  -> PC FFmpeg：采集并编码 H.264
  -> PC MediaMTX：提供 rtsp://PC_IP:8554/source
  -> RK3588 gatewayd：探测输入、创建并监督 FFmpeg-Rockchip
  -> MPP 解码 -> RGA 缩放/转换 -> MPP 编码
  -> 板卡 MediaMTX：接收 cam01，负责 WebRTC、RTSP、录像和回放
  -> PC 浏览器：MediaMTX 画面 + gatewayd 浏览器 OSD
```

`gatewayd` 不采集 PC 摄像头，也不实现 H.264、RTSP 或 WebRTC。它负责配置、状态机、
FFmpeg 子进程生命周期、故障恢复、HTTP 控制和运行状态。FFmpeg-Rockchip、MPP/RGA 和
MediaMTX 是上游组件。

本例使用以下地址，请按实际网络替换：

```text
PC_IP=192.168.1.16
BOARD_IP=192.168.1.45
上游路径=source
板卡输出路径=cam01
```

板卡还需要已经安装 FFmpeg-Rockchip、MediaMTX 和本项目 systemd 服务。全新板卡先完成
[systemd 部署指南](deployment.md)；推荐演示不再混用前台二进制与 systemd 服务。

## 2. 需要几个终端

| 终端 | 运行位置 | 用途 | 是否一直占用 |
| --- | --- | --- | --- |
| PC-Source | PC | 一个脚本管理 PC MediaMTX 和摄像头 FFmpeg | 是 |
| PC-Tunnel | PC | 只建立 gatewayd 9080 SSH 隧道 | 是 |
| Browser | PC | 打开唯一诊断 URL | 否 |
| RK3588 | 板卡 | 已部署的 gatewayd/MediaMTX systemd 服务 | 否 |

两个 PC 常驻终端都用 `Ctrl+C` 正常结束。看到命令一直不返回表示进程正在提供服务，不是
卡死。

## 3. PC 提供 RTSP 测试源

### 3.1 唯一推荐启动方式

MediaMTX 二进制必须放在持久目录，不能继续使用 `/tmp` 中的下载解压目录。以下路径与当前
开发机一致：

```bash
BOARD_IP=192.168.1.45
./scripts/run_pc_camera_source.sh \
  --board-ip "$BOARD_IP" \
  --mediamtx "$HOME/mediamtx/mediamtx" \
  --check-only

./scripts/run_pc_camera_source.sh \
  --board-ip "$BOARD_IP" \
  --mediamtx "$HOME/mediamtx/mediamtx"
```

脚本依次检查：

- `/dev/video0` 是可读字符设备，并明确提供 MJPEG 1280×720@30；
- 到板卡路由选择出的 PC IPv4 地址；当前应为 `192.168.1.16`；
- MediaMTX 是 v1.20.x，二进制和配置都不在 `/tmp`；
- TCP 8554 未被其他实例占用；
- PC MediaMTX 使用仓库内 `config/mediamtx.pc-source.yml`，只承担 RTSP source，关闭
  PC 不需要的 WebRTC/RTMP/HLS/SRT/MoQ；
- FFmpeg 发布后，本机 ffprobe 能读到 `source`。

若 8554 已被占用，脚本会明确失败，不会杀掉占用者。回到那个已知旧服务的终端按
`Ctrl+C`，然后重试。脚本启动成功后输出两个明确 PID；退出时只清理这两个子进程。

看到 `[READY]` 后可另开终端复核：

```bash
ffprobe -v error -rtsp_transport tcp \
  -show_entries stream=codec_name,width,height,avg_frame_rate \
  -of default=noprint_wrappers=1 \
  rtsp://127.0.0.1:8554/source
```

预期为 H.264、1920×1080、25/1。PC 自检失败时不要继续排查板卡。

## 4. 在 RK3588 编译

以下命令在板卡源码目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./scripts/check_media_env.sh
```

预期 CMake、编译和 CTest 成功，环境脚本确认 aarch64、MPP、RGA、DRM、`h264_rkmpp`
和 `scale_rkrga` 可用。普通 Debian FFmpeg 没有这些 Rockchip 能力，不能替代
FFmpeg-Rockchip。

### 编译和安装不是一回事

- `cmake --build build` 只生成当前目录下的 `./build/gatewayd`，适合前台试运行。
- `sudo cmake --install build` 会把程序安装到 `/usr/local/bin`，systemd 才会使用新版。
- 第一次验证时不要急着安装；前台确认媒体链路正常后，再按
  [systemd 部署指南](deployment.md) 部署。

## 5. 确认板端已部署服务

推荐演示只使用已安装版本，不临时停止或切换板端服务：

```bash
ssh cat@192.168.1.45 \
  'systemctl is-active rk-media-gateway.service mediamtx.service'
ssh cat@192.168.1.45 \
  'systemctl show rk-media-gateway.service -p MainPID -p ExecMainStartTimestamp'
```

两个服务都应为 `active`。记录 gatewayd PID，后续故障恢复验收必须保持同一 PID。

只有开发新代码时才在板卡源码目录使用以下检查；不要同时启动第二个 gatewayd：

```bash
export CAM01_RTSP_URL='rtsp://192.168.1.16:8554/source'
./build/gatewayd --config config/gateway.example.yaml --check-config
./build/gatewayd --config config/gateway.example.yaml --dry-run
```

两个命令含义不同：

1. `--check-config` 只完成 YAML 加载和语义校验，不连接摄像头。
2. `--dry-run` 打印脱敏后的探测/转码命令，不启动 FFmpeg。

已部署服务会自动探测 RTSP 并创建 FFmpeg 工作进程。PC 源没启动时，通道先进入
`BACKOFF`；超过快速重试上限后显示 `FAILED`，并按 `max_backoff_sec` 继续低频探测。

## 6. 看实时画面和查询状态

### 6.1 浏览器 WebRTC

在同一受信任局域网的 PC 浏览器打开：

```text
http://192.168.1.45:8889/cam01
```

这是 MediaMTX 自带的原始播放页面。项目另提供轻量诊断页，但不把它扩展成复杂的监控
管理前端。

### 6.2 带 OSD 的诊断页面

保持 gatewayd 的 `9080` 回环监听，不要为了演示改为局域网地址。在 PC-Tunnel 只建立
9080 转发：

```bash
ssh -N \
  -L 9080:127.0.0.1:9080 \
  cat@192.168.1.45
```

然后在 PC 浏览器打开：

```text
http://127.0.0.1:9080/view/cam01?media_host=192.168.1.45
```

`media_host` 经过页面严格校验，只接受 IPv4、IPv6 或 DNS 主机名，不接受端口、路径、
凭据或 IPv6 zone ID。视频 iframe 从浏览器直接访问板卡 `192.168.1.45:8889`，WebRTC
媒体走板卡 `8189/UDP`；SSH 隧道只承载 9080 页面和 JSON。左上角状态、PID、输入格式、
FPS、码率、帧数、丢帧、失败/重启次数以及 FFmpeg CPU/RSS 来自 gatewayd，并每秒读取一次
`/v1/channels/cam01/metrics`。某项没有当前样本时显示 `unavailable`，不会保留上一 PID 的
旧值。

OSD 是独立的原生 HTML/CSS/JavaScript 浏览器覆盖层，不经过 FFmpeg，不改变 MPP/RGA
链路，不写入视频或录像。它只用于诊断，不提供用户、告警、录像管理或 AI 功能。

板卡 RTSP 默认只监听回环地址。如需在 PC 用 ffplay 验证，先建立隧道：

```bash
ssh -N -L 8554:127.0.0.1:8554 cat@192.168.1.45
```

再从另一个 PC 终端播放：

```bash
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/cam01
```

### 6.3 HTTP 状态

示例配置让 `gatewayd` HTTP 只监听板卡回环地址。可直接从 PC 通过 SSH 执行板卡命令：

```bash
ssh cat@192.168.1.45 'curl -fsS http://127.0.0.1:9080/v1/health'
ssh cat@192.168.1.45 'curl -fsS http://127.0.0.1:9080/v1/channels'
ssh cat@192.168.1.45 \
  'curl -fsS http://127.0.0.1:9080/v1/channels/cam01'
ssh cat@192.168.1.45 \
  'curl -fsS http://127.0.0.1:9080/v1/channels/cam01/metrics'
```

健康状态应为 `ok`，通道应为 `RUNNING`，`frame` 应继续增长。API 不返回输入 URL 或密码。

如果只需要在 PC 连续查询 JSON，可只转发 `9080`：

```bash
ssh -N -L 9080:127.0.0.1:9080 cat@192.168.1.45
```

然后从另一个 PC 终端访问 `http://127.0.0.1:9080`。

## 7. 演示当前已有功能

### 7.1 停止、启动和重启单通道

以下命令在板卡执行，或包装在上一节的 SSH 命令中：

```bash
curl -fsS -X POST http://127.0.0.1:9080/v1/channels/cam01/stop
curl -fsS http://127.0.0.1:9080/v1/channels/cam01
curl -fsS -X POST http://127.0.0.1:9080/v1/channels/cam01/start
curl -fsS -X POST http://127.0.0.1:9080/v1/channels/cam01/restart
```

命令被接受返回 HTTP 202；停止后画面消失，重新启动后使用新的 FFmpeg PID 回到
`RUNNING`。重复或冲突操作返回 409，不存在的通道返回 404。

### 7.2 录像和回放状态

```bash
curl -fsS http://127.0.0.1:9080/v1/recording
curl -fsS 'http://127.0.0.1:9996/list?path=cam01'
sudo find /var/lib/rk-media-gateway/recordings/cam01 \
  -maxdepth 1 -type f -size +0c -printf '%TY-%Tm-%Td %TH:%TM:%TS  %s  %p\n'
```

录像文件由 MediaMTX 写入，不是 `gatewayd` 自己写 MP4。`gatewayd` 负责生成录像配置并
报告录像文件系统容量；自动删除由 MediaMTX 的 `recordDeleteAfter` 执行。

### 7.3 RTSP 断流自动恢复

首要验收必须覆盖“启动时源长期离线”，不能只测短暂断流：

1. 先启动板端 gatewayd，并提前打开诊断页面；此时 PC 不推流。
2. 等待通道超过 `max_retries`，确认它显示 `FAILED`，实时字段为 `unavailable`，gatewayd
   服务没有重启。
3. 继续等待至少一个 `max_backoff_sec` 周期，确认探测频率受限而监督线程仍在工作。
4. 再启动第 3.1 节的 PC 源脚本，全程不调用 start/restart 接口、不执行 systemctl restart。
5. 观察通道自行执行 `FAILED -> PROBING -> STARTING -> RUNNING`，出现新 FFmpeg PID、更新的
   CPU/RSS；诊断页在进入 `RUNNING` 时自动重新加载播放器并出现画面，无需手动刷新。
6. 在 PC-Source 输入 `s` 并回车，只停止 FFmpeg、保持 PC MediaMTX；看到通道离线后输入
   `r` 并回车，再次确认自动恢复、工作 PID 变化且 gatewayd PID 不变。

网关重启的是板卡上的 FFmpeg 工作进程，不是远程摄像头。摄像头完全死机时仍需要摄像头
自身看门狗、ONVIF/厂商重启接口或可管理 PoE 供电设备。

### 7.4 双路真实输入（有第二台 PC 时）

第二台 PC 使用不同路径发布真实摄像头，例如 `source2`，并在板卡配置中启用 `cam02`，其
输入指向第二台 PC、输出路径为 `cam02`。重载或重启 gatewayd 后同时打开：

```text
http://127.0.0.1:9080/view/cam01?media_host=BOARD_IP
http://127.0.0.1:9080/view/cam02?media_host=BOARD_IP
```

停止其中一台 PC 的推流，只允许对应页面进入 `BACKOFF/unavailable`；另一页必须继续
`RUNNING` 且指标增长。没有第二台 PC 或第二个真实源时必须把这项记录为 `PENDING`，不能
用同一路复制流冒充双真实输入验收。

## 8. 五分钟求职演示顺序

不要从 YAML 或测试数量讲起。按以下顺序能让观看者先看到结果，再理解技术：

1. 用 30 秒展示第 1 节的数据流，说明哪些代码属于本项目。
2. 打开 `/view/cam01?media_host=BOARD_IP`，同时展示真实 WebRTC 画面和浏览器 OSD。
3. 指出 OSD 的画面来自 MediaMTX、指标来自 gatewayd，并说明它不会烧入录像。
4. 停止 PC 推流，展示 `RUNNING -> BACKOFF/unavailable`；恢复推流，展示新 PID、指标和
   画面恢复。
5. 调用通道 `restart`，说明单通道生命周期控制不会重启整个网关。
6. 展示 [性能测试结果](benchmark-results.md) 中软件与 MPP/RGA 的实测 CPU/RSS 对比。

这段演示的重点不是“比摄像头 App 多一个播放器”，而是证明你实现并验证了 Linux C
服务、进程监督、故障隔离、硬件媒体链路和可复现性能测试。

## 9. 前台测试结束与正式部署

结束时在 PC-Source 和 PC-Tunnel 终端分别按 `Ctrl+C`。源脚本会停止本轮 FFmpeg 和 PC
MediaMTX；不要停止板端 systemd 服务。可检查是否残留本轮 PC 发布者：

```bash
pgrep -af 'ffmpeg.*video0'
ss -lntp 'sport = :8554'
```

两个查询都应无本轮进程输出。不要使用 `pkill ffmpeg` 或 `killall mediamtx`。

前台验证通过后，只有在希望 systemd 和开机启动使用新版时，才执行安装并按
[systemd 部署指南](deployment.md) 准备服务账号、受保护配置和环境文件。不要一边前台运行
`./build/gatewayd`，一边启动 `rk-media-gateway.service`。

## 10. 常见问题

| 现象 | 最可能原因 | 先检查什么 |
| --- | --- | --- |
| `--check-config` 通过，但没有画面 | 它只检查配置，不连接 RTSP | 是否真正运行了不带检查选项的命令 |
| 通道持续 `BACKOFF` | PC RTSP 源不可达或编码不匹配 | 先在 PC `ffprobe`，再从板卡 `ffprobe` 输入 URL |
| `Address already in use` | 旧服务或另一个实例占用 9080/8554 | `systemctl status` 和 `ss -ltnp` |
| 找不到 `h264_rkmpp` | 使用了 Debian 标准 FFmpeg | `./scripts/check_media_env.sh` |
| 浏览器打不开 8889 | MediaMTX 未运行、地址错误或防火墙阻止 | `systemctl status mediamtx` |
| OSD 有状态但无画面 | `media_host` 错误，或 PC 到板卡 8889/TCP、8189/UDP 不可达 | URL 使用真实 BOARD_IP；检查板端监听、防火墙和 MediaMTX WebRTC 日志，不要增加 8889 SSH 隧道 |
| `/dev/video0` 不存在 | PC 摄像头节点不同或未启用 | `ls -l /dev/video*` |
| 录像目录权限不足 | 文件属于专用服务账号 | 只用 `sudo find` 检查，不放宽整个目录权限 |
| 前台新版和 systemd 行为不同 | systemd 仍使用 `/usr/local/bin/gatewayd` | 理解第 4 节“编译和安装”的区别 |

## 11. 已完成的实板范围

2026-08-06 已在 LubanCat RK3588 板卡完成：

- PC 真实摄像头 RTSP 输入、MPP/RGA 转码、RTSP/WebRTC 播放。
- HTTP 查询、停止、启动、重启；录像、回放和低空间状态。
- 输入 EOF、FFmpeg 异常和 MediaMTX 停止后的恢复。
- 非 root systemd 生命周期与 31/31 板卡 CTest。
- 时间戳短测的稳定 RTSP 画面年龄约 `1.25 ± 0.5s`。

旧记录证明过板端原始 WebRTC，但此前“同时 SSH 转发 9080/8889”的 OSD 拓扑结论已经
纠正。2026-08-07 已按本文件重新实测带 `media_host` 的唯一页面、长期 `FAILED` 后自动恢复
和重复断流恢复：gatewayd PID 945811 全程未变，用户确认页面无需刷新即可两次恢复真实
画面和 OSD。双真实摄像头仍为 `PENDING`。项目不包含音频、30 分钟及更长稳定性测试或
复杂 Web 管理前端。详细证据和限制见
[测试与验收指南](test-plan.md)、[性能测试结果](benchmark-results.md)。
