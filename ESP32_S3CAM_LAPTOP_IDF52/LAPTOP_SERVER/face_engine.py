"""Pretrained CNN embeddings + per-employee incremental gallery training.
No model weights are fine-tuned with only 30 images: they form a new class of
face embeddings with optional prototypes and leave-one-out thresholds.
"""
from __future__ import annotations
import hashlib
import json
import math
import os
from collections import deque
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


def log(event, **fields):
    print('[FACE ' + event + '] ' + ' '.join(f'{k}={v}' for k, v in fields.items()), flush=True)


def unit_vector(value):
    a = np.asarray(value, dtype=np.float32).ravel().copy()
    if a.shape != (128,) or not np.isfinite(a).all():
        raise ValueError('Invalid SFace embedding')
    norm = float(np.linalg.norm(a.astype(np.float64)))
    if not math.isfinite(norm) or norm < 1e-8:
        raise ValueError('Invalid SFace norm')
    return (a / norm).astype(np.float32)


def valid_id(pid):
    return isinstance(pid, str) and pid.isascii() and pid.isdecimal() and 1 <= int(pid) <= 65535 and pid == str(int(pid))


class Gallery:
    def __init__(self, root: Path = DATA_DIR):
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        self.features = np.empty((0, 128), dtype=np.float32)
        self.ids = np.array([], dtype='<U12')
        self.people: dict[str, str] = {}
        self.revision = 0
        self.last_decision = {}
        self.load()

    def load(self):
        path = self.root / 'gallery.npz'
        p = self.root / 'people.json'
        if path.exists():
            with np.load(path, allow_pickle=False) as db:
                features = db['features']
                ids = db['ids']
                if features.ndim != 2 or features.shape[1] != 128 or ids.ndim != 1 or len(ids) != len(features):
                    raise ValueError('Invalid gallery shape; restore a verified backup')
                if ids.dtype.kind != 'U' or any(not valid_id(i) for i in ids.tolist()):
                    raise ValueError('Invalid gallery IDs')
                self.features = np.vstack([unit_vector(v) for v in features]) if len(features) else np.empty((0,128), np.float32)
                self.ids = ids.astype('<U12')
        if p.exists():
            self.people = json.loads(p.read_text(encoding='utf8'))
        if not isinstance(self.people, dict) or set(self.people) != set(self.ids.tolist()) or any(not valid_id(i) for i in self.people):
            raise ValueError('Gallery/people metadata mismatch; restore a verified backup')
        self.revision += 1
        log('GALLERY', employees=len(self.people), samples={i:int(np.sum(self.ids==i)) for i in sorted(self.people)})

    def save(self):
        # Stage both files before replacing either. Restore old files on ordinary I/O failure.
        gallery_path, people_path = self.root/'gallery.npz', self.root/'people.json'
        gallery_tmp, people_tmp = self.root/'gallery.tmp', self.root/'people.tmp'
        gallery_backup, people_backup = self.root/'gallery.rollback', self.root/'people.rollback'
        with gallery_tmp.open('wb') as out:
            np.savez_compressed(out, features=self.features, ids=self.ids)
        people_tmp.write_text(json.dumps(self.people, ensure_ascii=False, indent=2), encoding='utf8')
        targets = [(gallery_path,gallery_tmp,gallery_backup),(people_path,people_tmp,people_backup)]
        for target, _, backup in targets:
            if target.exists(): shutil.copy2(target,backup)
            elif backup.exists(): backup.unlink()
        replaced = []
        try:
            for target, tmp, backup in targets:
                os.replace(tmp,target)
                replaced.append((target,backup))
        except Exception:
            for target, backup in reversed(replaced):
                if backup.exists(): os.replace(backup,target)
                elif target.exists(): target.unlink()
            raise
        # A machine crash between replacements still requires checking the backup.
        for _,_,backup in targets:
            try: backup.unlink(missing_ok=True)
            except OSError: pass

    @staticmethod
    def consistent(matrix, floor=0.43):
        if len(matrix) < 3:
            return False
        pair = matrix @ matrix.T
        np.fill_diagonal(pair, np.nan)
        return bool(np.all(np.nanmedian(pair, axis=1) >= floor))

    def add(self, person_id: str, name: str, features: list[np.ndarray]):
        if not valid_id(person_id):
            raise ValueError('Invalid employee ID')
        matrix = np.vstack([unit_vector(v) for v in features])
        if len(matrix) != 30 or not self.consistent(matrix):
            raise ValueError('Enrollment requires 30 consistent samples')
        keep = self.ids != person_id
        old = self.features, self.ids, self.people.copy()
        self.features = np.vstack([self.features[keep], matrix])
        self.ids = np.hstack([self.ids[keep], np.full(len(matrix), person_id)])
        self.people[person_id] = name
        try:
            self.save()
        except Exception:
            self.features, self.ids, self.people = old
            raise
        finally:
            self.revision += 1

    def delete(self, person_id: str) -> bool:
        if person_id not in self.people:
            return False
        old = self.features, self.ids, self.people.copy()
        mask = self.ids != person_id
        self.features = self.features[mask]
        self.ids = self.ids[mask]
        del self.people[person_id]
        self.revision += 1
        try: self.save()
        except Exception:
            self.features, self.ids, self.people = old
            raise
        shutil.rmtree(self.root / 'photos' / person_id, ignore_errors=True)
        return True

    def predict(self, feature, threshold, margin, *, centroid_threshold=0.50,
                support_threshold=0.43, support_fraction=0.60):
        # Keep original top-3 score, but require independent gallery-wide support.
        self.last_decision = {'reason': 'EMPTY_GALLERY'}
        if not len(self.ids):
            return None, 0.0, 0.0
        query = unit_vector(feature)
        ranked = []
        for pid in sorted(self.people):
            matrix = self.features[self.ids == pid]
            if len(matrix) != 30 or not self.consistent(matrix):
                log('GALLERY', id=pid, samples=len(matrix), decision='QUARANTINE_INCONSISTENT')
                continue
            similarities = np.clip(matrix @ query, -1.0, 1.0)
            top = float(np.mean(np.sort(similarities)[-3:]))
            center = float(np.clip(unit_vector(matrix.mean(axis=0)) @ query, -1.0, 1.0))
            support = float(np.mean(similarities >= support_threshold))
            ranked.append((pid, top, center, support))
        if not ranked:
            self.last_decision = {'reason':'NO_VALID_GALLERY'}
            return None, 0.0, 0.0
        ranked.sort(key=lambda r:r[1], reverse=True)
        pid, best, center, support = ranked[0]
        second = ranked[1][1] if len(ranked)>1 else None
        gap = best-second if second is not None else 0.0
        reason = ('BELOW_THRESHOLD' if best < threshold else
                  'BELOW_CENTROID' if center < centroid_threshold else
                  'INSUFFICIENT_SUPPORT' if support < support_fraction else
                  'AMBIGUOUS' if second is not None and gap < margin else 'CANDIDATE')
        self.last_decision = dict(reason=reason, candidate_id=pid, top3=round(best,4),
            threshold=threshold, centroid=round(center,4), centroid_threshold=centroid_threshold,
            support=round(support,4), support_threshold=support_threshold,
            support_fraction=support_fraction, top2=round(second,4) if second is not None else 'NA',
            margin=round(gap,4) if second is not None else 'NA', minimum_margin=margin)
        log('MATCH', **self.last_decision)
        return (pid if reason=='CANDIDATE' else None), best, gap


