# WaveLink 自动化打包部署指南

本目录 (`deploy`) 下包含安装版和便携版两套发布流程。工程内部路径以脚本或打包配置文件所在目录为基准，移动整个工程文件夹后不需要修改旧工程路径。Qt、MinGW、CMake、Inno Setup、UPX 属于电脑上的外部工具；如果它们的安装位置变化，需修改对应 `.bat` 顶部的工具路径。

---

## 发布目录完全隔离

发布目录完全隔离：安装版使用 `setup/build-WaveLink-Release`、`setup/WaveLink_Release` 和 `setup/_Setup_Work`；便携版使用各自 `portable` 下的对应目录及 `_Portable_Work`。BLE 构建缓存、编译器 TEMP/TMP、源码压缩包和打包中间文件均位于各自工作目录。根目录 `build`（包括 Qt Creator 的 Debug 缓存）不参与发布，也不会被清理；两套发布流程不共享缓存。

每次发布会在自己的工作目录生成最新 `source.zip`，排除源码目录里原有的 `resources/source.zip`，并通过 CMake 的 `WAVELINK_SOURCE_ARCHIVE` 参数直接嵌入，不再改写源码目录中的压缩包。软件内资源名称仍是 `:/source.zip`，长按导出源码的功能不变。日常 IDE 构建未指定该参数时，仍会自动使用源码目录中可选的 `resources/source.zip`。

安装包生成成功后，自动删除安装版的主程序构建缓存、待发布文件夹和 `_Setup_Work`；失败时保留工作目录及 Inno Setup 日志。打包最多重试 4 次，每次使用不同的本地输出及临时目录，成功后才替换同版本的旧安装包。重试不能保证排除杀毒软件拦截；全部失败时可检查 `_Setup_Work/Inno-Attempt-*.log`。便携版保留本地构建和待发布文件夹，供之后使用 Enigma 封包。

清理在批处理内部完成，不需要额外的清理 `.ps1` 文件；删除前会核对精确目录，并拒绝链接或联接点。自动化验证可设置环境变量 `WAVELINK_NO_PAUSE=1` 跳过末尾暂停，通常双击运行不需要设置。

## BLE 辅助程序的构建与分发

主程序保持 Qt 5.14.2 / MinGW；Windows BLE 辅助程序使用本机 Visual Studio 2022 C++ 工具与 Windows 10/11 SDK 单独编译。首次调试运行 `Build_BLE_Host.bat Debug`，再重新配置 Qt 工程；默认会从根目录 `build/ble-helper-msvc-debug/Debug` 将 `WaveLinkBleHost.exe` 复制到主程序目录。也可将目标目录作为第二个参数传入脚本。

两套发布脚本顶部新增 `ENABLE_BLE=1`，发布时自动构建 Release 辅助程序并复制到发布文件夹。设为 `0` 可发布不含 BLE 接口的版本。辅助程序使用静态 MSVC 运行库，用户电脑无需安装 Qt 或 MSVC 开发工具；构建电脑需具备相应工具。

`Build_BLE_Host.bat` 的第三个参数可指定独立构建目录，第二个参数仍是可选的复制目标目录。两套发布脚本均传入自己的 BLE 构建目录；只有单独执行且不传第三个参数时，才使用上述根目录的调试/辅助构建目录。

所有新增工程和产物路径相对于脚本定位。安装包、多文件便携文件夹自动包含辅助程序。使用 Enigma 时，必须将脚本生成在 `deploy/portable` 下的真实 `WaveLinkBleHost.exe` 与最终便携主程序一起分发，不能只把辅助程序封入虚拟文件系统。首轮 BLE 便携发行优先使用完整文件夹。

---

## 标准系统安装包 (Setup)

此方案会生成一个专业的 Windows 安装向导。用户可以自由选择安装路径、自动生成桌面及开始菜单快捷方式，并在“控制面板”中注册规范的卸载程序。

**工作目录**：`deploy\setup\`
**一键脚本**：`Build_Setup_Version.bat`
**核心工具**：Inno Setup (业界标准的免费安装包制作工具)

### 前置条件：
首次打包前，请确保您的电脑已经安装了 Inno Setup 编译器。
- **下载地址**：[https://jrsoftware.org/isdl.php#v7](https://jrsoftware.org/isdl.php#v7) (下载普通安装版一路下一步即可，通常安装在 `C:\Program Files\Inno Setup 7` 或 `C:\Program Files (x86)\Inno Setup 6`)。

### 操作步骤：
1. **版本信息**：脚本默认读取 `WaveLink\CMakeLists.txt` 中的 `WAVELINK_DEFAULT_VERSION`；如需临时指定版本，可修改批处理顶部的 `APP_VERSION_OVERRIDE`。通常无需修改 `wavelink.iss`。
2. **一键生成安装包**：
   - 双击运行 `Build_Setup_Version.bat`。
   - 该脚本除了会自动完成前期的 CMake 编译和依赖提取外，在最后一步还会**自动唤醒 Inno Setup 编译器**，在后台默默完成终极打包。
3. **打包完成**：同目录下会生成 `WaveLink_vx.x_Windows_x64_Setup.exe`。安装脚本中的图标、待安装文件和输出目录均使用相对路径；成功后批处理会清理临时 Release 构建文件夹。

### 关于卸载：
通过此 Setup 安装的软件，安装目录下会自动包含 `unins000.exe` 卸载程序，同时我也已经在代码中为您配置了直观的 `卸载 WaveLink` 快捷方式，用户一键即可彻底卸载，体验极其专业。
