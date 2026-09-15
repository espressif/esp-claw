#!/usr/bin/env python3
"""
ai_shell.py — Natural Language Arduino Programmer

Talk to your Arduino in plain English. An LLM translates your words
into hardware commands and executes them in real-time.

This is the Arduino equivalent of ESP-Claw's "Chat Coding" — define
device behavior through conversation, not code.

Usage:
    python ai_shell.py                          # Auto-detect Arduino
    python ai_shell.py --port /dev/ttyACM0      # Specify port
    python ai_shell.py --model gpt-4o           # Choose model

Requires:
    - An Arduino UNO flashed with arduino_claw_uno firmware
    - An OpenAI API key (set OPENAI_API_KEY env variable)
      OR an Anthropic API key (set ANTHROPIC_API_KEY)

Examples:
    > turn on the LED
    > read the temperature sensor on A0
    > sweep a servo on pin 9 from 0 to 180 degrees
    > blink pin 13 five times quickly
    > scan for I2C devices and tell me what's connected
    > set up a traffic light on pins 4, 5, 6

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import os
import sys
import time
import re

from bridge import ArduinoClawBridge

# ═══════════════════ LLM Backend ═══════════════════

# Try to import LLM libraries — support both OpenAI and Anthropic
_llm_backend = None

try:
    import openai
    _llm_backend = "openai"
except ImportError:
    pass

if not _llm_backend:
    try:
        import anthropic
        _llm_backend = "anthropic"
    except ImportError:
        pass


# ═══════════════════ System Prompt ═══════════════════

SYSTEM_PROMPT = """You are an Arduino UNO controller. The user describes what they want in plain English, and you execute it by calling the available tools.

## Hardware You Control
- Arduino UNO R3 (ATmega328P, 16MHz, 5V logic)
- Connected via USB serial at 115200 baud

## Available Pins
- Digital I/O: D2–D13 (D0/D1 reserved for serial)
- PWM output: D3, D5, D6, D9, D10, D11 (8-bit: 0–255)
- Analog input: A0–A5 (10-bit ADC: 0–1023, 0V–5V)
- I2C: A4 (SDA), A5 (SCL)
- Built-in LED: D13

