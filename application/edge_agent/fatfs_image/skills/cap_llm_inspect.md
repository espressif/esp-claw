# Image Inspection

Use this skill when the user wants the device to inspect a local image and describe what is visible.

## When to use
- The user asks what is in an image, photo, screenshot, or camera frame.
- The image already exists on the device filesystem.
- The task needs visual analysis rather than plain file reading.

## Available capability
- `inspect_image`: analyze one local image from an absolute path using a prompt that says what to inspect.

## Calling rules
- Call `inspect_image` directly.
- Always pass an absolute local file path in `path`.
- Always pass a clear `prompt` that tells the model what to look for.
- Confirm or discover the image path first if it is not already known.
- Do not pass remote URLs or non-image files.

## Path guidance
- Prefer real local paths already stored on the device.
- If the exact path is unknown, inspect storage first with file capabilities such as `list_dir`.
- Common roots in this demo include `<storage_root>/inbox`, `<storage_root>`, or other application-managed storage paths.

## Example
```json
{
  "path": "<storage_root>/inbox/photo.jpg",
  "prompt": "Describe the main objects in this image and mention any visible text."
}
```

## Notes
- Keep the prompt specific. For example: identify objects, read visible text, describe a scene, or check whether a target item appears.
- If the image is blurry or uncertain, report that uncertainty instead of over-claiming.
