# TLockGUI

TLockGUI 是一个面向 Windows 10/11 x64、Qt 6.8.3 Widgets 和 MinGW 64-bit 的桌面前端。它不实现任何密码学；所有时间锁加密和解密均通过 `QProcess` 调用官方 `drand/tlock` `tle.exe`。

当前首个公开版本为 **v1.0.0**。

## 安全边界

- GUI 不读取加密/解密文件内容，文件数据完全由 `tle.exe` 处理；文件大小、进度和累计字节均使用 `qint64`。
- 不使用 `system()`、`cmd.exe`、shell 命令拼接或手工路径引号。程序和参数分别通过 `QProcess::setProgram()`、`QProcess::setArguments()` 传入。
- `tle.exe` 只写 `最终文件名.part`。仅在进程正常退出、退出代码为 0 且 `.part` 存在并非空时，才保存为最终文件。
- 覆盖已有目标时，旧文件不会预先删除。新 `.part` 完成后，旧文件先重命名为唯一备份，新文件就位后才删除备份；失败时尽量自动恢复旧文件。
- 已经存在的 `.part` 不会被自动覆盖或删除，避免把来源不明的文件当作本次临时输出。
- 取消时先 `terminate()`，2 秒后仍未退出才 `kill()`；随后清理本次 `.part`。输入文件永不删除。
- 默认不传 `-n`、`-c` 或 `-a`，使用官方默认网络、quicknet 和二进制密文。

## 已实现功能

- 加密/解密模式与顺序批处理，一次仅运行一个 `tle.exe`。
- 多文件选择、Explorer 拖拽、重复路径过滤、Delete 键移除。
- 绝对解锁日期时间、当前时区和 10 分钟至 1 年快捷时间。
- 每个加密任务真正开始前重新计算 `target - now`，生成 `-D <seconds>s`；目标到达后停止后续加密。
- 源文件目录或指定输出目录；加密追加 `.tle`，解密移除 `.tle`，其他输入追加 `.decrypted`。
- 已有目标的“覆盖 / 跳过 / 全部覆盖 / 全部跳过 / 取消整个任务”。
- `.part` 大小近似进度、最近约 5 秒滑动速度、耗时、ETA 和批量总进度。
- 批量开始前按输出卷汇总磁盘需求，并为每个文件增加 64 MiB 安全余量；每项启动前再次检查。
- 完整 stdout/stderr 日志；`stderr` 非空不等同失败。`too early`、网络和磁盘满错误提供中文提示。
- 启动时按“程序目录、QSettings、`C:/tlock/tle.exe`、PATH、手动选择”查找，并使用 `--metadata` 验证。
- QSettings 保存窗口几何、tle 路径、输出目录、解锁时间及高级设置，不保存文件列表。
- 右键按 8 MiB 块在后台线程流式计算 SHA-256，并支持复制。
- 关闭运行中的任务时确认、终止子进程、清理 `.part` 后再退出。
- 可选“处理期间阻止系统自动睡眠”：通过 Windows `SetThreadExecutionState` 保持系统唤醒，批次结束或程序退出时恢复默认策略；屏幕仍可正常关闭。
- 可选“全部完成后关机”：开始前明确确认，仅在无失败、无取消且未手动停止时，通过独立参数调用 Windows `shutdown.exe` 安排 60 秒后关机，并提供取消按钮。

## 项目结构

```text
TLockGUI/
├── CMakeLists.txt
├── LICENSE
├── README.md
├── THIRD_PARTY_NOTICES.md
├── test.txt
├── licenses/
│   └── tlock-LICENSE-MIT.txt
├── resources/
├── src/
│   ├── main.cpp
│   ├── MainWindow.h/.cpp
│   ├── FileTask.h
│   ├── TleRunner.h/.cpp
│   ├── TaskQueue.h/.cpp
│   ├── HashWorker.h/.cpp
│   ├── PowerManager.h/.cpp
│   └── Utils.h/.cpp
└── tests/
    ├── FakeTle.cpp
    ├── TestMain.cpp
    ├── GuiSmoke.cpp
    └── OfficialTleIntegration.cpp
```

`MainWindow` 负责界面、文件表、用户决策、设置和预检；`TaskQueue` 负责状态机、顺序调度、每项 duration、覆盖策略和安全落盘；`TleRunner` 独占一个 `QProcess` 并负责进程生命周期、日志与估算进度；`PowerManager` 封装 Windows 保持唤醒和计划关机；`HashWorker` 在后台流式计算 SHA-256；`Utils` 负责大小、时长、路径和输出命名等纯辅助逻辑。

