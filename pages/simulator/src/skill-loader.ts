import { buildRawUrl, fetchBinary, fetchText } from './repo-provider'
import { assertSimulatorSupported, getSimulatorFiles, inferEntry, splitSkillMarkdown } from './skill-parser'
import type { CapabilityMocks, LoadedSkill, SimulatorMocks, SimulatorParams, SkillFile } from './types'

const SIMULATOR_MOCK_FILE = 'simulator/mock.json'

function dirname(path: string): string {
  const index = path.lastIndexOf('/')
  return index >= 0 ? path.slice(0, index) : ''
}

function joinPath(root: string, file: string): string {
  return `${root}/${file}`.replace(/\/+/g, '/')
}

function decodeText(path: string, data: Uint8Array): string | undefined {
  if (!/\.(lua|md|json|txt|css|js|html)$/i.test(path)) return undefined
  return new TextDecoder().decode(data)
}

function normalizeSkillFilePath(path: string): string {
  const normalized = path.trim().replaceAll('\\', '/')
  const segments = normalized.split('/')
  if (!normalized || normalized.startsWith('/') || segments.some((segment) => !segment || segment === '..')) {
    throw new Error(`invalid simulator mock file path: ${path}`)
  }
  return normalized
}

async function fetchOptionalBinary(params: SimulatorParams, relativePath: string): Promise<Uint8Array | undefined> {
  const response = await fetch(buildRawUrl(params, relativePath), { cache: 'no-cache' })
  if (response.status === 404 || response.headers.get('content-type')?.includes('text/html')) return undefined
  if (!response.ok) {
    throw new Error(`failed to fetch ${relativePath}: HTTP ${response.status}`)
  }
  return new Uint8Array(await response.arrayBuffer())
}

function parseSimulatorMocks(text: string, path: string): SimulatorMocks {
  const parsed = JSON.parse(text) as SimulatorMocks
  if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
    return parsed
  }
  throw new Error(`${path} must define a top-level object`)
}

function collectBodyFiles(value: unknown, files = new Set<string>()): Set<string> {
  if (Array.isArray(value)) {
    value.forEach((item) => collectBodyFiles(item, files))
  } else if (value && typeof value === 'object') {
    for (const [key, item] of Object.entries(value)) {
      if (key === 'body_file' && typeof item === 'string') {
        files.add(normalizeSkillFilePath(item))
      } else {
        collectBodyFiles(item, files)
      }
    }
  }
  return files
}

function addUniqueFile(fileList: string[], file: string): void {
  if (!fileList.includes(file)) {
    fileList.push(file)
  }
}

export async function loadSkill(params: SimulatorParams): Promise<LoadedSkill> {
  const rootPath = dirname(params.skill)
  const rawSkillMd = await fetchText(params, params.skill)
  const { frontmatter, body } = splitSkillMarkdown(rawSkillMd)
  assertSimulatorSupported(frontmatter)

  const fileList = getSimulatorFiles(frontmatter, body)
  const entry = inferEntry(frontmatter, body, fileList)
  if (!fileList.includes(entry)) fileList.unshift(entry)
  if (frontmatter.execution?.icon) addUniqueFile(fileList, normalizeSkillFilePath(frontmatter.execution.icon))

  let capabilityMocks: CapabilityMocks = {}
  let simulatorMocks: SimulatorMocks = {}
  const mockFilePath = joinPath(rootPath, SIMULATOR_MOCK_FILE)
  const mockFileContent = await fetchOptionalBinary(params, mockFilePath)
  if (mockFileContent) {
    const text = decodeText(SIMULATOR_MOCK_FILE, mockFileContent) ?? ''
    simulatorMocks = parseSimulatorMocks(text, SIMULATOR_MOCK_FILE)
    capabilityMocks = simulatorMocks.capability ?? {}
    addUniqueFile(fileList, SIMULATOR_MOCK_FILE)
    for (const file of collectBodyFiles(capabilityMocks)) {
      addUniqueFile(fileList, file)
    }
  }

  if (!fileList.includes('SKILL.md')) fileList.unshift('SKILL.md')

  const files: SkillFile[] = []
  for (const file of fileList) {
    const relative = file === 'SKILL.md' ? params.skill : joinPath(rootPath, file)
    const content =
      file === 'SKILL.md'
        ? new TextEncoder().encode(rawSkillMd)
        : file === SIMULATOR_MOCK_FILE && mockFileContent
          ? mockFileContent
          : await fetchBinary(params, relative)
    files.push({
      path: file,
      content,
      text: decodeText(file, content),
    })
  }

  const virtualRoot = `/skills/${frontmatter.name}`

  return {
    params,
    rootPath,
    frontmatter,
    markdownBody: body,
    files,
    entry,
    virtualRoot,
    capabilityMocks,
    simulatorMocks,
  }
}
