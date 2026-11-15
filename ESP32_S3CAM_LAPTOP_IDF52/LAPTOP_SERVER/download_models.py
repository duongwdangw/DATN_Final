"""Fetch trusted pre-trained OpenCV Zoo neural models (not included in zip).
Size validation rejects Git LFS pointer files. HTTPS download only.
"""
from pathlib import Path
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent
MODEL_DIR = ROOT / "models"
SPECS = {
    "face_detection_yunet_2023mar.onnx": (200_000, "face_detection_yunet"),
    "face_recognition_sface_2021dec.onnx": (15_000_000, "face_recognition_sface"),
}


def download(name: str, min_size: int, category: str) -> None:
    MODEL_DIR.mkdir(exist_ok=True)
    path = MODEL_DIR / name
    if path.exists() and path.stat().st_size >= min_size:
        print(f"[OK] {name}: {path.stat().st_size / 1e6:.1f} MB")
        return
    # OpenCV Zoo files are tracked with Git LFS. Hugging Face resolve serves
    # the binary weight file, rather than the small pointer in git history.
    url = f"https://huggingface.co/opencv/opencv_zoo/resolve/main/models/{category}/{name}"
    print(f"Downloading {name}...")
    request = Request(url, headers={"User-Agent": "ESP32-Face-Lab/1.0"})
    tmp = path.with_suffix(".download")
    try:
        with urlopen(request, timeout=120) as response, tmp.open("wb") as out:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                out.write(chunk)
        if tmp.stat().st_size < min_size:
            raise RuntimeError(f"Downloaded file too small: {tmp.stat().st_size} bytes")
        tmp.replace(path)
    finally:
        tmp.unlink(missing_ok=True)
    print(f"[OK] {path}")

if __name__ == "__main__":
    for filename, (minimum, folder) in SPECS.items():
        download(filename, minimum, folder)
    print("Both neural networks are ready. Run: python server.py")
