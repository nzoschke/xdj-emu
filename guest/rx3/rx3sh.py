#!/usr/bin/env python3
"""rx3sh.py <shell.sock> <command>: run a command in the RX3 bring-up shell
(guest/rx3/init, rx3.shell=1) and print its output."""
import socket, sys, time, uuid
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.settimeout(0.5)
mark = uuid.uuid4().hex[:8]
# The echo is split so the shell's echo of this line does not contain the marker.
s.sendall(("\n" + " ".join(sys.argv[2:]) + f"; echo __END_''{mark}__\n").encode())
buf, deadline = b"", time.time() + float(30)
while time.time() < deadline:
    try:
        chunk = s.recv(65536)
    except socket.timeout:
        continue
    if not chunk:
        break
    buf += chunk
    if f"__END_{mark}__\r\n".encode() in buf or f"__END_{mark}__\n".encode() in buf:
        break
out = buf.decode(errors="replace").replace("\r", "")
body = out.split(f"__END_''{mark}__", 1)[-1]   # after the echoed command line
print(body.split(f"__END_{mark}__", 1)[0].split("\n", 1)[-1].rstrip())
