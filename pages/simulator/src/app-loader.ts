import { fetchAppFileList, fetchBinary, fetchText } from './repo-provider'
import { normalizeAppFilePath, parseAppManifest } from './app-parser'
import type { AppFile, CapabilityMocks, LoadedApp, SimulatorMocks, SimulatorParams } from './types'

const SIMULATOR_MOCK_FILE = 'simulator/mock.json'
// Map hardware-facing Lua modules to public peripheral names.
const MODULE_PERIPHERALS: Record<string, string> = {
  audio: 'speaker',
  button: 'button',
  camera: 'camera',
  display: 'display',
  gpio: 'gpio',
  ir: 'ir',
  lcd: 'display',
  led_strip: 'ws2812',
  lvgl: 'display',
  mcpwm: 'motor',
  touch: 'display',
}
const LUA_REQUIRE_PATTERN = /\brequire\s*(?:\(\s*)?["']([^"']+)["']/g

function dirname(path: string): string {
  const index = path.lastIndexOf('/')
  return index >= 0 ? path.slice(0, index) : ''
}

function basename(path: string): string {
  const index = path.lastIndexOf('/')
  return index >= 0 ? path.slice(index + 1) : path
}

function joinPath(root: string, file: string): string {
  return `${root}/${file}`.replace(/\/+/g, '/')
}

function decodeText(path: string, data: Uint8Array): string | undefined {
  if (!/\.(lua|md|json|txt|css|js|html)$/i.test(path)) return undefined
  return new TextDecoder().decode(data)
}

function parseSimulatorMocks(text: string): SimulatorMocks {
  const parsed = JSON.parse(text) as SimulatorMocks
  if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
    if (parsed.args !== undefined && (!parsed.args || typeof parsed.args !== 'object' || Array.isArray(parsed.args))) {
      throw new Error(`${SIMULATOR_MOCK_FILE} args must define an object`)
    }
    return parsed
  }
  throw new Error(`${SIMULATOR_MOCK_FILE} must define an object`)
}

async function loadFile(params: SimulatorParams, rootPath: string, file: string, manifestText: string): Promise<Uint8Array> {
  if (file === 'launcher.json') return new TextEncoder().encode(manifestText)
  return fetchBinary(params, joinPath(rootPath, file))
}

export function inferLuaPeripherals(files: AppFile[]): string[] {
  const peripherals = new Set<string>()
  for (const file of files) {
    if (!file.path.endsWith('.lua') || !file.text) continue
    for (const match of file.text.matchAll(LUA_REQUIRE_PATTERN)) {
      const peripheral = MODULE_PERIPHERALS[match[1]]
      if (peripheral) peripherals.add(peripheral)
    }
  }
  return Array.from(peripherals).sort()
}

export async function loadApp(params: SimulatorParams): Promise<LoadedApp> {
  const rootPath = dirname(params.app)
  const manifestText = await fetchText(params, params.app)
  const manifest = parseAppManifest(manifestText, basename(rootPath))
  const fileList = Array.from(new Set((await fetchAppFileList(params, manifest.id)).map(normalizeAppFilePath)))
  if (!fileList.includes('launcher.json')) fileList.push('launcher.json')
  if (!fileList.includes(manifest.entry)) throw new Error(`App entry not found in file list: ${manifest.entry}`)

  const files: AppFile[] = []
  for (const file of fileList) {
    const content = await loadFile(params, rootPath, file, manifestText)
    files.push({ path: file, content, text: decodeText(file, content) })
  }
  const mockText = files.find((file) => file.path === SIMULATOR_MOCK_FILE)?.text
  const simulatorMocks: SimulatorMocks = mockText ? parseSimulatorMocks(mockText) : {}
  const capabilityMocks: CapabilityMocks = simulatorMocks.capability ?? {}

  return {
    params,
    rootPath,
    manifest,
    files,
    entry: manifest.entry,
    virtualRoot: `/apps/${manifest.id}`,
    peripherals: inferLuaPeripherals(files),
    capabilityMocks,
    simulatorMocks,
  }
}
