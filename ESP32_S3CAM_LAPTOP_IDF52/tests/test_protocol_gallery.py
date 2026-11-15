"""Smoke tests without ESP32 hardware or downloaded ONNX weights.
Run from project root: python -m unittest discover -s tests -v
"""
import sys
import unittest
import tempfile
from pathlib import Path
import numpy as np
import cv2
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'LAPTOP_SERVER'))
from protocol import response, unpack, STATUS_MATCH, STATUS_IDLE, HEADER, rgb565_bytes
from face_engine import Gallery

class TestProtocol(unittest.TestCase):
    def test_idle_envelope(self):
        data = response(STATUS_IDLE)
        self.assertEqual(len(data), 15)
        self.assertEqual(unpack(data), (0, '', 0, 0, b''))
    def test_success_image_payload(self):
        img = np.zeros((100,100,3), dtype=np.uint8)
        img[:,:,:] = (0,0,255) # RED in BGR
        data = response(STATUS_MATCH,'17',img)
        status,pid,w,h,pixels = unpack(data)
        self.assertEqual((status,pid,w,h,len(pixels)), (1,'17',96,96,18432))
        self.assertEqual(pixels[:2], b'\x00\xf8') # LE RGB565 red
        import zlib
        self.assertEqual(zlib.crc32(pixels),zlib.crc32(rgb565_bytes(img)))

class TestGallery(unittest.TestCase):
    def test_gallery_incremental_add_and_remove(self):
        with tempfile.TemporaryDirectory() as d:
            g=Gallery(Path(d))
            v=np.zeros((128,), dtype=np.float32);v[0]=1
            q=np.zeros((128,), dtype=np.float32);q[1]=1
            g.add('11','A',[v]*30)
            g.add('22','B',[q]*30)
            self.assertEqual(g.predict(v,0.43,0.025)[0],'11')
            self.assertEqual(g.predict(q,0.43,0.025)[0],'22')
            g2=Gallery(Path(d))
            self.assertEqual(len(g2.features),60)
            self.assertTrue(g2.delete('11'))
            self.assertEqual(g2.predict(v,0.43,0.025)[0],None)
    def test_reject_invalid_id(self):
        img=np.zeros((96,96,3),dtype=np.uint8)
        with self.assertRaises(ValueError):response(STATUS_MATCH,'abc',img)

if __name__=='__main__':unittest.main()
