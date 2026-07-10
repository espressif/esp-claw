import { fetchBinary, fetchText } from './repo-provider'
import { assertSimulatorSupported, getSimulatorFiles, inferEntry, splitSkillMarkdown } from './skill-parser'
import type { LoadedSkill, SimulatorParams, SkillFile } from './types'

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

export async function loadSkill(params: SimulatorParams): Promise<LoadedSkill> {
  const rootPath = dirname(params.skill)
  const rawSkillMd = await fetchText(params, params.skill)
  const { frontmatter, body } = splitSkillMarkdown(rawSkillMd)
  assertSimulatorSupported(frontmatter)

  const fileList = getSimulatorFiles(frontmatter, body)
  const entry = inferEntry(frontmatter, body, fileList)
  if (!fileList.includes(entry)) {
    fileList.unshift(entry)
  }
  if (!fileList.includes('SKILL.md')) {
    fileList.unshift('SKILL.md')
  }

  const files: SkillFile[] = []
  for (const file of fileList) {
    const relative = file === 'SKILL.md' ? params.skill : joinPath(rootPath, file)
    const content = file === 'SKILL.md' ? new TextEncoder().encode(rawSkillMd) : await fetchBinary(params, relative)
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
  }
}
