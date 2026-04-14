# github-releases-autoupdater

这个目录里的代码现在被 CandyLauncher 当作“更新检查 + 更新包下载 + 安装启动器”使用。

## 当前职责

- 访问 GitHub Releases API。
- 解析 release 列表，找出比当前版本新的版本。
- 从 release assets 里选择合适的更新包下载地址。
- 下载更新包到系统临时目录。
- 启动安装流程。

## CandyLauncher 当前接入方式

- 仓库名固定为 `CandyTek/CandyLauncher`。
- 当前版本字符串由 CandyLauncher 主程序提供。
- 主程序侧会在启动后按配置自动检查更新，也支持手动检查更新。
- 这个第三方库只负责：
  - 查 GitHub release
  - 挑选更新资产
  - 下载更新包
  - 拉起安装流程

## 目前约定的发布格式

CandyLauncher 现在的 latest release 不是安装器，而是便携包：

- tag 示例：`1.1`
- asset 示例：`CandyLauncher.zip`

因此这里的实现已经不是“只支持 exe 安装器”，而是：

1. 优先选择 `.zip` 资产。
2. 如果没有 `.zip`，再考虑 `.exe` 或 `.msi`。
3. 不再回退到 release HTML 页面本身，避免把网页当安装包下载。

## zip 更新流程

当下载的是 `CandyLauncher.zip` 时，当前实现按“便携软件自更新”处理：

1. 下载 zip 到临时目录。
2. 生成一个临时 PowerShell 脚本。
3. 由脚本等待当前 `CandyLauncher.exe` 退出。
4. 解压 zip 到临时目录。
5. 用 `robocopy` 把解压后的文件覆盖到当前程序目录。
6. 重新启动新的 `CandyLauncher.exe`。

主程序在收到“更新包已启动”的通知后会退出，以便释放自身文件占用。

## exe/msi 更新流程

如果将来 release 提供的是安装器：

- `.exe` / `.msi` 仍然可以继续使用。
- 当前逻辑会直接 `ShellExecuteW` 启动安装器。

## 版本比较理解

这个库内部用“自然排序”比较版本字符串，适合：

- `1.0`
- `1.1`
- `1.10`
- `1.1.2`

但 CandyLauncher 当前主程序里显示的版本字符串可能类似：

- `1.1.20260414`

如果 GitHub tag 只是 `1.1`，那么库会认为本地版本更新，不会提示升级。

所以发布时要注意：

- GitHub tag 必须大于主程序传入的当前版本串。
- 最好统一版本策略，不要一边用日期版本，一边只发短 tag。

## 修改这里时的注意事项

- 这是第三方目录，但在 CandyLauncher 当前工程里已经被直接改造过，不要按“原版上游行为”做假设。
- 如果再改 asset 选择规则，要优先考虑 CandyLauncher 的真实发布方式。
- 如果再改 zip 安装逻辑，重点关注：
  - 当前进程退出时机
  - 文件占用
  - 覆盖目录是否正确
  - 重启路径是否正确
  - 临时脚本是否会被安全软件拦截

## 当前已知限制

- zip 自更新依赖 PowerShell `Expand-Archive` 和 `robocopy`。
- 如果用户把程序放在受保护目录，覆盖写入可能失败。
- 如果 release zip 内部目录结构变化，覆盖逻辑可能需要同步调整。
- 如果后续想降低脚本依赖，可以单独增加一个 `Updater.exe`，把脚本逻辑迁到原生程序里。

## 推荐后续方向

如果 CandyLauncher 长期保持便携版发布，推荐加一个独立的 `Updater.exe`：

- 由主程序下载 zip 后启动 `Updater.exe`
- `Updater.exe` 负责等待主程序退出、解压、替换、重启

这样会比 PowerShell 脚本更稳定，也更容易处理：

- 权限提升
- 失败回滚
- 更新日志展示
- 用户提示
- 安全软件兼容性
