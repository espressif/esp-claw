# Attachment Message Shape

## Decision

An orchestrator inbound message is a chat message envelope with optional text
and zero or more attachments:

```rust
pub struct OrchestratorMessage {
    pub text: Option<String>,
    pub attachments: Vec<AttachmentRef>,
}
```

This is intentionally **not**:

```rust
pub enum OrchestratorMessage {
    Text(String),
    Attachment(Attachment),
}
```

The enum belongs, if ever needed, at a lower "content part" layer. The top-level
chat app shape is `text + attachments`, because a single real user message can
carry both a text body and one or more non-text objects.

The invariant is:

```rust
message.text.as_deref().is_some_and(|text| !text.trim().is_empty())
    || !message.attachments.is_empty()
```

## Message Semantics

The shape maps cleanly onto chat app behavior:

| Inbound event | Message shape |
| --- | --- |
| Plain text | `text = Some(...)`, `attachments = []` |
| Image only | `text = None`, `attachments = [image]` |
| Image with caption | `text = Some(caption)`, `attachments = [image]` |
| Multiple files with a question | `text = Some(question)`, `attachments = [file_a, file_b]` |

`Image` is not a top-level message variant. It is an attachment kind:

```rust
pub enum AttachmentKind {
    Image,
    File,
    Audio,
    Video,
    Unknown,
}
```

The current product rule is simpler than a fully ordered content-part model:
the text body is the message body, and attachments are the message's attached
objects. This does not preserve exact interleaving such as "text A, image 1,
text B, image 2". That precision is not required for the current IM and agent
flows. If it becomes necessary, add an optional presentation/order layer later
without replacing the stable top-level shape.

## Attachment Source Of Truth

Attachments are non-text state. Their source of truth should be a session
attachment manifest, not the transcript text.

```rust
pub struct AttachmentRecord {
    pub id: AttachmentId,
    pub kind: AttachmentKind,
    pub path: String,
    pub mime: Option<String>,
    pub filename: Option<String>,
    pub caption: Option<String>,
    pub size_bytes: Option<u64>,
    pub source: AttachmentSource,
    pub metadata: serde_json::Value,
    pub derived: AttachmentDerived,
}

pub struct AttachmentRef {
    pub id: AttachmentId,
}
```

The manifest preserves information that prompt text cannot safely carry:

- the local saved path used by tools;
- MIME type and attachment kind;
- original filename and size;
- source channel, chat id, platform message id, and timestamps;
- platform-specific payload fields;
- derived outputs such as OCR, image description, PDF text, or summaries.

The transcript should only store the user-visible message projection, plus stable
attachment references. It should not be the only copy of attachment metadata.

## Context Pipeline

Text and attachments have different homes:

- `message.text` enters the normal transcript/context pipeline as the user text.
- `message.attachments` are written to the session attachment manifest.
- The context pipeline receives a lightweight projection of the attachment refs.

For example, the model-visible user message can be rendered as:

```text
Please inspect this image.

[attachments]
- att-3 image/png photo.png path=/data/im/feishu/room/feishu_abcd_image.png
```

Derived content can be projected later by context adapters or tool results:

```text
[attachment att-3 derived]
ocr: ...
summary: ...
```

This keeps large/binary/non-text state out of the prompt while still telling the
model that the user sent something and giving tools stable ids or paths to use.

## Caption Rule

Transport captions should be copied into both places when present:

- `message.text`, so the agent sees the user's natural language immediately;
- `AttachmentRecord.caption`, so the original platform attachment metadata stays
  intact.

If a platform has both a normal text body and per-attachment captions, ingress
should normalize the user-visible text into `message.text` and keep original
caption fields on each attachment record.

## Ingress Boundary

Current C-side event data already carries the right raw ingredients:

- `claw_event_t.text`
- `claw_event_t.content_type`
- `claw_event_t.payload_json`
- IM attachment payload fields such as `saved_path`, `mime`, `caption`,
  `original_filename`, and `size_bytes`

The semantic loss happens when the agent adapter extracts only `"text"` and calls
the text-only submit path. The intended boundary is:

1. Convert an inbound event into `OrchestratorMessage`.
2. Store any non-text payloads as `AttachmentRecord`s in the session manifest.
3. Submit `OrchestratorMessage { text, attachments }` to the orchestrator.
4. Let the agent transcript/context layer render text plus attachment projection.

## Non-Goals

- Do not encode provider-specific multimodal request JSON in the orchestrator.
  LLM adapter/provider code owns that later.
- Do not inline binary data or base64 into the transcript.
- Do not make every attachment eagerly become extracted text. Extraction is
  derived state and should be adapter/tool-driven.
- Do not make `Image` a top-level message kind. It is an attachment kind.

## Future Direction

When a visual or multimodal model path is available, `AttachmentKind::Image`
records can be passed to the LLM adapter as first-class multimodal inputs. The
orchestrator shape does not need to change: it already carries text plus stable
attachment refs. Only the context/request realization changes.
