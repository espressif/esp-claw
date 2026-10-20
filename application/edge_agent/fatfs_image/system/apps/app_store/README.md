# App Store

使用 Lua LVGL 在设备上浏览、安装和卸载 Apps。需要触摸屏、网络、可写 DATA 分区，以及 `lvgl`、`image`、`thread`、`storage`、`json`、`capability`、`delay` 模块。当前网页模拟器不支持后台网络安装，因此不提供在线试玩。

默认连接 `https://skills-lab.esp-claw.com`。本地调试时可在 `launcher.json` 的 `args` 中设置 `{"server":"http://<电脑IP>:5173"}`。设备的 HTTP allowlist 必须允许该服务器；设备无法使用电脑的 `localhost` 地址。

首页可切换全部、游戏、工具、已安装，点击卡片查看详情。安装成功后返回 Launcher 启动应用。卸载需要再次确认；内置应用和商店自身不可卸载。首版不覆盖安装、不提供更新或直接启动其他 App。

主脚本只管理界面；后台任务读取 `/raw/apps-data.json`，安装时读取 `/raw/apps/<id>/_metadata.json`，严格按 `files` 下载。文件先保存到 DATA 下的 `cache/app_store/staging/`，完整校验后移入 `apps/<id>/`，最后调用 `publish_app`。失败会清理本次下载；目录和图标缓存允许离线浏览。退出时会取消后台任务，当前 HTTP 请求可能需要等待最多 20 秒。

`已安装` 同时读取 DATA 和 SYSTEM 中的 Launcher 清单。目录中的外设需求仅供用户判断，不代表设备一定具有对应硬件。大小是目录估算值，下载仍可能因存储不足失败。

开发校验：`lua tests/app_store.lua` 覆盖安装回滚、路径检查、取消、重复安装、发布失败和卸载保护。该测试使用主机端模块替身，不代替真机验证。
