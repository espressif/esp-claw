# ESP-Claw Web 模拟器

用于在浏览器中运行独立的 Lua display/LVGL App。页面根据 URL 中的 `app` 参数加载 `launcher.json` 和 App 文件，并根据 Lua 模块使用情况推导外设。

## 本地运行

依赖 Docker、Node.js 和 pnpm。在仓库根目录执行：

```bash
./pages/simulator/run-local.sh
```

浏览器访问：

```text
http://localhost:5173/?repo=skills-lab&ref=main&app=apps/flappybird/launcher.json
```

远程使用 VS Code 时转发 `5173` 端口。按 `Ctrl+C` 停止服务；编译缓存、运行时文件、前端依赖和 Docker 镜像会保留，以便下次增量构建。

彻底清理模拟器依赖、构建输出和 Docker 镜像：

```bash
./pages/simulator/run-local.sh --clean
```

## App 数据约定

`apps-data.json` 中每个 App 必须包含 `id`、`simulator: true` 和相对 App 目录的 `files` 数组。`VITE_SKILLS_LAB_WEB_BASE` 如需覆盖，必须指向 Skills Lab 站点根地址，例如：

```text
VITE_SKILLS_LAB_WEB_BASE=https://skills-lab.esp-claw.com
```

WASM 运行时实现和原生 Emscripten 构建方式见 [`tools/lua_lvgl_web_sim`](../../tools/lua_lvgl_web_sim/README.md)。

文件地址为 `<rawBase>/apps/<id>/<file>`，索引地址为 `<rawBase>/apps-data.json`。未开放的 App 在加载阶段报错，Skill 不使用该协议。

`launcher.json.args` 会传入 Lua 全局 `args`；`simulator/mock.json.args` 可覆盖同名顶层字段。引导脚本保留实际入口目录的 `package.path`。HTTP mock 使用 `HTTP <status>\n<body>`，页面在配置能力 mock 时标注示例数据。

## 与本地 Skills Lab 联调

Skills Lab 在 5173 启动时设置 `VITE_SIMULATOR_BASE_URL=http://localhost:5174/`，模拟器在本目录执行：

```sh
VITE_SKILLS_LAB_RAW_BASE=/lab/raw VITE_SKILLS_LAB_WEB_BASE=http://localhost:5173 pnpm dev --port 5174 --strictPort
```

Vite 会将 `/lab/raw` 代理到 `http://localhost:5173/raw`。生产环境使用两个域名时，raw 服务必须允许模拟器域名的 CORS 读取。

手动编译后执行 `pnpm copy-runtime`，默认从 `build/lua_lvgl_web_sim` 复制；可用 `SIMULATOR_BUILD_DIR` 覆盖。不同容器挂载路径请使用独立 CMake 缓存。`pnpm test` 检查参数引导脚本，`pnpm build` 检查前端；修改 C 或 shell 后还需重建 WASM。
