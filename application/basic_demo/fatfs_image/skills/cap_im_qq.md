# QQ Messaging

Use this skill when the user wants to interact through QQ Bot channels, especially to reply in the current QQ conversation or send local files back to QQ.

## When to use
- The user sends a message from QQ and expects a reply in the same chat.
- The user asks to send a text, image, screenshot, report, log, or other local file to a QQ chat.
- The task is clearly QQ-specific rather than Telegram, Feishu, or WeChat.

## Available capabilities
- `qq_send_message`: send plain text to a QQ chat.
- `qq_send_image`: send a local image file to a QQ chat.
- `qq_send_file`: send a local non-image file to a QQ chat.

## Calling rules
- Call the direct QQ capabilities.
- When replying to the current inbound QQ conversation, you can omit `chat_id` if the runtime context already contains it.
- When starting a new outbound send or the target chat is ambiguous, pass an explicit `chat_id`.
- Use `qq_send_message` for text only.
- Use `qq_send_image` for image files such as `.jpg`, `.jpeg`, `.png`, `.gif`, or `.webp`.
- Use `qq_send_file` for non-image files such as `.txt`, `.json`, `.log`, `.csv`, `.pdf`, or archives.
- `caption` is optional for image and file sends. Include it only when the user wants accompanying text.

## Chat ID guidance
- Private QQ chats are normalized as `c2c:<openid>`.
- Group QQ chats are normalized as `group:<group_openid>`.
- For replies triggered by an inbound QQ message, prefer using the current context instead of inventing a `chat_id`.
- If the user explicitly provides a QQ target, preserve it exactly when it already matches the expected `c2c:` or `group:` format.

## File path guidance
- `path` must be a real local filesystem path on the device.
- If the exact path is unknown, inspect storage first with file capabilities such as `list_dir`.
- Do not attempt to send remote URLs directly. Download or locate the file on local storage first.
- In this demo app, inbound QQ attachments are typically saved under `<storage_root>/inbox`.

## Recommended workflow
1. Determine whether the user wants text, an image, or a generic file.
2. Resolve the target chat.
3. Resolve the local file path if sending media.
4. Call `qq_send_message`, `qq_send_image`, or `qq_send_file` directly.
5. After the capability returns success, tell the user the reply or file has already been sent.

## Examples

Reply with text to the current QQ chat:
```json
{
  "message": "The task has been completed."
}
```

Send text to an explicit QQ group:
```json
{
  "chat_id": "c2c:1234567890",
  "message": "Latest status: device is online."
}
```

Send an image:
```json
{
  "chat_id": "c2c:1234567890",
  "path": "<storage_root>/inbox/capture.jpg",
  "caption": "Here is the image."
}
```

Send a file:
```json
{
  "chat_id": "c2c:abcdefg123456",
  "path": "<storage_root>/reports/status.json",
  "caption": "Latest report."
}
```

## Notes
- This skill is for QQ only. If the user is on another IM channel
- The capability returns success text like `reply already sent to user`; do not repeat the same content again as if it still needs to be delivered.
- Generic file delivery may still depend on QQ platform-side support. If `qq_send_file` fails, report the failure clearly and consider whether the file can be sent as an image instead.
