---
{
  "name": "skills_lab_search",
  "description": "Search skills from the ESP-Skill hub by keywords and advanced filters, then guide the user to download a selected skill with skills_lab_downloader.",
  "metadata": {
    "cap_groups": [
      "cap_http_request",
      "cap_boards"
    ],
    "manage_mode": "web"
  }
}
---

# Skills Lab Search

Use this skill when the user wants to search or discover skills from the ESP-Claw's Skill hub.

## When to use
- The user wants to search for skills from the ESP-Claw's Skill hub.
- The user provides a topic, feature, peripheral, tag, or category and wants matching skill candidates.
- The user does not know the exact skill id needed by `skills_lab_downloader`.

## When not to use
- Do not use this skill for Codex pet downloads, pet ids, pet selection, or
  `codex-pets.net` links. Those belong to the app pet manager skill, not the
  ESP-Claw Skill hub.
- If the user says "下载 pet", "下载 messi", "安装这个宠物", or sends a
  `https://codex-pets.net/#/pets/...` link, do not search Skills Lab.

## Hard rules
1. Before searching, make sure `cap_web_search` is enabled and available. If it is unavailable, stop and tell the user to enable web search first.
2. Prefer English keywords for better search results unless the user provides a specific query.
3. Always URL encode the full query string before appending it to `https://skills-lab.esp-claw.com/api/search?q=`.
4. Use the search API endpoint directly. Do not scrape the web UI page for results.
5. If the user asks which tags, peripherals, or categories are available, fetch `https://skills-lab.esp-claw.com/raw/tags.json` and summarize the relevant values.
6. Preserve advanced search syntax exactly before URL encoding: `t:"tag"`, `p:"peripheral"`, and `c:"category"`.
7. Use `featured=true` when the user wants to filter results to featured skills only.
8. If the user asks to list all available skills, request featured skills only with `featured=true` and no `q` parameter, present those featured skills, and ask the user for more specific search criteria.
9. After presenting results, remind the user that installation requires the exact skill id and can be done with `skills_lab_downloader`.

## Search syntax
- Free text keywords: `bilibili fans`
- Tag filter: `t:"info"`
- Peripheral filter: `p:"display"`
- Category filter: `c:"utility"`
- Filters can be combined with free text keywords: `bilibili fans t:"info"`
- A filter-only query is valid: `t:"info"`
- Featured-only filter: `featured=true`

Available `tag`, `peripheral`, and `category` values can be checked at:

```text
https://skills-lab.esp-claw.com/raw/tags.json
```

## Workflow
1. If the user asks to list all available skills, request featured skills only with no `q` parameter:

```text
https://skills-lab.esp-claw.com/api/search?featured=true
```

2. Present the featured skill results, then ask the user for more specific search criteria before doing a broader search.
3. Ask for search keywords if the user did not provide any searchable terms, filters, or a request for featured skills.
4. Build a search query with English keywords when possible.
5. Include advanced filters only when the user asks for them or when they are clearly implied.
6. Add `featured=true` when the user wants featured skills only.
7. URL encode the complete `q` query value when `q` is used.
8. Request the search API:

```text
https://skills-lab.esp-claw.com/api/search?q=<url_encoded_query>
```

9. When using both `q` and `featured=true`, request:

```text
https://skills-lab.esp-claw.com/api/search?q=<url_encoded_query>&featured=true
```

10. Read the returned search results and present the best matches with exact skill ids, titles, and short descriptions when available.
11. If there are no results, suggest a broader query, fewer filters, or checking available values from `tags.json`.
12. If the user chooses a skill to install, activate `skills_lab_downloader` and pass the exact skill id(name).

## Examples
- Query: `bilibili fans`

```text
https://skills-lab.esp-claw.com/api/search?q=bilibili+fans
```

- Query: `bilibili fans t:"info"`

```text
https://skills-lab.esp-claw.com/api/search?q=bilibili+fans+t%3A%22info%22
```

- Query: `t:"info"`

```text
https://skills-lab.esp-claw.com/api/search?q=t%3A%22info%22
```

- Query: `t:"info" quote`

```text
https://skills-lab.esp-claw.com/api/search?q=t%3A%22info%22+quote
```

- Featured-only query: `bilibili fans`

```text
https://skills-lab.esp-claw.com/api/search?q=bilibili+fans&featured=true
```

- List featured skills:

```text
https://skills-lab.esp-claw.com/api/search?featured=true
```

## Notes
- Search results are only candidates. Do not install a result until the user selects the exact skill id.
- Do not guess a skill id from a visible title. Use the exact skill id returned by the search API.
- `skills_lab_downloader` handles metadata checks, peripheral compatibility, and installation.
