"""Deterministic regression tests. No real impostor dataset or firmware required."""
import io
import json
import tempfile
import threading
import unittest
import urllib.request
from contextlib import redirect_stdout
from pathlib import Path
import cv2 as cv
import numpy as np
from face_engine import Gallery, FaceEngine, Face, unit_vector
from protocol import *
from server import FaceHTTP

CFG = dict(minimum_cosine=.43, minimum_margin=.025, required_confirmations=3,
           minimum_face_width=65, minimum_blur_variance=28, enroll_images=30,
           enroll_interval_seconds=1, match_cooldown_seconds=10,
           device_key='test-device-key-only', admin_key='test-admin-key-only')
def basis(i):
    a=np.zeros(128,np.float32); a[i]=1; return a

def jpeg(i):
    a=np.full((100,100,3),i,np.uint8)
    return cv.imencode('.jpg',a)[1].tobytes()

class HardeningTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.silent=redirect_stdout(io.StringIO()); self.silent.__enter__(); self.addCleanup(self.silent.__exit__,None,None,None)
        self.gallery=Gallery(self.root)
    def add(self, pid='1', vec=None):
        self.gallery.add(pid,'test',[basis(0) if vec is None else vec]*30)
    def engine(self):
        e=FaceEngine(CFG,self.root)
        e._face_from_frame=lambda frame: (Face(basis(0),frame,[0,0,80,80],100.),1)
        return e
    def status(self,e,i,**kw):
        return unpack(e.process(jpeg(i),**kw))[0]
    def test_single_unknown(self):
        self.add(); self.assertIsNone(self.gallery.predict(basis(1),.43,.025)[0])
        self.assertEqual(self.gallery.predict(basis(0),.43,.025)[0],'1')
    def test_top3_false_accept_regression(self):
        q=basis(1); common=basis(0)
        # 27 samples at .1 vs query, 3 at .55; same class internally coherent.
        a=unit_vector(np.sqrt(.99)*common+.1*q); b=unit_vector(np.sqrt(1-.55**2)*common+.55*q)
        self.gallery.add('1','test',[a]*27+[b]*3)
        self.assertGreater(float(np.mean(np.sort(self.gallery.features@q)[-3:])),.43)
        self.assertIsNone(self.gallery.predict(q,.43,.025)[0])
    def test_support_gate(self):
        q=basis(1); a=unit_vector(.4*q+np.sqrt(.84)*basis(0)); b=unit_vector(.95*q+np.sqrt(1-.95**2)*basis(0))
        self.gallery.add('1','test',[a]*15+[b]*15)
        self.assertIsNone(self.gallery.predict(q,.43,.025)[0]); self.assertEqual(self.gallery.last_decision['reason'],'INSUFFICIENT_SUPPORT')
    def test_ambiguity(self):
        self.add('1'); self.add('2'); self.assertIsNone(self.gallery.predict(basis(0),.43,.025)[0])
        self.assertEqual(self.gallery.last_decision['reason'],'AMBIGUOUS')
    def test_invalid_embedding(self):
        self.add()
        for v in (np.zeros(128),np.full(128,np.nan),np.full(128,np.inf),np.ones(127)):
            with self.assertRaises(ValueError): self.gallery.predict(v,.43,.025)
    def test_mixed_enrollment(self):
        with self.assertRaises(ValueError): self.gallery.add('1','test',[basis(0)]*29+[basis(1)])
        self.assertEqual(len(self.gallery.ids),0)
    def test_corrupt_gallery(self):
        self.add(); np.savez(self.root/'gallery.npz',features=np.full((30,128),np.nan),ids=np.array(['1']*30))
        with self.assertRaises(ValueError): Gallery(self.root)
    def test_three_new_frames(self):
        self.add(); e=self.engine(); self.assertEqual([self.status(e,i) for i in (50,51,52)],[STATUS_IDLE,STATUS_IDLE,STATUS_MATCH])
    def test_duplicate_frame(self):
        self.add(); e=self.engine(); self.assertEqual([self.status(e,i) for i in (50,50,51,52,53)],[STATUS_IDLE,STATUS_UNKNOWN,STATUS_IDLE,STATUS_IDLE,STATUS_MATCH])
    def test_low_quality_resets(self):
        self.add(); e=self.engine(); self.status(e,50); self.status(e,51); good=e._face_from_frame
        e._face_from_frame=lambda frame: (None,1); e.frame_reason='BLURRY'; self.assertEqual(self.status(e,52),STATUS_UNKNOWN); e._face_from_frame=good
        self.assertEqual([self.status(e,i) for i in (53,54,55)],[STATUS_IDLE,STATUS_IDLE,STATUS_MATCH])
    def test_error_resets(self):
        self.add(); e=self.engine(); self.status(e,50); self.status(e,51); self.assertEqual(unpack(e.process(b'bad'))[0],STATUS_ERROR)
        self.assertEqual([self.status(e,i) for i in (53,54,55)],[STATUS_IDLE,STATUS_IDLE,STATUS_MATCH])
    def test_timeout_reset_header(self):
        self.add(); e=self.engine(); self.status(e,50); self.status(e,51)
        self.assertEqual(self.status(e,52,reset=True),STATUS_IDLE); self.assertEqual(self.status(e,53),STATUS_IDLE); self.assertEqual(self.status(e,54),STATUS_MATCH)
    def test_unknown_and_no_face_reset(self):
        self.add(); e=self.engine(); good=e._face_from_frame
        for count in (0,2):
            self.status(e,50+count*10); self.status(e,51+count*10); e._face_from_frame=lambda frame:(None,count)
            self.assertIn(self.status(e,52+count*10),(STATUS_IDLE,STATUS_UNKNOWN)); self.assertEqual(e.streak,0); e._face_from_frame=good
    def test_stream_and_gallery_revision(self):
        self.add(); e=self.engine(); self.status(e,50,stream='a'); self.status(e,51,stream='a'); self.assertEqual(self.status(e,52,stream='b'),STATUS_IDLE)
        e.gallery.delete('1'); self.assertEqual(self.status(e,53,stream='b'),STATUS_UNKNOWN)
    def test_protocol(self):
        for status in (STATUS_IDLE,STATUS_UNKNOWN,STATUS_ENROLL,STATUS_ERROR):
            self.assertEqual(unpack(response(status)),(status,'',0,0,b''))
            with self.assertRaises(ValueError): response(status,'1')
        for pid in ('0','65536','01','x'):
            with self.assertRaises(ValueError): response(STATUS_MATCH,pid,np.zeros((96,96,3),np.uint8))
        self.assertEqual(len(unpack(response(STATUS_MATCH,'1',np.zeros((96,96,3),np.uint8)))[4]),18432)
    def test_http(self):
        self.add(); e=self.engine(); srv=FaceHTTP(('127.0.0.1',0),CFG,e)
        t=threading.Thread(target=srv.serve_forever,daemon=True); t.start()
        self.addCleanup(srv.server_close); self.addCleanup(t.join,3); self.addCleanup(srv.shutdown)
        url=f'http://127.0.0.1:{srv.server_port}/api/frame'
        def post(body,reset=False):
            req=urllib.request.Request(url,data=body,headers={'Content-Type':'image/jpeg','X-Device-Key':CFG['device_key'],'X-Face-Reset':str(int(reset))})
            with urllib.request.urlopen(req,timeout=5) as f:
                self.assertEqual(f.headers['Cache-Control'],'no-store'); return unpack(f.read())
        self.assertEqual(post(jpeg(50),True)[0],STATUS_IDLE); self.assertEqual(post(jpeg(51))[0],STATUS_IDLE)
        self.assertEqual(post(b'bad'),(STATUS_ERROR,'',0,0,b''))
        self.assertEqual(post(jpeg(52))[0],STATUS_IDLE); self.assertEqual(post(jpeg(53))[0],STATUS_IDLE); self.assertEqual(post(jpeg(54))[0],STATUS_MATCH)
    def test_real_models_blank(self):
        self.add(); e=FaceEngine(CFG,self.root); self.assertEqual(unpack(e.process(jpeg(0))),(STATUS_IDLE,'',0,0,b''))

if __name__=='__main__': unittest.main(verbosity=2)
