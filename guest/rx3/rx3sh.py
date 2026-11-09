#!/usr/bin/env python3
"""rx3sh.py <shell.sock> <command>: run a command in the RX3 bring-up shell
(guest/rx3/init, rx3.shell=1) and print its output."""
import base64, socket, sys, time, uuid
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.settimeout(0.5)
mark = uuid.uuid4().hex[:8]
# The echo is split so the shell's echo of this line does not contain the marker.
command = " ".join(sys.argv[2:])
if command.rstrip().endswith("&"):
    command += " true"  # keep the completion marker valid after a background job
# Busybox's interactive line editor truncates long lines. Transfer an encoded
# script in short lines, then execute it, so firmware/config probes cannot
# silently lose half a command or leave the shell inside an open quote.
if len(command.encode()) > 600:
    encoded = base64.b64encode(command.encode()).decode()
    path = f"/root/.rx3sh-{mark}"
    lines = [f"printf %s '{encoded[i:i+400]}' {'>' if i == 0 else '>>'} {path}"
             for i in range(0, len(encoded), 400)]
    lines.append(f"echo __START_''{mark}__; base64 -d {path} | sh; rm -f {path}; echo __END_''{mark}__")
    for line in lines:
        s.sendall((line + "\n").encode())
        time.sleep(0.02)
else:
    s.sendall((f"\necho __START_''{mark}__; " + command + f"; echo __END_''{mark}__\n").encode())
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
start = f"__START_{mark}__\n"
if start not in out or f"__END_{mark}__" not in out:
    print(out, file=sys.stderr)
    raise SystemExit("RX3 shell command did not complete within 30 seconds")
print(out.split(start, 1)[1].split(f"__END_{mark}__", 1)[0].rstrip())
