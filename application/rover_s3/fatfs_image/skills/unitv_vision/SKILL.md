---
{
  "name": "unitv_vision",
  "description": "Camera vision: use unitv_scan for quick object detection, unitv_capture for detailed scene analysis with LLM. Activate when user asks to look, see, detect, identify, or analyze visual scene.",
  "metadata": {
    "manage_mode": "readonly"
  }
}
---

# UnitV Vision

The rover has a UnitV2 camera connected via UART.

## Tools

- `unitv_scan` — quick YOLO-based detection, returns detected objects with confidence scores. Fast (~500 ms), no LLM call.
- `unitv_capture` — capture JPEG and analyze with vision LLM. Slow (~5-12 s), detailed description.

## When to Use

| Task | Tool |
|------|------|
| "What's in front of you?" | `unitv_capture` |
| "Is there a cup nearby?" | `unitv_scan` then confirm with `unitv_capture` if uncertain |
| "Find a red object" | `unitv_scan` (check labels) |
| "Describe the scene" | `unitv_capture` |

## Guidelines

- Prefer `unitv_scan` for quick checks during movement.
- Use `unitv_capture` for final confirmation or detailed questions.
- Do not call `unitv_capture` more than 3 times per user turn.
- The camera faces forward and is fixed — rotate the rover to change view.
