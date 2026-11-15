"""Pretrained CNN embeddings + per-employee incremental gallery training.
No model weights are fine-tuned with only 30 images: they form a new class of
face embeddings with optional prototypes and leave-one-out thresholds.
"""
from __future__ import annotations
import json
import os
import shutil
import threading
import time
from pathlib import Path
from dataclasses import dataclass
import cv2 as cv
import numpy as np

ROOT = Path(__file__).resolve().parent
MODEL_DIR = ROOT / 'models'
DATA_DIR = ROOT / 'data'
POSE_GUIDES = (
    'Nhin thang vao camera',
    'Quay mat nhe sang trai',
    'Quay mat nhe sang phai',
    'Nang cam len mot chut',
    'Ha cam xuong mot chut',
)


@dataclass
class Face:
    feature: np.ndarray
    crop: np.ndarray
    xywh: list[int]
    quality: float


class Gallery:
    def __init__(self, root: Path = DATA_DIR):
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        self.features = np.empty((0, 128), dtype=np.float32)
        self.ids = np.array([], dtype='<U12')
        self.people: dict[str, str] = {}
        self.load()

    def load(self):
        path = self.root / 'gallery.npz'
        if path.exists():
            with np.load(path, allow_pickle=False) as db:
                self.features = db['features'].astype(np.float32)
                self.ids = db['ids'].astype('<U12')
        p = self.root / 'people.json'
        if p.exists():
            self.people = json.loads(p.read_text(encoding='utf8'))

    def save(self):
        tmp = self.root / 'gallery.tmp'
        with tmp.open('wb') as out:
            np.savez_compressed(out, features=self.features, ids=self.ids)
        os.replace(tmp, self.root / 'gallery.npz')
        tmp = self.root / 'people.tmp'
        tmp.write_text(json.dumps(self.people, ensure_ascii=False, indent=2), encoding='utf8')
        os.replace(tmp, self.root / 'people.json')

    def add(self, person_id: str, name: str, features: list[np.ndarray]):
        matrix = np.vstack(features).astype(np.float32)
        if matrix.shape[1] != 128:
            raise ValueError(f'Expected 128-dimensional SFace vectors, got {matrix.shape}')
        matrix /= np.maximum(np.linalg.norm(matrix, axis=1, keepdims=True), 1e-9)
        keep = self.ids != person_id
        self.features = np.vstack([self.features[keep], matrix])
        self.ids = np.hstack([self.ids[keep], np.full(len(matrix), person_id)])
        self.people[person_id] = name
        self.save()

    def delete(self, person_id: str) -> bool:
        if person_id not in self.people:
            return False
        mask = self.ids != person_id
        self.features = self.features[mask]
        self.ids = self.ids[mask]
        del self.people[person_id]
        self.save()
        shutil.rmtree(self.root / 'photos' / person_id, ignore_errors=True)
        return True

    def predict(self, feature: np.ndarray, threshold: float, margin: float):
        if len(self.ids) == 0:
            return None, 0.0, 0.0
        query = feature.ravel().astype(np.float32)
        query /= max(float(np.linalg.norm(query)), 1e-8)
        similarities = self.features @ query
        # Maximum nearest example per class handles different enrollment angles.
        # Top-3 mean is smoother than taking only one very high similarity.
        scores = {}
        for person_id in np.unique(self.ids):
            selected = np.sort(similarities[self.ids == person_id])[-3:]
            scores[str(person_id)] = float(np.mean(selected))
        ranked = sorted(scores.items(), key=lambda x: x[1], reverse=True)
        person_id, best = ranked[0]
        second = ranked[1][1] if len(ranked)>1 else -1.0
        if best >= threshold and (best - second) >= margin:
            return person_id, best, best-second
        return None, best, best-second


