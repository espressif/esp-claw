# WeChat Messaging

Use this skill when the user wants to interact through WeChat, especially to send text or a local image to a WeChat contact or group.

## When to use
- The user asks to send a text reply or image through WeChat.
- The task is clearly WeChat-specific rather than QQ, Telegram, or Feishu.
- The target WeChat `chat_id` is known or can be taken from the current workflow context outside the capability call.

## Available capabilities
- `wechat_send_message`: send plain text to a WeChat chat.
- `wechat_send_image`: send a local image file to a WeChat chat.
- `wechat_gateway`: WeChat inbound event source. This is infrastructure, not a capability the model should call directly.

## Calling rules
- Call the direct WeChat capabilities.
- `wechat_send_message` requires explicit `chat_id` and `message`.
- `wechat_send_image` requires explicit `chat_id` and `path`, with optional `caption`.
- Unlike Telegram, QQ, and Feishu, the current WeChat callable implementation does not fall back to `ctx->chat_id`. Always pass `chat_id` explicitly.
- Use `wechat_send_message` for text only.
- Use `wechat_send_image` for image files such as `.jpg`, `.jpeg`, `.png`, `.gif`, or `.webp`.

## Chat ID guidance
- WeChat inbound routing uses the group id when present, otherwise the sender user id.
- In practice, `chat_id` is typically a room id or a user id string already known by the integration.
- If the user gives a concrete WeChat target such as a room id or contact id, preserve it exactly.

## File path guidance
- `path` must be a real local filesystem path on the device.
- If the exact image path is unknown, inspect storage first with file capabilities such as `list_dir`.
- Do not pass remote URLs directly to WeChat send capabilities.
- In this demo app, inbound WeChat media is typically saved under `<storage_root>/inbox`.

## Recommended workflow
1. Confirm that the target channel is WeChat.
2. Resolve the explicit WeChat `chat_id`.
3. Resolve the local image path if sending media.
4. Call `wechat_send_message` or `wechat_send_image` directly.
5. After the capability returns success, tell the user the message or image has already been sent.

## Examples

Send text to a WeChat chat:
```json
{
  "chat_id": "room123",
  "message": "Latest status: device is online."
}
```

Send an image:
```json
{
  "chat_id": "wxid_abc123",
  "path": "<storage_root>/inbox/capture.jpg",
  "caption": "Here is the image."
}
```

## Notes
- This skill is for WeChat only. If the user is on another IM channel, use that channel's capability group instead.
- The current capability surface supports text and image send, but not generic non-image file send.
- WeChat send capabilities return `{\"ok\":true}` on success.
