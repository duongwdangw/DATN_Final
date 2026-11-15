"""Regression tests for UART-triggered device API (no camera/ESP32 required)."""
import contextlib
import http.client
import json
import threading
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'LAPTOP_SERVER'))
from http.server import ThreadingHTTPServer
import server
from face_engine import Gallery
import numpy as np


class FakeGallery:
    def __init__(self):
        self.ids = {'14', '21'}
        self.people = {'14': 'Old name', '21': 'Test name'}
    def delete(self, pid):
        self.ids.discard(pid)
        return self.people.pop(pid, None) is not None


class FakeEngine:
    def __init__(self):
        self.lock = threading.RLock()
        self.enrollment = None
        self.enroll_result_id = None
        self.gallery = FakeGallery()
    def start_enroll(self, pid, name):
        with self.lock:
            if self.enrollment is not None: raise ValueError('Busy')
            self.enroll_result_id = None
            self.enrollment = {'id': pid, 'name':name, 'count':0}
    def cancel_enroll_if(self, pid):
        with self.lock:
            if self.enrollment and self.enrollment['id'] == pid:
                self.enrollment = None
                self.enroll_result_id = None
                return True
            return False


@contextlib.contextmanager
def webserver():
    web = ThreadingHTTPServer(('127.0.0.1', 0), server.Handler)
    web.engine = FakeEngine()
    web.cfg = {'device_key':'test-secret', 'admin_key':'a-different-secret'}
    thread = threading.Thread(target=web.serve_forever, daemon=True)
    thread.start()
    try:yield web
    finally:
        web.shutdown();web.server_close();thread.join(timeout=3)


def request(web, path, body=None, key='test-secret'):
    cli = http.client.HTTPConnection('127.0.0.1', web.server_address[1], timeout=3)
    headers={'X-Device-Key':key}
    if body is not None: headers['Content-Type']='application/json'
    cli.request('POST' if body is not None else 'GET',path,
                json.dumps(body).encode() if body is not None else None,headers)
    resp=cli.getresponse()
    result=resp.status, resp.read().decode()
    cli.close()
    return result


def test_device_enroll_start_and_progress():
    with webserver() as web:
        assert request(web,'/api/device/enroll/start',{'id':'14'})==(200,'STARTED:14')
        assert request(web,'/api/device/enroll/state')==(200,'ENROLLING:14:0')
        web.engine.enrollment['count']=17
        assert request(web,'/api/device/enroll/state')==(200,'ENROLLING:14:17')
        web.engine.enrollment=None
        web.engine.enroll_result_id='14'
        assert request(web,'/api/device/enroll/state')==(200,'DONE:14')


def test_device_requires_key():
    with webserver() as web:
        assert request(web,'/api/device/enroll/start',{'id':'12'},'bad')[0]==403
        assert request(web,'/api/device/enroll/state',key='bad')[0]==403
        assert web.engine.enrollment is None


def test_enrollment_cancel_only_exact_id():
    with webserver() as web:
        request(web,'/api/device/enroll/start',{'id':'14'})
        assert request(web,'/api/device/enroll/cancel',{'id':'15'})==(200,'CANCELLED:15')
        assert web.engine.enrollment['id']=='14'
        assert request(web,'/api/device/enroll/cancel',{'id':'14'})==(200,'CANCELLED:14')
        assert request(web,'/api/device/enroll/state')==(200,'IDLE')


def test_delete_idempotent_and_denied_during_registration():
    with webserver() as web:
        request(web,'/api/device/enroll/start',{'id':'15'})
        assert request(web,'/api/device/face/delete',{'id':'14'})[0]==409
        request(web,'/api/device/enroll/cancel',{'id':'15'})
        assert request(web,'/api/device/face/delete',{'id':'14'})==(200,'DELETED:14')
        assert request(web,'/api/device/face/delete',{'id':'14'})==(200,'DELETED:14')
        assert '14' not in web.engine.gallery.people


def test_reject_bad_device_id():
    with webserver() as web:
        for pid in ('0','-1','abc','999999'):
            assert request(web,'/api/device/enroll/start',{'id':pid})[0]==409


def test_gallery_overwrites_existing_employee(tmp_path):
    gallery=Gallery(tmp_path)
    feature=[np.eye(1,128,0,dtype=np.float32).reshape(-1),
             np.eye(1,128,1,dtype=np.float32).reshape(-1)]
    gallery.add('14','Employee 14',feature)
    assert gallery.ids.tolist()==['14','14']
    gallery.add('14','Reenrolled 14',[feature[1]])
    assert gallery.ids.tolist()==['14']
    assert gallery.people['14']=='Reenrolled 14'
    loaded=Gallery(tmp_path)
    assert loaded.features.shape==(1,128)
    assert loaded.delete('14') is True
    assert Gallery(tmp_path).people=={}
