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
          -> gw_pipeline_build()：生成 FFmpeg argv
          -> gw_process_start()：posix_spawnp 创建子进程
          -> poll() 读取 progress/stderr，检查退出和超时
          -> 成功：RUNNING
          -> 失败：BACKOFF -> 再次 STARTING
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
4. 构造好的 `argv[]` 为什么可以直接交给 `posix_spawnp()`。

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

### 第 5 层：运行进度

文件：

- `src/monitor/progress_parser.c`
- 对应的 `include/gateway/*.h`

`progress_parser.c` 增量解析 FFmpeg `-progress pipe:1` 的键值记录，不依赖普通日志的
自然语言格式。

读完应能区分两类失败：工作进程已经退出，以及工作进程仍在但长时间没有进度。

### 第 6 层：状态机和自动恢复

文件：

- `src/channel/channel_state.c`
- `src/channel/supervisor.c`
- `include/gateway/channel_state.h`
- `include/gateway/supervisor.h`

先读纯状态转换 `channel_state.c`，再读负责 I/O 和时间的 `supervisor.c`。关键路径：

```text
STARTING -> RUNNING
   ^           |
   |           | 退出、启动超时或 progress 超时
   +-- BACKOFF-+
   ^
   | max_backoff_sec
 FAILED
```

`supervisor` 每 250ms 左右轮询管道和进程状态；失败后清理旧工作进程，按有上限的退避时间
等待，再重新启动 FFmpeg。连续稳定运行达到 `stable_run_sec` 后，连续失败计数会清零。
超过 `max_retries` 后进入 `FAILED`，但默认 supervisor 线程仍存活，并按
`max_backoff_sec` 低频执行 `FAILED -> STARTING`。stop、disable、配置 reload 和进程退出会
通过短周期 stop check 打断这段等待。`--exit-when-idle` 才把重试耗尽视为一次性运行结束。

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

- 解析 `--config`、`--check-config` 等 CLI 参数。
- 加载配置并处理三种检查模式。
- 安装 SIGINT、SIGTERM、SIGHUP 信号处理。
- 创建 Channel Manager 和 HTTP 线程。
- 处理配置重载、全局停止和退出码。

不要从 main 开始追进所有函数；那会同时遇到配置、线程、信号、HTTP 和进程管理，最容易
失去主线。

## 4. 四条必须能自己讲清楚的流程

### 4.1 启动流程

```text
YAML -> 默认值 -> 读取 -> 校验 -> 通道线程 -> FFmpeg -> RUNNING
```

### 4.2 断流恢复流程

