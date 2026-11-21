export interface SimulatorParams {
  repo: string
  ref: string
  app: string
}

export interface RepoConfig {
  id: string
  label: string
  rawBase: string
  webBase: string
  kind: 'skillsLabRaw' | 'gitRaw'
}

export interface AppManifest {
  schema_version: number
  id: string
  display_name?: string
  entry: string
  icon?: string
  args?: Record<string, unknown>
  order?: number
  visible?: boolean
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
  args?: Record<string, unknown>
  capability?: CapabilityMocks
  network_radio?: {
    stations?: Array<{
      title: string
      url: string
    }>
  }
}

export interface AppFile {
  path: string
  content: Uint8Array
  text?: string
}

export interface LoadedApp {
  params: SimulatorParams
  rootPath: string
  manifest: AppManifest
  files: AppFile[]
  entry: string
  virtualRoot: string
  peripherals: string[]
  capabilityMocks: CapabilityMocks
  simulatorMocks: SimulatorMocks
}
