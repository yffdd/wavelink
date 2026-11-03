# WaveLink 在线更新发布指南

软件入口统一放在“帮助 → 版本信息”；没有服务器地址时明确显示未配置，手动下载仍可用。版本保持当前工程版本，不因添加更新功能自动升版。

## 1. 首次接入

1. 准备稳定的 HTTPS 静态托管：一个 `latest.json` 和完整的 Windows x64 安装包。不要求数据库或后台服务。服务器必须支持正常 HTTPS 文件下载；应用不绕过证书验证，不使用网盘页面自动抓取。
2. 只生成一次签名密钥。在 Windows PowerShell 中运行（路径自行替换）：

   ```powershell
   .\New-UpdateSigningKey.ps1 -KeyPath "$env:USERPROFILE\.wavelink-release\update-signing.key" -PublicKeyPath "$env:USERPROFILE\.wavelink-release\update-public.txt"
   ```

   私钥必须在项目/发布目录以外，使用 Windows DPAPI 加密，并限制为当前用户访问。不要上传私钥、放进 Git、安装包或源码压缩包。DPAPI 文件只能由同一 Windows 用户环境解密；需妥善备份密钥及对应用户恢复材料。丢失后不能直接让已安装客户端信任新公钥，应先通过旧密钥签名的版本迁移信任，或人工重新安装。

