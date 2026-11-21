import type { LoadedApp } from './types'

function luaValue(value: unknown): string {
  if (value === null) return 'nil'
  if (typeof value === 'string') {
    return '"' + Array.from(new TextEncoder().encode(value), (byte) => `\\${String(byte).padStart(3, '0')}`).join('') + '"'
  }
  if (typeof value === 'boolean') return String(value)
  if (typeof value === 'number' && Number.isFinite(value)) return String(value)
  if (Array.isArray(value)) return `{${value.map((item, index) => `[${index + 1}]=${luaValue(item)}`).join(',')}}`
  if (value && typeof value === 'object') {
    return `{${Object.entries(value).map(([key, item]) => `[${luaValue(key)}]=${luaValue(item)}`).join(',')}}`
  }
  throw new Error('App arguments must be JSON values')
}

export function buildAppBootstrap(app: LoadedApp): string {
  const entry = `${app.virtualRoot}/${app.entry}`
  const dir = entry.slice(0, entry.lastIndexOf('/'))
  const args = { ...app.manifest.args, ...app.simulatorMocks.args }
  return `args=${luaValue(args)}\npackage.path=${luaValue(`${dir}/?.lua;${dir}/?/init.lua;`)}..package.path\nreturn dofile(${luaValue(entry)})\n`
}
