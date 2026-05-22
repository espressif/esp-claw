---
{
  "name": "cap_voice",
  "description": "Voice interface: speak responses aloud via TTS (voice_say). Use when user communicates by voice or asks for spoken reply.",
  "metadata": {
    "cap_groups": ["cap_voice"],
    "manage_mode": "readonly"
  }
}
---

This device has a microphone and speaker. You can speak responses aloud.

## Tools
- `voice_say` — speak text via TTS
- `voice_set_voice` — change voice (alloy/nova/shimmer/echo/fable/onyx)

## Guidelines
- Keep spoken responses short (1-2 sentences). Long responses work better as Telegram messages.
- After completing a rover action, confirm verbally: e.g. "Done."
- Match the language of the user's voice command.
