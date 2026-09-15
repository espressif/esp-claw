#!/usr/bin/env python3
"""
bridge.py — ESP-Claw Arduino UNO Serial Bridge

Connects to an Arduino UNO running the arduino_claw_uno firmware
and exposes its hardware capabilities as callable Python functions.

This bridge can be used:
  1. Standalone — interactive CLI to control the UNO from your terminal
  2. As a library — import ArduinoClawBridge in your own scripts
  3. As an MCP proxy — expose UNO hardware as MCP tools for ESP-Claw

Usage:
    python bridge.py                    # Auto-detect port
    python bridge.py --port /dev/ttyACM0
    python bridge.py --port COM3        # Windows
    python bridge.py --interactive      # Interactive CLI mode

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import sys
import time
import glob

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial is required. Install with: pip install pyserial")
    sys.exit(1)


class ArduinoClawBridge:
    """
    Serial bridge to an Arduino UNO running arduino_claw_uno firmware.

    Provides high-level Python methods that map to ESP-Claw capability
    concepts (GPIO, ADC, PWM, Servo, I2C).
    """

    def __init__(self, port=None, baud=115200, timeout=2.0):
        """
        Initialize the bridge.

        Args:
            port: Serial port (e.g., '/dev/ttyACM0', 'COM3').
                  If None, auto-detects Arduino UNO.
            baud: Baud rate (default 115200, must match firmware).
            timeout: Serial read timeout in seconds.
        """
        self.baud = baud
        self.timeout = timeout
        self.port = port or self._auto_detect_port()
        self.ser = None
        self.board_info = None

    def _auto_detect_port(self):
        """Auto-detect Arduino UNO serial port."""
        ports = serial.tools.list_ports.comports()
        for p in ports:
            desc = (p.description or "").lower()
            vid = p.vid or 0
            # Arduino UNO typically uses ATmega16U2 USB-to-Serial
            # VID:PID = 2341:0043 (official) or 1A86:7523 (CH340 clone)
            if vid == 0x2341 or vid == 0x1A86:
                return p.device
            if "arduino" in desc or "ch340" in desc or "uno" in desc:
                return p.device

        # Fallback: try common paths
        candidates = glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")
        if candidates:
            return candidates[0]

        raise RuntimeError(
            "No Arduino UNO detected. Specify port with --port.\n"
            f"Available ports: {[p.device for p in ports]}"
        )

    def connect(self):
        """Open serial connection and verify the Arduino responds."""
        print(f"Connecting to {self.port} at {self.baud} baud...")
        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)

        # Arduino resets on serial open — wait for bootloader
        time.sleep(2.0)

        # Flush any startup messages
        self.ser.reset_input_buffer()

        # Identify the board
        self.board_info = self.identify()
        board = self.board_info.get("board", "unknown")
        chip = self.board_info.get("chip", "unknown")
        caps = self.board_info.get("caps", [])
        print(f"Connected: {board} ({chip})")
        print(f"Capabilities: {', '.join(caps)}")
        return self.board_info

    def disconnect(self):
        """Close serial connection."""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("Disconnected.")

    def _send(self, cmd_dict):
        """Send a JSON command and return the parsed response."""
        if not self.ser or not self.ser.is_open:
            raise RuntimeError("Not connected. Call connect() first.")

        msg = json.dumps(cmd_dict, separators=(",", ":")) + "\n"
        self.ser.write(msg.encode("ascii"))
        self.ser.flush()

        # Read response line
        line = self.ser.readline().decode("ascii", errors="replace").strip()
        if not line:
            raise TimeoutError(f"No response for command: {cmd_dict}")

        try:
            resp = json.loads(line)
        except json.JSONDecodeError:
            raise RuntimeError(f"Invalid response: {line}")

        if isinstance(resp, dict) and resp.get("ok") is False:
            raise RuntimeError(f"Arduino error: {resp.get('e', 'unknown')}")

        return resp

    # ───────────────── Identity & Heartbeat ─────────────────

    def identify(self):
        """Get board identification."""
        return self._send({"c": "id"})

    def ping(self):
        """Ping the board (heartbeat check)."""
        return self._send({"c": "pi"})

    # ───────────────── GPIO (maps to lua_module_gpio) ─────────────────

    def pin_mode(self, pin, mode="out"):
        """
        Set pin mode.

        Args:
            pin: Digital pin number (2-13, A0-A5)
            mode: "in", "out", or "pu" (input_pullup)
        """
        return self._send({"c": "pm", "p": pin, "m": mode})

    def digital_write(self, pin, value):
        """
        Write digital value to pin.

        Args:
            pin: Digital pin number
            value: 0 (LOW) or 1 (HIGH)
        """
        return self._send({"c": "dw", "p": pin, "v": int(bool(value))})

    def digital_read(self, pin):
        """
        Read digital value from pin.

        Returns: 0 or 1
        """
        resp = self._send({"c": "dr", "p": pin})
        return resp.get("v", 0)

    # ───────────────── Analog (maps to lua_module_adc + mcpwm) ─────────────────

    def analog_read(self, pin):
        """
        Read analog value (10-bit ADC, 0-1023).

        Args:
            pin: Analog pin (0-5 for A0-A5)
        """
        resp = self._send({"c": "ar", "p": pin})
        return resp.get("v", 0)

    def pwm_write(self, pin, value):
        """
        Write PWM value (8-bit, 0-255).

        Args:
            pin: PWM-capable pin (3, 5, 6, 9, 10, 11)
            value: Duty cycle 0-255
        """
        return self._send({"c": "pw", "p": pin, "v": value})

    # ───────────────── Servo ─────────────────

    def servo_attach(self, pin):
        """Attach a servo to a pin."""
        return self._send({"c": "sa", "p": pin})

    def servo_write(self, pin, angle):
        """
        Write angle to servo.

        Args:
            pin: Pin the servo is attached to
            angle: 0-180 degrees
        """
        return self._send({"c": "sv", "p": pin, "v": angle})

    def servo_read(self, pin):
        """Read current servo angle."""
        resp = self._send({"c": "sr", "p": pin})
        return resp.get("v", 0)

    def servo_detach(self, pin):
        """Detach servo from pin."""
        return self._send({"c": "sd", "p": pin})

    # ───────────────── I2C (maps to lua_module_i2c) ─────────────────

    def i2c_write(self, addr, data):
        """
        Write bytes to an I2C device.

        Args:
            addr: 7-bit I2C address (1-127)
            data: bytes or list of ints to write
        """
        if isinstance(data, (bytes, bytearray)):
            hex_str = data.hex().upper()
        else:
            hex_str = "".join(f"{b:02X}" for b in data)
        return self._send({"c": "i2w", "a": addr, "d": hex_str})

    def i2c_read(self, addr, count):
        """
        Read bytes from an I2C device.

        Args:
            addr: 7-bit I2C address
            count: Number of bytes to read (1-16)

        Returns: bytes
        """
        resp = self._send({"c": "i2r", "a": addr, "n": count})
        hex_str = resp.get("d", "")
        return bytes.fromhex(hex_str)

    def i2c_scan(self):
        """
        Scan the I2C bus for devices.

        Returns: list of discovered I2C addresses
        """
        resp = self._send({"c": "i2s"})
        return resp.get("devs", [])


# ═══════════════════ Interactive CLI ═══════════════════

def run_interactive(bridge):
    """Run an interactive command-line session."""
    print("\n╔══════════════════════════════════════════════════════╗")
    print("║     ESP-Claw Arduino UNO — Interactive Bridge       ║")
    print("╠══════════════════════════════════════════════════════╣")
    print("║  Commands:                                          ║")
    print("║    id             — Board identification            ║")
    print("║    ping           — Heartbeat check                 ║")
    print("║    mode PIN MODE  — Set pin mode (in/out/pu)        ║")
    print("║    dw PIN VAL     — Digital write (0 or 1)          ║")
    print("║    dr PIN         — Digital read                    ║")
    print("║    ar PIN         — Analog read (0-1023)            ║")
    print("║    pw PIN VAL     — PWM write (0-255)               ║")
    print("║    sa PIN         — Servo attach                    ║")
    print("║    sv PIN ANGLE   — Servo write (0-180)             ║")
    print("║    sr PIN         — Servo read                      ║")
    print("║    sd PIN         — Servo detach                    ║")
    print("║    i2s            — I2C scan                        ║")
    print("║    raw JSON       — Send raw JSON command           ║")
    print("║    quit           — Exit                            ║")
    print("╚══════════════════════════════════════════════════════╝\n")

    while True:
        try:
            line = input("claw-uno> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue

        parts = line.split()
        cmd = parts[0].lower()

        try:
            if cmd == "quit" or cmd == "exit":
                break
            elif cmd == "id":
                print(json.dumps(bridge.identify(), indent=2))
            elif cmd == "ping":
                bridge.ping()
                print("OK — board is alive")
            elif cmd == "mode" and len(parts) >= 3:
                bridge.pin_mode(int(parts[1]), parts[2])
                print("OK")
            elif cmd == "dw" and len(parts) >= 3:
                bridge.digital_write(int(parts[1]), int(parts[2]))
                print("OK")
            elif cmd == "dr" and len(parts) >= 2:
                val = bridge.digital_read(int(parts[1]))
                print(f"Value: {val}")
            elif cmd == "ar" and len(parts) >= 2:
                val = bridge.analog_read(int(parts[1]))
                print(f"Value: {val} ({val * 5.0 / 1023:.2f}V)")
            elif cmd == "pw" and len(parts) >= 3:
                bridge.pwm_write(int(parts[1]), int(parts[2]))
                print("OK")
            elif cmd == "sa" and len(parts) >= 2:
                bridge.servo_attach(int(parts[1]))
                print("OK")
            elif cmd == "sv" and len(parts) >= 3:
                bridge.servo_write(int(parts[1]), int(parts[2]))
                print("OK")
            elif cmd == "sr" and len(parts) >= 2:
                angle = bridge.servo_read(int(parts[1]))
                print(f"Angle: {angle}°")
            elif cmd == "sd" and len(parts) >= 2:
                bridge.servo_detach(int(parts[1]))
                print("OK")
            elif cmd == "i2s":
                devs = bridge.i2c_scan()
                if devs:
                    addrs = ", ".join(f"0x{a:02X}" for a in devs)
                    print(f"Found {len(devs)} device(s): {addrs}")
                else:
                    print("No I2C devices found.")
            elif cmd == "raw" and len(parts) >= 2:
                raw_json = " ".join(parts[1:])
                resp = bridge._send(json.loads(raw_json))
                print(json.dumps(resp, indent=2))
            else:
                print(f"Unknown command: {line}")
                print("Type 'quit' to exit, or see the command list above.")
        except Exception as e:
            print(f"Error: {e}")


# ═══════════════════ Main Entry Point ═══════════════════

def main():
    parser = argparse.ArgumentParser(
        description="ESP-Claw Arduino UNO Serial Bridge"
    )
    parser.add_argument(
        "--port", "-p",
        help="Serial port (e.g., /dev/ttyACM0, COM3). Auto-detects if omitted."
    )
    parser.add_argument(
        "--baud", "-b", type=int, default=115200,
        help="Baud rate (default: 115200)"
    )
    parser.add_argument(
        "--interactive", "-i", action="store_true",
        help="Start interactive CLI mode"
    )
    parser.add_argument(
        "--cmd", "-c",
        help="Send a single command and exit (e.g., '{\"c\":\"id\"}')"
    )

    args = parser.parse_args()

    bridge = ArduinoClawBridge(port=args.port, baud=args.baud)

    try:
        bridge.connect()
    except Exception as e:
        print(f"Connection failed: {e}")
        sys.exit(1)

    try:
        if args.cmd:
            # Single command mode
            resp = bridge._send(json.loads(args.cmd))
            print(json.dumps(resp, indent=2))
        elif args.interactive:
            run_interactive(bridge)
        else:
            # Default: interactive mode
            run_interactive(bridge)
    except KeyboardInterrupt:
        print("\nInterrupted.")
    finally:
        bridge.disconnect()


if __name__ == "__main__":
    main()
