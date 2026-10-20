import { batch, createSignal } from 'solid-js';
import { createStore, reconcile } from 'solid-js/store';
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

import { t } from '../i18n';
import { pushToast } from './toast';

/* ── Runtime status ─────────────────────────────────────────────────── */

const [status, setStatus] = createSignal<StatusInfo | null>(null);
export const appStatus = status;

const [connected, setConnected] = createSignal(false);
const [generation, setGeneration] = createSignal(0);
const [configError, setConfigError] = createSignal(false);
export const deviceConnected = connected;
export const configGeneration = generation;
export const configSyncError = configError;
let statusRequest: Promise<StatusInfo> | undefined;

export function reloadStatus(): Promise<StatusInfo> {
  if (statusRequest) return statusRequest;
  statusRequest = fetchStatus(AbortSignal.timeout(4000))
    .then((next) => {
      const restarted = status() !== null && status()!.boot_id !== next.boot_id;
      batch(() => {
        setStatus(next);
        setConnected(true);
        if (restarted) invalidateConfig();
      });
      if (restarted) pushToast(t('deviceRestarted'), 'info', 8000);
      return next;
    })
    .catch((err) => {
      setConnected(false);
      throw err;
    })
    .finally(() => {
      statusRequest = undefined;
    });
  return statusRequest;
}

// Remount page state and reject responses belonging to an earlier device lifetime.
export function invalidateConfig() {
  batch(() => {
    pending.clear();
    setConfigStore(reconcile({}));
    setLoadedGroups(new Set<ConfigGroup>());
    setConfigError(false);
    setCapabilities([]);
    setLuaModules([]);
    setGeneration((value) => value + 1);
  });
}

/* ── Capabilities & Lua modules ─────────────────────────────────────── */

const [capabilities, setCapabilities] = createSignal<CapabilityItem[]>([]);
const [luaModules, setLuaModules] = createSignal<LuaModuleItem[]>([]);
export const appCapabilities = capabilities;
export const appLuaModules = luaModules;

export async function reloadCapabilities() {
  const version = generation();
  const items = await fetchCapabilities();
  if (version === generation()) setCapabilities(items);
  return items;
}

export async function reloadLuaModules() {
  const version = generation();
  const items = await fetchLuaModules();
  if (version === generation()) setLuaModules(items);
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

  const toFetch = missing.filter((group) => !pending.has(group));

  if (toFetch.length > 0) {
    const task = reloadConfigGroups(toFetch).finally(() => {
      for (const group of toFetch) {
        if (pending.get(group) === task) pending.delete(group);
      }
    });
    for (const group of toFetch) pending.set(group, task);
  }

  await Promise.all(missing.map((group) => pending.get(group)).filter(Boolean) as Promise<void>[]);
}

/** Force-reload a set of groups, bypassing the cache. */
export async function reloadConfigGroups(groups: ConfigGroup[]): Promise<void> {
  if (groups.length === 0) return;
  const version = generation();
  let data: Partial<AppConfig>;
  try {
    data = await fetchConfigGroups(groups);
  } catch (err) {
    if (version === generation()) setConfigError(true);
    throw err;
  }
  if (version !== generation()) throw new Error(t('deviceRestarted'));
  setConfigStore(data as Partial<AppConfig>);
  setLoadedGroups((prev) => {
    const next = new Set(prev);
    groups.forEach((group) => next.add(group));
    return next;
  });
}

export async function saveConfig(patch: Partial<AppConfig>) {
  const version = generation();
  await reloadStatus();
  if (version !== generation() || configError()) throw new Error(t('configSyncFailed'));
  const result = await saveConfigPatch(patch);
  if (version !== generation()) throw new Error(t('deviceRestarted'));
  setConfigStore(patch);
  return result;
}
