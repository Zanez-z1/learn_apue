# 性能测试结果

本文档只记录可由原始 CSV、网关状态快照和媒体探测结果复核的数据。测试命令和验收方法
见 `docs/test-plan.md`；没有实际执行的矩阵项保持 `PENDING`，不得根据单点观察外推。

## 1. 指标口径

- `gateway-metrics` 从 Linux `/proc` 读取目标进程的 CPU tick、RSS 和打开文件描述符数。
- CPU 百分比按相邻样本计算，100% 表示占满一个逻辑 CPU，允许多线程进程超过 100%。
- RSS 和文件描述符是每个采样时刻的进程值，不包含该进程未拥有的外部服务资源。
- FPS、处理速度、丢帧和重启次数取自 `GET /v1/channels/{id}` 的开始/结束快照。
- 编码、分辨率和帧率由 ffprobe 实测，不根据配置文件推断。
- 端到端延迟必须使用画面时间戳或专用测试源测量，不能用 FFmpeg `speed` 代替。
- 软件和硬件方案必须使用相同输入、输出规格、采样窗口和播放/录像负载才可比较。

## 2. 环境记录

```text
板卡：LubanCat aarch64
系统：Debian 11
内核：Linux 5.10.160
FFmpeg-Rockchip：388741a
MediaMTX：v1.20.0 linux arm64
输入：PENDING
提交：PENDING
采样窗口：PENDING
原始日志目录：PENDING
```

## 3. 转码矩阵

| 方案 | 输入 | 输出 | CPU 平均/峰值 | RSS 平均/峰值 | FPS/速度/丢帧 | 延迟 | 状态 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 软件解码 + 软件编码 | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | `PENDING` |
| MPP 解码 + MPP 编码 | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | `PENDING` |
| MPP 解码 + RGA 缩放 + MPP 编码 | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | `PENDING` |

## 4. 稳定性与资源趋势

长时间稳定性测试暂未执行。当前只允许记录短时基线；2 小时和 24 小时结果留待用户明确
安排后执行，不能把 Phase 4 的 6 分 12 秒功能验收写成长稳结果。

| 窗口 | 进程重启 | RSS 变化 | FD 变化 | 丢帧 | 僵尸进程 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 短时基线 | PENDING | PENDING | PENDING | PENDING | PENDING | `PENDING` |
| 2 小时 | PENDING | PENDING | PENDING | PENDING | PENDING | `PENDING` |
| 24 小时 | PENDING | PENDING | PENDING | PENDING | PENDING | `PENDING` |

## 5. 已知边界

- 当前媒体链路没有音频，所有性能结果只覆盖视频。
- 当前只有一个真实 PC 摄像头 RTSP 源，不能伪造双路真实输入吞吐量。
- 用户已取消本轮 30 分钟长测；Phase 5 首轮只执行短时、可重复的对比基线。
