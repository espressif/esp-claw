import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const projectDir = path.resolve(here, '..')
const repoRoot = path.resolve(projectDir, '..', '..')
const sourceDir = process.env.SIMULATOR_BUILD_DIR
  ? path.resolve(process.env.SIMULATOR_BUILD_DIR)
  : path.join(repoRoot, 'build', 'lua_lvgl_web_sim_test')
const targetDir = path.join(projectDir, 'public', 'runtime')
const files = ['esp_claw_sim.html', 'esp_claw_sim.js', 'esp_claw_sim.wasm', 'esp_claw_sim.data']

fs.mkdirSync(targetDir, { recursive: true })

for (const file of files) {
  const source = path.join(sourceDir, file)
  if (!fs.existsSync(source)) {
    if (file.endsWith('.data')) continue
    throw new Error(`missing runtime artifact: ${source}`)
  }
  fs.copyFileSync(source, path.join(targetDir, file))
  console.log(`copied ${file}`)
}
