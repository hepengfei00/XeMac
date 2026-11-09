"""Temporary LAN-only receiver for this authorized test's scalar diagnostics."""
from http.server import HTTPServer,BaseHTTPRequestHandler
from pathlib import Path
import json,secrets,socket,datetime
p=Path(__file__).resolve().parent
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.connect(('192.0.2.123',9));addr=s.getsockname()[0];s.close()
token=secrets.token_urlsafe(32);out=p/'postboot';out.mkdir(exist_ok=True)
count=0
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  if self.path!='/'+token:self.send_error(404);return
  self.send_response(200);self.end_headers();self.wfile.write(b'BT_RECEIVER_READY')
 def do_POST(self):
  global count
  if self.path!='/'+token or self.client_address[0] not in {'192.0.2.123','192.0.2.135'}:self.send_error(404);return
  n=int(self.headers.get('Content-Length','0'))
  if not 0<n<65536:self.send_error(413);return
  raw=self.rfile.read(n).decode('utf-8','replace')
  # The sender only selects driver scalar counters, module identity and health.
  if 'BT_SAMPLE=' not in raw or 'BOOT_UUID=' not in raw:self.send_error(400);return
  count+=1
  (out/f'sample-{count:03d}.txt').write_text(raw)
  (out/'latest.txt').write_text(raw)
  self.send_response(200);self.end_headers();self.wfile.write(b'OK')
server=HTTPServer((addr,0),Handler)
(p/'receiver-connection.json').write_text(json.dumps({'url':f'http://{addr}:{server.server_port}/{token}','bind_ip':addr,'port':server.server_port,'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2)+'\n')
(p/'receiver-connection.json').chmod(0o600)
print('Temporary scalar-diagnostic receiver ready.',flush=True)
server.serve_forever()
