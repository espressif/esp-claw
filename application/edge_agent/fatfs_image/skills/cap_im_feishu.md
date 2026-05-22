# Feishu Messaging

Use this skill when the user wants to interact through Feishu, especially to reply in the current Feishu conversation or send local files back to Feishu.

## When to use
- The user sends a message from Feishu and expects a reply in the same chat.
- The user asks to send a text, image, document, report, log, or other local file to a Feishu chat.
- The task is clearly Feishu-specific rather than QQ, Telegram, or WeChat.

## Available capabilities
- `feishu_send_message`: send text to a Feishu chat, rendered through a Markdown-capable interactive card when possible.
- `feishu_send_image`: send a local image file to a Feishu chat.
- `feishu_send_file`: send a local non-image file to a Feishu chat.

## Calling rules
- Call the direct Feishu capabilities.
- When replying to the current inbound Feishu conversation, you can omit `chat_id` if the runtime context already contains it.
- When starting a new outbound send or the target chat is ambiguous, pass an explicit `chat_id`.
- Use `feishu_send_message` for text only. Markdown such as bold text, links, and lists is sent unchanged and rendered by Feishu's card Markdown support when card delivery succeeds.
- Use `feishu_send_image` for image files such as `.jpg`, `.jpeg`, `.png`, `.gif`, or `.webp`.
- Use `feishu_send_file` for non-image files such as `.txt`, `.json`, `.log`, `.csv`, `.pdf`, or archives.
- `caption` is optional for image and file sends. In Feishu media send flow, caption is sent as a follow-up text message.

## Chat ID guidance
- Feishu text send accepts either a chat id or a user `open_id`.
- If the target begins with `ou_`, the runtime treats it as a user `open_id`.
- Otherwise it is treated as a Feishu `chat_id`.
- For replies triggered by an inbound Feishu message, prefer using the current context instead of inventing a `chat_id`.

## File path guidance
- `path` must be a real local filesystem path on the device.
- If the exact path is unknown, inspect storage first with file capabilities such as `list_dir`.
- Do not pass remote URLs directly to Feishu send capabilities.
- In this demo app, inbound Feishu attachments are typically saved under `<storage_root>/inbox`.

## Recommended workflow
1. Determine whether the user wants text, an image, or a generic file.
2. Resolve the target Feishu chat or `open_id`.
3. Resolve the local file path if sending media.
4. Call `feishu_send_message`, `feishu_send_image`, or `feishu_send_file` directly.
5. After the capability returns success, tell the user the reply or file has already been sent.

## Examples

Reply with text to the current Feishu chat:
```json
{
  "message": "The task has been completed."
}
```

Send text to an explicit Feishu user:
```json
{
  "chat_id": "ou_xxx123456",
  "message": "Latest status: device is online."
}
```

Send an image:
```json
{
  "chat_id": "oc_xxx123456",
  "path": "<storage_root>/inbox/capture.jpg",
  "caption": "Here is the image."
}
```

Send a file:
```json
{
  "chat_id": "oc_xxx123456",
  "path": "<storage_root>/reports/status.json",
  "caption": "Latest report."
}
```

## Notes
- This skill is for Feishu only. If the user is on another IM channel, use that channel's capability group instead.
- Feishu send capabilities return JSON such as `{\"ok\":true}` on success.
- `feishu_send_message` falls back to the plain-text Feishu message path if Markdown card construction or delivery fails.
