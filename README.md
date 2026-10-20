# check7z

> [!IMPORTANT]
> **AI 生成提示 / AI-Generated Code Notice**
>
> 本仓库代码（包括 v2.0 的重写部分）由 **AI 辅助生成**，可能存在错误、缺陷或安全漏洞。
> 代码仅供学习、研究与娱乐参考，不保证正确性、稳定性或安全性。
> 使用前请**自行审查代码**，并自行承担由此产生的风险。

> [!WARNING]
> **免责声明 / Disclaimer**
>
> 本软件按**“原样”（AS IS）**提供，不附带任何明示或暗示的担保，包括但不限于适销性、
> 特定用途适用性及不侵权担保。在任何情况下，作者或贡献者均不对因使用本软件或无法使用
> 本软件而产生的任何直接、间接、偶然、特殊或后果性损失承担责任。
> 是否使用本软件以及如何使用，由使用者自行判断并承担全部风险。

扫描本机自带的 **7z 解压程序**：看看有多少软件偷偷带了一份 `7z.exe`，它们在哪、什么版本、占多大。

```
============================================================
        check7z v2.0.0 — 7z 解压程序扫描
============================================================
[1/3] 检索注册表
      + D:\Tools\7-Zip\7z.exe
[2/3] 扫描目录（8 线程）
      根目录: C:\Program Files
      根目录: C:\Program Files (x86)
      根目录: D:\
      + C:\Program Files\NVIDIA Corporation\NVIDIA app\7z.exe
      + D:\Tools\7-Zip\7zFM.exe
      + C:\Program Files (x86)\Lenovo\VantageService\5.1.2609.9\arm64\7zr.exe
      ...
[3/3] 汇总

------------------------------------------------------------
共找到 12 个 7z 可执行文件，分布在 9 个软件目录。
------------------------------------------------------------
[1] C:\Program Files (x86)\Lenovo\VantageService
      5.1.2609.9\arm64\7zr.exe  26.2.0.0      856.1 KB
      5.1.2609.9\x64\7zr.exe    26.2.0.0      922.1 KB
[2] C:\Program Files\NVIDIA Corporation\NVIDIA app
      7z.exe          26.1.0.0      543.1 KB
[3] D:\Tools\7-Zip
      7z.exe          26.4.0.0      564.0 KB
      7zFM.exe        26.4.0.0      980.0 KB
      7zG.exe         26.4.0.0      708.5 KB
...
------------------------------------------------------------
合计: 9 个软件 / 12 个文件 / 9.7 MB（exe 体积）
耗时: 0.6 秒 | 目录 58,293 | 文件 510,221 | 无权限 0
============================================================
```

> 最初版本由 AI 生成、仅供娱乐；2.0 在原思路上重写，重点是**结果准、速度快、能脚本化**。

## 2.0 改了什么

| 问题（1.x） | 2.0 的做法 |
|---|---|
| 用 `std::string` 存路径，中文/日文路径乱码 | 全程 `std::filesystem::path`（宽字符），输出统一 UTF-8 |
| `system("chcp 65001 > nul")`：闪黑框、污染子进程 | `SetConsoleOutputCP(CP_UTF8)`，不调用外部命令 |
| 单线程递归扫 `D:\`，大目录要等很久 | 多线程工作队列（默认 CPU 核心数，最多 8），跳过 junction/符号链接 |
| 结果是一堆 exe 路径，看不出“几个软件” | 按**软件目录**聚合，自动合并 `bin` / `x64` / `arm64` / `5.1.2609.9` 等子目录 |
| `RegOpenKeyExA` + `MAX_PATH` 缓冲，32 位视图读不到 | 宽字符 API、64/32 位视图、`REG_EXPAND_SZ`、`App Paths`，缓冲按需分配 |
| 把 WinRAR / Bandizip 也算成 7z | 只认 7z 官方组件：`7z.exe` `7zG.exe` `7zFM.exe` `7za.exe` `7zr.exe` |
| 漏掉 `7zr.exe`、`7zFM.exe` | 一并识别（联想 Vantage 自带的就是 `7zr.exe`） |
| 扫完必须按回车，不能接管道/计划任务 | 控制台才暂停，可用 `--no-pause`；支持 `--quiet` |
| 出错静默、结果无法复用 | 统计“无权限”目录数，支持 `--json` / `--csv` 导出 |
| 无法中断 | `Ctrl+C` 停止后输出已完成的部分结果（退出码 130） |

**本机实测**（C: Program Files + D: 全盘，58,293 个目录 / 510,221 个文件）：

| | 旧版 | 新版 |
|---|---|---|
| 耗时 | 3.5 s | **0.6 s** |
| 结果 | 9 个 exe，无分组 | 12 个 exe，9 个软件，含版本与体积 |
| 漏报 | 漏 `7zr.exe`、`7zFM.exe` | 全部命中 |

## 用法

```
check7z [选项] [目录...]

  -r, --root <目录>      指定扫描根目录，可重复
      --all-drives       扫描所有固定磁盘（含 C:\ 全盘）
  -x, --exclude <目录>   额外排除的目录，可重复
      --max-depth <n>    最大递归深度，0 表示不限（默认 0）
  -j, --json <文件>      导出 JSON
      --csv <文件>       导出 CSV（带 BOM，Excel 直接打开）
  -t, --threads <n>      线程数（默认 CPU 核心数，上限 8）
  -q, --quiet            不打印进度和逐个命中
      --no-pause         结束后不等待回车
  -h, --help             帮助
  -v, --version          版本