3. **已有上述密钥后，无需再手动创建配置。**直接运行 `deploy/setup/Build_Setup_Version.bat`：脚本会在清理工作目录和编译之前，自动调用 `Initialize-UpdatePublishing.ps1`，验证密钥并创建缺失的 `channel.cmake`、`publish.local.json`。当前默认仓库为 `yffdd/wavelink`，检查地址为 `https://github.com/yffdd/wavelink/releases/latest/download/latest.json`。如需单独初始化，也可运行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Initialize-UpdatePublishing.ps1`；其他仓库使用 `-Repository owner/repository`。
4. `channel.cmake` 只含公开的更新地址和**公钥**，会编入应用；`publish.local.json` 只记录私钥所在路径、下载地址模板和说明来源，不含私钥内容。已有自定义配置原样保留，发现密钥不匹配或配置损坏会停止，不会重新生成密钥或擅自覆盖。没有密钥也没有发布配置的机器仍可正常生成离线安装包。
5. 重新打包并先手动发布、安装一次包含正确公钥/地址的版本；已有的旧安装包不会自动获得新地址，缺少更新功能的旧程序也不会凭空获得在线检查能力。

## 2. 每次发布

1. 只需照常修改工程版本及帮助中心“当前版本”的更新日志，然后执行原来的 `deploy/setup/Build_Setup_Version.bat`。
2. 初始化/验证配置后编译正式安装包；打包成功后自动生成签名 `latest.json`，并生成可复制到 GitHub Release 描述中的 `WaveLink_v版本_ReleaseNotes.txt`。更新说明自动提取自 `WaveLink/src/ui/helpdialog.cpp` 中当前版本的日志，不混入历史版本。日志格式无法识别时会停止，不会静默生成空说明。
3. 下载地址中的 `{version}` 自动替换为本次打包的实际版本（包含版本覆盖参数），例如 `.../releases/download/v{version}` 自动变为 `.../releases/download/v0.14.1`。无需每次编辑配置。GitHub Release 的标签必须与生成地址中的 `v版本` 一致；本工具不创建标签、不上传文件、不发布 Release。
4. **先上传安装包**，验证下载地址，再**最后上传 latest.json**；也可在 GitHub Release 草稿中先上传两者，再一同发布为正式 Latest 版本。两个发布工具均不会自动修改外部服务。签名失败保留安装包并停止流程，**不能上传旧的 latest.json**。
5. 推荐使用 Windows Authenticode 为程序及安装包签名并添加时间戳。签名会改变文件摘要，因此必须在对最终安装包进行 Authenticode 签名**之后**生成或重新生成清单：

   ```powershell
   .\New-UpdateManifest.ps1 -Installer 'D:\Releases\WaveLink_v0.14_Windows_x64_Setup.exe' -Version '0.14' -DownloadUrl 'https://downloads.example.com/WaveLink_v0.14_Windows_x64_Setup.exe' -SigningKey "$env:USERPROFILE\.wavelink-release\update-signing.key" -NotesFile 'D:\Releases\notes.txt' -OutputPath 'D:\Releases\latest.json' -Force
   ```

   清单 RSA-SHA256 签名已经强制验证；Authenticode 证书获取、硬件签名和证书密码由发布者管理，当前脚本不会代购证书或自动签 Windows 证书。Windows 权限提示仍可能出现，不隐藏或绕过 UAC。

清单默认 30 天有效，最多 90 天；即使没有新版本，也应在到期前用同一密钥重新生成并发布清单。过期会明确拒绝，不把网络失败或过期当成“已经是最新版”。版本按数字比较，`0.13` 与 `0.13.0` 等价，不降级、不支持预发布字符串；当前渠道只处理 stable/windows-x64。

需要自定义说明时，可在 `publish.local.json` 中删除 `notesSource`，改用 `notesFile` 指向 UTF-8 文本；旧版 `installerBaseUrl` 固定地址和 `notesFile` 配置继续可用。默认自动模式不需要创建 `release-notes.txt`，也不需要维护版本路径。初始化可用 `-KeyDirectory` / `-UpdateDirectory` 指定位置，便于离线测试；私钥始终不允许位于项目/发布树内。

## 3. 升级行为与数据保护

- 启动 10 秒后后台检查，每天最多一次成功检查；“软件更新”设置可关闭。自动检查失败不弹框打断工作，手动进入版本窗口可查看原因；忽略版本只抑制自动提示，手动检查仍可查看。
- 下载使用 Windows WinHTTP 和系统 TLS 根证书，不依赖 Qt 5 的外置 OpenSSL DLL；支持最多 5 次 HTTPS 重定向，不接受明文 HTTP、URL 内的凭据或不安全重定向。使用 Windows WinHTTP 代理配置；特殊企业代理需由系统管理员配置。
- 清单限制 128 KiB、更新说明 20,000 字符、安装包 1 GiB；清单必须通过编译进程序的公钥验证，安装包必须与签名清单声明的大小及 SHA-256 一致。
- 下载在后台线程完成，不暂停录制；下载失败/取消只清理此操作新建的临时目录，不修改当前安装和配置。关闭版本窗口不取消后台下载，退出主程序则取消未完成的下载。
- 安装需要主动结束录制、停止发送并断开设备，不自动强制关设备。确认后先导出全局配置、复制协议/命令/状态配置原文件到应用数据目录 `UpdateBackups/时间-UUID/`。备份失败不退出、不安装。
- 全局备份为 `WaveLink_Config.ini`，可从“设置 → 导入配置”恢复。保留旧格式原文件以支持人工恢复。更新不删除原文件，不清理任何录制缓存或用户保存的数据。
- 独立原生启动器复制到临时目录，不占用 Qt DLL；先确认主进程身份并握手，只有主窗口确认关闭才提交安装，超时或未确认不会因之后的普通退出而安装。再等待主进程正常退出（最多 120 秒），再次验证安装包并锁定文件防止被替换，再启动 Inno Setup。固定 AppId 和原安装路径，不先卸载；仅对已登记的安装目录启用，开发/便携目录提示手动下载安装。使用可见进度，不强制重启 Windows。
- 安装成功由非提权启动器重新打开软件；不额外连接设备或发送命令。失败或用户取消时提示并保留 `install.log` 与配置备份，**没有承诺自动回滚程序**。成功后清除本次安装包，保留小型启动器和安装日志；不自动批量清理用户临时目录。

## 4. 验证范围

`WaveLinkUpdate` / `WaveLinkUpdateHiDpi` 测试实际 PowerShell 签名工具与 Windows CNG 验签互通、自动初始化、重复运行不覆盖、跨版本 URL 替换、当前日志提取、自定义说明兼容、密钥不匹配拒绝、版本号、篡改/过期、大小/摘要错误、重试、取消、忽略版本及两种主题。使用临时密钥、临时文件和注册表沙箱，**不会执行安装包**。系统设置 UI 测试覆盖版本入口、自动检查设置重启记忆与全局配置导入导出。

正式上线前还需在独立虚拟机实际验证：正确 HTTPS 地址/代理、UAC 同意/取消、旧版→新版覆盖安装、安装失败、备份恢复、正在录制时拒绝安装，以及所有 Qt/BLE 文件均被正确替换。
