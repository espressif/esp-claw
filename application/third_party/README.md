# Third-Party Example Applications

This directory hosts product-specific firmware examples that extend ESP-Claw
with local components, skills, and board overlays.

Each example is a standalone ESP-IDF application for a concrete device or
product scenario. It can provide its own `main/`, board definitions, local
components, FATFS seed content, skills, router rules, and default configuration
while reusing the shared ESP-Claw runtime from the repository root.

## Examples

| Application | Description |
|-------------|-------------|
| [`buddy_pet/`](buddy_pet/) | ESP-Ditto companion firmware: Match Watch, downloadable screen buddies, IM control, and WeChat setup. Default board: `esp_Ditto`. |

## Build

From the example directory:

```bash
. $IDF_PATH/export.sh
pip install esp-bmgr-assist   # once per ESP-IDF environment
cd application/third_party/buddy_pet
idf.py bmgr -c boards -b esp_Ditto
idf.py build
idf.py flash monitor
```

If `bmgr` / `gen-bmgr-config` reports a missing `esp_board_manager`, clear
`managed_components` and `dependencies.lock` in the example directory and retry
(see `buddy_pet/README.md`).

See each example's `README.md` for product-specific notes.