## Rules
1. Always set pinMode before reading/writing a pin for the first time.
2. For analog read, pass pin 0–5 (meaning A0–A5).
3. PWM values are 0–255 (0=off, 255=full).
4. Servo angles are 0–180 degrees. Always attach before writing.
5. For multi-step sequences (like blinking), execute the steps one at a time using the tools.
6. When the user says "LED" without specifying a pin, they mean pin 13 (built-in LED).
7. Convert voltage requests: value = voltage * 1023 / 5.0 for ADC, or voltage * 255 / 5.0 for PWM.
8. Be conversational — explain what you're doing in simple terms.
9. If something fails, explain what went wrong and suggest a fix.
10. For repeating actions (like blinking), do a reasonable number of repetitions (3–5) unless specified.
"""

# ═══════════════════ Tool Definitions ═══════════════════

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "pin_mode",
            "description": "Set a pin's mode. Must be called before reading or writing a pin.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Pin number (2-13 for digital, 14-19 for A0-A5)"},
                    "mode": {"type": "string", "enum": ["out", "in", "pu"], "description": "out=OUTPUT, in=INPUT, pu=INPUT_PULLUP"}
                },
                "required": ["pin", "mode"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "digital_write",
            "description": "Set a digital pin HIGH (1) or LOW (0). Pin must be set to output mode first.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Pin number (2-13)"},
                    "value": {"type": "integer", "enum": [0, 1], "description": "0=LOW, 1=HIGH"}
                },
                "required": ["pin", "value"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "digital_read",
            "description": "Read the digital value (0 or 1) from a pin.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Pin number (2-19)"}
                },
                "required": ["pin"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "analog_read",
            "description": "Read the analog value (0-1023) from an analog pin. Returns 10-bit ADC value. 0=0V, 1023=5V.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Analog pin number: 0=A0, 1=A1, ..., 5=A5"}
                },
                "required": ["pin"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "pwm_write",
            "description": "Write a PWM value (0-255) to a PWM-capable pin. Only pins 3, 5, 6, 9, 10, 11 support PWM.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "PWM pin (3, 5, 6, 9, 10, or 11)"},
                    "value": {"type": "integer", "description": "PWM duty cycle 0-255 (0=off, 255=full)"}
                },
                "required": ["pin", "value"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "servo_attach",
            "description": "Attach a servo motor to a pin. Must call before servo_write.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Pin to attach servo to"}
                },
                "required": ["pin"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "servo_write",
            "description": "Set servo angle. Servo must be attached first.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Pin the servo is attached to"},
                    "angle": {"type": "integer", "description": "Angle in degrees (0-180)"}
                },
                "required": ["pin", "angle"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "servo_read",
            "description": "Read the current angle of a servo.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Pin the servo is attached to"}
                },
                "required": ["pin"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "servo_detach",
            "description": "Detach a servo from a pin, freeing the pin for other use.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pin": {"type": "integer", "description": "Pin to detach servo from"}
                },
                "required": ["pin"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "i2c_scan",
            "description": "Scan the I2C bus and return a list of found device addresses.",
            "parameters": {
                "type": "object",
                "properties": {},
                "required": []
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "i2c_write",
            "description": "Write bytes to an I2C device.",
            "parameters": {
                "type": "object",
                "properties": {
                    "address": {"type": "integer", "description": "7-bit I2C address (1-127)"},
                    "data": {"type": "array", "items": {"type": "integer"}, "description": "Bytes to write (0-255 each)"}
                },
                "required": ["address", "data"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "i2c_read",
            "description": "Read bytes from an I2C device.",
            "parameters": {
                "type": "object",
                "properties": {
                    "address": {"type": "integer", "description": "7-bit I2C address (1-127)"},
                    "count": {"type": "integer", "description": "Number of bytes to read (1-16)"}
                },
                "required": ["address", "count"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "delay_ms",
            "description": "Wait for a specified number of milliseconds before the next action. Use for timing in sequences like blinking.",
            "parameters": {
                "type": "object",
                "properties": {
                    "ms": {"type": "integer", "description": "Milliseconds to wait"}
                },
                "required": ["ms"]
            }
        }
    },
]

# Anthropic tool format
TOOLS_ANTHROPIC = []
for t in TOOLS:
    f = t["function"]
    TOOLS_ANTHROPIC.append({
        "name": f["name"],
        "description": f["description"],
        "input_schema": f["parameters"]
    })


# ═══════════════════ Tool Executor ═══════════════════

class ToolExecutor:
    """Executes LLM tool calls against the Arduino bridge."""

    def __init__(self, bridge):
        self.bridge = bridge

    def execute(self, name, args):
        """Execute a tool call and return the result as a string."""
        try:
            if name == "pin_mode":
                self.bridge.pin_mode(args["pin"], args["mode"])
                mode_name = {"out": "OUTPUT", "in": "INPUT", "pu": "INPUT_PULLUP"}
                return f"Pin {args['pin']} set to {mode_name.get(args['mode'], args['mode'])}"

            elif name == "digital_write":
                self.bridge.digital_write(args["pin"], args["value"])
                state = "HIGH" if args["value"] else "LOW"
                return f"Pin {args['pin']} → {state}"

            elif name == "digital_read":
                val = self.bridge.digital_read(args["pin"])
                return f"Pin {args['pin']} = {val} ({'HIGH' if val else 'LOW'})"

            elif name == "analog_read":
                val = self.bridge.analog_read(args["pin"])
                voltage = val * 5.0 / 1023
                return f"A{args['pin']} = {val} ({voltage:.2f}V)"

            elif name == "pwm_write":
                self.bridge.pwm_write(args["pin"], args["value"])
                pct = args["value"] * 100 / 255
                return f"Pin {args['pin']} PWM = {args['value']} ({pct:.0f}%)"

            elif name == "servo_attach":
                self.bridge.servo_attach(args["pin"])
                return f"Servo attached on pin {args['pin']}"

            elif name == "servo_write":
                self.bridge.servo_write(args["pin"], args["angle"])
                return f"Servo on pin {args['pin']} → {args['angle']}°"

            elif name == "servo_read":
                angle = self.bridge.servo_read(args["pin"])
                return f"Servo on pin {args['pin']} = {angle}°"

            elif name == "servo_detach":
                self.bridge.servo_detach(args["pin"])
                return f"Servo detached from pin {args['pin']}"

            elif name == "i2c_scan":
                devs = self.bridge.i2c_scan()
                if devs:
                    addrs = ", ".join(f"0x{a:02X}" for a in devs)
                    return f"Found {len(devs)} I2C device(s): {addrs}"
                return "No I2C devices found on the bus."

            elif name == "i2c_write":
                self.bridge.i2c_write(args["address"], args["data"])
                return f"Wrote {len(args['data'])} byte(s) to I2C 0x{args['address']:02X}"

            elif name == "i2c_read":
                data = self.bridge.i2c_read(args["address"], args["count"])
                hex_str = " ".join(f"0x{b:02X}" for b in data)
                return f"Read from I2C 0x{args['address']:02X}: [{hex_str}]"

            elif name == "delay_ms":
                ms = args["ms"]
                time.sleep(ms / 1000.0)
                return f"Waited {ms}ms"

            else:
                return f"Unknown tool: {name}"

        except Exception as e:
            return f"Error: {e}"


# ═══════════════════ LLM Conversation Loop ═══════════════════

def run_openai_loop(user_input, messages, executor, model):
    """Run one turn of conversation using OpenAI API."""
    client = openai.OpenAI()

    messages.append({"role": "user", "content": user_input})

    while True:
        response = client.chat.completions.create(
            model=model,
            messages=messages,
            tools=TOOLS,
            tool_choice="auto",
        )

        msg = response.choices[0].message
        messages.append(msg.to_dict())

        # If no tool calls, we're done — print the response
        if not msg.tool_calls:
            if msg.content:
                print(f"\n🤖 {msg.content}\n")
            return

        # Execute tool calls
        for tc in msg.tool_calls:
            fn_name = tc.function.name
            fn_args = json.loads(tc.function.arguments)

            print(f"  ⚡ {fn_name}({json.dumps(fn_args, separators=(',', ':'))})")
            result = executor.execute(fn_name, fn_args)
            print(f"     → {result}")

            messages.append({
                "role": "tool",
                "tool_call_id": tc.id,
                "content": result,
            })


def run_anthropic_loop(user_input, messages, executor, model):
    """Run one turn of conversation using Anthropic API."""
    client = anthropic.Anthropic()

    messages.append({"role": "user", "content": user_input})

    while True:
        response = client.messages.create(
            model=model,
            max_tokens=4096,
            system=SYSTEM_PROMPT,
            messages=messages,
            tools=TOOLS_ANTHROPIC,
        )

        # Build the assistant message content
        assistant_content = response.content
        messages.append({"role": "assistant", "content": assistant_content})

        # Check if we have tool use blocks
        tool_uses = [b for b in assistant_content if b.type == "tool_use"]

        # Print any text blocks
        for block in assistant_content:
            if hasattr(block, "text") and block.text:
                print(f"\n🤖 {block.text}\n")

        if not tool_uses:
            return

        # Execute tool calls and collect results
        tool_results = []
        for tu in tool_uses:
            fn_name = tu.name
            fn_args = tu.input

            print(f"  ⚡ {fn_name}({json.dumps(fn_args, separators=(',', ':'))})")
            result = executor.execute(fn_name, fn_args)
            print(f"     → {result}")

            tool_results.append({
                "type": "tool_result",
                "tool_use_id": tu.id,
                "content": result,
            })

        messages.append({"role": "user", "content": tool_results})


# ═══════════════════ Main Shell ═══════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Natural Language Arduino Programmer — Talk to your Arduino in English"
    )
    parser.add_argument("--port", "-p", help="Serial port for Arduino")
    parser.add_argument("--baud", "-b", type=int, default=115200)
    parser.add_argument(
        "--model", "-m",
        help="LLM model to use (default: auto-detect based on API key)"
    )
    parser.add_argument(
        "--backend",
        choices=["openai", "anthropic"],
        help="Force a specific LLM backend"
    )
    args = parser.parse_args()

    # ── Detect LLM backend ──
    backend = args.backend
    model = args.model

    if not backend:
        if os.environ.get("OPENAI_API_KEY"):
            backend = "openai"
        elif os.environ.get("ANTHROPIC_API_KEY"):
            backend = "anthropic"
        elif _llm_backend:
            backend = _llm_backend
        else:
            print("❌ No LLM API key found.")
            print("   Set OPENAI_API_KEY or ANTHROPIC_API_KEY environment variable.")
            print("   Install the client: pip install openai  OR  pip install anthropic")
            sys.exit(1)

    if not model:
        model = "gpt-4o" if backend == "openai" else "claude-sonnet-4-20250514"

    print(f"🧠 Using {backend} ({model})")

    # ── Connect to Arduino ──
    bridge = ArduinoClawBridge(port=args.port, baud=args.baud)
    try:
        bridge.connect()
    except Exception as e:
        print(f"❌ Arduino connection failed: {e}")
        sys.exit(1)

    executor = ToolExecutor(bridge)

    # ── Conversation state ──
    if backend == "openai":
        messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    else:
        messages = []  # Anthropic uses system param separately

    # ── Main loop ──
    print()
    print("╔══════════════════════════════════════════════════════════╗")
    print("║   🔌 Arduino Natural Language Programmer                ║")
    print("║                                                         ║")
    print("║   Just describe what you want in plain English.         ║")
    print("║   Examples:                                             ║")
    print("║     • turn on the LED                                   ║")
    print("║     • blink pin 13 three times                          ║")
    print("║     • read the sensor on A0                             ║")
    print("║     • move the servo to 90 degrees                      ║")
    print("║     • set up a traffic light on pins 4, 5, 6            ║")
    print("║     • fade an LED in and out on pin 9                   ║")
    print("║                                                         ║")
    print("║   Type 'quit' to exit.                                  ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()

    while True:
        try:
            user_input = input("You: ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not user_input:
            continue
        if user_input.lower() in ("quit", "exit", "q"):
            break

        try:
            if backend == "openai":
                run_openai_loop(user_input, messages, executor, model)
            else:
                run_anthropic_loop(user_input, messages, executor, model)
        except Exception as e:
            print(f"\n❌ Error: {e}\n")

    bridge.disconnect()
    print("Goodbye! 👋")


if __name__ == "__main__":
    main()
