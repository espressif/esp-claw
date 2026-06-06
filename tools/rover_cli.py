#!/usr/bin/env python3
"""Wave Rover CLI. Never sends motion commands without --allow-motion."""
import argparse
import json
import sys
import urllib.request
import urllib.error


def mcp_call(host, port, method, params, token=None):
    url = f"http://{host}:{port}/mcp"
    body = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": method, "params": params
    }).encode()
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, data=body, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())
    except urllib.error.URLError as e:
        print(f"Connection error: {e}", file=sys.stderr)
        sys.exit(1)


def tool_call(host, port, tool, arguments, token=None):
    return mcp_call(host, port, "tools/call", {"name": tool, "arguments": arguments},
                    token=token)


def pretty(resp):
    if "result" in resp:
        print(json.dumps(resp["result"], indent=2))
    elif "error" in resp:
        print(f"ERROR: {resp['error']}", file=sys.stderr)
        sys.exit(1)
    else:
        print(json.dumps(resp, indent=2))


def main():
    p = argparse.ArgumentParser(description="Wave Rover CLI")
    p.add_argument("--host",  default="wave-rover.local")
    p.add_argument("--port",  type=int, default=8080)
    p.add_argument("--token", default=None,
                   help="Bearer token (required when auth_enabled=true on device)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status",         help="Get rover status")
    sub.add_parser("power",          help="Get INA219 power status")
    sub.add_parser("imu",            help="Get IMU data")
    sub.add_parser("display-status", help="Refresh OLED status display")

    stp = sub.add_parser("stop", help="Stop all motors")
    stp.add_argument("--reason", default="cli")

    est = sub.add_parser("emergency-stop", help="Set emergency stop flag")
    est.add_argument("--reason", default="cli")

    sub.add_parser("clear-estop",
                   help="Clear emergency stop flag (requires confirmation)")

    mv = sub.add_parser("move", help="Move rover (linear + angular)")
    mv.add_argument("--linear",       type=float, default=0.0)
    mv.add_argument("--angular",      type=float, default=0.0)
    mv.add_argument("--duration-ms",  type=int,   default=500)
    mv.add_argument("--allow-motion", action="store_true",
                    help="Required to send movement")

    tank = sub.add_parser("tank", help="Tank drive (independent left/right)")
    tank.add_argument("--left",        type=float, default=0.0)
    tank.add_argument("--right",       type=float, default=0.0)
    tank.add_argument("--duration-ms", type=int,   default=500)
    tank.add_argument("--allow-motion", action="store_true",
                      help="Required to send movement")

    args = p.parse_args()
    host, port = args.host, args.port

    if args.cmd == "status":
        pretty(tool_call(host, port, "rover.get_status", {}, token=args.token))
    elif args.cmd == "power":
        pretty(tool_call(host, port, "rover.get_power", {}, token=args.token))
    elif args.cmd == "imu":
        pretty(tool_call(host, port, "rover.get_imu", {}, token=args.token))
    elif args.cmd == "display-status":
        pretty(tool_call(host, port, "rover.display_status", {}, token=args.token))
    elif args.cmd == "stop":
        pretty(tool_call(host, port, "rover.stop", {"reason": args.reason},
                         token=args.token))
    elif args.cmd == "emergency-stop":
        pretty(tool_call(host, port, "rover.emergency_stop",
                         {"reason": args.reason}, token=args.token))
    elif args.cmd == "clear-estop":
        pretty(tool_call(host, port, "rover.clear_emergency_stop",
                         {"confirm": True}, token=args.token))
    elif args.cmd == "move":
        if not args.allow_motion:
            print("ERROR: pass --allow-motion to send movement commands",
                  file=sys.stderr)
            sys.exit(1)
        pretty(tool_call(host, port, "rover.move", {
            "linear":      args.linear,
            "angular":     args.angular,
            "duration_ms": args.duration_ms,
        }, token=args.token))
    elif args.cmd == "tank":
        if not args.allow_motion:
            print("ERROR: pass --allow-motion to send movement commands",
                  file=sys.stderr)
            sys.exit(1)
        pretty(tool_call(host, port, "rover.drive_tank", {
            "left":        args.left,
            "right":       args.right,
            "duration_ms": args.duration_ms,
        }, token=args.token))


if __name__ == "__main__":
    main()
