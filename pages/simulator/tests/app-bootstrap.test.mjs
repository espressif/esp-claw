import assert from 'node:assert/strict'
import test from 'node:test'
import { buildAppBootstrap } from '../src/app-bootstrap.ts'

test('bootstrap preserves entry search path and encodes parameters as Lua data', () => {
  const app = { virtualRoot: '/apps/demo', entry: 'scripts/main.lua', manifest: { args: { text: '";error("injected")--\n中文', count: 1 } }, simulatorMocks: { args: { count: 2, nested: [true, null, { key: 'value' }] } } }
  const source = buildAppBootstrap(app)
  assert.ok(!source.includes('error("injected")'))
  assert.ok(source.includes('package.path='))
  assert.ok(source.includes('[1]=true,[2]=nil,[3]='))
  assert.ok(source.includes('["\\099\\111\\117\\110\\116"]=2'))
  assert.throws(() => buildAppBootstrap({ ...app, simulatorMocks: { args: { bad: Infinity } } }), /JSON values/)
})
