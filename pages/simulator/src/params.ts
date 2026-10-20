import type { SimulatorParams } from './types'

const DEFAULT_REPO = 'skills-lab'
const DEFAULT_REF = 'main'

function normalizeAppPath(value: string): string {
  const path = value.trim()
  if (path.startsWith('/') || path.includes('\\') || path.split('/').some((segment) => !segment)) {
    throw new Error('app path must be repository-relative')
  }
  if (!path.endsWith('/launcher.json')) {
    throw new Error('app must point to a launcher.json file')
  }
  if (path.includes('..')) {
    throw new Error('app path must not contain ..')
  }
  return path
}

export function readSimulatorParams(search = window.location.search): SimulatorParams {
  const params = new URLSearchParams(search)
  const repo = params.get('repo')?.trim() || DEFAULT_REPO
  const ref = params.get('ref')?.trim() || DEFAULT_REF
  const appParam = params.get('app')?.trim()

  if (!appParam) {
    throw new Error('missing URL parameter: app')
  }

  return {
    repo,
    ref,
    app: normalizeAppPath(appParam),
  }
}
