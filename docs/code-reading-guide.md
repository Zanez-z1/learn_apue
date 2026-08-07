# 源码阅读与面试准备路线

本文面向第一次阅读本项目源码的人。目标不是一次看完所有 C 文件，而是按依赖顺序建立
四条能够自己讲清楚的主线：配置如何进入程序、FFmpeg 如何启动、故障如何恢复、HTTP
如何控制通道。

## 1. 先用一句话说清项目

`gatewayd` 是运行在 RK3588 上的 C17 控制服务：它读取 RTSP 通道配置，为每路通道创建并
监督一个 FFmpeg-Rockchip 子进程，让 MPP/RGA 完成硬件媒体处理，再把输出发布给
MediaMTX 播放和录像。

本项目自行实现的是“管理和可靠性”，不是编解码器、RTSP/WebRTC 协议栈或 AI 模型。

## 2. 运行时全链路

先记住下面的调用方向，暂时不用理解每个函数：

```text
main.c
  -> gw_config_load_file() / gw_config_validate()
  -> gw_channel_manager_create()
  -> 每个通道启动独立线程
      -> gw_supervisor_run()
          -> run_probe_attempt()：构造并运行 ffprobe 探测
          -> gw_pipeline_build()：生成 FFmpeg argv
          -> gw_process_start()：posix_spawnp 创建子进程
          -> poll() 读取 progress/stderr，检查退出和超时
          -> 成功：RUNNING
          -> 失败：BACKOFF -> 再次 PROBING
  -> HTTP 线程查询快照或向 Channel Manager 发送控制命令
  -> SIGINT/SIGTERM：停止线程并回收所有子进程
```

媒体数据不会流经 `gatewayd` 自己的内存缓冲区。实际视频数据由 FFmpeg 从 RTSP 读取，
经过 MPP/RGA 后直接发布给 MediaMTX；`gatewayd` 只读取 FFmpeg 的结构化进度和错误日志。

## 3. 推荐阅读顺序

每次只读一层。读完一层后，先回答该节的问题，再进入下一层。

### 第 1 层：配置的数据结构

文件：

- `include/gateway/config.h`
- `config/gateway.example.yaml`
- `src/config/config.c` 中的 `gw_config_init()`、`gw_config_load_file()`、
  `gw_config_validate()`

先理解：

1. `gw_config` 如何包含 server、MediaMTX、defaults 和 channels。
2. `gw_config_init()` 为什么先写默认值。
3. YAML 中缺少可选字段时，默认值为什么仍然保留。
4. `gw_config_load_file()` 负责“读取和转换”，`gw_config_validate()` 负责“业务约束”的区别。

读完应能回答：`--check-config` 到底检查了什么，为什么它通过不代表摄像头在线。

### 第 2 层：libyaml 节点如何变成 C 字段

文件：`src/config/config.c`

按以下顺序阅读辅助函数：

```text
mapping_value()
  -> read_string()
  -> read_int()
  -> read_bool()
  -> parse_channel()
  -> gw_expand_environment()
```

重点概念：

- `yaml_document_t` 是 libyaml 加载出的整棵 YAML 文档树。
- Mapping 是键值集合，Sequence 是列表，Scalar 是字符串/数字/布尔值对应的标量文本。
- libyaml 的 mapping pair 保存 key/value 节点编号，`yaml_document_get_node()` 再按编号取节点。
- `read_int()` 先用较宽的整数接收和检查范围，不能直接让外部文本写入 `uint16_t`。
- `${CAM01_RTSP_URL}` 先在临时缓冲区展开，再经过边界检查复制到配置字段。

读完应能沿着 `server.port: 9080` 解释它如何最终写入 `config->server.port`。

### 第 3 层：配置如何变成 FFmpeg 参数

文件：

- `include/gateway/pipeline_builder.h`
- `src/pipeline/pipeline_builder.c`

重点理解：

1. 输入 URL、解码器、分辨率、编码器和输出路径如何变成 `argv[]`。
2. 为什么返回参数数组，而不是拼接一个 shell 字符串。
3. `-progress pipe:1`、`-an`、MPP/RGA、GOP 和 RTSP/TCP 参数分别解决什么问题。
4. dry-run 为什么能打印命令，却不会创建 FFmpeg。