class FaceEngine:
    def __init__(self, config: dict):
        model_a = MODEL_DIR / 'face_detection_yunet_2023mar.onnx'
        model_b = MODEL_DIR / 'face_recognition_sface_2021dec.onnx'
        if not model_a.exists() or not model_b.exists():
            raise RuntimeError('Missing OpenCV Zoo model ONNX. Run: python download_models.py')
        self.cfg = config
        self.lock = threading.RLock()
        self.detector = cv.FaceDetectorYN_create(str(model_a), '', (640,480), score_threshold=0.78,
                                                 nms_threshold=0.3, top_k=5000)
        self.recognizer = cv.FaceRecognizerSF_create(str(model_b), '')
        self.gallery = Gallery()
        self.enrollment = None
        self.enroll_result_id = None  # last completed, durable only during server uptime
        self.last_status = 'READY'
        self.last_score = 0.0
        self.last_face_count = 0
        self.last_preview = None
        self.streak_id = None
        self.streak = 0
        self.last_announcement = 0.0
        self.latched_id = None
        self.last_frame = 0.0
        self.no_face_since = None

    def _face_from_frame(self, frame):
        h, w = frame.shape[:2]
        self.detector.setInputSize((w, h))
        _, faces = self.detector.detect(frame)
        faces = [] if faces is None else list(faces)
        if len(faces) != 1:
            return None, len(faces)
        face = faces[0]
        x,y,bw,bh = map(int,face[:4])
        if bw < self.cfg['minimum_face_width'] or bh < self.cfg['minimum_face_width']:
            self.last_status = 'FACE_TOO_SMALL'
            return None, 1
        # aligned SFace 112x112 crop supplied by the face model's landmarks
        aligned = self.recognizer.alignCrop(frame, face)
        sharp = float(cv.Laplacian(cv.cvtColor(aligned,cv.COLOR_BGR2GRAY),cv.CV_64F).var())
        if sharp < self.cfg['minimum_blur_variance']:
            self.last_status = f'BLURRY ({sharp:.0f})'
            return None, 1
        emb = np.asarray(self.recognizer.feature(aligned),dtype=np.float32).ravel()
        if emb.shape != (128,):
            raise RuntimeError(f'SFace output dimension not 128: {emb.shape}')
        emb /= max(float(np.linalg.norm(emb)),1e-8)
        # Crop original pixels with padding so the TFT thumbnail has context.
        half = int(max(bw,bh)*0.68)
        cx,cy = x+bw//2,y+bh//2
        left,right=max(0,cx-half),min(w,cx+half)
        top,bottom=max(0,cy-half),min(h,cy+half)
        crop = frame[top:bottom,left:right]
        return Face(emb,crop,[x,y,bw,bh],sharp), 1

    def start_enroll(self, pid: str, name: str):
        with self.lock:
            if self.enrollment is not None:
                raise ValueError('Another registration is active')
            # Re-enrollment is allowed: old gallery stays until 30 new photos
            # complete; main-controller face_enabled flag remains false meanwhile.
            folder = DATA_DIR / 'enroll_tmp' / pid
            if folder.exists(): shutil.rmtree(folder)
            folder.mkdir(parents=True,exist_ok=True)
            self.enroll_result_id = None
            self.enrollment = {'id':pid,'name':name,'count':0,'features':[],
                               'last_saved':0.0,'folder':folder,'started':time.time()}
            self.streak=0;self.streak_id=None

    def cancel_enroll(self):
        with self.lock:
            if self.enrollment is not None:
                shutil.rmtree(self.enrollment['folder'], ignore_errors=True)
                self.enrollment=None
                self.enroll_result_id=None
                return True
            return False

    def cancel_enroll_if(self, person_id: str) -> bool:
        with self.lock:
            if self.enrollment is not None and self.enrollment['id'] == person_id:
                return self.cancel_enroll()
            return False

    def enroll_status(self):
        with self.lock:
            e=self.enrollment
            if not e: return None
            count=e['count']
            guide=POSE_GUIDES[min(len(POSE_GUIDES)-1,count//6)]
            return {'id':e['id'],'name':e['name'],'count':count,
                    'target':self.cfg['enroll_images'],'pose_guide':guide}

    def _process_enroll(self, frame, face, now):
        e=self.enrollment
        if face is None:
            return
        if now-e['last_saved'] < self.cfg['enroll_interval_seconds']:
            return
        # Save BOTH original JPEG and feature vector. User needs to change pose.
        filename=e['folder'] / f"{e['count']+1:02d}.jpg"
        if not cv.imwrite(str(filename),frame,[int(cv.IMWRITE_JPEG_QUALITY),90]):
            self.last_status='DISK_WRITE_ERROR'
            return
        e['features'].append(face.feature.copy())
        e['count']+=1
        e['last_saved']=now
        self.last_status=f"ENROLL_{e['count']}/{self.cfg['enroll_images']}"
        if e['count'] >= self.cfg['enroll_images']:
            self.gallery.add(e['id'],e['name'],e['features'])
            destination = DATA_DIR / 'photos' / e['id']
            if destination.exists(): shutil.rmtree(destination)
            destination.parent.mkdir(parents=True,exist_ok=True)
            e['folder'].rename(destination)
            self.enroll_result_id=e['id']
            self.last_status=f"ENROLL_FINISHED_ID_{e['id']}"
            self.enrollment=None
            self.streak=0
            self.streak_id=None

    def process(self, jpg:bytes) -> bytes:
        from protocol import response, STATUS_MATCH, STATUS_IDLE, STATUS_ENROLL, STATUS_UNKNOWN
        with self.lock:
            now=time.monotonic()
            if now-self.last_frame>2.0:
                self.streak=0;self.streak_id=None;self.latched_id=None
            self.last_frame=now
            raw=np.frombuffer(jpg,dtype=np.uint8)
            frame=cv.imdecode(raw,cv.IMREAD_COLOR)
            if frame is None or len(frame.shape)!=3 or frame.shape[0]>1200 or frame.shape[1]>1600:
                raise ValueError('Invalid JPEG or image too large')
            face,count=self._face_from_frame(frame)
            self.last_face_count=count
            if count==0:
                self.last_status='NO_FACE'
                self.streak=0;self.streak_id=None
                if self.no_face_since is None:self.no_face_since=now
                if now-self.no_face_since>1.5:self.latched_id=None
            elif count>1:
                self.last_status='MULTIPLE_FACES'
                self.streak=0;self.streak_id=None
                self.no_face_since=None
            else:
                self.no_face_since=None
            if face is not None:
                x,y,w,h=face.xywh
                cv.rectangle(frame,(x,y),(x+w,y+h),(50,230,50),2)
            # Show preview without network exposing raw stored photos.
            cv.putText(frame,self.last_status,(12,22),cv.FONT_HERSHEY_SIMPLEX,.65,(0,240,240),2)
            ok, preview = cv.imencode('.jpg',frame,[int(cv.IMWRITE_JPEG_QUALITY),75])
            if ok:self.last_preview=preview.tobytes()
            if self.enrollment:
                if count==1:self._process_enroll(cv.imdecode(raw,cv.IMREAD_COLOR),face,now)
                return response(STATUS_ENROLL)
            if face is None:
                return response(STATUS_IDLE if count==0 else STATUS_UNKNOWN)
            identity,score,score_margin = self.gallery.predict(face.feature,
                            self.cfg['minimum_cosine'],self.cfg['minimum_margin'])
            self.last_score=score
            if identity is None:
                self.last_status=f'UNKNOWN score={score:.3f}'
                self.streak=0;self.streak_id=None
                return response(STATUS_UNKNOWN)
            self.last_status=f'MATCH_CANDIDATE_{identity} score={score:.3f}'
            if self.streak_id==identity:
                self.streak+=1
            else:
                self.streak_id=identity;self.streak=1
            if (self.streak>=self.cfg['required_confirmations'] and
                self.latched_id!=identity and
                now-self.last_announcement>=self.cfg['match_cooldown_seconds']):
                self.last_status=f'ACCEPT_{identity}'
                self.latched_id=identity
                self.last_announcement=now
                return response(STATUS_MATCH,identity,face.crop)
            return response(STATUS_IDLE)
