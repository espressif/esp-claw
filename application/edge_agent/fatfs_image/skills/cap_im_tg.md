# Telegram Messaging

Use this skill when the user wants to interact through Telegram, especially to reply in the current Telegram conversation or send local files back to Telegram.

## When to use
- The user sends a message from Telegram and expects a reply in the same chat.
- The user asks to send a text, image, screenshot, report, log, or other local file to a Telegram chat.
- The task is clearly Telegram-specific rather than QQ, Feishu, or WeChat.

## Available capabilities
- `tg_send_message`: send plain text to a Telegram chat.
- `tg_send_image`: send a local image file to a Telegram chat.
- `tg_send_file`: send a local non-image file to a Telegram chat.

## Calling rules
- Call the direct Telegram capabilities.
- When replying to the current inbound Telegram conversation, you can omit `chat_id` if the runtime context already contains it.
- When starting a new outbound send or the target chat is ambiguous, pass an explicit `chat_id`.
- Use `tg_send_message` for text only.
- Use `tg_send_image` for image files such as `.jpg`, `.jpeg`, `.png`, `.gif`, or `.webp`.
- Use `tg_send_file` for non-image files such as `.txt`, `.json`, `.log`, `.csv`, `.pdf`, or archives.
- `caption` is optional for image and file sends. Include it only when the user wants accompanying text.

## Chat ID guidance
- Telegram `chat_id` is usually a numeric string such as `"123456789"` or `"-1001234567890"`.
- For replies triggered by an inbound Telegram message, prefer using the current context instead of reconstructing the `chat_id`.
- If the user gives a Telegram target explicitly, preserve it exactly.

## File path guidance
- `path` must be a real local filesystem path on the device.
- If the exact path is unknown, inspect storage first with file capabilities such as `list_dir`.
- Do not pass remote URLs directly to Telegram send capabilities.
- In this demo app, inbound Telegram attachments are typically saved under `<storage_root>/inbox`.

## Recommended workflow
1. Determine whether the user wants text, an image, or a generic file.
2. Resolve the target Telegram chat.
3. Resolve the local file path if sending media.
4. Call `tg_send_message`, `tg_send_image`, or `tg_send_file` directly.
5. After the capability returns success, tell the user the reply or file has already been sent.

## Examples

Reply with text to the current Telegram chat:
```json
{
  "message": "Task completed."
}
```

Send text to an explicit Telegram chat:
```json
{
  "chat_id": "-1001234567890",
  "message": "Latest status: device is online."
}
```

Send an image:
```json
{
  "chat_id": "123456789",
  "path": "<storage_root>/inbox/capture.jpg",
  "caption": "Here is the image."
}
```

Send a file:
```json
{
  "chat_id": "123456789",
  "path": "<storage_root>/reports/status.json",
  "caption": "Latest report."
}
```

## Notes
- This skill is for Telegram only. If the user is on another IM channel, use that channel's capability group instead.
- The capability returns success text like `reply already sent to user`; do not phrase the result as a pending action.