```text
RTSP EOF/错误 -> FFmpeg 退出 -> waitpid 得到结果 -> 清理管道和 PID
-> BACKOFF -> 重新启动 FFmpeg -> 摄像头恢复后进入 RUNNING
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
4. `RUNNING -> BACKOFF -> STARTING` 每一步由什么事件触发？
5. 单路断流为什么不会重启健康通道？
6. `--check-config` 和真正运行有什么差别？
7. SIGHUP 配置错误时，为什么现有通道不会中断？
8. 哪部分是你写的，哪部分来自 FFmpeg-Rockchip 和 MediaMTX？
9. MPP/RGA 性能数据使用了什么输入、窗口和限制，为什么不能随意外推？
10. 当前项目最明显缺少的产品能力是什么？

## 7. 可用于简历的事实表达

只有自己能解释并且测试记录已经支持的内容才能使用。例如：

> 基于 C17 和 RK3588 实现多通道媒体任务管理服务，使用独立 supervisor 管理
> FFmpeg-Rockchip 子进程，通过结构化 progress 超时、进程回收和有上限
> 退避实现单通道故障隔离与 RTSP 断流恢复；完成 MPP/RGA 硬件转码、HTTP 控制、SIGHUP
> 差异化重载及 systemd 非 root 部署。

性能数字从 [性能测试结果](benchmark-results.md) 选择，并同时写清固定样本、短时测试和
最大吞吐/在线运行的区别。不要把重复固定样本的四路微基准写成“四路真实摄像头长期
稳定运行”。

## 8. 当前阅读进度

- 已完成：配置结构、libyaml 解析、环境变量展开和公共错误处理。
- 已完成：`pipeline_builder.h` 与 `gw_pipeline_build()`，包括 argv 所有权、
  FFmpeg 输入/输出参数、MPP/RGA、码率、FPS 和 GOP。
- 已完成：`process_manager.c`，包括管道所有权、`posix_spawnp()`、非阻塞读取、退出检查、
  超时停止和子进程回收。
- 已删除：启动前 ffprobe 预检；supervisor 现在直接启动 FFmpeg，由启动超时、运行进度和
  退出状态判断输入及工作进程故障。
- 已完成：`progress_parser.h` 与 `progress_parser.c`，理解 FFmpeg progress 分片输入、逐行
  拼接和 `progress=continue/end` 记录边界。
- 阅读中：`channel_state.h` 与 `channel_state.c`。

后续阅读只在本节更新进度，不再建立额外的开发流水账文档。

### Process Manager：管道端点所有权

`pipe[0]` 固定为读端，`pipe[1]` 固定为写端。创建管道后、启动 FFmpeg 前，四个端点暂时
都由 gatewayd 持有：

```text
stdout_pipe[0]：准备由 gatewayd 读取 progress
stdout_pipe[1]：准备交给 FFmpeg 写 stdout
stderr_pipe[0]：准备由 gatewayd 读取错误日志
stderr_pipe[1]：准备交给 FFmpeg 写 stderr
```

启动时将两个写端复制到 FFmpeg 的标准文件描述符：

```text
dup2(stdout_pipe[1], 1)  -> FFmpeg stdout
dup2(stderr_pipe[1], 2)  -> FFmpeg stderr
```

启动完成后的最终所有权和数据方向：

```text
FFmpeg fd 1 -> stdout 管道 -> gatewayd stdout_pipe[0]
FFmpeg fd 2 -> stderr 管道 -> gatewayd stderr_pipe[0]
```

gatewayd 只保留两个读端；FFmpeg 只保留复制后的 fd 1、fd 2。双方都关闭不再使用的原始
端点。原始端点还设置 `FD_CLOEXEC` 作为防泄漏保护，否则其他进程意外持有写端时，
gatewayd 可能在 FFmpeg 退出后仍收不到 EOF。

### Process Manager：启动 FFmpeg

父进程是 gatewayd，子进程是 FFmpeg。`gw_process` 位于 gatewayd 内存中，是管理一个
FFmpeg 的记录，不是子进程内部结构：

```text
gw_process.pid       -> 被管理的 FFmpeg PID
gw_process.stdout_fd -> gatewayd 持有的 stdout 管道读端
gw_process.stderr_fd -> gatewayd 持有的 stderr 管道读端
```

`posix_spawnp()` 创建子进程时，子进程先继承 gatewayd 当时打开的四个管道端点。随后
按 `actions` 在子进程中执行：

```text
dup2(stdout_pipe[1], 1)
dup2(stderr_pipe[1], 2)
close(stdout_pipe[0])
close(stdout_pipe[1])
close(stderr_pipe[0])
close(stderr_pipe[1])
```

关闭的是子进程继承的原始描述符，不影响父进程自己的描述符表。`dup2()` 后，FFmpeg
只需保留 fd 1 和 fd 2。启动成功后，gatewayd 再关闭自己持有的两个写端，并把两个读端
转移到 `gw_process`：

```text
FFmpeg fd 1 -> stdout 管道 -> gw_process.stdout_fd -> progress 解析
FFmpeg fd 2 -> stderr 管道 -> gw_process.stderr_fd -> 错误日志
```

`gw_process_start()` 的完整顺序：

```text
检查参数 -> 创建/配置两条管道 -> 初始化 actions -> 登记 dup2/close
-> 初始化 attributes -> 设置进程组和信号 -> posix_spawnp 启动
-> 父进程关闭写端 -> 保存子 PID 和两个读端
```

任一步失败都跳到统一的 `fail` 分支，只销毁已经初始化的 spawn 对象，并关闭已经创建的
管道端点，避免描述符和内存资源泄漏。

### Process Manager：读取子进程管道

`gw_process_read()` 对 FFmpeg stdout 或 stderr 做一次非阻塞读取。主要参数：

```text
process       管理 FFmpeg PID 和父进程持有的两个管道读端
stream        选择 GW_PROCESS_STDOUT 或 GW_PROCESS_STDERR
buffer        接收本次读取的原始字节，不保证自动添加 '\0'
capacity      buffer 可写容量
bytes_read    返回本次实际读取的字节数
end_of_stream 返回管道是否已经 EOF
error         可选的详细错误
```

`bytes_read` 和 `end_of_stream` 必须同时存在，因为读取 0 字节可能只是暂时无数据，也可能
是所有写端都已关闭。返回组合：

```text
GW_OK + bytes_read > 0 + end=false -> 读到数据
GW_OK + bytes_read = 0 + end=false -> 暂时无数据，稍后再读
GW_OK + bytes_read = 0 + end=true  -> 管道结束，读端已关闭
GW_ERR_IO                         -> 真正读取错误
```

当 `read()` 返回 `-1` 时检查 `<errno.h>` 中的错误码：

```text
EAGAIN / EWOULDBLOCK -> 非阻塞 fd 当前无数据，不是故障
EINTR                 -> read 被信号打断，可以重试
其他 errno            -> 真正 I/O 错误
```

写类似代码时不必死背全部错误码，应查看 `man 2 read` 的 `ERRORS`，结合当前 fd 已设置
`O_NONBLOCK`，把错误分成“可重试”和“真正失败”两类。

### Process Manager：检查并回收子进程

`gw_process_poll_exit()` 非阻塞检查 FFmpeg 是否退出。主要参数：

```text
process  保存 FFmpeg PID、运行状态和原始 wait_status
exited   返回 FFmpeg 是否已经退出并被回收
error    可选的详细错误
```

这里必须区分“检查操作”和“被检查的进程状态”：

```text
gw_status -> gatewayd 是否成功完成这次检查
exited    -> 检查成功后，FFmpeg 已退出还是仍在运行
```

因此 `GW_OK` 不是说 FFmpeg 一定运行正常，而是 `waitpid()` 成功给出了可靠结果：

```text
GW_OK + exited=false -> 成功确认 FFmpeg 仍在运行
GW_OK + exited=true  -> 成功确认 FFmpeg 已退出
错误状态             -> 检查失败，不能读取 exited 判断进程状态
```

核心调用：

```c
waitpid(process->pid, &process->wait_status, WNOHANG);
```

`WNOHANG` 表示 FFmpeg 仍运行时立即返回，不阻塞 supervisor。主要返回值：

```text
0                  -> FFmpeg 还未退出，exited=false
process->pid       -> FFmpeg 已退出且已回收，running=false、reaped=true
-1 + errno=EINTR   -> 本次检查被信号打断，稍后重试
其他负值           -> 真正的 waitpid 错误
```

`waitpid()` 在确认退出的同时完成回收，避免僵尸进程。进程退出不代表管道已经读空；上层
仍需继续读取 stdout/stderr 中的缓冲数据，直到两个读端都收到 EOF。

### Process Manager：停止 FFmpeg

`gw_process_stop(process, timeout_ms, error)` 尝试在指定毫秒数内停止 FFmpeg，并确保子进程
被 `waitpid()` 回收。整体流程：

```text
检查参数和进程状态
-> 向 FFmpeg 所在的整个进程组发送 SIGTERM
-> 在 timeout_ms 内每隔 10ms 非阻塞检查一次退出状态
-> 已退出：立即返回 GW_OK
-> 超时仍未退出：向整个进程组发送 SIGKILL
-> 阻塞 waitpid，直到子进程被回收
```

开始时的三个分支分别表示：

```text
process == NULL 或 timeout_ms < 0 -> 调用参数错误
process->reaped == true           -> 已经回收，无需重复停止，返回 GW_OK
process->pid <= 0                 -> 进程从未成功启动
```

发送信号时使用的是负 PID：

```c
kill(-process->pid, SIGTERM);
```

负 PID 表示信号发给进程组，而不是只发给 FFmpeg 主进程。这样 FFmpeg 创建的辅助进程也会
一起停止。`ESRCH` 表示目标已经不存在，在停止流程中不作为错误处理。

`make_deadline(timeout_ms)` 根据当前单调时钟计算绝对截止时间；循环内调用
`gw_process_poll_exit()`，并短暂休眠 10ms，避免持续占用 CPU。检查结果要分开处理：

```c
if (status != GW_OK) {
    return status;       /* waitpid 检查本身失败 */
}
if (exited) {
    return GW_OK;        /* 检查成功，而且 FFmpeg 已退出并被回收 */
}
```

到达截止时间后还会再检查一次，避免 FFmpeg 恰好在循环结束时退出却被误发 `SIGKILL`。
如果仍未退出，才发送 `SIGKILL`。最后的 `wait_after_kill()` 使用阻塞式 `waitpid()`，因为
发出强制终止信号并不等于子进程已经被内核回收；省略这一步会留下僵尸进程。

### Process Manager：解析退出码

`gw_process_exit_code()` 解析 `waitpid()` 保存到 `process->wait_status` 中的原始状态。
`wait_status` 不等于子进程的退出码，必须先判断退出类型，再使用与之配对的宏取值：

```text
WIFEXITED(status)    -> 判断子进程是否通过 exit() 或 main 返回而正常退出
WEXITSTATUS(status)  -> 取得正常退出码

