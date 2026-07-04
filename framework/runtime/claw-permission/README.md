# claw-permission

The tool-permission policy layer for the ESP-Claw agent framework.

A small, pure crate that answers one question: **may this tool call run?** It
models a call as an [`Action`] (a verb + optional target [`Resource`] +
[`RiskClass`]), evaluates it through a [`PermissionPolicy`] into a
[`PermissionDecision`] (`Allow` / `Ask` / `Deny`), and — for the `Ask` path —
remembers the human's answer in a [`GrantStore`] so a retried call resolves
without asking twice (and cannot loop).

## How it fits together

```text
Action  ──evaluate──>  PermissionPolicy  ──>  PermissionDecision
(verb +                (AllowAll,              (Allow | Ask | Deny)
 resource +             AskAtOrAbove,                 │
 risk)                  PolicyChain, …)               │ Ask
                                                      ▼
                                               human decides
                                                      │
                                                      ▼
                                           GrantStore (signature → Grant)
                                           → retried call resolves directly
```

The permission layer never sees the tool itself — only the `Action` it
produces. A tool builds an `Action` for each call in its `classify` method
(over in `claw-capability::tool`); this crate classifies it.

## Public API

Re-exported from the crate root:

| Type | Role |
|------|------|
| `Action` | What a call *does*: `verb` (stable action label), optional `resource`, and `risk`. `signature()` is the stable `verb[:resource]` key a grant is scoped to. |
| `Resource` | The target an action touches (`Path` / `Host` / `Agent`), part of the grant signature so an approval is scoped to *that* resource. |
| `RiskClass` | `Safe < Low < Moderate < High`, ordered so policies can threshold on it. |
| `PermissionDecision` | The verdict: `Allow`, `Ask { reason }`, or `Deny { reason }`. |
| `PermissionPolicy` | The policy trait — pure classification, no side effects. Object-safe, so a chain holds `Box<dyn PermissionPolicy>`. |
| `PermissionRequest` | One action to evaluate. Agent identity is not carried (no built-in policy keys on it); add it back as borrowed primitives if a policy needs the acting principal, keeping this crate below `claw-core`. |
| `AllowAll` | The permissive base: allows everything. |
| `AskAtOrAbove` | Asks for approval at or above a risk threshold; allows the rest. |
| `PolicyChain` | Composes policies, **most-restrictive-wins**: any `Deny` short-circuits, else any `Ask`, else `Allow`. Empty chain allows everything. |
| `GrantStore` / `Grant` | Records `Granted` / `Denied(reason)` decisions, keyed by `Action::signature`, so an approved/denied action resolves without re-asking. |

### Design notes

- **The crate sits below `claw-core`.** A `PermissionRequest` carries the acting
  agent as borrowed primitives (`u64` + `&str`) rather than `claw-core`'s
  `AgentId` / `AgentKind`, so the dependency stays one-directional.
- **Signatures scope approvals.** `Action::signature()` is `verb` or
  `verb:resource`; a grant recorded under it applies to *that* verb-on-resource,
  not the verb in general. A different resource re-asks.
- **Most-restrictive composition is safe.** Adding a rule to a `PolicyChain` can
  only tighten access, never loosen it — so policies compose without surprises.
- **`Ask` cannot loop.** A recorded grant/denial is consulted before the policy,
  so an approved (or denied) call resolves directly on retry instead of asking
  forever.

## Usage

```rust
use claw_permission::{
    Action, AskAtOrAbove, GrantStore, PermissionDecision, PermissionPolicy,
    PermissionRequest, PolicyChain, Resource, RiskClass,
};

let policy = PolicyChain::new().with(AskAtOrAbove::new(RiskClass::Moderate));
let action = Action::new("write_file", RiskClass::Moderate)
    .with_resource(Resource::Path("/data/x".into()));
let request = PermissionRequest::new(&action);

// First time: the policy asks for approval.
assert!(matches!(policy.evaluate(&request), PermissionDecision::Ask { .. }));

// After a human approves, the grant short-circuits the next identical call.
let mut grants = GrantStore::new();
grants.grant(action.signature());
assert!(grants.lookup(&action.signature()).is_some());
```

## Examples

Runnable on the host:

```bash
cargo run --example policy_chain   --target x86_64-unknown-linux-gnu
cargo run --example approval_flow  --target x86_64-unknown-linux-gnu
cargo run --example custom_policy  --target x86_64-unknown-linux-gnu
```

## Where it fits

`claw-permission` is a pure-Rust core crate with no platform dependencies. In the
agent runtime, `base_agent` wraps a `PermissionPolicy` plus a `GrantStore` and
implements `claw-capability`'s `ToolGate`, which the `ToolRunner` consults
before executing each classified tool call.
