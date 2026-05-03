import {
  Activity,
  Blocks,
  Bot,
  DatabaseZap,
  Folder,
  MessageSquareCode,
  MessagesSquare,
  Search,
  Settings,
  SquareFunction,
  WifiPen,
} from 'lucide-solid';
import { createMemo, createSignal, For, Show, type Component } from 'solid-js';
import { t } from '../../i18n';
import { isDirty, type TabId } from '../../state/dirty';
import { StatusSummary } from './StatusBar';

const iconClass = 'w-4 h-4 shrink-0';

const IconStatus: Component = () => <Activity class={iconClass} />;
const IconGear: Component = () => <Settings class={iconClass} />;
const IconWifi: Component = () => <WifiPen class={iconClass} />;
const IconLlm: Component = () => <Bot class={iconClass} />;
const IconIm: Component = () => <MessageSquareCode class={iconClass} />;
const IconSearch: Component = () => <Search class={iconClass} />;
const IconMemory: Component = () => <DatabaseZap class={iconClass} />;
const IconCaps: Component = () => <Blocks class={iconClass} />;
const IconSkills: Component = () => <SquareFunction class={iconClass} />;
const IconFiles: Component = () => <Folder class={iconClass} />;
const IconWebIm: Component = () => <MessagesSquare class={iconClass} />;

export type LeafNode = {
  kind: 'leaf';
  id: TabId;
  labelKey:
    | 'navStatus'
    | 'navBasic'
    | 'navLlm'
    | 'navIm'
    | 'navSearch'
    | 'navMemory'
    | 'navCapabilities'
    | 'navLuaModules'
    | 'navFiles'
    | 'navWebIm';
  icon: Component;
};
export type GroupNode = {
  kind: 'group';
  id: string;
  labelKey: 'navSystemSettings';
  icon: Component;
  children: LeafNode[];
};
export type NavNode = LeafNode | GroupNode;

export const NAV_TREE: NavNode[] = [
  { kind: 'leaf', id: 'status', labelKey: 'navStatus', icon: IconStatus },
  {
    kind: 'group',
    id: 'basic-settings',
    labelKey: 'navSystemSettings',
    icon: IconGear,
    children: [
      { kind: 'leaf', id: 'basic', labelKey: 'navBasic', icon: IconWifi },
      { kind: 'leaf', id: 'llm', labelKey: 'navLlm', icon: IconLlm },
      { kind: 'leaf', id: 'im', labelKey: 'navIm', icon: IconIm },
      { kind: 'leaf', id: 'search', labelKey: 'navSearch', icon: IconSearch },
    ],
  },
  { kind: 'leaf', id: 'memory', labelKey: 'navMemory', icon: IconMemory },
  { kind: 'leaf', id: 'webim', labelKey: 'navWebIm', icon: IconWebIm },
  { kind: 'leaf', id: 'capabilities', labelKey: 'navCapabilities', icon: IconCaps },
  { kind: 'leaf', id: 'skills', labelKey: 'navLuaModules', icon: IconSkills },
  { kind: 'leaf', id: 'files', labelKey: 'navFiles', icon: IconFiles },
];

export const LEAF_IDS = collectLeafIds(NAV_TREE);

function collectLeafIds(nodes: NavNode[]): TabId[] {
  const out: TabId[] = [];
  for (const node of nodes) {
    if (node.kind === 'leaf') out.push(node.id);
    else out.push(...collectLeafIds(node.children));
  }
  return out;
}

function groupContains(group: GroupNode, id: TabId): boolean {
  return group.children.some((child) => child.id === id);
}

function groupDirty(group: GroupNode): boolean {
  return group.children.some((child) => isDirty(child.id));
}

type SidebarProps = {
  current: TabId;
  onSelect: (id: TabId) => void;
  collapsed: boolean;
  onToggleCollapsed: () => void;
  mobileOpen?: boolean;
  onCloseMobile?: () => void;
};