class FaceEngine:
    def __init__(self, config: dict, data_root: Path = DATA_DIR):
        model_a = MODEL_DIR / 'face_detection_yunet_2023mar.onnx'
        model_b = MODEL_DIR / 'face_recognition_sface_2021dec.onnx'
        if not model_a.exists() or not model_b.exists():
            raise RuntimeError('Missing OpenCV Zoo model ONNX. Run: python download_models.py')
        self.cfg = dict(config)
        self.cfg.setdefault('minimum_centroid_cosine', 0.50)
        self.cfg.setdefault('minimum_support_cosine', 0.43)
        self.cfg.setdefault('minimum_support_fraction', 0.60)
        for key in ('minimum_cosine','minimum_centroid_cosine','minimum_support_cosine','minimum_margin','minimum_support_fraction'):
            v = float(self.cfg[key])
            if not math.isfinite(v) or not 0 < v <= 1:
                raise ValueError('Invalid recognition policy: '+key)
            self.cfg[key] = v
        if self.cfg['enroll_images'] != 30 or not 2 <= self.cfg['required_confirmations'] <= 5:
            raise ValueError('Expected 30 enrollment images and 2..5 confirmations')
        self.data_root = data_root
        self.lock = threading.RLock()
        self.detector = cv.FaceDetectorYN_create(str(model_a), '', (640,480), score_threshold=0.78,
                                                 nms_threshold=0.3, top_k=5000)
        self.recognizer = cv.FaceRecognizerSF_create(str(model_b), '')
        self.gallery = Gallery(data_root)
        self.gallery_revision = self.gallery.revision
        self.recent_frames = deque(maxlen=32)
        self.stream = None
        self.last_candidate_feature = None
        self.frame_reason = "READY"
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
        start = time.perf_counter()
        _, faces = self.detector.detect(frame)
        faces = [] if faces is None else list(faces)
        log('DETECT', count=len(faces), confidence=','.join(f'{f[14]:.3f}' for f in faces), ms=round((time.perf_counter()-start)*1000,1))
        if len(faces) != 1:
            self.frame_reason = 'NO_FACE' if not faces else 'MULTIPLE_FACES'
            return None, len(faces)
        face = faces[0]
        if face.shape != (15,) or not np.isfinite(face).all():
            self.frame_reason = 'INVALID_LANDMARK'
            return None, 1
        x,y,bw,bh = map(int,face[:4])
        landmarks = face[4:14].reshape(5,2)
        if (bw <= 0 or bh <= 0 or x < 0 or y < 0 or x+bw > w or y+bh > h or
                np.any(landmarks < 0) or np.any(landmarks[:,0] >= w) or np.any(landmarks[:,1] >= h)):
            self.frame_reason = 'FACE_CLIPPED'
            return None, 1
        if bw < self.cfg['minimum_face_width'] or bh < self.cfg['minimum_face_width']:
            self.frame_reason = 'FACE_TOO_SMALL'
            return None, 1
        # aligned SFace 112x112 crop supplied by the face model's landmarks
        start = time.perf_counter()
        aligned = self.recognizer.alignCrop(frame, face)
        sharp = float(cv.Laplacian(cv.cvtColor(aligned,cv.COLOR_BGR2GRAY),cv.CV_64F).var())
        if sharp < self.cfg['minimum_blur_variance']:
            self.frame_reason = f'BLURRY_{sharp:.0f}'
            return None, 1
        gray = cv.cvtColor(aligned, cv.COLOR_BGR2GRAY)
        brightness = float(gray.mean())
        if not 35 <= brightness <= 225:
            self.frame_reason = 'BAD_EXPOSURE'
            return None, 1
        emb = unit_vector(self.recognizer.feature(aligned))
        log('FEATURE', blur=round(sharp,1), brightness=round(brightness,1), ms=round((time.perf_counter()-start)*1000,1))
        self.frame_reason = 'QUALITY_OK'
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
            folder = self.data_root / 'enroll_tmp' / pid
            if folder.exists(): shutil.rmtree(folder)
            folder.mkdir(parents=True,exist_ok=True)
            self.enroll_result_id = None
            self.enrollment = {'id':pid,'name':name,'count':0,'features':[],
                               'last_saved':0.0,'folder':folder,'started':time.time(),'hashes':set()}
            self.reset_verification()

    def cancel_enroll(self):
        with self.lock:
            if self.enrollment is not None:
                shutil.rmtree(self.enrollment['folder'], ignore_errors=True)
                self.enrollment=None
                self.enroll_result_id=None
                self.reset_verification()
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

    def _process_enroll(self, frame, face, now, digest):
        e=self.enrollment
        if face is None:
            return
        if now-e['last_saved'] < self.cfg['enroll_interval_seconds']:
            return
        if digest in e['hashes']:
            self.last_status = 'ENROLL_DUPLICATE_FRAME'
            return
        if e['features'] and float(np.median(np.vstack(e['features']) @ face.feature)) < 0.43:
            self.last_status = 'ENROLL_INCONSISTENT_FACE'
            log('ENROLL', decision='REJECT', reason=self.last_status)
            return
        # Save BOTH original JPEG and feature vector. User needs to change pose.
        filename=e['folder'] / f"{e['count']+1:02d}.jpg"
        if not cv.imwrite(str(filename),frame,[int(cv.IMWRITE_JPEG_QUALITY),90]):
            self.last_status='DISK_WRITE_ERROR'
            return
        e['hashes'].add(digest)
        e['features'].append(face.feature.copy())
        e['count']+=1
        e['last_saved']=now
        self.last_status=f"ENROLL_{e['count']}/{self.cfg['enroll_images']}"
        if e['count'] >= self.cfg['enroll_images']:
            if not self.gallery.consistent(np.vstack(e['features'])):
                self.cancel_enroll()
                self.last_status = 'ENROLL_INCONSISTENT_RESTART'
                return
            destination = self.data_root / 'photos' / e['id']
            backup = destination.with_name(destination.name + '.rollback')
            destination.parent.mkdir(parents=True,exist_ok=True)
            try:
                if backup.exists(): raise RuntimeError('Existing photo recovery folder; inspect before re-enrollment')
                if destination.exists(): destination.rename(backup)
                try:
                    e['folder'].rename(destination)
                    self.gallery.add(e['id'],e['name'],e['features'])
                except Exception:
                    if destination.exists(): destination.rename(e['folder'])
                    if backup.exists(): backup.rename(destination)
                    raise
            except Exception:
                self.cancel_enroll()
                raise
            shutil.rmtree(backup,ignore_errors=True)
            self.enroll_result_id=e['id']
            self.last_status=f"ENROLL_FINISHED_ID_{e['id']}"
            self.enrollment=None
            self.streak=0
            self.streak_id=None

    def reset_verification(self):
        self.streak = 0
        self.streak_id = None
        self.last_candidate_feature = None

    def _result(self, status, reason, start, identity='', crop=None):
        from protocol import response, STATUS_MATCH, STATUS_UNKNOWN, STATUS_ERROR
        self.last_status = reason
        if status in (STATUS_UNKNOWN, STATUS_ERROR):
            self.reset_verification()
        out = response(status, identity, crop)
        log('RESULT', decision='ACCEPT' if status == STATUS_MATCH else 'NO_ATTENDANCE',
            reason=reason, sent_id=identity or 'NONE', status=status,
            total_ms=round((time.perf_counter()-start)*1000,1))
        return out

    def process(self, jpg:bytes, *, stream=None, reset=False) -> bytes:
        from protocol import STATUS_MATCH, STATUS_IDLE, STATUS_ENROLL, STATUS_UNKNOWN, STATUS_ERROR
        with self.lock:
            start=time.perf_counter()
            try:
                now=time.monotonic()
                if reset or stream != self.stream or now-self.last_frame>2.0 or self.gallery_revision != self.gallery.revision:
                    self.reset_verification()
                if now-self.last_frame>2.0:
                    self.latched_id=None
                self.stream=stream
                self.gallery_revision=self.gallery.revision
                self.last_frame=now
                self.last_score=0.0
                self.last_face_count=0
                if not jpg or len(jpg)>180000 or jpg[:2]!=b'\xff\xd8':
                    raise ValueError('Invalid JPEG')
                frame=cv.imdecode(np.frombuffer(jpg,dtype=np.uint8),cv.IMREAD_COLOR)
                if frame is None or frame.ndim!=3 or frame.shape[2]!=3 or frame.shape[0]>1200 or frame.shape[1]>1600:
                    raise ValueError('Invalid image dimensions')
                digest=hashlib.sha256(frame.tobytes()).digest()
                if digest in self.recent_frames:
                    return self._result(STATUS_UNKNOWN,'DUPLICATE_FRAME',start)
                self.recent_frames.append(digest)
                face,count=self._face_from_frame(frame)
                self.last_face_count=count
                if count==0:
                    if self.no_face_since is None:self.no_face_since=now
                    if now-self.no_face_since>1.5:self.latched_id=None
                else:
                    self.no_face_since=None
                # Use the current frame for the preview; never modify the recognition crop.
                preview_frame=frame.copy()
                if face is not None:
                    x,y,w,h=face.xywh
                    cv.rectangle(preview_frame,(x,y),(x+w,y+h),(50,230,50),2)
                ok, preview=cv.imencode('.jpg',preview_frame,[int(cv.IMWRITE_JPEG_QUALITY),75])
                self.last_preview=preview.tobytes() if ok else None
                if self.enrollment:
                    self.reset_verification()
                    self.last_status=self.frame_reason
                    if face is not None:self._process_enroll(frame,face,now,digest)
                    return self._result(STATUS_ENROLL,self.last_status,start)
                if face is None:
                    self.reset_verification()
                    return self._result(STATUS_IDLE if count==0 else STATUS_UNKNOWN,self.frame_reason,start)
                t=time.perf_counter()
                identity,score,_=self.gallery.predict(face.feature,self.cfg['minimum_cosine'],self.cfg['minimum_margin'],
                    centroid_threshold=self.cfg['minimum_centroid_cosine'],support_threshold=self.cfg['minimum_support_cosine'],
                    support_fraction=self.cfg['minimum_support_fraction'])
                self.last_score=score
                log('TIMING', match_ms=round((time.perf_counter()-t)*1000,1), employees=len(self.gallery.people))
                if identity is None:
                    return self._result(STATUS_UNKNOWN,self.gallery.last_decision['reason'],start)
                # Require consecutive fresh candidates and continuity between embeddings.
                same = self.streak_id==identity and self.last_candidate_feature is not None and float(self.last_candidate_feature @ face.feature)>=0.43
                self.streak=self.streak+1 if same else 1
                self.streak_id=identity
                self.last_candidate_feature=face.feature.copy()
                log('VERIFY', candidate_id=identity, consecutive=f"{self.streak}/{self.cfg['required_confirmations']}")
                if (self.streak>=self.cfg['required_confirmations'] and self.latched_id!=identity and
                        now-self.last_announcement>=self.cfg['match_cooldown_seconds']):
                    out=self._result(STATUS_MATCH,f'ACCEPT_{identity}',start,identity,face.crop)
                    self.latched_id=identity
                    self.last_announcement=now
                    self.reset_verification()
                    return out
                return self._result(STATUS_IDLE,f'MATCH_CANDIDATE_{identity}',start)
            except Exception as exc:
                self.reset_verification()
                self.last_score=0.0
                self.last_face_count=0
                self.last_preview=None
                log('ERROR', kind=type(exc).__name__)
                return self._result(STATUS_ERROR,'PROCESS_ERROR',start)
