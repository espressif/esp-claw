# match_watch_runtime Directory Layout

```
match_watch_runtime/
├── match_watch_runtime.h      # Public types + start (impl in api/)
├── api/                       # Thread-safe requests, start, runtime config
├── core/                      # Context, main loop, shared types/headers
├── schedule/                  # Host team, NVS, match/stage selection
├── ui/                        # GFX widgets, render, touch/input
├── notify/                    # Match reminders (kickoff, goal, HT, FT)
├── pet/                       # Pet action policy + pet_renderer adapter
├── data/                      # Normalized schedule cache
├── platform/                  # display_session + mmap assets
└── assets/                    # Linked fonts & icons (generated .c)
```

## Public API

| Path | Role |
|------|------|
| `match_watch_runtime.h` | `start`, status/notification types |
| `api/match_watch_app.h` | `request_*`, favorite, notify, reminders, status |
| `data/match_data.h` | Schedule model (also used by `cap_match_watch`) |

Include via component `INCLUDE_DIRS`: e.g. `#include "match_watch_runtime.h"`, `#include "match_data.h"`.

## api/

| File | Lines (approx) | Role |
|------|----------------|------|
| `match_watch_app.c` | ~320 | `runtime_start`, `request_close`, `set_favorite`, notify/reminders, status |

## core/

| File | Role |
|------|------|
| `match_watch_ctx.c` | `s_app`, ensure, notification callback |
| `match_watch_loop.c` | `main`, run loop, external dispatch, pet open |
| `match_watch_types.h` | UI macros, `match_watch_app_ctx_t`, page/transition enums |
| `match_watch_ctx.h` | Context accessor declarations |
| `match_watch_module.h` | Cross-module function declarations |
| `match_watch_internal.h` | Umbrella + `#define s_app` |
| `match_watch_app_internal.h` | Deprecated alias → `match_watch_internal.h` |

## schedule/

| File | Role |
|------|------|
| `match_watch_schedule.c` | Team list, pick match, stage offset, NVS host team |

## ui/

| File | Role |
|------|------|
| `match_watch_ui.c` | Card formatting, `render_page`, live refresh |
| `match_watch_ui_widgets.c` | Create/destroy GFX tree |
| `match_watch_input.c` | Touch, swipe, transitions |
| `match_watch_home_state.h` | Home phase / timing types |

## notify/

| File | Role |
|------|------|
| `match_watch_notify.c` | `check_notifications`, IM payload |

## pet/

| File | Role |
|------|------|
| `match_watch_pet.c` | Match events → action policy |

## data/ · platform/ · assets/

| Path | Role |
|------|------|
| `data/match_data.c` | Live group/knockout cache (~36KB cap) |
| `platform/match_watch_platform.c` | `DISPLAY_ARBITER_OWNER_EMOTE_GFX` session |
| `assets/` | `font_*.c`, `ui/match_icon_*.c` |

## Call flow

```text
cap_match_watch
  → api/match_watch_app.c (start + requests)
  → core/match_watch_loop.c (lifecycle)
       → schedule / ui / notify / ui/input
       → pet/
       → data/
       → platform/
```