读完应能根据一条 channel 配置，手工说出生成命令的输入、硬件处理和输出三部分。

### 第 4 层：Linux 子进程生命周期

文件：

- `include/gateway/process_manager.h`
- `src/process/process_manager.c`

重点理解：

1. `posix_spawnp()` 如何启动 FFmpeg，为什么不用 `system()` 或 `popen()`。
2. 父进程与子进程分别保留哪些管道端点。
3. `waitpid()` 如何判断仍在运行、正常退出、非零退出或被信号杀死。
4. 停止时为什么先发 SIGTERM，超时后才强制清理整个进程组。
5. 为什么必须回收子进程，否则会产生 zombie。

读完应能解释：FFmpeg 崩溃后，网关如何知道旧 PID 已经结束并安全创建新进程。

### 第 5 层：输入探测和运行进度

文件：

- `src/probe/probe.c`
- `src/monitor/progress_parser.c`
- 对应的 `include/gateway/*.h`

`probe.c` 启动 ffprobe，在 FFmpeg 前确认 RTSP 可达、首个视频流的编码和尺寸，并检查输入
编码是否适合配置的硬件解码器。`progress_parser.c` 增量解析 FFmpeg
`-progress pipe:1` 的键值记录，不依赖普通日志的自然语言格式。

读完应能区分三类失败：输入探测失败、工作进程已经退出、工作进程仍在但长时间没有进度。

### 第 6 层：状态机和自动恢复

文件：

- `src/channel/channel_state.c`
- `src/channel/supervisor.c`
- `include/gateway/channel_state.h`
- `include/gateway/supervisor.h`

先读纯状态转换 `channel_state.c`，再读负责 I/O 和时间的 `supervisor.c`。关键路径：

```text
PROBING -> STARTING -> RUNNING
   ^                       |
   |                       | 退出、启动超时或 progress 超时
   +------ BACKOFF <-------+
```

`supervisor` 每 250ms 左右轮询管道和进程状态；失败后清理旧工作进程，按有上限的退避时间
等待，再重新从 ffprobe 开始。连续稳定运行达到 `stable_run_sec` 后，连续失败计数会清零。
超过 `max_retries` 才进入 `FAILED`。

读完应能准确说明：网关恢复的是本机 FFmpeg/RTSP 会话，不会让远程摄像头断电重启。

### 第 7 层：多通道、快照和 HTTP

文件：

- `src/channel/channel_snapshot.c`
- `src/channel/channel_manager.c`
- `src/api/http_server.c`

重点理解：

1. 每通道为什么有独立线程、supervisor 和 FFmpeg PID。
2. 快照如何把内部运行状态安全地交给 HTTP 线程读取。
3. stop/start/restart 命令为什么先由 Channel Manager 串行化。
4. 一个通道失败时，为什么其他通道不需要重启。
5. SIGHUP 如何比较旧配置和候选配置，只重启发生变化的通道。

读完应能从一个 HTTP `POST /v1/channels/cam01/restart` 一路追到旧 FFmpeg 被回收、新 PID
创建和快照更新。

### 第 8 层：总入口

最后阅读 `src/main.c`。此时再看 main，应该只剩下“把模块装配起来”：

- 解析 `--config`、`--check-config`、`--dry-run` 等 CLI 参数。
- 加载配置并处理三种检查模式。
- 安装 SIGINT、SIGTERM、SIGHUP 信号处理。
- 创建 Channel Manager 和 HTTP 线程。
- 处理配置重载、全局停止和退出码。

不要从 main 开始追进所有函数；那会同时遇到配置、线程、信号、HTTP 和进程管理，最容易
失去主线。

## 4. 四条必须能自己讲清楚的流程

### 4.1 启动流程

```text
YAML -> 默认值 -> 读取 -> 校验 -> 通道线程 -> ffprobe -> FFmpeg -> RUNNING
```

### 4.2 断流恢复流程

