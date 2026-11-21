import assert from 'node:assert/strict'
import test from 'node:test'
import { createSimAudio } from '../../../tools/lua_lvgl_web_sim/sim/audio.js'

class Node {
  gain = { value: 1, setValueAtTime() {}, linearRampToValueAtTime() {} }
  playbackRate = { value: 1 }
  connect() {}
  disconnect() { this.disconnected = true }
  start(at) { this.at = at }
  stop(at) { this.until = at; this.stopped = true }
}
globalThis.AudioContext = class {
  currentTime = 0
  destination = {}
  createGain() { return new Node() }
  createBufferSource() { return new Node() }
  createBuffer(channels, frames, rate) { return {duration:frames/rate, getChannelData:()=>new Float32Array(frames)} }
  resume() { return Promise.resolve() }
}
globalThis.Audio = class {
  play() { return new Promise((resolve,reject) => { this.resolve=resolve; this.reject=reject }) }
  pause() {}
  load() {}
  removeAttribute() {}
}
const setup = () => createSimAudio(() => ({FS:{readFile:()=>new Uint8Array([1,2])}}))
const pcm = new Uint8Array(3200)

test('output ownership survives rejected playback and requires closing consumers first', () => {
  const s=setup(), out=s.call('openOutput',[]), sampler=s.call('createSampler',[out,{}]), player=s.createPlayer(out)
  s.call('load',[sampler,1],pcm); s.call('start',[sampler])
  assert.match(s.play(player,'/sound.aac'),/busy/)
  s.closePlayer(player)
  assert.throws(()=>s.call('write',[out],pcm),/busy/)
  assert.throws(()=>s.closeOutput(out),/busy/)
  s.call('clear',[sampler]); assert.throws(()=>s.call('write',[out],pcm),/busy/)
  s.call('closeSampler',[sampler]); assert.equal(s.call('write',[out],pcm),3200)
  s.closeOutput(out); s.closeOutput(out); assert.equal(s.outputs.size,0)
})
test('sample budget is atomic and stale replaced handles cannot stop new voices', () => {
  const s=setup(), out=s.call('openOutput',[]), sampler=s.call('createSampler',[out,{max_voices:1,sample_budget_bytes:3200}])
  s.call('load',[sampler,7],pcm)
  assert.throws(()=>s.call('load',[sampler,8],pcm),/budget/)
  assert.equal(s.call('sampleStatus',[sampler]).sample_bytes,3200)
  s.call('start',[sampler]); const first=s.call('trigger',[sampler,7,0.5,100,60,2])
  const old=s.samplers.get(sampler).voices.get(first)
  assert.equal(old.source.playbackRate.value,2); assert.equal(old.source.at,0.1); assert.ok(Math.abs(old.source.until-0.15)<1e-9)
  const second=s.call('trigger',[sampler,7]); assert.equal(old.source.stopped,true)
  s.call('stopVoice',[sampler,first]); assert.equal(s.samplers.get(sampler).voices.has(second),true)
  s.call('stopVoice',[sampler,second]); assert.equal(s.call('sampleStatus',[sampler]).active_voices,0)
  s.reset(); assert.equal(s.samplers.size,0); assert.equal(s.outputs.size,0)
})
test('local file playback uses mounted data and ignores rejection from a stopped request', async () => {
  const s=setup(), out=s.createOutput(), player=s.createPlayer(out)
  assert.equal(s.play(player,'/sound.aac'),''); const first=s.players.get(player).audio
  assert.match(first.src,/^blob:/); assert.equal(s.poll(player).running,true)
  s.call('setMute',[out,true]); s.setVolume(out,37)
  assert.equal(first.muted,true); assert.equal(s.outputs.get(out).gain.gain.value,0)
  s.call('setMute',[out,false]); assert.equal(first.muted,false); assert.equal(s.outputs.get(out).gain.gain.value,0.37)
  s.stop(player); assert.equal(s.play(player,'/next.aac'),'')
  first.reject(Error('obsolete request')); await Promise.resolve()
  assert.equal(s.poll(player).state,'playing'); s.reset()
  assert.equal(s.players.size,0); assert.equal(s.outputs.size,0)
})
test('invalid PCM and rates cannot consume the budget or evict an active voice', () => {
  const s=setup(), out=s.createOutput(), id=s.call('createSampler',[out,{max_voices:1}])
  assert.throws(()=>s.call('load',[id,1],new Uint8Array(3)),/PCM/)
  s.call('load',[id,1],pcm); s.call('start',[id]); const voice=s.call('trigger',[id,1])
  assert.throws(()=>s.call('trigger',[id,1,1,0,0,Infinity]),/invalid/)
  assert.equal(s.samplers.get(id).voices.has(voice),true); s.reset()
})
