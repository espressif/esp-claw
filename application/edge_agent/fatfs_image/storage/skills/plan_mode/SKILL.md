---
{
  "name": "plan_mode",
  "description": "Enter Plan Mode and return an executable plan when a user request cannot be directly satisfied by the current Skills, Tools, firmware configuration, hardware, network, storage, or known parameters.",
  "metadata": {
    "cap_groups": [
      "cap_system"
    ],
    "manage_mode": "readonly"
  }
}
---

# Plan Mode

Use this skill when the user gives a request that cannot be directly completed with the currently available Skills, Tools, hardware capabilities, firmware configuration, network state, storage state, or known parameters.

The purpose of Plan Mode is to return an executable plan. Do not fail without explanation, and do not claim that a physical action, network action, file write, configuration change, scheduled task, or hardware control has happened unless a tool actually performed it successfully.

## Trigger Conditions

Use Plan Mode in these cases:

- The current Skills or Tools are not enough to complete the user's goal directly.
- The request requires a new Skill, Tool, Lua script, router rule, scheduler rule, file, or firmware configuration.
- The user must provide key parameters such as GPIO, target address, trigger condition, save path, execution frequency, retry policy, or network permission.
- The request involves hardware, peripherals, network, files, screen, sensors, actuators, power control, door locks, motors, relays, or similar resources, but current capabilities or hardware information are insufficient.
- The current firmware configuration, RAM, Flash, PSRAM, filesystem, Wi-Fi, BLE, camera, display, audio, GPIO, ADC, PWM, time sync, network, or available storage may be insufficient.

## Required Capability Assessment

Before writing the Plan Mode response, call `assess_agent_capabilities`.

If the request involves concrete onboard hardware, external hardware, occupied GPIOs, sensors, actuators, display, audio, camera, or power control, also activate and read `board_hardware_info` when it is present in the Skills List. If that skill is not present, explicitly state that the board hardware inventory is unknown.

If the request depends on live network state, Wi-Fi state, system state, time sync, or storage state, call `get_system_info` or `get_wifi_info` as needed.

## Plan Mode Response Structure

Reply with the following structure. Keep it concise and executable.

### Current Understanding

- State what task the user wants to complete.
- Identify the task input, output, trigger condition, and execution frequency. If the user did not provide a field, mark it as unknown.
- State whether the task involves peripherals, network, files, screen, sensors, actuators, time, storage, or external services.

### Current Capability Assessment

- State whether the current Skills and Tools cover the task.
- State whether the current hardware has the required capability. The answer may be sufficient, insufficient, unknown, or not applicable.
- Mention firmware support only when relevant, such as Wi-Fi, BLE, filesystem, camera, display, audio, GPIO, ADC, PWM, RAM, Flash, PSRAM, available network, time sync, and storage space.
- Give one explicit conclusion:
  - Can complete directly.
  - Can complete partially.
  - Needs more information before completion.
  - Needs a new Skill or Tool before completion.
  - Needs new or changed hardware before completion.
  - Current hardware cannot complete it.

### Missing Information

- Ask only for information required for the next step. Avoid asking too many questions at once.
- Prefer at most three questions.
- Common missing fields include:
  - Which GPIO to use.
  - Target device address, URL, topic, MAC, IP, or account.
  - Trigger condition.
  - Save path.
  - Execution interval.
  - Whether to retry after failure, and how many retries.
  - Whether network access is required.
  - Whether file writes or configuration changes are allowed.
  - Whether scheduler rules or router rules may be created.

### Execution Plan

- Break the task into concrete steps.
- State which existing Skills or Tools will be used.
- State which parts require a new Skill, Tool, Lua script, configuration file, router rule, or scheduler rule.
- State whether the task should be persisted as an automation.
- State whether user confirmation is required before execution. File writes, configuration changes, rule changes, scheduled tasks, and hardware actions require confirmation by default.

### Risks And Limits

- State likely failure causes, such as missing hardware, incorrect wiring, unavailable network, insufficient permission, latency, power consumption, insufficient storage, insufficient memory, reliability issues, or firmware configuration limits.
- For safety-sensitive operations, clearly warn about risk and require user confirmation. Examples include relays, motors, door locks, power control, heaters, pumps, fans, and other actuators.
- Do not underestimate physical-world risk. If hardware inventory, wiring, or load information is unknown, mark it as unknown.

### Next Step

- Give selectable paths, but recommend one path.
- Common paths include:
  - Execute the currently possible part now.
  - Continue after the user provides missing information.
  - Create a new Skill.
  - Change hardware wiring.
  - Change firmware configuration.
  - Abandon the request.

## Style Rules

- Be direct, practical, and execution-oriented.
- Do not overstate certainty. Use "unknown" when the capability snapshot or hardware information does not prove something.
- For ordinary user-facing replies, prefer Skill names or natural language instead of exposing internal capability group names. Internal capability names may be mentioned when the user is asking as a developer.
- If the user only needs to provide a few missing fields, focus the response on those fields and the recommended next step instead of producing a long plan.
