import type { AppManifest } from './types'

export function normalizeAppFilePath(path: string): string {
  if (typeof path !== 'string') throw new Error('App file path must be a string')
  const normalized = path.trim()
  if (!normalized || normalized.startsWith('/') || normalized.includes('..') || normalized.includes('\\') || normalized.split('/').some((segment) => !segment)) {
    throw new Error(`invalid App file path: ${path}`)
  }
  return normalized
}

export function parseAppManifest(text: string, directoryId: string): AppManifest {
  const manifest = JSON.parse(text) as AppManifest
  if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) {
    throw new Error('launcher.json must define an object')
  }
  if (manifest.schema_version !== 1) {
    throw new Error('launcher.json schema_version must be 1')
  }
  if (!/^[A-Za-z0-9_-]{1,63}$/.test(manifest.id) || manifest.id !== directoryId) {
    throw new Error('App id must match its directory name')
  }
  if (manifest.display_name !== undefined && (typeof manifest.display_name !== 'string' || !manifest.display_name)) {
    throw new Error('display_name must be a non-empty string')
  }
  manifest.entry = normalizeAppFilePath(manifest.entry)
  if (!manifest.entry.endsWith('.lua')) {
    throw new Error('App entry must be a .lua file')
  }
  if (manifest.icon !== undefined) {
    manifest.icon = normalizeAppFilePath(manifest.icon)
    if (!manifest.icon.endsWith('.jpg') && !manifest.icon.endsWith('.jpeg')) {
      throw new Error('App icon must be a .jpg or .jpeg file')
    }
  }
  if (manifest.args !== undefined && (!manifest.args || typeof manifest.args !== 'object' || Array.isArray(manifest.args))) {
    throw new Error('args must be an object')
  }
  if (manifest.order !== undefined && !Number.isInteger(manifest.order)) throw new Error('order must be an integer')
  if (manifest.visible !== undefined && typeof manifest.visible !== 'boolean') throw new Error('visible must be a boolean')
  return manifest
}
