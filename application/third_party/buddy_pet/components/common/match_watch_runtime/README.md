# Match Watch Runtime

Native Match Watch UI: normalized match data, rendering, touch, reminders, and in-watch pet policy.

- [PROVIDER_CONTRACT_CN.md](PROVIDER_CONTRACT_CN.md) — provider session contract and push data schema
- [MODULES.md](MODULES.md) — directory and file map

## Top-level layout

| Directory | Purpose |
|-----------|---------|
| `api/` | External requests & config (`match_watch_app_*`) |
| `core/` | Context, main loop, shared internal headers |
| `schedule/` | Favorite team & match selection |
| `ui/` | Widgets, pages, touch |
| `notify/` | Push reminders |
| `pet/` | Pet policy + renderer adapter |
| `data/` | `match_data_*` schedule store |
| `platform/` | Display session glue |
| `assets/` | Fonts and icons (compiled-in) |

Root `match_watch_runtime.[ch]` is the only facade most callers need.

Providers push scores via `match_watch_push_data`; this component does not poll the network.