const EXPANDED_STORAGE_KEY = 'esp-claw-sidebar-expanded';

function readExpanded(): Set<string> {
  try {
    const raw = localStorage.getItem(EXPANDED_STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (Array.isArray(parsed)) {
        return new Set(parsed.filter((v) => typeof v === 'string'));
      }
    }
  } catch {
    /* ignore */
  }
  return new Set(['basic-settings']);
}

export const Sidebar: Component<SidebarProps> = (props) => {
  const [expanded, setExpanded] = createSignal<Set<string>>(readExpanded());
  const isMobileDrawer = () => props.mobileOpen !== undefined;
  const isCollapsed = () => !isMobileDrawer() && props.collapsed;

  const persistExpanded = (next: Set<string>) => {
    setExpanded(next);
    try {
      localStorage.setItem(EXPANDED_STORAGE_KEY, JSON.stringify(Array.from(next)));
    } catch {
      /* ignore */
    }
  };

  const toggleGroup = (id: string) => {
    const next = new Set(expanded());
    if (next.has(id)) next.delete(id);
    else next.add(id);
    persistExpanded(next);
  };

  const autoExpanded = createMemo(() => {
    const current = props.current;
    for (const node of NAV_TREE) {
      if (node.kind === 'group' && groupContains(node, current)) {
        if (!expanded().has(node.id)) {
          const next = new Set(expanded());
          next.add(node.id);
          persistExpanded(next);
        }
        return;
      }
    }
  });

  autoExpanded();

  const handleSelect = (id: TabId) => {
    props.onSelect(id);
    props.onCloseMobile?.();
  };

  const renderLeaf = (leaf: LeafNode, options: { indent?: boolean }) => (
    <button
      type="button"
      class={[
        'relative w-full flex items-center gap-3 rounded-[var(--radius-sm)] transition text-left',
        isCollapsed() ? 'justify-center py-2 px-0' : options.indent ? 'px-3 py-2 pl-8' : 'px-3 py-2',
        props.current === leaf.id
          ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)] ring-1 ring-[var(--color-accent-soft)]/40'
          : 'text-[var(--color-text-secondary)] hover:bg-white/5 hover:text-[var(--color-text-primary)]',
      ].join(' ')}
      onClick={() => handleSelect(leaf.id)}
      title={t(leaf.labelKey) as string}
    >
      <span class="relative inline-flex">
        <leaf.icon />
        <Show when={isCollapsed() && isDirty(leaf.id)}>
          <span
            class="absolute -top-1 -right-1 w-1.5 h-1.5 rounded-full bg-[var(--color-orange)]"
            title={t('unsavedIndicator') as string}
          />
        </Show>
      </span>
      <Show when={!isCollapsed()}>
        <span class="flex-1 text-[0.9rem] truncate">{t(leaf.labelKey)}</span>
        <Show when={isDirty(leaf.id)}>
          <span
            class="w-2 h-2 rounded-full bg-[var(--color-orange)]"
            title={t('unsavedIndicator') as string}
          />
        </Show>
      </Show>
    </button>
  );

  const renderGroup = (group: GroupNode) => {
    const isOpen = () => expanded().has(group.id) || isCollapsed();
    const activeChild = () => groupContains(group, props.current);
    return (
      <div>
        <button
          type="button"
          class={[
            'relative w-full flex items-center gap-3 rounded-[var(--radius-sm)] transition text-left',
            isCollapsed() ? 'justify-center py-2 px-0' : 'px-3 py-2',
            activeChild()
              ? 'text-[var(--color-text-primary)] bg-white/[0.04]'
              : 'text-[var(--color-text-secondary)] hover:bg-white/5 hover:text-[var(--color-text-primary)]',
          ].join(' ')}
          onClick={() => {
            if (isCollapsed()) {
              const first = group.children[0];
              if (first) handleSelect(first.id);
            } else {
              toggleGroup(group.id);
            }
          }}
          title={t(group.labelKey) as string}
          aria-expanded={expanded().has(group.id)}
        >
          <span class="relative inline-flex">
            <group.icon />
            <Show when={isCollapsed() && groupDirty(group)}>
              <span class="absolute -top-1 -right-1 w-1.5 h-1.5 rounded-full bg-[var(--color-orange)]" />
            </Show>
          </span>
          <Show when={!isCollapsed()}>
            <span class="flex-1 text-[0.9rem] truncate">{t(group.labelKey)}</span>
            <Show when={groupDirty(group)}>
              <span class="w-2 h-2 rounded-full bg-[var(--color-orange)]" />
            </Show>
            <Show when={!isMobileDrawer()}>
              <svg
                class={['w-3.5 h-3.5 text-[var(--color-text-muted)] transition-transform', expanded().has(group.id) ? 'rotate-90' : ''].join(' ')}
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"
              >
                <path d="M9 6l6 6-6 6" />
              </svg>
            </Show>
          </Show>
        </button>
        <Show when={!isCollapsed() && isOpen()}>
          <ul class="flex flex-col gap-0.5 mt-0.5">
            <For each={group.children}>
              {(leaf) => <li>{renderLeaf(leaf, { indent: true })}</li>}
            </For>
          </ul>
        </Show>
      </div>
    );
  };

  return (
    <aside
      class={[
        isMobileDrawer()
          ? [
              'fixed inset-y-0 left-0 z-50 w-72 max-w-[88vw] border-r border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]/95 backdrop-blur-md flex flex-col transition-transform duration-200 lg:hidden',
              props.mobileOpen ? 'translate-x-0' : '-translate-x-full',
            ].join(' ')
          : [
              'hidden lg:flex border-r border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]/60 flex-col transition-[width] duration-200',
              props.collapsed ? 'w-16' : 'w-60',
            ].join(' '),
      ].join(' ')}
    >
      <div class="flex items-center justify-between px-3 h-14 border-b border-[var(--color-border-subtle)]">
        <Show when={isMobileDrawer() || !props.collapsed}>
          <div class="min-w-0 flex-1 ml-2 pr-3">
            <Show
              when={isMobileDrawer()}
              fallback={
                <span class="block text-[0.7rem] font-bold text-[var(--color-text-muted)] truncate">
                  ESP-Claw Web Config
                </span>
              }
            >
              <StatusSummary compact class="text-[0.74rem]" />
            </Show>
          </div>
        </Show>
        <button
          type="button"
          onClick={isMobileDrawer() ? props.onCloseMobile : props.onToggleCollapsed}
          class="inline-flex items-center justify-center w-9 h-9 rounded-[var(--radius-sm)] text-[var(--color-text-muted)] hover:bg-white/5 hover:text-[var(--color-text-primary)] transition shrink-0"
          aria-label={isMobileDrawer() ? (t('closeMenu') as string) : (t('toggleSidebar') as string)}
        >
          <Show
            when={isMobileDrawer()}
            fallback={
              <svg
                class={['w-4 h-4 transition-transform', props.collapsed ? 'rotate-180' : ''].join(' ')}
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"
              >
                <path d="M15 18l-6-6 6-6" />
              </svg>
            }
          >
            <svg
              class="w-4 h-4"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
              stroke-linecap="round"
              stroke-linejoin="round"
            >
              <path d="M18 6L6 18" />
              <path d="M6 6l12 12" />
            </svg>
          </Show>
        </button>
      </div>
      <nav class="flex-1 overflow-y-auto py-2">
        <ul class="flex flex-col gap-0.5 px-2">
          <For each={NAV_TREE}>
            {(node) => (
              <li>
                {node.kind === 'leaf'
                  ? renderLeaf(node, { indent: false })
                  : renderGroup(node)}
              </li>
            )}
          </For>
        </ul>
      </nav>
    </aside>
  );
};
