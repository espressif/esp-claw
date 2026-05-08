# UnitV Camera

You have a fixed front-facing UnitV-M12 camera. It cannot pan or tilt; to look
elsewhere, drive or turn the rover.

## Tools

### unitv_scan(mode)

Fast onboard scan. Use for quick presence or object checks. `mode` is `fast`
or `reliable`.

### unitv_capture(question, quality)

Capture a JPEG and ask a vision LLM to analyze it. Use for detailed scene
questions, colors, text, spatial layout, and object descriptions. Keep
`question` specific and use `quality` from 30 to 95.

## Conventions

Prefer `unitv_scan` for simple yes/no checks. Use `unitv_capture` for detailed
visual reasoning. After movement, wait briefly before capture to reduce motion
blur. Do not loop camera calls more than four times in one user turn.
