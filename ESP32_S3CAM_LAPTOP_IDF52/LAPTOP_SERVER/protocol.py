"""Network response protocol shared conceptually with ESP32-S3 firmware.
FCR1 binary frame = <4sBHHHI + ASCII id + RGB565 little-endian image.
"""
import struct
import numpy as np
import cv2

HEADER = struct.Struct('<4sBHHHI')
MAGIC = b'FCR1'
STATUS_IDLE = 0
STATUS_MATCH = 1
STATUS_UNKNOWN = 2
STATUS_ENROLL = 3
STATUS_ERROR = 4
THUMB_SIZE = 96


def rgb565_bytes(image_bgr: np.ndarray) -> bytes:
    """Convert cropped BGR image to 96x96 RGB565 pixel words (little endian)."""
    image = cv2.resize(image_bgr, (THUMB_SIZE, THUMB_SIZE), interpolation=cv2.INTER_AREA)
    r = image[:, :, 2].astype(np.uint16)
    g = image[:, :, 1].astype(np.uint16)
    b = image[:, :, 0].astype(np.uint16)
    words = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return words.astype('<u2').tobytes()


def response(status: int = STATUS_IDLE, employee_id: str = '', face_bgr=None) -> bytes:
    if status not in (STATUS_IDLE, STATUS_MATCH, STATUS_UNKNOWN, STATUS_ENROLL, STATUS_ERROR):
        raise ValueError('Invalid FCR1 status')
    if status != STATUS_MATCH and (employee_id or face_bgr is not None):
        raise ValueError('Non-MATCH must not carry identity or image')
    if status == STATUS_MATCH:
        if not (employee_id.isascii() and employee_id.isdigit() and 0 < len(employee_id) <= 5 and 1 <= int(employee_id) <= 65535 and employee_id == str(int(employee_id))):
            raise ValueError("Employee ID must be canonical decimal 1..65535")
        if face_bgr is None:
            raise ValueError("Successful recognition must include a face crop")
        image = rgb565_bytes(face_bgr)
        ident = employee_id.encode('ascii')
        return HEADER.pack(MAGIC, status, len(ident), THUMB_SIZE, THUMB_SIZE, len(image)) + ident + image
    return HEADER.pack(MAGIC, status, 0, 0, 0, 0)


def unpack(data: bytes):
    magic, status, id_len, w, h, length = HEADER.unpack_from(data)
    if magic != MAGIC or len(data) != HEADER.size + id_len + length:
        raise ValueError("Corrupt binary reply")
    if status not in (STATUS_IDLE, STATUS_MATCH, STATUS_UNKNOWN, STATUS_ENROLL, STATUS_ERROR):
        raise ValueError('Invalid FCR1 status')
    if status != STATUS_MATCH and any((id_len,w,h,length)):
        raise ValueError('Non-MATCH with payload')
    person_id = data[HEADER.size:HEADER.size + id_len].decode('ascii')
    if status == STATUS_MATCH and (not person_id.isascii() or not person_id.isdecimal() or not 1 <= len(person_id) <= 5 or not 1 <= int(person_id) <= 65535 or person_id != str(int(person_id)) or (w,h,length)!=(96,96,18432)):
        raise ValueError('Invalid MATCH payload')
    return status, person_id, w, h, data[HEADER.size + id_len:]
