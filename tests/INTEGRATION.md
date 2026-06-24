# Integration: multi-client broadcast

`broadcast_test.py` connects two WebSocket clients (A and B) to a running
server, performs `create` / `update` / `delete` from client A, and asserts that
client B receives the corresponding broadcast events.

## Run

```bash
# Terminal 1
./bin/student_server --ws-port 9105

# Terminal 2 (needs `pip install websocket-client`)
python3 tests/broadcast_test.py
```

Expected output:

```
Client B received 4 messages
  - snapshot
  - created
  - updated
  - deleted

Expected events also seen by listener: {'created', 'updated', 'deleted'}
Missing: NONE - all good!
```

This is intentionally Python (not C++) because it exercises the wire protocol
end-to-end from a different runtime, catching any C++-specific assumptions in
the framing or JSON layer.
