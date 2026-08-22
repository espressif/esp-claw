import type { SkillFrontmatter } from './types'

export interface ParsedSkillMd {
  frontmatter: SkillFrontmatter
  body: string
}

export function splitSkillMarkdown(raw: string): ParsedSkillMd {
  const source = raw.charCodeAt(0) === 0xfeff ? raw.slice(1) : raw
  if (!source.startsWith('---')) {
    throw new Error('SKILL.md must start with JSON frontmatter')
  }

  const firstLineEnd = source.indexOf('\n')
  const close = source.indexOf('\n---', firstLineEnd + 1)
  if (close < 0) {
    throw new Error('SKILL.md frontmatter is not closed')
  }

  const json = source.slice(firstLineEnd + 1, close).trim()
  const body = source.slice(close + 4).replace(/^\r?\n/, '')
  const frontmatter = JSON.parse(json) as SkillFrontmatter
  return { frontmatter, body }
}

export function parseFilesSection(body: string): string[] {
  const lines = body.split(/\r?\n/)
  const files: string[] = []
  let inFiles = false

  for (const line of lines) {
    if (/^##\s+Files\s*$/i.test(line.trim())) {
      inFiles = true
      continue
    }
    if (inFiles && /^##\s+/.test(line)) break
    if (!inFiles) continue

    const match = line.match(/^\s*[-*]\s+(?:`([^`]+)`|([^\s#]+))/)
    const value = (match?.[1] ?? match?.[2] ?? '').trim()
    if (value) {
      files.push(normalizeSkillFilePath(value))
    }
  }

  return Array.from(new Set(files))
}

function normalizeSkillFilePath(path: string): string {
  const normalized = path.trim().replaceAll('\\', '/')
  const segments = normalized.split('/')
  if (!normalized || normalized.startsWith('/') || segments.some((segment) => !segment || segment === '..')) {
    throw new Error(`invalid simulator file path: ${path}`)
  }
  return normalized
}

function normalizeEntryPath(path: string): string {
  const normalized = normalizeSkillFilePath(path)
  if (!normalized.endsWith('.lua')) {
    throw new Error(`invalid simulator entry path: ${path}`)
  }
  return normalized
}

export function getSimulatorFiles(frontmatter: SkillFrontmatter, body: string): string[] {
  const configured = frontmatter.simulator?.files
  if (configured !== undefined) {
    if (!Array.isArray(configured)) {
      throw new Error('simulator.files must be an array of skill-relative paths')
    }
    return Array.from(new Set(configured.map(normalizeSkillFilePath)))
  }
  return parseFilesSection(body)
}

export function inferEntry(frontmatter: SkillFrontmatter, body: string, files: string[]): string {
  const configured = frontmatter.execution?.entry ?? frontmatter.simulator?.entry
  if (configured) return normalizeEntryPath(configured)

  const jsonBlocks = body.matchAll(/```json\s*([\s\S]*?)```/g)
  for (const block of jsonBlocks) {
    try {
      const value = JSON.parse(block[1] ?? '{}') as { path?: string }
      if (typeof value.path === 'string' && value.path.includes('/scripts/')) {
        return normalizeEntryPath(value.path
          .replace('{CUR_SKILL_DIR}/', '')
          .replace('{CUR_SKILL_DIR}', ''))
      }
    } catch {
      // Ignore non-object examples; SKILL.md examples are user-facing prose.
    }
  }

  const luaFiles = files.filter((file) => file.startsWith('scripts/') && file.endsWith('.lua'))
  if (luaFiles.length === 1) return luaFiles[0]
  if (luaFiles.includes(`scripts/${frontmatter.name}.lua`)) return `scripts/${frontmatter.name}.lua`

  throw new Error('unable to infer simulator entry script')
}

export function assertSimulatorSupported(frontmatter: SkillFrontmatter): void {
  const entry = frontmatter.execution?.entry ?? frontmatter.simulator?.entry
  if (!entry) {
    throw new Error('simulator-capable skills must define execution.entry or legacy simulator.entry')
  }
  normalizeEntryPath(entry)
}
