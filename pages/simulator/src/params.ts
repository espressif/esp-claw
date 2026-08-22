import type { SimulatorParams } from './types'

const DEFAULT_REPO = 'skills-lab'
const DEFAULT_REF = 'main'

function normalizeSkillPath(value: string): string {
  const path = value.trim().replaceAll('\\', '/').replace(/^\/+/, '')
  if (!path.endsWith('/SKILL.md')) {
    throw new Error('skill must point to a SKILL.md file')
  }
  if (path.includes('..')) {
    throw new Error('skill path must not contain ..')
  }
  return path
}

export function readSimulatorParams(search = window.location.search): SimulatorParams {
  const params = new URLSearchParams(search)
  const repo = params.get('repo')?.trim() || DEFAULT_REPO
  const ref = params.get('ref')?.trim() || DEFAULT_REF
  const skillParam = params.get('skill')?.trim()

  if (!skillParam) {
    throw new Error('missing URL parameter: skill')
  }

  return {
    repo,
    ref,
    skill: normalizeSkillPath(skillParam),
  }
}
