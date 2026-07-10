---
{
  "name": "cap_web_search",
  "description": "Search the public web for current information through the configured Tavily or Brave provider.",
  "metadata": {
    "cap_groups": [
      "cap_web_search"
    ],
    "manage_mode": "readonly"
  }
}
---

# Web Search

Use this skill when the user needs current public web information that should be fetched through the built-in search capability instead of answered from memory.

## When to use
- The user asks for current news, recent developments, live information, or up-to-date facts.
- The user asks to search the web for a topic, company, product, documentation page, or article.
- The task needs a few likely sources or result links before summarizing.
- The answer would be risky if guessed from stale model knowledge.

## Available capability
- `web_search`: search the web with the configured provider and return concise formatted results.

## Provider behavior
- The runtime prefers `Tavily` when a Tavily API key is configured.
- If Tavily is not configured but Brave Search is configured, it falls back to `Brave`.
- If neither provider key is configured, the capability returns an error instead of search results.

## Calling rules
- Call `web_search` directly. Do not route web search through CLI wrappers unless the user explicitly asks for console commands.
- Input must be a JSON object with one required field:

```json
{
  "query": "your search query"
}
```

- Keep queries short and concrete. Prefer one clear search intent per call.
- If the user asks a compound question, split it into separate searches when needed instead of cramming everything into one long query.
- Use search when freshness matters. Do not use it for stable facts that can be answered locally.

## Output shape
- The capability returns plain text, not structured JSON.
- Results are formatted as a short numbered list.
- Each item typically contains:
  - title
  - URL
  - short snippet/content
- If no result is found, the output is `No web results found.`
- Common error strings include:
  - `Error: no search provider credentials configured`
  - `Error: invalid input JSON`
  - `Error: missing query`
  - `Error: search request failed (...)`
  - `Error: failed to parse search results`

## Recommended workflow
1. Decide whether the user needs fresh web information or a normal answer.
2. Write one focused query.
3. Call `web_search`.
4. Read the returned snippets and URLs.
5. Summarize the findings for the user and mention uncertainty when the results are weak or mixed.

## Common failure causes
- Calling `web_search` with no `query`.
- Using a vague query like `news` or `weather` without the target subject or location.
- Expecting structured JSON output; this capability returns formatted text.
- Assuming the provider is available when no API key is configured.

## Examples

Search for current documentation:

```json
{
  "query": "ESP-IDF mDNS example"
}
```

Search for a recent topic:

```json
{
  "query": "latest Espressif ESP32 AI news"
}
```

Search for a product or company:

```json
{
  "query": "Brave Search API pricing"
}
```
