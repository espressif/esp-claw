# Pet Buddy

`pet_buddy` is the single common pet component. It keeps the pet as the primary
on-device companion while scene modules such as Match Watch and Buddy Home
temporarily attach the pet to their own UI.

## Model

- `pet_buddy_start()` opens the default empty pet scene.
- Scene modules call `pet_buddy_attach_scene_pet()` to create their `pet_host_t`
  and attach lifecycle/selected-pet change notifications in one step.
- Scene modules may call `pet_buddy_attach_host()` when they only need action
  routing and do not care about selected-pet metadata.
- Scene modules call `pet_buddy_detach_scene_pet()` before closing scene-owned
  pet hosts.
- `pet_buddy_action()` applies an action to the active scene pet. If no scene is
  active, the default empty pet scene is opened first.

Only one pet host is active at a time. The selected pet package and metadata
still come from `pet_registry`; `pet_buddy` listens for selection changes and
notifies the active scene hook. `on_mount` receives the current selected pet so
the scene can initialize its own state, `on_pet_changed` receives later
selection changes, and `on_unmount` lets the scene clean up attach-local state.
Business modules interpret metadata such as `profile.team` inside their own
hook.

## Internal Modules

- `pet_buddy`: public coordinator and action control entry.
- `pet_runtime`: default empty pet scene used when no business scene is active.
- `pet_host`: host-side open/close/action/visible/place/touch adapter.
- `pet_renderer`: mmap pet package loading, action assets, frame draw, and touch
  movement.
- `pet_registry`: package discovery, selected-pet persistence, and change
  notification.

These modules live in one ESP-IDF component so business scenes only depend on
`pet_buddy`.

## Package Layout

```text
/fatfs/skills/pet_module/my_pet.bin
```

The `.bin` is an mmap-assets package. Metadata can be provided in either form:

- embedded `pet.json`, or
- a sidecar file next to the bin, for example `my_pet.pet.json`.

Metadata uses `id`, optional `name`, `title`, and `profile`. `profile` is a
generic host binding string; themed hosts can interpret it as a team, role, or
other subject. Existing fixed packages that expose `country` or top-level
`team` are accepted and normalized into `profile`.

The package should also contain `index.json` plus the action assets referenced
by that index.
