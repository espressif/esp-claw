import type { RepoConfig, SimulatorParams } from './types'

const REPOS: Record<string, RepoConfig> = {
  'skills-lab': {
    id: 'skills-lab',
    label: 'ESP-Claw Skills Lab',
    rawBase: import.meta.env.VITE_SKILLS_LAB_RAW_BASE || 'https://skills-lab.esp-claw.com/raw',
    webBase: import.meta.env.VITE_SKILLS_LAB_WEB_BASE || 'https://skills-lab.esp-claw.com',
    kind: 'skillsLabRaw',
  },
}

function normalizeBaseUrl(value: string): string {
  return value.replace(/\/+$/, '')
}

function getSkillsLabSiteRoot(webBase: string): string {
  const normalized = normalizeBaseUrl(webBase)
  if (/\/(?:skill|app)$/i.test(normalized)) {
    throw new Error(
      'VITE_SKILLS_LAB_WEB_BASE must be the Skills Lab site root, for example https://skills-lab.esp-claw.com, not a detail route.',
    )
  }
  return normalized
}

function buildSkillsLabAppUrl(webBase: string, appId: string): string {
  return `${getSkillsLabSiteRoot(webBase)}/zh-cn/app/${encodeURIComponent(appId)}`
}

export function getRepoConfig(id: string): RepoConfig {
  const config = REPOS[id]
  if (!config) {
    throw new Error(`unsupported repo: ${id}`)
  }
  return config
}

export function buildRawUrl(params: SimulatorParams, relativePath: string): string {
  const repo = getRepoConfig(params.repo)
  const path = relativePath.replace(/^\/+/, '')
  if (repo.kind === 'skillsLabRaw') {
    const match = path.match(/^apps\/([^/]+)\/(.+)$/)
    if (!match) {
      throw new Error(`invalid Skills Lab path: ${path}`)
    }
    return `${repo.rawBase.replace(/\/+$/, '')}/apps/${encodeURIComponent(match[1])}/${match[2]}`
  }
  return `${repo.rawBase}/${encodeURIComponent(params.ref)}/${path}`
}

export function buildWebUrl(params: SimulatorParams, relativePath: string): string {
  const repo = getRepoConfig(params.repo)
  const path = relativePath.replace(/^\/+/, '')
  if (repo.kind === 'skillsLabRaw') {
    const match = path.match(/^apps\/([^/]+)\/(.+)$/)
    const webBase = getSkillsLabSiteRoot(repo.webBase)
    return match ? buildSkillsLabAppUrl(webBase, match[1]) : webBase
  }
  return `${normalizeBaseUrl(repo.webBase)}/${encodeURIComponent(params.ref)}/${path}`
}

export async function fetchText(params: SimulatorParams, relativePath: string): Promise<string> {
  const url = buildRawUrl(params, relativePath)
  const response = await fetch(url, { cache: 'no-cache' })
  if (!response.ok) {
    throw new Error(`failed to fetch ${relativePath}: HTTP ${response.status}`)
  }
  return response.text()
}

export async function fetchBinary(params: SimulatorParams, relativePath: string): Promise<Uint8Array> {
  const url = buildRawUrl(params, relativePath)
  const response = await fetch(url, { cache: 'no-cache' })
  if (!response.ok) {
    throw new Error(`failed to fetch ${relativePath}: HTTP ${response.status}`)
  }
  return new Uint8Array(await response.arrayBuffer())
}

export async function fetchAppFileList(params: SimulatorParams, appId: string): Promise<string[]> {
  const repo = getRepoConfig(params.repo)
  const response = await fetch(`${normalizeBaseUrl(repo.rawBase)}/apps-data.json`, { cache: 'no-cache' })
  if (!response.ok) throw new Error(`failed to fetch App index: HTTP ${response.status}`)

  const apps = await response.json() as unknown
  if (!Array.isArray(apps)) throw new Error('App index must define an array')
  const app = apps.find((item): item is { id: string; files: string[]; simulator?: boolean } => Boolean(item && typeof item === 'object' && (item as { id?: unknown }).id === appId))
  if (!app || !Array.isArray(app.files) || app.files.some((file) => typeof file !== 'string')) {
    throw new Error(`App file list not found: ${appId}`)
  }
  if (app.simulator !== true) throw new Error(`App ${appId} is not enabled for browser simulation`)
  return app.files
}
