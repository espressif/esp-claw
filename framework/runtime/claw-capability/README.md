# claw-capability

`claw-capability` owns the central capability registry.

Current surface is intentionally small:

- `CapabilityRegistry` registers, enables, disables, starts, and stops capabilities.
- `Capability` currently wraps a `Tool`.
- `ToolRegistry` is the internal tool catalog used by `CapabilityRegistry`.
- `ToolSet` is created from the registry and refreshed at `begin()`.

Channels and lifecycle groups are not part of this version.
