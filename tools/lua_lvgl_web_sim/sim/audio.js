/* Web Audio playback uses the same Lua-facing ownership rules as the device. */
function createSimAudio(getModule, log = () => {}) {
  const outputs = new Map(), players = new Map(), samplers = new Map();
  let nextId = 1, context, volume = 70;
  const finite = (value, min, max, integer = false) => {
    if (!Number.isFinite(value) || value < min || value > max || (integer && !Number.isInteger(value))) throw Error('invalid audio argument');
    return value;
  };
  const get = (map, id) => { const value = map.get(id); if (!value) throw Error('audio object closed'); return value; };
  const ctx = () => context ||= new (globalThis.AudioContext || globalThis.webkitAudioContext)();
  const unlock = () => { try { ctx().resume()?.catch(error => log(error.message)); } catch (error) { log(error.message); } };
  function output(id) { return get(outputs, id); }
  function claim(out, owner) {
    if (out.sources.size || (out.owner && out.owner !== owner)) throw Error('audio output: busy');
    out.owner = owner;
  }
  function release(out, owner) { if (out.owner === owner) out.owner = null; }
  function createOutput() {
    if (outputs.size) throw Error('audio output: busy');
    const id = nextId++, gain = ctx().createGain();
    gain.gain.value = volume / 100; gain.connect(ctx().destination);
    outputs.set(id, {id, gain, owner: null, muted: false, nextTime: 0, sources: new Set()});
    return id;
  }
  function makeBuffer(bytes, rate) {
    if (!bytes || !bytes.length || bytes.length % 2) throw Error('PCM must contain complete 16-bit mono frames');
    const buffer = ctx().createBuffer(1, bytes.length / 2, rate), data = buffer.getChannelData(0);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    for (let i = 0; i < data.length; i++) data[i] = view.getInt16(i * 2, true) / 32768;
    return buffer;
  }
  function stopSource(voice) {
    voice.source.onended = null;
    voice.source.stop(); voice.source.disconnect(); voice.gain?.disconnect();
  }
  function write(id, bytes) {
    const out = output(id);
    if (out.owner) throw Error('audio output: busy');
    if (out.nextTime - ctx().currentTime > 2) throw Error('audio output queue full');
    const source = ctx().createBufferSource(); source.buffer = makeBuffer(bytes, 16000);
    source.connect(out.gain); out.sources.add(source);
    source.onended = () => { out.sources.delete(source); source.disconnect(); };
    const at = Math.max(ctx().currentTime, out.nextTime);
    source.start(at); out.nextTime = at + source.buffer.duration;
    return bytes.length;
  }
  function setVolume(id, value) {
    const out = output(id); volume = finite(value, 0, 100, true);
    out.gain.gain.value = out.muted ? 0 : volume / 100;
    for (const player of players.values()) if (player.outputId === id && player.audio) player.audio.volume = volume / 100;
    return true;
  }
  function createPlayer(id) {
    output(id); const player = {id: nextId++, outputId: id, state: 'idle', error: '', audio: null, blob: null};
    players.set(player.id, player); return player.id;
  }
  function stop(id) {
    const player = get(players, id);
    if (player.audio) {
      const audio = player.audio; player.audio = null;
      audio.pause(); audio.removeAttribute('src'); audio.load();
    }
    if (player.blob) URL.revokeObjectURL(player.blob);
    player.blob = null; player.state = 'stopped';
    release(output(player.outputId), player);
    return true;
  }
  function play(id, path) {
    const player = get(players, id), out = output(player.outputId);
    if (typeof path !== 'string' || !path) throw Error('audio path is required');
    if (out.owner && out.owner !== player) throw Error('audio output: busy');
    stop(id); claim(out, player);
    try {
      const audio = new Audio();
      player.audio = audio; player.state = 'playing'; player.error = ''; player.url = path;
      if (/^https?:\/\//.test(path)) {
        const direct = new URLSearchParams(globalThis.location?.search).get('stream') === 'direct';
        audio.src = direct ? path : 'http://127.0.0.1:8092/stream?url=' + encodeURIComponent(path);
      }
      else {
        const file = path.replace(/^file:\/\//, '');
        const bytes = getModule().FS.readFile(file);
        const type = /\.aac$/i.test(file) ? 'audio/aac' : /\.wav$/i.test(file) ? 'audio/wav' : 'audio/mpeg';
        player.blob = URL.createObjectURL(new Blob([bytes], {type})); audio.src = player.blob;
      }
      audio.volume = volume / 100; audio.muted = out.muted;
      const failed = message => {
        if (player.audio !== audio) return;
        stop(id); player.error = message; player.state = 'error'; log(message);
      };
      audio.onended = () => { if (player.audio === audio) { stop(id); player.state = 'finished'; } };
      audio.onerror = () => failed('media error ' + (audio.error?.code || 'unknown'));
      audio.play().catch(error => failed(error.message));
      return true;
    } catch (error) { stop(id); player.state = 'error'; player.error = error.message; throw error; }
  }
  function poll(id) {
    const player = get(players, id);
    return {state: player.state, running: ['playing', 'paused'].includes(player.state), error: player.error, url: player.url || ''};
  }
  function closePlayer(id) { if (players.has(id)) { stop(id); players.delete(id); } return true; }
  function createSampler(id, opts = {}) {
    output(id);
    const config = {
      rate: finite(opts.source_rate ?? 16000, 8000, 48000, true),
      max: finite(opts.max_voices ?? 6, 1, 16, true),
      budget: finite(opts.sample_budget_bytes ?? 2097152, 2, 4294967295, true),
      attack: finite(opts.attack_ms ?? 0, 0, 1000, true) / 1000,
      release: finite(opts.release_ms ?? 0, 0, 1000, true) / 1000,
      limit: finite(opts.limit_threshold ?? 0, 0, 1 - Number.EPSILON),
    };
    const player = {id: nextId++, outputId: id, config, samples: new Map(), voices: new Map(), bytes: 0, started: false};
    samplers.set(player.id, player); return player.id;
  }
  function load(id, sampleId, bytes) {
    const player = get(samplers, id); finite(sampleId, -2147483648, 2147483647, true);
    if (player.started || player.samples.has(sampleId) || player.samples.size >= 64) throw Error('sample cannot be loaded');
    if (!bytes || player.bytes + bytes.length > player.config.budget) throw Error('sample budget exceeded');
    const buffer = makeBuffer(bytes, player.config.rate);
    player.samples.set(sampleId, buffer); player.bytes += bytes.length; return true;
  }
  function start(id) {
    const player = get(samplers, id), out = output(player.outputId);
    if (player.started) throw Error('sample player already started');
    claim(out, player);
    const gain = ctx().createGain(); gain.connect(out.gain); player.bus = gain;
    if (player.config.limit) {
      const limiter = ctx().createDynamicsCompressor();
      limiter.threshold.value = 20 * Math.log10(player.config.limit);
      limiter.knee.value = 0; limiter.ratio.value = 20;
      limiter.attack.value = 0; limiter.release.value = 0.05;
      limiter.connect(gain); player.limiter = limiter;
    }
    player.started = true; return true;
  }
  function stopVoice(id, voiceId) {
    const player = get(samplers, id), voice = player.voices.get(voiceId);
    if (voice) { player.voices.delete(voiceId); stopSource(voice); }
    return true;
  }
  function clear(id) { const player = get(samplers, id); for (const key of [...player.voices.keys()]) stopVoice(id, key); return true; }
  function trigger(id, sampleId, level = 1, delay = 0, duration = 0, rate = 1) {
    const player = get(samplers, id), buffer = player.samples.get(sampleId);
    if (!player.started || !buffer) throw Error('sample player is not ready');
    finite(level, 0, 1); finite(delay, 0, 1000, true); finite(duration, 0, 60000, true); finite(rate, 0.25, 4);
    if (player.voices.size >= player.config.max) stopVoice(id, player.voices.keys().next().value);
    const source = ctx().createBufferSource(), gain = ctx().createGain(), voiceId = nextId++;
    const at = ctx().currentTime + delay / 1000, length = Math.min(buffer.duration / rate, duration ? duration / 1000 : Infinity);
    const attack = Math.min(player.config.attack, length / 2), releaseTime = Math.min(player.config.release, length / 2);
    gain.gain.setValueAtTime(attack ? 0 : level, at);
    if (attack) gain.gain.linearRampToValueAtTime(level, at + attack);
    if (releaseTime) { gain.gain.setValueAtTime(level, at + length - releaseTime); gain.gain.linearRampToValueAtTime(0, at + length); }
    source.buffer = buffer; source.playbackRate.value = rate;
    source.connect(gain); gain.connect(player.limiter || player.bus);
    player.voices.set(voiceId, {source, gain});
    source.onended = () => { player.voices.delete(voiceId); source.disconnect(); gain.disconnect(); };
    source.start(at); source.stop(at + length);
    return voiceId;
  }
  function closeSampler(id) {
    if (!samplers.has(id)) return true;
    const player = samplers.get(id); clear(id); player.bus?.disconnect(); player.limiter?.disconnect();
    release(output(player.outputId), player); samplers.delete(id); return true;
  }
  function closeOutput(id) {
    if (!outputs.has(id)) return true;
    const out = output(id);
    if ([...players.values(), ...samplers.values()].some(p => p.outputId === id)) throw Error('audio device: busy');
    for (const source of out.sources) { source.onended = null; source.stop(); source.disconnect(); }
    out.gain.disconnect(); outputs.delete(id); return true;
  }
  function stopAll() {
    for (const id of players.keys()) stop(id);
    for (const id of samplers.keys()) clear(id);
    for (const out of outputs.values()) {
      for (const source of out.sources) { source.onended = null; source.stop(); source.disconnect(); }
      out.sources.clear(); out.nextTime = 0;
    }
  }
  function reset() {
    stopAll(); for (const id of [...players.keys()]) closePlayer(id);
    for (const id of [...samplers.keys()]) closeSampler(id);
    for (const id of [...outputs.keys()]) closeOutput(id);
  }
  const methods = {
    openOutput: createOutput, closeOutput, setVolume,
    setMute: (id, muted) => {
      const out = output(id); if (typeof muted !== 'boolean') throw Error('invalid mute argument');
      out.muted = muted; out.gain.gain.value = muted ? 0 : volume / 100;
      for (const p of players.values()) if (p.outputId === id && p.audio) p.audio.muted = muted;
      return true;
    }, getVolume: id => { output(id); return volume; },
    outputInfo: id => { output(id); return {role:'output', opened:true, sample_rate:16000, channels:1, bits:16, bytes_per_frame:2}; },
    outputStatus: id => { const out = output(id); const ms = Math.max(0, (out.nextTime - ctx().currentTime) * 1000); return {queued_bytes:Math.ceil(ms * 32), queued_ms:ms, capacity_bytes:64000}; },
    createPlayer, play, stop, poll, closePlayer, createSampler, start, trigger, stopVoice, clear, closeSampler,
    pause: id => { const p = get(players,id); if (p.audio) { p.audio.pause(); p.state = 'paused'; } return true; },
    resume: id => { const p = get(players,id); if (p.audio) { const media = p.audio; p.state = 'playing'; media.play().catch(e => { if(p.audio === media) {stop(id); p.state='error'; p.error=e.message;} }); } return true; },
    sampleStatus: id => { const p=get(samplers,id); return {active_voices:p.voices.size, loaded_samples:p.samples.size, sample_bytes:p.bytes, sample_budget_bytes:p.config.budget, error:'ESP_OK'}; },
  };
  return {
    outputs, players, samplers, unlock, reset, stopAll, createOutput, createPlayer, setVolume, closeOutput, closePlayer, poll,
    play(id, path) { try { play(id,path); return ''; } catch(e) {return e.message;} },
    stop,
    call(method, args, bytes) {
      if (method === 'load') return load(args[0],args[1],bytes);
      if (method === 'write') return write(args[0],bytes);
      if (!Object.hasOwn(methods, method)) throw Error('unknown audio operation');
      return methods[method](...(Array.isArray(args) ? args : []));
    },
  };
}
if (typeof module !== 'undefined') module.exports = {createSimAudio};
