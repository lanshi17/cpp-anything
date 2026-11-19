# cpp-anything

**把你的应用重写成 C++20/23，并证明它行为不变。**

cpp-anything 先录制 Python 代码今天的行为，再生成一个必须表现一致的 C++23 工程，
然后逐条重放录制用例，直到全部一致。它不翻译代码：C++ 由你（或你的 agent）来写；
它提供 agent 猜不准的东西——边界在哪、要对齐的具体行为，以及一个说"完成"的 gate。

```
$ cpp-anything analyze .                          # 仓库里有什么，移植代价多大
$ cpp-anything bridge . textkit.words             # 每个单元一个 C++23 工程
$ cpp-anything record cpp/textkit-words           # 105 条用例：原实现的返回值/异常
$ $EDITOR cpp/textkit-words/src/port.cpp          # 你写 C++
$ cpp-anything diff cpp/textkit-words             # 105/105 全部一致
$ cpp-anything gate cpp/textkit-words             # 完成，且被证明
$ cpp-anything bench cpp/textkit-words            # 333 ns → 141 ns（2.37x）
```

## 快速开始

需要 CMake 3.28+、C++23 编译器（GCC 13+ / Clang 17+ / MSVC 19.33+），以及运行被移植
代码的那个 Python 3。

```sh
cargo install --path .
cpp-anything doctor          # 检查 cmake、C++ 编译器、python3、矩阵、文档
cpp-anything analyze .
```

然后让 agent 做一个单元：

> *"用 cpp-anything 把 `textkit.words` 移植到 C++：录原实现、写 `src/port.cpp`，
> `gate` 通过再停。"*

CLI 和 MCP 服务（`cpp-anything mcp`）暴露同一套步骤，所以只会用终端的 agent 和有工具
调用的 agent 走的是同一条路。

## 它替你解决的问题

| 只有 agent | 加上 cpp-anything |
| --- | --- |
| 读代码猜行为 | `record` 在生成的边界输入上跑原实现，保存返回值和异常 |
| 写完没人能验证 | `diff` 用 C++ driver 逐条重放，逐字节比较 |
| 猜编译器支持什么 | `matrix` 用 cppreference 的编译器支持表回答，`profile.hpp` 逐个设施门控 |
| 编译通过就说"完成" | `gate` 在残留桩、签名不一致、用例不一致、工具链表达不了时失败 |
| 悄悄改了公开 API | 生成的 `port.hpp` 固定了 driver 调用的签名 |

## C++20/23 与编译器矩阵

矩阵是事实基础：C++11–C++26 共 651 个特性，每个特性记录 GCC、Clang/libc++、
MSVC/MSVC STL、Apple Clang 等实现它的第一个版本、feature-test 宏与提案编号。
数据来自 cppreference 的 compiler support 模板，并记录页面 revision 与抓取时间。

- [工具链指南索引](guides/README.md)：该选哪个编译器/标准，为什么
- [GCC](guides/compilers/gcc.md) · [Clang + libc++](guides/compilers/clang.md) ·
  [MSVC](guides/compilers/msvc.md) · [LLVM 版本](guides/compilers/llvm.md)
- 标准：[C++17](guides/standards/c++17.md) · [C++20](guides/standards/c++20.md) ·
  [C++23](guides/standards/c++23.md) · [C++26](guides/standards/c++26.md)
- [完整矩阵](guides/compiler-matrix.md)

```sh
cpp-anything matrix features --toolchain clang --version 17 --std c++23
cpp-anything matrix explain __cpp_lib_expected
cpp-anything matrix profile  --toolchain msvc --version 19.44 --std c++23
```

指南由数据生成（`cpp-anything matrix docs`），`matrix docs --check` 在指南过期时失败；
数据用 `scripts/sync_matrix.py` 显式同步，并记录读到的 cppreference revision。

## 生成代码的规则

- **默认 C++23，支持 C++20**：失败用 `cppan::Result<T>`（C++23 目标下就是
  `std::expected`），入参 `std::string_view`、出参 `std::string`，只读序列用
  `std::span`，全部 `[[nodiscard]]`，driver 用 ranges。
- **按矩阵门控，绝不靠猜**：`CPPAN_HAS_EXPECTED`、`CPPAN_HAS_PRINT`、
  `CPPAN_HAS_MDSPAN`、`CPPAN_HAS_MODULES` 等宏由目标工具链的矩阵决定。
- **gate 通过前不允许留桩**，也不需要 `unsafe`：driver 负责 JSON，port 负责逻辑。

[移植规则](porting.md) 覆盖 unicode 偏移、`int()` 与 `std::from_chars`、Python 无界整数、
dict 顺序、浮点格式化、正则引擎和异常类型。

## RSI：工具用同一套机制验证自己

`cpp-anything rsi check` 把循环用在自己身上：

1. 矩阵数据仍可解析，每个来源页都有 cppreference revision；
2. 指南与数据渲染结果逐字节一致；
3. 九条 CLI 录制输出仍与 `rsi record` 记录的一致；
4. 生成器仍能为随附示例生成工程，**编译通过并 105/105 重放一致**。

改坏生成器、wire 格式或数据，`rsi check` 会在到达用户之前失败。这就是"递归"的部分：
用来卡住移植的机制，同样卡住工具本身。

## 语言支持

| 源语言 | 状态 |
| --- | --- |
| Python | `analyze`、`bridge`、`record`、`diff`、`bench`、`gate` |
| C、Go、TypeScript、Java、C#、Ruby | 计划中，每个语言一个插件：循环、wire 格式与 gate 与语言无关，分析器、录制器和调用映射不是 |

目标语言是 C++20/23。把 C++ 移植到别的语言是另一个工具。

## 更多

- [工作原理](how-it-works.md) — wire 格式、比较规则、风险、限制
- [移植规则](porting.md) — 移植的 C++ 一侧
- [MCP 工具](tools.md) — 给 agent 的同一套步骤
- [安装](install.md) — 从源码、agent 接入、CI
- [FAQ](faq.md) · [决策记录](../DECISIONS.md) · [更新日志](../CHANGELOG.md)

## 许可

MIT。你仓库里生成的代码和移植结果归你。矩阵数据来自 cppreference.com
（CC-BY-SA 4.0 / GFDL），每份生成的指南页首都会注明。