```text
RTSP EOF/错误 -> FFmpeg 退出 -> waitpid 得到结果 -> 清理管道和 PID
-> BACKOFF -> ffprobe 重试 -> 摄像头恢复后启动新 FFmpeg -> RUNNING
```

如果 FFmpeg 没退出但不再产生 progress，则由 `progress_timeout_sec` 触发停止和重建。

### 4.3 配置热重载流程

```text
SIGHUP -> 加载候选配置 -> 完整校验 -> 与现有通道比较
-> 未变化通道保持 -> 新增通道启动 -> 删除通道停止 -> 变化通道重启
```

候选配置无效时，现有运行配置不被破坏。

### 4.4 停止流程

```text
SIGTERM/SIGINT -> 停止接受新控制 -> 通知全部通道
-> SIGTERM 工作进程组 -> 限时 waitpid -> 必要时强制清理
-> join 线程 -> 关闭 HTTP -> gatewayd 退出
```

## 5. 项目价值和边界

如果只比较最终用户功能，一台现代智能摄像头确实已经有 App、录像和入侵检测，本项目
没有必要替代它。这个项目用于求职时的价值来自工程实现：

- 在 C17 中管理线程、进程、信号、管道、`poll()` 和资源所有权。
- 把状态机、超时、退避、故障隔离落到真实 FFmpeg 子进程上。
- 在 RK3588 实机完成 MPP/RGA 硬件链路，而不是只写出命令。
- 用 HTTP、SIGHUP 和 systemd 把演示代码做成可控制、可部署的服务。
- 用单元/集成测试、Sanitizer 和实测数据证明行为，而不是只声称“支持”。

它不是成熟监控产品，也没有 Web 管理前端、音频、AI、双真实摄像头或长时间稳定性结论。
简历和面试都应主动说明这些边界。

## 6. 写入简历前的自检

下面问题如果还不能脱离文档回答，就先继续阅读和动手验证，不要急着背简历话术：

1. 为什么每路使用独立 FFmpeg 进程，而不是一个进程处理所有通道？
2. 为什么使用 `posix_spawnp()`，它与 `system()` 的安全边界有什么不同？
3. 如何同时检测 FFmpeg 崩溃和“进程没死但卡住”？
4. `RUNNING -> BACKOFF -> PROBING` 每一步由什么事件触发？
5. 单路断流为什么不会重启健康通道？
6. `--check-config`、`--dry-run` 和真正运行有什么差别？
7. SIGHUP 配置错误时，为什么现有通道不会中断？
8. 哪部分是你写的，哪部分来自 FFmpeg-Rockchip 和 MediaMTX？
9. MPP/RGA 性能数据使用了什么输入、窗口和限制，为什么不能随意外推？
10. 当前项目最明显缺少的产品能力是什么？

## 7. 可用于简历的事实表达

只有自己能解释并且测试记录已经支持的内容才能使用。例如：

> 基于 C17 和 RK3588 实现多通道媒体任务管理服务，使用独立 supervisor 管理
> FFmpeg-Rockchip 子进程，通过 ffprobe 预检、结构化 progress 超时、进程回收和有上限
> 退避实现单通道故障隔离与 RTSP 断流恢复；完成 MPP/RGA 硬件转码、HTTP 控制、SIGHUP
> 差异化重载及 systemd 非 root 部署。

性能数字从 [性能测试结果](benchmark-results.md) 选择，并同时写清固定样本、短时测试和
最大吞吐/在线运行的区别。不要把重复固定样本的四路微基准写成“四路真实摄像头长期
稳定运行”。

## 8. 文档地图

- [从零跑通与演示完整视频链路](demo.md)：第一次运行和五分钟演示。
- [架构设计](../ARCHITECTURE.md)：设计决策、线程/状态机和系统边界。
- [systemd 部署指南](deployment.md)：前台验证后，安装为非 root systemd 服务。
- [测试与验收指南](test-plan.md)：测试方法和真实验收记录，不适合第一次运行时从头阅读。
- [性能测试结果](benchmark-results.md)：可以在简历和面试中引用的性能数据及限制。
- [开发状态](development-status.md)：开发历史和上下文恢复记录，不是用户手册。
