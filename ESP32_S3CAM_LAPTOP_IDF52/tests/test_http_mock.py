"""In-memory HTTP contract test: no ONNX weights or actual ESP32 required."""
import sys
import json
import threading
import unittest
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from http.server import ThreadingHTTPServer
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'LAPTOP_SERVER'))
from server import Handler
from protocol import unpack, response, STATUS_MATCH
import numpy as np

class DummyEngine:
    def __init__(self):
        self.lock = threading.RLock()
        self.last_status = 'READY'
        self.last_score = 0.0
        self.last_face_count = 0
        self.last_preview = None
        self.gallery = type('G',(),{'people':{},'ids':[]})()
    def process(self, jpg):
        if not jpg.startswith(b'\xff\xd8'): raise ValueError('JPEG missing')
        return response(STATUS_MATCH, '12', np.zeros((96,96,3),dtype=np.uint8))
    def enroll_status(self):return None

class TestHttp(unittest.TestCase):
    def test_camera_binary_http(self):
        server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
        server.engine=DummyEngine()
        server.cfg={'device_key':'CAMTEST_SOME_LONG_KEY_123','admin_key':'ADMINTEST_LONG_KEY_456'}
        t=threading.Thread(target=server.serve_forever,daemon=True)
        t.start()
        try:
            url=f'http://127.0.0.1:{server.server_port}/api/frame'
            req=Request(url, data=b'\xff\xd8HELLO',headers={'Content-Type':'image/jpeg','X-Device-Key':'CAMTEST_SOME_LONG_KEY_123'}, method='POST')
            with urlopen(req) as r:
                data=r.read()
            status,pid,w,h,im=unpack(data)
            self.assertEqual((status,pid,w,h,len(im)),(1,'12',96,96,18432))
            req=Request(url,data=b'\xff\xd8HELLO',headers={'Content-Type':'image/jpeg'},method='POST')
            with self.assertRaises(HTTPError) as err:urlopen(req)
            self.assertEqual(err.exception.code,403)
        finally:
            server.shutdown();server.server_close();t.join(timeout=3)

if __name__=='__main__':unittest.main()
