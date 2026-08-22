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
  execution?: {
    entry?: string
    icon?: string
    args?: Record<string, unknown>
    order?: number
    visible?: boolean
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

export interface CapabilityMocks {
  http_request?: Array<{
    method?: string
    url?: string
    url_contains?: string
    status?: number
    status_text?: string
    body?: string
    body_file?: string
  }>
}

export interface SimulatorMocks {
  capability?: CapabilityMocks
  network_radio?: {
    stations?: Array<{
      title: string
      url: string
    }>
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
  capabilityMocks: CapabilityMocks
  simulatorMocks: SimulatorMocks
}
