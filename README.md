# 2048 AI

> 用 **expectimax 搜索 + 80 位位棋盘（bitboard）** 把 2048 打到很高的分数。
> 核心引擎用 C++ 写成，Python 只负责"读棋盘 / 按键"，因此单次决策通常在
> 0.02~0.05 秒内完成，可在真实网页上实时自动对局。

本仓库基于 [nneonneo/2048-ai](https://github.com/nneonneo/2048-ai)，在其之上做了
**棋盘表示升级（4-bit → 5-bit，突破 32768 上限）** 与一批 Python 3 缺陷修复，
并补上了两份中文实战文档：[`docs/使用指南.md`](docs/使用指南.md)（冲分规范）与
[`docs/实战经验.md`](docs/实战经验.md)（真实网站接入踩坑记录）。

---

## 目录

- [它能做什么](#它能做什么)
- [技术亮点](#技术亮点)
- [快速开始](#快速开始)
- [三种使用方式](#三种使用方式)
- [环境变量（加速旋钮）](#环境变量加速旋钮)
- [目录结构](#目录结构)
- [工作原理](#工作原理)
- [常见问题](#常见问题)
- [文档](#文档)
- [许可](#许可)

---

## 它能做什么

| 能力              | 说明                                              |
| --------------- | ----------------------------------------------- |
| 🎯 **命令行自走棋**   | `bin/2048` 自己和自己下，打印每步统计与最终分数/最高方块              |
| 🌐 **真实网页自动对局** | 通过远程调试（CDP / Firefox debugger）驱动真实浏览器里的 2048 网页 |
| 🧑🏫 **教练模式**   | 你手动报棋盘、AI 报下一步方向，手机/平板上也能用                      |
| 🚀 **高性能**      | 位棋盘 + 转换表缓存，现代 CPU 上每秒可评估**上千万步**               |

实测强度：**稳定合成 2048 与 4096，继续对局可达 8192 及以上**（属概率性结果，
偶有崩盘属正常波动）。引擎的**表示上限是 2³¹**，所以 65536、131072 级别的大块
在表示层面完全支持——不再是老版本 32768 就顶格的局面。

---

## 技术亮点

- **80 位位棋盘**：每格用 **5 bit** 存"指数 rank"（0=空，1=2，2=4，…，16=65536），
  16 格 = 80 bit = 两个 `uint64`（`lo` 存第 0~7 格，`hi` 存第 8~15 格）。
  rank 0~31 可表示，即最大方块 **2³¹**。
- **移动用查表 + XOR 完成**：一行 4 格打包成 20 bit，预计算 `2²⁰` 项移动表，
  执行一次移动只需几次查表与异或。
- **expectimax 搜索**：对每个方向枚举"新方块落在每个空格"的随机节点，
  2 与 4 分别按 0.9 / 0.1 加权；配**转换表（transposition table）**缓存节点值。
- **自适应深度**：`depth_limit = max(3, 不同方块种类数 − 2 + 调整值)`，
  盘面越复杂看得越深。
- **概率剪枝**：累计概率低于阈值（默认 `1e-4`）的分支直接剪掉。
- **启发式即"角落策略"**：空位奖励 + 可合并奖励 − 单调性惩罚 − 总和大惩罚
  − 死亡惩罚，权重都在 `2048.cpp` 顶部常量里，可自行调参。
- **多线程**：Python 侧用线程池并行评估 4 个方向（`2048_THREADS` 可调）。

---

## 快速开始

### 1. 编译

需要 C++11 编译器（g++ / clang++ / MSVC 均可）。

```sh
# 方式一：autotools（需要 autoconf/automake）
./configure && make

# 方式二：一条命令编译（macOS / Linux，无需 autotools）
mkdir -p bin
g++ -O3 -Wall -Wextra -fPIC -shared -o bin/2048.so 2048.cpp   # Python 绑定用的共享库
g++ -O3 -o bin/2048 2048.cpp                                  # 命令行自走棋
```

产物都在 `bin/` 下：`bin/2048`（可执行文件）与 `bin/2048.{so,dylib,dll}`（共享库）。
**没有 `make install`**——程序按设计就在本目录下运行（它会加载 `bin/2048.so`）。

<details>
<summary>Windows 编译</summary>

- **Cygwin**：按上面的 Unix 步骤即可。注意生成的 DLL 只能被 Cygwin 程序加载，
  所以要用 Cygwin 版 Python 做浏览器控制。

- **Cygwin + MinGW**（生成可被非 Cygwin 程序加载的 64 位 DLL）：

  ```sh
  CXX=x86_64-w64-mingw32-g++ \
    CXXFLAGS='-static-libstdc++ -static-libgcc -D_WINDLL -D_GNU_SOURCE=1' \
    ./configure ; make
  ```

  需要 32 位 DLL 就把 `CXX` 换成 `i686-w64-mingw32-g++`。

- **Visual Studio**：打开对应位数的 *Native Tools Command Prompt*，运行 `make-msvc.bat`。

> ⚠️ **位数必须一致**：Python 解释器与 DLL 同位数（32/32、64/64），
> 否则加载时报 `%1 is not a valid Win32 application`。

</details>

### 2. 跑起来

```sh
bin/2048                     # 看 AI 自己冲分
python3 2048.py -b manual    # 问 AI 该怎么走（手机也能用）
```

---

## 三种使用方式

### 方式一：命令行自动对局（最省心）

```sh
bin/2048
```

逐步打印当前分数、启发式评估、搜索节点数 / 缓存命中 / 耗时 / 最大深度，
结束时给出最终分数与最高方块 rank。**不想动手只想看它冲分就用这个。**

### 方式二：浏览器自动控制（在真实网页上演示）

`2048.py` 通过远程调试驱动真实的 Firefox / Chrome 标签页。

**Chrome**：先带远程调试参数重启 Chrome

```sh
google-chrome --remote-debugging-port=9222 \
              --remote-allow-origins=http://localhost:9222 \
              --user-data-dir=chrome.tmp
```

打开 2048 网页，然后：

```sh
pip install websocket-client     # 依赖
python3 2048.py -b chrome -p 9222
```

**Firefox**：在 `about:config` 打开 `devtools.debugger.remote-enabled` 与
`devtools.chrome.enabled`，用 `--start-debugger-server 32000` 重启，打开游戏后：

```sh
python3 2048.py -b firefox -p 32000
```

对网页的兼容模式用 `-k` 切换（默认 `hybrid`）：

| `-k` 模式      | 说明                    |
| ------------ | --------------------- |
| `hybrid`     | 默认，原始 2048 及大多数兼容克隆   |
| `fast`       | 更快，但对克隆兼容性差           |
| `keyboard`   | 较慢，能兼容部分克隆（纯键盘事件）     |
| `play2048co` | 针对 2025 版 play2048.co |