WIFSIGNALED(status)  -> 判断子进程是否被信号终止
WTERMSIG(status)     -> 取得终止它的信号编号
```

正常退出示例：

```c
if (WIFEXITED(wait_status)) {
    code = WEXITSTATUS(wait_status);
}
```

如果 FFmpeg 执行 `exit(2)`，`WIFEXITED()` 为非零，`WEXITSTATUS()` 得到 `2`。

信号终止示例：

```c
if (WIFSIGNALED(wait_status)) {
    code = 128 + WTERMSIG(wait_status);
}
```

例如 FFmpeg 被 `SIGKILL` 终止：

```text
SIGKILL 的信号编号 = 9
最终返回值 = 128 + 9 = 137
```

这是 Shell 常用的信号退出码表示习惯：

```text
130 = 128 + SIGINT(2)
137 = 128 + SIGKILL(9)
143 = 128 + SIGTERM(15)
```

必须先调用 `WIFEXITED()` 或 `WIFSIGNALED()` 判断类型，不能直接读取 `wait_status`，也不能
在未确认类型时调用对应的取值宏。

### Channel State：状态机全貌

状态表示通道“当前处于什么阶段”，事件表示“刚刚发生了什么”。状态机只计算
`旧状态 + 事件 + 重试策略 -> 新状态`，不负责启动 FFmpeg、创建线程或真正等待；这些操作
由 supervisor 完成。

六种状态：

```text
DISABLED  配置中禁用了通道
STOPPED   通道可用，但当前没有运行
STARTING  正在启动 FFmpeg，等待第一份完整 progress
RUNNING   已收到 FFmpeg progress，工作进程正在运行
BACKOFF   本次运行失败，等待一段时间后重试
FAILED    连续失败超过上限，按最大退避时间低频重试
```

八种事件：

```text
ENABLE           启用通道
DISABLE          禁用通道
START            首次启动或手动启动
PROGRESS         收到一份完整 FFmpeg progress
FAILURE          启动失败、进程退出或 progress 超时
BACKOFF_ELAPSED  本次退避等待结束
STABLE           已连续稳定运行指定时间
STOP             主动停止通道
```

主要转换：

```text
DISABLED -- ENABLE --> STOPPED

