"""Local HTTP camera ingestion + face recognition + admin enrollment UI.
Runs without cloud or paid services, on a LAN-hosted Windows laptop.
"""
from __future__ import annotations
import json
import re
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlsplit
from face_engine import FaceEngine
from protocol import response, STATUS_ERROR

ROOT = Path(__file__).resolve().parent
CONFIG_FILE = ROOT / 'config.json'
REQUIRED_CONFIG = ('device_key','admin_key','bind','port')


def read_config():
    if not CONFIG_FILE.exists():
        raise RuntimeError('No config.json! Copy config.example.json to config.json and edit both keys.')
    config = json.loads(CONFIG_FILE.read_text(encoding='utf-8'))
    for k in REQUIRED_CONFIG:
        if k not in config:
            raise RuntimeError(f'Missing config key {k}')
    for k in ('device_key','admin_key'):
        if len(str(config[k]))<20 or str(config[k]).startswith('REPLACE'):
            raise RuntimeError(f'Set a different random secret for {k} (>=20 chars)')
    if config['device_key']==config['admin_key']:
        raise RuntimeError('Use different keys for device and admin')
    return config


class Handler(BaseHTTPRequestHandler):
    server_version = 'FaceLAN/1.0'

    @property
    def engine(self): return self.server.engine
    @property
    def cfg(self): return self.server.cfg

    def reply(self, code:int, body:bytes, content_type='application/json; charset=utf-8'):
        self.send_response(code)
        self.send_header('Content-Type',content_type)
        self.send_header('Content-Length',str(len(body)))
        self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('X-Frame-Options','DENY')
        self.end_headers()
        try:self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError):pass

    def json_reply(self, code, payload):
        self.reply(code,json.dumps(payload,ensure_ascii=False).encode('utf8'))

    def _admin_ok(self):
        return self.headers.get('X-Admin-Key') == self.cfg['admin_key']

    def _admin_required(self):
        if not self._admin_ok():
            self.json_reply(403,{'error':'Invalid admin key'})
            return False
        return True

    def _read_body(self, limit):
        try:n=int(self.headers.get('Content-Length','0'))
        except ValueError:n=0
        if n<1 or n>limit: raise ValueError(f'Body size must be 1..{limit} bytes')
        body=self.rfile.read(n)
        if len(body)!=n: raise ValueError('Incomplete request')
        return body

    def do_GET(self):
        route=urlsplit(self.path).path
        if route=='/':
            self.reply(200,(ROOT/'templates'/'index.html').read_bytes(),'text/html; charset=utf-8');return
        if route=='/api/health':
            self.json_reply(200,{'ready':True,'service':'OpenCV YuNet + SFace CNN'});return
        if route == '/api/device/enroll/state':
            if self.headers.get('X-Device-Key') != self.cfg['device_key']:
                self.reply(403,b'DENIED','text/plain');return
            with self.engine.lock:
                active=self.engine.enrollment
                if active:
                    result=f'ENROLLING:{active["id"]}:{active["count"]}'
                elif self.engine.enroll_result_id:
                    result=f'DONE:{self.engine.enroll_result_id}'
                else:
                    result='IDLE'
            self.reply(200,result.encode('ascii'),'text/plain; charset=ascii')
            return
        if route in ('/api/preview','/api/status','/api/people'):
            if not self._admin_required():return
            with self.engine.lock:
                if route=='/api/preview':
                    if self.engine.last_preview is None:
                        self.json_reply(404,{'error':'No camera frame received yet'})
                    else:self.reply(200,self.engine.last_preview,'image/jpeg')
                    return
                if route=='/api/status':
                    self.json_reply(200,{'status':self.engine.last_status,
                       'score':round(self.engine.last_score,3),
                       'face_count':self.engine.last_face_count,
                       'enrollment':self.engine.enroll_status(),
                       'people_count':len(self.engine.gallery.people),
                       'camera_frame_received':self.engine.last_preview is not None})
                    return
                self.json_reply(200,{'people':[{'id':k,'name':v,'images':int(sum(self.engine.gallery.ids==k))}
                                                   for k,v in self.engine.gallery.people.items()]})
                return
        self.json_reply(404,{'error':'Not found'})

    def do_POST(self):
        route=urlsplit(self.path).path
        if route=='/api/frame':
            if self.headers.get('X-Device-Key')!=self.cfg['device_key']:
                self.reply(403,b'', 'application/octet-stream');return
            if self.headers.get('Content-Type','').split(';')[0].strip()!='image/jpeg':
                self.reply(415,b'', 'application/octet-stream');return
            try:
                jpg=self._read_body(180_000)
                data=self.engine.process(jpg)
                self.reply(200,data,'application/octet-stream')
            except ValueError as exc:
                self.log_error('Bad camera frame: %s',exc)
                self.reply(400,b'', 'application/octet-stream')
            except Exception as exc:
                self.log_error('Frame processing error: %s',exc)
                self.reply(200,response(STATUS_ERROR),'application/octet-stream')
            return
        # Physical ESP32-MAIN admin PIN authorizes these commands.
        # S3 is authenticated by X-Device-Key, and all are restricted to LAN.
        if route in ('/api/device/enroll/start','/api/device/enroll/cancel','/api/device/face/delete'):
            if self.headers.get('X-Device-Key') != self.cfg['device_key']:
                self.reply(403,b'DENIED','text/plain');return
            try:
                payload=json.loads(self._read_body(256).decode('utf8'))
                pid=str(payload.get('id','')).strip()
                if not re.fullmatch(r'[0-9]{1,5}',pid) or not 1 <= int(pid) <= 65535:
                    raise ValueError('Invalid employee ID')
                pid=str(int(pid))
                if route == '/api/device/enroll/start':
                    self.engine.start_enroll(pid,f'EMPLOYEE {pid}')
                    result=f'STARTED:{pid}'
                elif route == '/api/device/enroll/cancel':
                    self.engine.cancel_enroll_if(pid)
                    result=f'CANCELLED:{pid}'
                else:
                    with self.engine.lock:
                        if self.engine.enrollment is not None:
                            raise ValueError('Enrollment in progress; retry delete later')
                        self.engine.gallery.delete(pid)
                        if self.engine.enroll_result_id == pid:
                            self.engine.enroll_result_id = None
                    result=f'DELETED:{pid}'
                self.reply(200,result.encode('ascii'),'text/plain; charset=ascii')
            except (ValueError, json.JSONDecodeError) as exc:
                self.reply(409,f'ERROR:{exc}'.encode('utf-8'),'text/plain; charset=utf-8')
            except Exception as exc:
                self.log_error('Device operation failure: %s',exc)
                self.reply(500,b'ERROR:SERVER','text/plain')
            return
        if route not in ('/api/enroll/start','/api/enroll/cancel','/api/people/delete'):
            self.json_reply(404,{'error':'Not found'});return
        if not self._admin_required():return
        try:
            payload=json.loads(self._read_body(4096).decode('utf8'))
            if route=='/api/enroll/start':
                pid=str(payload.get('id','')).strip()
                name=str(payload.get('name','')).strip()
                if not re.fullmatch(r'[0-9]{1,5}',pid) or int(pid)==0:
                    raise ValueError('Employee ID must be a number 1..99999, consistent with ESP32 main')
                if not 1<=len(name)<=64:raise ValueError('Name must contain 1..64 characters')
                self.engine.start_enroll(pid,name)
                self.json_reply(200,{'ok':True,'message':'Enrollment started; face camera and rotate head'})
            elif route=='/api/enroll/cancel':
                self.json_reply(200,{'ok':self.engine.cancel_enroll()})
            else:
                pid=str(payload.get('id','')).strip()
                with self.engine.lock:
                    if self.engine.enrollment:raise ValueError('Finish enrollment before deleting')
                    deleted=self.engine.gallery.delete(pid)
                self.json_reply(200,{'ok':deleted})
        except (ValueError,KeyError,json.JSONDecodeError) as exc:
            self.json_reply(400,{'error':str(exc)})
        except Exception as exc:
            self.log_error('Admin error %s',exc)
            self.json_reply(500,{'error':'Server error; check terminal logs'})


def main():
    config=read_config()
    engine=FaceEngine(config)
    host=ThreadingHTTPServer((config['bind'],int(config['port'])),Handler)
    host.engine=engine
    host.cfg=config
    print(f'AI server started on http://127.0.0.1:{config["port"]}',flush=True)
    print('LAN: use http://YOUR_LAPTOP_IPV4:5000 for UI',flush=True)
    print('Open browser and enter the ADMIN KEY to control enrollment.',flush=True)
    try:host.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:pass
    finally:host.server_close()


if __name__=='__main__':main()
