"""Offline frame-level evaluation, no HTTP, no attendance and no gallery writes.
Dataset: <root>/genuine/<employee_id>/*.jpg and <root>/impostor/<label>/*.jpg.
Use held-out images from fresh capture sessions; never use data/photos as test set.
"""
import argparse
import csv
from pathlib import Path
import time
import cv2 as cv
import numpy as np
from server import cfg_load
from face_engine import FaceEngine, valid_id


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dataset',type=Path)
    parser.add_argument('--csv',type=Path,default=Path('face_scores.csv'))
    args=parser.parse_args()
    e=FaceEngine(cfg_load())
    rows=[]
    for group in ('genuine','impostor'):
        root=args.dataset/group
        for file in sorted(root.rglob('*')):
            if not file.is_file() or file.suffix.lower() not in ('.jpg','.jpeg','.png'): continue
            label=file.relative_to(root).parts[0]
            if group=='genuine' and (not valid_id(label) or label not in e.gallery.people):
                raise ValueError('Genuine directory must be a registered employee ID')
            start=time.perf_counter(); row=dict(group=group,label=label,frame=len(rows)+1,reason='',candidate_id='',top3='',centroid='',support='',top2='',accepted_id='',correct=0,ms=0)
            try:
                image=cv.imdecode(np.fromfile(file,dtype=np.uint8),cv.IMREAD_COLOR)
                if image is None or image.shape[0]>1200 or image.shape[1]>1600: raise ValueError('Invalid image')
                f,count=e._face_from_frame(image)
                if f is None: row['reason']=e.frame_reason
                else:
                    pid,_,_=e.gallery.predict(f.feature,e.cfg['minimum_cosine'],e.cfg['minimum_margin'],centroid_threshold=e.cfg['minimum_centroid_cosine'],support_threshold=e.cfg['minimum_support_cosine'],support_fraction=e.cfg['minimum_support_fraction'])
                    for k in ('reason','candidate_id','top3','centroid','support','top2'):
                        row[k]=e.gallery.last_decision.get(k,'')
                    row['accepted_id']=pid or ''
            except Exception as exc: row['reason']='ERROR_'+type(exc).__name__
            row['correct']=int(row['accepted_id']==label if group=='genuine' else not row['accepted_id'])
            row['ms']=round((time.perf_counter()-start)*1000,2); rows.append(row)
    if not rows: raise SystemExit('No labeled test images found')
    with args.csv.open('w',newline='',encoding='utf-8-sig') as out:
        writer=csv.DictWriter(out,fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    for group in ('genuine','impostor'):
        subset=[r for r in rows if r['group']==group]
        if not subset: print(group, 'N/A (no samples)'); continue
        errors=sum(not r['correct'] for r in subset)
        scores=[float(r['top3']) for r in subset if r['top3']!='']
        print(group, f'frame_errors={errors}/{len(subset)} ({100*errors/len(subset):.2f}%)', 'top3_quantiles_5_50_95=',np.percentile(scores,[5,50,95]).round(4).tolist() if scores else 'N/A')
    print('Frame-level candidate metrics only; measure event FAR/FRR separately with consecutive live frames.')

if __name__=='__main__': main()