```

示例：

```bat
check7z
check7z --root "D:\Games" --json result.json
check7z --all-drives --csv result.csv --threads 8
```

## 统计口径

- **一个“软件目录”算一个软件**：7z 程序所在目录；若在 `bin` / `x64` / `arm64` / 版本号目录里，自动向上归并。
- 只统计可执行文件，**不含 `7z.dll`**。
- 注册表命中的官方 7-Zip（HKLM/HKCU，64/32 位，`App Paths`）也会计入并去重。
- 默认排除 `$Recycle.Bin`、`System Volume Information`、`.git`、`Windows\WinSxS` 等系统/缓存目录，可用 `--exclude` 追加。

## 构建

**Visual Studio**：打开 `check7z.sln`，选 `Release | x64` 直接生成（工程已配置 `/utf-8`、C++17、静态 CRT）。

**命令行（MSVC）**：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++17 /utf-8 /O2 /MT check7z.cpp /Fe:check7z.exe advapi32.lib shell32.lib version.lib
```

**CMake**：

```bat
cmake -B build -A x64
cmake --build build --config Release
```

**MinGW-w64**（可选，本机未验证）：

```bat
g++ -std=c++17 -O2 -static check7z.cpp -o check7z.exe -ladvapi32 -lshell32 -lversion
```

生成的 exe 静态链接 CRT，只依赖 `KERNEL32` / `ADVAPI32` / `SHELL32` / `VERSION`。

## JSON 输出

```json
{
  "tool": "check7z",
  "version": "2.0.0",
  "generated_at": "2026-10-20T21:05:00",
  "interrupted": false,
  "roots": ["C:\\Program Files", "C:\\Program Files (x86)"],
  "summary": {
    "software": 9,
    "files": 12,
    "bytes": 10221768,
    "directories_scanned": 58293,
    "files_scanned": 510221,
    "access_denied": 0,
    "elapsed_seconds": 0.59
  },
  "software": [
    {
      "path": "C:\\Program Files\\Vendor\\App",
      "files": [
        {"name": "7z.exe", "path": "...", "version": "26.1.0.0", "bytes": 556032}
      ]
    }
  ]
}
```

## 安全说明

本项目在 2.0 中做过一次针对性自查：

- **无命令注入面**：不使用 `system()` / `popen()` / `CreateProcess` / `ShellExecute`，不拼接 shell 命令，也不调用 `chcp` 等外部程序。
- **注册表只读**：只读取固定的 7-Zip 安装路径（HKLM/HKCU、64/32 位视图、`App Paths`），不会执行注册表中的任何命令。
- **无硬编码密钥**：代码不含 API Key / Token / 密码，程序不联网。
- **资源管理**：内存与容器使用 RAII，注册表句柄在每条路径上都会 `RegCloseKey`，`CommandLineToArgvW` 的结果会 `LocalFree`。
- **缓冲安全**：版本信息与注册表读取按实际长度分配缓冲，格式化输出使用有界 `snprintf` / `swprintf`。
- **仅本地读写**：`--json` / `--csv` 会写入用户指定的路径，这是命令行工具的预期行为；请勿以管理员权限指向系统文件。

## 已知限制

- 不跟随 junction / 符号链接（避免死循环与重复计数），链接目录里的 7z 可能漏掉。
- 无管理员权限时个别系统目录读不到，会计入“无权限”数量。
- 软件目录归并是启发式的：极少数布局奇怪的软件可能显示为它的某个子目录。
- 未启用 `longPathAware`，超过 260 字符的深路径可能被跳过（会计入“无权限”）。

## 许可

仅供学习与娱乐使用；扫描结果仅供参考，请勿用于任何非授权用途。
