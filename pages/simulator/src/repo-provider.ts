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
  if (/\/skill$/i.test(normalized)) {
    throw new Error(
      'VITE_SKILLS_LAB_WEB_BASE must be the Skills Lab site root, for example https://skills-lab.esp-claw.com, not a /skill route.',
    )
  }
  return normalized
}

function buildSkillsLabSkillUrl(webBase: string, skillId: string): string {
  return `${getSkillsLabSiteRoot(webBase)}/skill/${encodeURIComponent(skillId)}`
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
    const match = path.match(/^skills\/([^/]+)\/(.+)$/)
    if (!match) {
      throw new Error(`invalid Skills Lab path: ${path}`)
    }
    return `${repo.rawBase.replace(/\/+$/, '')}/${encodeURIComponent(match[1])}/${match[2]}`
  }
  return `${repo.rawBase}/${encodeURIComponent(params.ref)}/${path}`
}

export function buildWebUrl(params: SimulatorParams, relativePath: string): string {
  const repo = getRepoConfig(params.repo)
  const path = relativePath.replace(/^\/+/, '')
  if (repo.kind === 'skillsLabRaw') {
    const match = path.match(/^skills\/([^/]+)\/(.+)$/)
    const webBase = getSkillsLabSiteRoot(repo.webBase)
    return match ? buildSkillsLabSkillUrl(webBase, match[1]) : webBase
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
