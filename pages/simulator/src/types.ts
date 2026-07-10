export interface SimulatorParams {
  repo: string
  ref: string
  skill: string
}

export interface RepoConfig {
  id: string
  label: string
  rawBase: string
  webBase: string
  kind: 'skillsLabRaw' | 'gitRaw'
}

export interface SkillFrontmatter {
  name: string
  description: string
  author?: string
  metadata: {
    category?: string[]
    peripherals?: string[]
    tags?: string[]
    cap_groups?: string[]
    manage_mode?: string
  }
  simulator?: {
    type?: string
    entry?: string
    files?: string[]
    width?: number
    height?: number
    touch?: boolean
    audio?: string
  }
}

export interface SkillFile {
  path: string
  content: Uint8Array
  text?: string
}

export interface LoadedSkill {
  params: SimulatorParams
  rootPath: string
  frontmatter: SkillFrontmatter
  markdownBody: string
  files: SkillFile[]
  entry: string
  virtualRoot: string
}
