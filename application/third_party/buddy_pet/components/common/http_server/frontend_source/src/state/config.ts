import { createSignal } from 'solid-js';
import { createStore } from 'solid-js/store';
import {
  fetchCapabilities,
  fetchConfigGroups,
  fetchLuaModules,
  fetchStatus,
  saveConfigPatch,
  type AppConfig,
  type CapabilityItem,
  type ConfigGroup,
  type LuaModuleItem,
  type StatusInfo,
} from '../api/client';

/* ── Runtime status ─────────────────────────────────────────────────── */

const [status, setStatus] = createSignal<StatusInfo | null>(null);
export const appStatus = status;

export async function reloadStatus() {
  const next = await fetchStatus();
  setStatus(next);
  return next;
}

/* ── Capabilities & Lua modules ─────────────────────────────────────── */

const [capabilities, setCapabilities] = createSignal<CapabilityItem[]>([]);
const [luaModules, setLuaModules] = createSignal<LuaModuleItem[]>([]);
export const appCapabilities = capabilities;
export const appLuaModules = luaModules;

export async function reloadCapabilities() {
  const items = await fetchCapabilities();
  setCapabilities(items);
  return items;
}

export async function reloadLuaModules() {
  const items = await fetchLuaModules();
  setLuaModules(items);
  return items;
}

/* ── Partial configuration cache ────────────────────────────────────── */

const [configStore, setConfigStore] = createStore<Partial<AppConfig>>({});
const [loadedGroups, setLoadedGroups] = createSignal(new Set<ConfigGroup>());
const pending = new Map<ConfigGroup, Promise<void>>();

export const appConfig = () => configStore;

export function isGroupLoaded(group: ConfigGroup) {
  return loadedGroups().has(group);
}

/** Load the requested groups if they are not already cached. Concurrent
 * callers share a single in-flight fetch per group. */
export async function ensureConfigGroups(groups: ConfigGroup[]): Promise<void> {
  const missing = Array.from(new Set(groups.filter((group) => !loadedGroups().has(group))));
  if (missing.length === 0) return;

  const alreadyPending = missing.filter((group) => pending.has(group));
  const toFetch = missing.filter((group) => !pending.has(group));

  if (toFetch.length > 0) {
    const task = fetchConfigGroups(toFetch).then((data) => {
      setConfigStore(data as Partial<AppConfig>);
      setLoadedGroups((prev) => {
        const next = new Set(prev);
        toFetch.forEach((group) => next.add(group));
        return next;
      });
    });
    for (const group of toFetch) {
      const cleanup = task.finally(() => pending.delete(group));
      pending.set(group, cleanup);
    }
  }

  await Promise.all(missing.map((group) => pending.get(group)).filter(Boolean) as Promise<void>[]);
  void alreadyPending;
}

/** Force-reload a set of groups, bypassing the cache. */
export async function reloadConfigGroups(groups: ConfigGroup[]): Promise<void> {
  if (groups.length === 0) return;
  const data = await fetchConfigGroups(groups);
  setConfigStore(data as Partial<AppConfig>);
  setLoadedGroups((prev) => {
    const next = new Set(prev);
    groups.forEach((group) => next.add(group));
    return next;
  });
}

/** Apply a locally-known patch to the cache after a successful save. */
export function patchConfigLocal(patch: Partial<AppConfig>) {
  setConfigStore(patch);
}

export async function saveConfig(patch: Partial<AppConfig>) {
  const result = await saveConfigPatch(patch);
  setConfigStore(patch);
  return result;
}
