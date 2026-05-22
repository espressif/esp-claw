# Weather 天气

Get current weather and forecasts through direct capabilities. Support weather, forecast, temperature, 天气, 预报, 温度 queries.

## When to use
When the user asks about weather, temperature, forecasts, 天气, 温度, or 预报.

## How to use
1. Call `get_current_time` with `{}`
2. Call `web_search` with `{"query":"weather in [city] today"}`
3. Extract temperature, conditions, and forecast from results
4. Present in a concise, friendly format

## Example
User: "What's the weather in Tokyo?"
-> `get_current_time` with `{}`
-> `web_search` with `{"query":"weather Tokyo today February 2026"}`
-> "Tokyo: 8C, partly cloudy. High 12C, low 4C. Light wind from the north."
