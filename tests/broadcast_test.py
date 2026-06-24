#!/usr/bin/env python3
"""End-to-end broadcast test: connect two clients, do CRUD on A, verify B sees it."""
import json
import threading
import time
import websocket

URL = "ws://127.0.0.1:9001/"

recv_b = []
done = threading.Event()

def on_b_msg(ws, m):
    try:
        recv_b.append(json.loads(m))
    except Exception:
        pass

def on_b_open(ws):
    ws.send(json.dumps({"op": "list"}))

ws_b = websocket.WebSocketApp(URL, on_message=on_b_msg, on_open=on_b_open)
t = threading.Thread(target=ws_b.run_forever, daemon=True)
t.start()
time.sleep(0.4)

ws_a = websocket.create_connection(URL)
ws_a.send(json.dumps({"op": "create", "student": {"id": 200, "name": "Bcast", "age": 30, "grade": "S"}}))
time.sleep(0.2)
ws_a.send(json.dumps({"op": "update", "student": {"id": 200, "name": "BcastV2", "age": 31, "grade": "SS"}}))
time.sleep(0.2)
ws_a.send(json.dumps({"op": "delete", "id": 200}))
time.sleep(0.4)
ws_a.close()
ws_b.close()

events_b = [m.get("event") for m in recv_b]
print(f"Client B received {len(recv_b)} messages")
for e in events_b:
    print(f"  - {e}")

needed = {"created", "updated", "deleted"}
got = set(events_b)
missing = needed - got
print(f"\nExpected events also seen by listener: {needed}")
print(f"Missing: {missing if missing else 'NONE - all good!'}")
import sys
sys.exit(0 if not missing else 1)
