#!/usr/bin/env python3
"""Wave Rover MCP smoke test. Run against a live device."""
import argparse
import json
import sys
import urllib.request
import urllib.error


def mcp_call(host, port, method, params=None, token=None):
    url = f"http://{host}:{port}/mcp"
    body = json.dumps({
        "jsonrpc": "2.0", "id": 1,
        "method": method, "params": params or {}
    }).encode()
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, data=body, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())
    except urllib.error.URLError as e:
        return {"error": str(e)}


def check(label, resp, key=None):
    ok = "result" in resp and (key is None or key in resp["result"])
    status = "PASS" if ok else "FAIL"
    print(f"  [{status}] {label}")
    if not ok:
        print(f"         got: {resp}")
    return ok


def main():
    p = argparse.ArgumentParser(description="Wave Rover MCP smoke test")
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--token", default=None,
                   help="Bearer token (required when auth_enabled=true on device)")
    p.add_argument("--allow-motion", action="store_true",
                   help="Enable motion tests (rover must be in a safe position)")
    args = p.parse_args()

    print(f"\n=== Wave Rover MCP Smoke Test ({args.host}:{args.port}) ===\n")
    passed = failed = 0

    protocol_tests = [
        ("initialize", "initialize",
         {"protocolVersion": "2024-11-05", "capabilities": {},
          "clientInfo": {"name": "smoke", "version": "1"}}),
        ("ping",           "ping",           {}),
        ("tools/list",     "tools/list",     {}),
        ("resources/list", "resources/list", {}),
    ]
    for label, method, params in protocol_tests:
        r = mcp_call(args.host, args.port, method, params, token=args.token)
        if check(label, r):
            passed += 1
        else:
            failed += 1

    tool_tests = [
        ("rover.get_status",     "rover.get_status",     {}),
        ("rover.get_config",     "rover.get_config",     {}),
        ("rover.get_power",      "rover.get_power",      {}),
        ("rover.get_ups",        "rover.get_ups",        {}),
        ("rover.get_imu",        "rover.get_imu",        {}),
        ("rover.display_status", "rover.display_status", {}),
        ("rover.get_wifi",       "rover.get_wifi",       {}),
        ("rover.stop",           "rover.stop",           {"reason": "smoke_test"}),
    ]
    for label, tool_name, tool_args in tool_tests:
        r = mcp_call(args.host, args.port, "tools/call",
                     {"name": tool_name, "arguments": tool_args}, token=args.token)
        if check(label, r):
            passed += 1
        else:
            failed += 1

    if args.allow_motion:
        r = mcp_call(args.host, args.port, "tools/call", {
            "name": "rover.move",
            "arguments": {"linear": 0.2, "angular": 0.0, "duration_ms": 200}
        }, token=args.token)
        if check("rover.move (motion enabled)", r):
            passed += 1
        else:
            failed += 1
    else:
        print("  [SKIP] rover.move (pass --allow-motion to test real motion)")

    print(f"\n=== Results: {passed} passed, {failed} failed ===\n")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
