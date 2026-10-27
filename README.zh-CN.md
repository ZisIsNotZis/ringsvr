<p align="center">
  <img src="assets/logo.svg" alt="ringsvr 标志" width="160" height="160">
</p>

<p align="center">
  <a href="README.md">English</a> · 简体中文
</p>

# ringsvr

*面向 Linux 的单线程 HTTP/1.1 文件服务器。*

**一个完全由 `io_uring` 驱动的单线程 HTTP/1.1 文件服务器：accept、读取、打开、发送和文件零拷贝传输全部走同一个 ring，没有线程池。**

`ringsvr` 是一个小型参考实现，整个 I/O 路径都是异步的。它小到可以一口气读完，却覆盖了 `io_uring` 里真正有意思的部分：SQPOLL 提交、带标签的续接任务、每连接独立的管道，以及文件到 socket 的 `splice()`。

这是一个研究／归档项目，不是生产级服务器。它想展示的是一个由 `io_uring` 驱动的请求状态机长什么样，以及这种朴素设计会在哪些地方开始吃力。

## 快速开始

**环境要求：** 支持 `io_uring` 的 Linux、C11 编译器、CMake ≥ 3.16，以及 `liburing`。

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake liburing-dev

cmake -S . -B build
cmake --build build

mkdir -p demo
printf 'hello from ringsvr\n' > demo/index.html
./build/ringsvr &

curl -s http://localhost:8000/demo/index.html
# hello from ringsvr

printf 'uploaded\n' > demo/in.txt
curl -s -X POST --data-binary @demo/in.txt http://localhost:8000/demo/out.txt
cat demo/out.txt
# uploaded

kill %1
```

服务器绑定 `0.0.0.0:8000`，没有命令行参数，也没有配置文件。请求路径相对于进程工作目录解析，URL 路径开头的 `/` 会被去掉，所以 `/demo/index.html` 对应 `./demo/index.html`。

## 现在能做什么

- **GET 与零拷贝文件传输**：`openat(O_RDONLY)` → 用 `statx(AT_EMPTY_PATH)` 取文件大小 → 手写响应头并通过 `MSG_ZEROCOPY` 发送 → 用 `splice()` 把文件内容经每连接管道送到 socket。
- **POST 直接落盘**：`openat(O_RDWR | O_TRUNC | O_CREAT)` → 先把已经读进请求缓冲区的 body 写入文件 → 再用 `splice()` 把 socket 中剩余的 body 经管道送进文件 → 返回 `200 OK`。
- **所有操作共用一个 ring**：accept、recv、openat、statx、send、splice、write、close 都以 SQE 入队，请求路径上没有阻塞系统调用。
- **SQPOLL 事件循环**：`sq_thread_idle = 10` 毫秒，1024 项 ring，256 个预先分配的连接槽。
- **明确的静态边界**：文件不存在返回 404；路径包含 `..` 会被拒绝；请求头超过 512 字节缓冲区会关闭连接；不支持的方法直接丢弃。
- **持久连接**：一次响应结束后，同一个 socket 会重新挂起，继续处理下一个请求。

### 边界

没有 TLS、身份认证、目录列表、`Content-Type`、`Date`/`Server` 响应头，也没有 `HEAD`、`Range`、分块传输、请求超时或优雅关闭。调试日志默认开启（`main.c` 中的 `#define DBG`），每个完成事件都会打印一行。512 字节的请求缓冲区不会增长。

## 架构

整个服务器就是一个事件循环，围绕一个 `io_uring`：

```text
GET
  accept -> recv -> openat -> statx -> send headers
         -> splice file -> pipe -> socket -> close file -> recv

POST
  accept -> recv -> openat -> write buffered body
         -> splice socket -> pipe -> file -> send 200 -> close file -> recv
```

每个连接槽持有 socket、文件描述符、一对管道、请求缓冲区，以及当前传输的计数器：

```c
struct {
    int file;
    int pipe[2];
    int sock;
    size_t srcn, pipen, dstn;
    char buf[512];
    struct statx stat;
    unsigned short bufi, bufn;
} TH[256];
```

每个 SQE 的 `user_data` 都是 `(slot_index << 4) | job`。完成事件到达后，低四位决定下一个状态，主循环从共享标签处继续挂起下一步操作。这里没有回调、协程或工作线程：完成事件里的标签就是程序计数器。

| 文件 | 用途 |
| --- | --- |
| `main.c` | 整个服务器：ring 初始化、连接表和请求状态机。 |
| `CMakeLists.txt` | 链接 `liburing` 的最小 C11 构建配置。 |
| `assets/logo.svg` | README 使用的项目标志。 |
| `docs/project-status.md` | 项目范围、验证记录和已知限制。 |

## 如何开发

标准 CMake 构建流程：

```sh
cmake -S . -B build
cmake --build build
```

如果 `liburing` 安装在非系统前缀下，把 CMake 指过去：

```sh
cmake -S . -B build -DLIBURING_ROOT=/opt/liburing
cmake --build build
```

要关掉每个完成事件的日志，删除或注释掉 `main.c` 顶部的 `#define DBG`。验证改动时，在一个临时目录启动服务器，按[快速开始](#快速开始)里的方式用 `curl` 请求它。

项目没有测试套件，也没有 CI。最近一次有记录的验证是在 Ubuntu、GCC 13.3、liburing 2.5 下手工完成构建与端到端运行：GET 返回文件内容，缺失路径返回 404，POST 写入上传的字节，含 `..` 的路径被拒绝。这只是一次时间点上的结果，不是兼容性承诺。

## 如何贡献

欢迎提交 issue 和 pull request。请保持改动聚焦，保留服务器单文件的风格，并在提交中写明测试所用的内核、编译器和 liburing 版本。涉及行为变化时，请附上请求和实际观察到的响应。

这是归档的研究代码，因此大型重写比小而清楚的修复或实验更难被接受。

## 路线图

### 已实现

- [x] 只通过 `io_uring` 完成 accept/recv/open/statx/send/splice/write/close
- [x] SQPOLL 提交与 1024 项 ring
- [x] 256 个连接槽，每个槽拥有独立管道
- [x] HTTP/1.1 GET，文件到 socket 的零拷贝传输
- [x] HTTP/1.1 POST，socket 到文件的传输
- [x] 文件缺失返回 404，并拦截 `..` 路径穿越
- [x] 持久连接，可继续处理后续请求
- [x] 响应头使用 `MSG_ZEROCOPY`

### 计划 / 可能的方向

- [ ] 为 GET、POST、404、路径穿越和流水线请求补一个小型测试工具
- [ ] `Content-Type`、`Date` 和 `Server` 响应头
- [ ] `HEAD`、`Range` 和分块传输编码
- [ ] 请求超时、优雅关闭和信号处理
- [ ] 正确处理 `MSG_ZEROCOPY` 的错误队列
- [ ] 可选的请求缓冲区增长，突破 512 字节上限

### 未来设想

如果这个项目继续做下去，有意思的方向不是“加更多功能”，而是把它做成一份忠实、易读的参考：当 ring 本身就是调度器时，一个全异步文件服务器应该长什么样——一张连接表、一个完成事件标签，以及一条没有隐藏拷贝的传输路径。上面的路线图是朝这个形状推进的实验清单，不是发布承诺。

## 许可证

GNU Affero 通用公共许可证第 3 版。见 [`LICENSE`](LICENSE)。