STOPPED -- START --> STARTING -- PROGRESS --> RUNNING
                          |                      |
                          | FAILURE              | FAILURE
                          v                      |
                       BACKOFF <-----------------+
                          |
                   BACKOFF_ELAPSED
                          v
                       STARTING

连续失败超过 max_retries：STARTING/RUNNING -- FAILURE --> FAILED
低频等待结束：            FAILED -- BACKOFF_ELAPSED --> STARTING
主动停止：                非 DISABLED 状态 -- STOP --> STOPPED
```

`FAILED` 不是默认守护模式下的永久终态。supervisor 仍会等待 `max_backoff_sec`，再触发
`BACKOFF_ELAPSED` 重新启动；只有一次性运行模式才会在重试耗尽后退出。

`gw_channel_runtime` 保存每个通道的可变运行状态：

```text
state                 当前状态
consecutive_failures  从上次稳定运行或手动启动以来的连续失败次数
total_restarts        已执行的自动重启总次数
backoff_sec           当前 BACKOFF/FAILED 需要等待的秒数
```

其中连续失败次数在收到 `STABLE` 或手动 `START/STOP` 后清零；累计重启次数不会随稳定运行
清零，用于反映通道生命周期内发生过多少次自动恢复。

#### 指数退避

退避表示操作失败后先等待一段时间再重试；指数退避表示连续失败越多，等待时间按倍数
增长。`calculate_backoff()` 使用的基本规律是：

```text
等待时间 = 2^(连续失败次数 - 1)
```

当 `max_backoff_sec=30` 时，实际序列为：

```text
第 1 次失败 -> 1 秒
第 2 次失败 -> 2 秒
第 3 次失败 -> 4 秒
第 4 次失败 -> 8 秒
第 5 次失败 -> 16 秒
第 6 次及以后 -> 30 秒
```

代码在执行 `delay *= 2` 前先比较 `delay > maximum / 2`；如果下一次翻倍会超过上限，就
直接使用最大值，避免越界和无意义的继续增长。指数退避让偶发断流能够快速重连，同时
避免输入长期不可用时不停创建 FFmpeg、占用 CPU 和刷日志。

## 9. 文档地图

- [单路摄像头用户手册](user-manual.md)：第一次运行和五分钟演示。
- [架构设计](../ARCHITECTURE.md)：设计决策、线程/状态机和系统边界。
- [systemd 部署指南](deployment.md)：前台验证后，安装为非 root systemd 服务。
- [性能测试结果](benchmark-results.md)：可以在简历和面试中引用的性能数据及限制。
