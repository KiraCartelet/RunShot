# Changelog

## 2026-11-06

### scripts 重构

对应提交：[`8db105f`](https://github.com/KiraCartelet/RunShot/commit/8db105f)。

- 按现有 `tools/`、`qemu-plugins/` 风格整理 `wrap_stack.c`，统一命名与注释，提取重复的内存映射和字符串数组复制逻辑，并使用现有 clang-format 配置格式化。
- 将 `make_entry.sh` 中的快照解析拆到 `snapshot_regs.py`；启动、寄存器恢复和异常处理汇编分别拆到 `entry/boot.S`、`entry/restore.S` 和 `entry/trap.S`。
- 修复 Python 快照解析失败时，错误状态被 Bash 进程替换隐藏的问题；解析失败后停止生成和编译。
- `build.sh` 和 `make_entry.sh` 根据脚本位置查找辅助源码，支持从其他工作目录调用；输出仍写入当前工作目录，已有的 `entry.ld` 保持不变。
- 新增辅助脚本回归测试，覆盖快照格式、稀疏与完整寄存器、截断数据、缺失必要寄存器、跨目录调用和入口生成。
- 将详细修改记录集中到本文件；README 保留使用说明、检查命令和本文件链接。

### 兼容性

- 继续面向 Linux、Bash 4+、Python 3 和 RISC-V 工具链，保留 `mapfile`。
- 固定地址、编译参数、寄存器恢复顺序和跳转到保存的 `ra` 的逻辑保持不变。
- 复制辅助脚本时需复制整个 `scripts/` 目录，包括新增的 Python 文件和 `entry/` 目录。
- 本次仅修改 `main`，未同步修改 `runshot-gui`。

### 验证

- 原有 pre-commit 检查及 Shell 语法检查通过。
- 三段拆出的汇编与原内容逐字一致；稀疏、完整和混合寄存器快照的解析输出与旧版一致。
- 本地回归测试通过 9 项，1 项因 macOS 自带 Bash 版本低于 4 而跳过；编译器调用使用模拟工具。
- 尚未完成 Linux/RISC-V 环境下的实际编译及快照保存、恢复验证。
