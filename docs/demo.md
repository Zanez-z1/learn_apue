# 完整视频链路演示指南

本文用于在已部署的 RK3588 板卡上复现一条完整演示链路：真实 RTSP 输入经过
`gatewayd` 管理的 FFmpeg-Rockchip MPP/RGA 转码，发布到 MediaMTX，再验证状态、
控制、播放、录像和故障恢复。当前范围只有视频，不包含音频。

## 1. 组件边界

`gatewayd`、`gateway-metrics`、配置解析、通道状态机、进程监督、HTTP API、录像状态
检查和测试脚本由本项目实现。FFmpeg-Rockchip、Rockchip MPP/RGA 和 MediaMTX 是独立
上游组件；演示证明本项目能安全地编排它们，不表示本项目实现了编解码器或流媒体协议栈。

## 2. 启动前检查

先按 `docs/deployment.md` 完成非 root 部署，并在受保护的
`/etc/rk-media-gateway/gateway.env` 中配置真实输入。不要把输入 URL、环境文件或完整
journal 复制到演示材料。然后在板卡执行：

```bash
/usr/local/share/rk-media-gateway/scripts/check_media_env.sh
sudo systemctl start mediamtx.service rk-media-gateway.service
systemctl is-active mediamtx.service rk-media-gateway.service
curl -fsS http://127.0.0.1:9080/v1/health
curl -fsS http://127.0.0.1:9080/v1/channels/cam01
```

预期两个服务均为 `active`，健康状态为 `ok`，`cam01` 最终为 `RUNNING`，且 progress
中的帧数持续增长。API 不应出现输入 URL 或密码。

## 3. 播放输出

默认 RTSP 和控制接口只监听板卡回环地址。可从 PC 建立 SSH 隧道：

```bash
ssh -N \
  -L 8554:127.0.0.1:8554 \
  -L 9080:127.0.0.1:9080 \
  cat@BOARD_ADDRESS
```

另开 PC 终端播放并查询：

```bash
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/cam01
curl -fsS http://127.0.0.1:9080/v1/channels/cam01
```

示例 MediaMTX 的 WebRTC HTTP 监听为 8889。只在受信任测试网中访问
`http://BOARD_ADDRESS:8889/cam01`；它没有认证，不应直接暴露到公网。生产环境应通过
认证反向代理、TLS 和防火墙发布。

## 4. 控制接口演示

以下命令在板卡或 SSH 隧道后的 PC 执行：

```bash
curl -fsS -X POST http://127.0.0.1:9080/v1/channels/cam01/stop
curl -fsS http://127.0.0.1:9080/v1/channels/cam01
curl -fsS -X POST http://127.0.0.1:9080/v1/channels/cam01/start
curl -fsS -X POST http://127.0.0.1:9080/v1/channels/cam01/restart
curl -fsS http://127.0.0.1:9080/v1/channels/cam01
```

控制命令被接受时返回 HTTP 202；停止后应为 `STOPPED`，启动和重启后应重新进入
`RUNNING`。对不存在的通道返回 404，重复或冲突操作返回 409。

## 5. 录像与回放演示

保持通道运行至少一个录像 part 后，在板卡执行：

```bash
curl -fsS http://127.0.0.1:9080/v1/recording
curl -fsS 'http://127.0.0.1:9996/list?path=cam01'
find /var/lib/rk-media-gateway/recordings/cam01 -type f -size +0c
```

预期录像状态为 `ok`，列表返回 `cam01` 的可用时间段，录像目录中存在非空 fMP4 文件。
具体回放 URL 使用列表返回的开始时间构造；完整检查方法见 `docs/test-plan.md` 的 Phase 4
录像章节。不要通过写满磁盘测试低空间状态，应提高测试配置阈值来模拟。

## 6. 故障恢复演示

短暂停止上游 RTSP 测试源，观察通道进入 `BACKOFF`；在重试预算耗尽前恢复上游，通道
应重新进入 `RUNNING` 且 `restart_count` 增长。再分别验证 FFmpeg 工作进程异常退出和
MediaMTX 重启。只操作已确认属于本次演示的 PID 和服务，不使用模糊匹配批量杀进程。

## 7. 结束与清理

```bash
sudo systemctl stop rk-media-gateway.service mediamtx.service
systemctl is-enabled rk-media-gateway.service mediamtx.service
systemctl is-active rk-media-gateway.service mediamtx.service
pgrep -x gatewayd
pgrep -x ffmpeg
pgrep -x mediamtx
```

已启用服务应保持 `enabled`，停止后为 `inactive`；三个精确进程查询均应无输出。录像和
原始验收证据按保留策略留存，不在清理步骤中删除。

## 8. 已完成的实板演示记录

2026-08-06 已在 LubanCat RK3588 板卡完成以下视频范围：

- PC 真实摄像头经 RTSP、MPP/RGA、MediaMTX 输出，连续功能窗口 6 分 12 秒。
- RTSP 与 WebRTC 可见画面；HTTP 查询、停止、启动和重启通过。
- 真实录像、回放、自动删除、低空间阈值模拟和 MediaMTX 恢复通过。
- 输入 EOF、FFmpeg SIGKILL、MediaMTX 停止后的有限退避和恢复通过。
- 非 root systemd 启动、优雅停止、开机启用和异常恢复通过。
- 画面时间戳测试的稳定 RTSP 画面年龄约 `1.25 ± 0.5s`；测试截图位于板卡
  `/home/cat/rk3588-acceptance/2026-08-06/phase5-latency/`。

这不是 30 分钟、2 小时或 24 小时长稳记录，也不是双真实摄像头或音频验收。详细命令、
原始日志位置和限制以 `docs/test-plan.md`、`docs/benchmark-results.md` 为准。