## 电源选项

- “处理期间阻止系统自动睡眠”默认开启，只在批处理运行期间生效。它防止由系统空闲计时器触发的自动睡眠，不阻止屏幕关闭，也不绕过合盖、低电量、休眠按钮或用户手动睡眠。
- “全部完成后关机”默认关闭并保存用户选择。选中后，开始批处理前会再次确认。仅当队列没有失败或取消时才安排关机；失败、手动停止或取消任务都会抑制关机。
- 成功安排后有 60 秒撤销时间，可点击“取消计划关机”。Windows 的定时关机可能关闭其他程序，请事先保存未保存内容。

## CPU 利用率说明

TLockGUI 默认一次只运行一个 `tle.exe`，GUI 本身只轮询文件元数据，因此总 CPU 利用率较低通常是正常现象。任务管理器的总百分比按全部逻辑处理器平均：例如 12 个逻辑处理器上的约 8% 已接近一个逻辑核心满载。判断性能时应同时查看 `tle.exe` 的单进程/单核占用、源盘与目标盘吞吐和实际 MiB/s；为避免多个 10–20GB 文件争用磁盘，本项目不会通过并行运行多个 `tle.exe` 来追求更高总 CPU。

## Qt Creator 编译

1. 在 Qt Creator 选择 **File → Open File or Project**，打开根目录的 `CMakeLists.txt`。
2. 选择 Kit：**Desktop Qt 6.8.3 MinGW 64-bit**。
3. 首次开发选择 Debug，点击 **Configure Project**，然后 Build/Run。
4. 最终构建在左侧 Projects 中把 Build configuration 切换为 Release 后重新构建。

不需要 Visual Studio 或 MSVC。

## PowerShell 命令行构建

下面是当前标准 Qt 安装布局对应的 Release 示例：

```powershell
$env:Path = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.8.3\mingw_64\bin;" + $env:Path
cmake -S . -B build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64 `
  -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe `
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
cmake --build build-release -j
```

也可把生成器改为 `"MinGW Makefiles"`；此时确保 `mingw32-make.exe` 在 PATH 中，并省略 Ninja 的 make program 参数。

## 测试

普通自动化测试使用一个仅复制数据的测试替身验证队列，不模拟或替代任何密码学：

```powershell
ctest --test-dir build --output-on-failure
```

官方联网集成测试会真实等待约 35 秒：

```powershell
.\build\OfficialTleIntegration.exe C:\tlock\tle.exe .\test.txt
```

它通过项目自己的 `TaskQueue` 检查官方加密、提前解密失败、到期解密、中文及特殊字符路径、`.part` 重命名和最终内容 SHA-256。

## Release 部署

```powershell
New-Item -ItemType Directory -Path dist\TLockGUI -Force
Copy-Item build-release\TLockGUI.exe dist\TLockGUI\
C:\Qt\6.8.3\mingw_64\bin\windeployqt.exe --release --compiler-runtime dist\TLockGUI\TLockGUI.exe
Copy-Item C:\tlock\tle.exe dist\TLockGUI\tle.exe
```

让 `windeployqt` 自动决定 Qt DLL、平台插件和 MinGW runtime，不要维护硬编码 DLL 清单。推荐把官方 `tle.exe` 放在 `TLockGUI.exe` 同目录；开发阶段也会回退查找 `C:/tlock/tle.exe`。

## 已知限制

- `tle.exe` 没有进度 API，界面百分比、速度和 ETA 都是基于 `.part` 大小的近似值，解密时可能不线性。
- 第一版不提供暂停/恢复、并行 tle 任务、自动删除输入文件、armor、SHA-256 清单导出或批量任务恢复。
- `--metadata` 和正式操作依赖 drand 网络；代理、防火墙或网络不可用会使验证/任务失败。
- 超长路径最终能否工作还取决于 Windows 的长路径策略以及官方 Go 可执行文件；Qt 侧始终保留 Unicode `QString` 路径。
- 批量磁盘估算有意偏保守，实际 tlock 密文大小可能与输入略有差异。
- 保持唤醒针对空闲睡眠，不保证阻止合盖、临界电量、系统更新或用户主动触发的关机/睡眠。

## 许可证

TLockGUI 自身以 [MIT License](LICENSE) 发布。官方 `drand/tlock`、Qt 和 MinGW runtime 仍归各自作者所有，具体信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。Windows 二进制发布包同时附带随包组件的许可证文本。
