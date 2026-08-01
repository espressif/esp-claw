# claw_hw_registry / registry_basic

Unit test app for the hardware registry (§7 WS-1 acceptance).

## Build

```
. $IDF_PATH/export.sh
idf.py -C components/common/claw_hw_registry/test_apps/registry_basic build
```

Default target is `esp32s3` (via `sdkconfig.defaults`).
