# Long-term Memory

Use on-device long-term memory tools to remember, recall, list, update, and forget structured memories.

## When to use
Use this skill when the user explicitly asks to remember, save, keep, or not forget something, asks what is remembered, asks to update or delete a remembered item, or asks for an answer that should be based on prior long-term memory.

## Hard Rules
1. If the user clearly asks to remember, save, keep, or not forget something, call `memory_store`.
2. Write `memory_store.content` as a concise normalized memory fact, not the user's raw quote.
3. Do not write raw user quotes into memory content.
4. Do not exceed the maximum number of retrieval terms.
5. Keep retrieval terms in the same language as the memory fact unless the memory itself is primarily in another language.
6. If the user asks what you remember, asks to verify a remembered fact, or asks for a personalized answer based on prior memory, inspect the injected summary labels first and call `memory_recall` when relevant labels are present.
7. Do not place natural-language questions into `summary_labels`.
8. `memory_recall` requires exact `summary_labels` chosen from the injected catalog.
9. Use `query` only to narrow the search within the selected summary labels.
10. Use `memory_list` when the user wants to inspect stored memories.
11. Use `memory_update` only when one existing memory should be modified.
12. Use `memory_forget` only when one existing memory should be removed.
13. Use `memory_recall` plus exact `memory_id` only when you are already doing an explicit memory-inspection or memory-editing flow and need to inspect the recalled memory bodies yourself.
14. Do not use non-whitelisted or free-text values as summary labels.
15. Do not read or write `memory_records.jsonl`, `memory_index.json`, `memory_digest.log`, or `MEMORY.md` to make decisions directly.
16. Summary labels are not the memory body. Use `memory_recall` to obtain detailed stored content.
17. Do not call `memory_store` for ordinary self-introductions or casual preference statements unless the user explicitly asks to save them. Let automatic extraction handle durable facts after the reply silently.
18. Do not make the whole reply an operation log such as “I have remembered” or “deleted” unless the user explicitly asked only for a memory operation.
19. Do not explain internal memory policy, auto-extraction behavior, or whether you will proactively remember something unless the user explicitly asks about memory behavior.
20. Do not ask whether the user wants you to remember ordinary profile or preference statements when automatic extraction can handle them. Do not offer memory-save help unless the user explicitly asks about memory management.
21. Do not answer long-term memory recall questions from session history alone when long-term memory may contain additional relevant items.
22. Do not claim that a memory was updated or forgotten unless the corresponding tool call succeeded.

## Soft Rules
1. Prefer stable summary labels that group a reusable topic.
2. Good labels include `schedule`, `dietary_preferences`, `daily_routine`, `hydration_habits`, `commute`.
3. When a stable category label clearly fits the memory, prefer it over one-off fact-instance words.
4. Avoid using one-off detail words as the only summary label when a broader reusable category exists.
5. Avoid using one-off detail words as summary labels when a stable topic label already fits.
6. For example, prefer `daily_routine` over `nap`, and `hydration_habits` over `8_glasses_of_water`.
7. Avoid replacing a clearly supported stable topic label with a broader or vaguer abstraction.
8. Avoid labels such as `development`, `coding`, `healthy_lifestyle`, or `daily_arrangement` when a more precise stable label already fits better.
9. If you use memory tools during a conversation, prefer keeping the main reply focused on the user's request first.
10. If the user combines a memory request with another conversational request, prefer answering the conversational request in the main reply and keeping the memory operation secondary.

## Typical flow
- Explicit save request: call `memory_store`, then continue the main conversation naturally.
- Recall or verification request: inspect summary labels, call `memory_recall` if relevant labels exist, then answer from recalled content.
- Update or delete request: use `memory_recall` or `memory_list` to inspect the target memory and obtain its exact `memory_id`, then call `memory_update` or `memory_forget`.

## Examples
- A good summary label is `daily_routine`.
- A good summary label is `hydration_habits`.
- Save a profile fact explicitly: if the user says “Remember my profession is a programmer”, store `content="The user's profession is a programmer"` and use the summary label `profession`.
- Save a preference explicitly: if the user says “Remember I like lettuce”, store `content="The user likes lettuce"` and use the summary label `dietary_preferences`.
- Save a routine fact explicitly: if the user says “Remember I wake up at 7 and drink milk before going to work”, use summary labels such as `daily_routine`, `dietary_preferences`, and `commute`.
- Save a midday habit explicitly: if the user says “Remember I must take a nap at noon”, a suitable summary label is `daily_routine`.
- Save a hydration preference explicitly: if the user says “Remember I drink 8 glasses of water a day”, a suitable summary label is `hydration_habits`.
- Recall from summary labels: if the injected label catalog contains `dietary_preferences`, questions like “What do I like to eat?” or “What do I like to drink?” should recall that label before answering.
- Update or forget safely: if the user says “Forget that I like apples”, use `memory_recall` or `memory_list` to find the exact `memory_id` first, then call `memory_forget` with that `memory_id`.
