# ESP32-S3-CAM OV3660 + laptop CNN — ESP-IDF 5.2

## Thư mục
- `ESP32_S3_CAM/`: firmware ESP32-S3, camera OV3660, HTTP JPEG client, UART ảnh và điều khiển đăng ký mặt.
- `LAPTOP_SERVER/`: server Python nhận ảnh, phát hiện mặt YuNet, trích xuất đặc trưng SFace (CNN), quản trị 30 ảnh/nhân viên, cơ sở dữ liệu đặc trưng và web quản trị.
- `tests/`: kiểm thử Python không cần webcam.

## Phần mềm cần cài trên Windows
- Python **3.11 x64** (khuyên dùng); CMD/PowerShell.
- Espressif ESP-IDF **5.2.x**, ESP-IDF Tools Installer, có thể cài VS Code cùng extension Espressif IDF.
- Internet lần đầu để cài OpenCV/NumPy và tải model ONNX (không đóng gói model khoảng vài chục MB vào ZIP).
- Laptop và S3 dùng **cùng LAN Wi-Fi**, không chặn kết nối port TCP 5000.

## Chạy server laptop
1. Trong `LAPTOP_SERVER/`, chạy `SETUP_WINDOWS.bat`. Script tạo môi trường `.venv`, cài thư viện và tải `YuNet` + `SFace`.
2. Mở `config.json` (tạo từ `config.example.json`). Đặt `device_key` và `admin_key` bằng hai chuỗi **khác nhau**, dài tối thiểu 20 ký tự. Chỉnh thêm ngưỡng nếu cần sau khi đã đo nhận diện. Không nên đặt ngưỡng quá thấp vì sẽ tăng false accept.
3. Trong CMD chạy `ipconfig` để tìm **IPv4 của Wi-Fi laptop**, ví dụ `192.168.1.100`. Cho phép Python nhận kết nối TCP 5000 trên mạng Private trong Windows Firewall.
4. Chạy `START_WINDOWS.bat` (hoặc `python server.py` sau khi kích hoạt `.venv`). Mở `http://127.0.0.1:5000` trên laptop, nhập `admin_key` để xem preview, số ảnh và nhân viên.
5. Nếu không tải được model, kiểm tra Internet và chạy lại `python download_models.py`.

## Sửa cấu hình và nạp ESP32-S3
1. Mở `ESP32_S3_CAM/main/face_config.h`:
   - `FACE_WIFI_SSID`, `FACE_WIFI_PASSWORD`: Wi-Fi sử dụng.
   - `FACE_SERVER_BASE_URL`: thay `192.168.1.100` bằng địa chỉ IPv4 thật của **laptop**, giữ port 5000 và không thêm `/api/frame`.
   - `FACE_DEVICE_KEY`: giống hệt `device_key` trong `LAPTOP_SERVER/config.json`.
   - `FACE_S3_TX_GPIO=2`, `FACE_S3_RX_GPIO=1`: xác nhận hai chân này có sẵn và không xung đột trên bo S3 thực tế, đổi nếu cần. Đây là *chân UART ngoài*, không phải chân camera OV3660.
2. Kiểm tra sơ đồ camera ở `face_config.h` giống với mẫu ESP32-S3-EYE bạn đã thử stream Arduino thành công.
3. Trong ESP-IDF 5.2 Command Prompt, vào thư mục `ESP32_S3_CAM` và chạy:
   ```sh
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COMx flash monitor
   ```
4. Component Manager sẽ tải driver **espressif/esp32-camera==2.0.12** theo `main/idf_component.yml`. Cần Internet khi build lần đầu. Nếu bo không có PSRAM hoặc cấu hình camera chưa khớp, hệ thống có thể hạ độ phân giải sang QVGA hoặc không khởi tạo camera được.
5. Nối UART: S3 TX GPIO2 → MAIN RX GPIO32; S3 RX GPIO1 ← MAIN TX GPIO33; nối GND chung. Tất cả là logic 3.3 V. KHÔNG cấp 5 V vào các GPIO.

## Quy trình đăng ký 30 ảnh từ menu ESP32 MAIN
1. Nhân viên đã phải được HR tạo trên ESP32 MAIN bằng **RFID + AS608**.
2. HR nhấn `*`, nhập PIN, chọn `ENROLL FACE`, nhập ID, `C` xác nhận.
3. MAIN gửi UART `ENROLL_START:<id>` → S3 → HTTP server. Server tự tạo tên `EMPLOYEE <id>` trong web (có thể chỉnh cách hiển thị tên sau).
4. Đứng trước camera và thay đổi góc mặt: thẳng, trái, phải, ngẩng nhẹ, cúi nhẹ. Server cố gắng lưu một ảnh hợp lệ mỗi giây, đủ 30 ảnh. Tiến độ xuất hiện trên web và TFT MAIN.
5. Sau khi đủ ảnh và cập nhật **gallery vector đặc trưng của CNN SFace**, server trả `DONE:<id>`, S3 gửi `ENROLL_DONE:<id>` → MAIN bật quyền FACE vào NVS. Không cần flash lại hai bo khi thêm người.
6. Nếu bấm `B` / `#` trong lúc đăng ký, MAIN gửi hủy; nếu lỗi mạng, hệ thống không bật quyền FACE. Sau khi khắc phục, thực hiện lại.
7. Khi nhận dạng thành công, S3 gửi `FACEIMG` + bitmap 96×96 RGB565 + CRC32 + `FACE_ID` cho MAIN. MAIN kiểm tra ID/CRC và hiện ảnh trên ST7735; Apps Script vẫn quản lý ca và check-in/check-out như trước.

**Chú ý:** `SFace` là CNN pretrained. Thêm người **không fine-tune trọng số CNN** bằng 30 ảnh, mà trích xuất 30 embedding có nhãn và cập nhật gallery. Đây là lựa chọn nhẹ, phù hợp đồ án và bổ sung người ngay mà không cần huấn luyện lại toàn bộ mạng. Phần web vẫn cho phép quản lý mặt trực tiếp, nhưng ID đăng ký qua web sẽ **không tự được bật quyền FACE trên MAIN** nếu chưa có ACK đăng ký qua keypad.

## Khi xóa nhân viên
ESP32 MAIN vô hiệu hóa FACE trước khi xóa RFID/vân tay. Nó lưu dấu yêu cầu xóa `face_del` và định kỳ gửi `FACE_DELETE:<id>` sang S3. S3 gọi API laptop xóa cả vector embedding và thư mục ảnh của ID. Khi server xác nhận, S3 gửi `FACE_DELETED:<id>` cho MAIN để xóa dấu chờ. Nếu mất điện/mạng, dấu chờ vẫn nằm trong NVS trên MAIN và sẽ gửi lại khi thiết bị hoạt động.

## Tối ưu độ nhạy và chất lượng
- Camera gửi JPEG VGA khoảng 3 fps nếu có người; khi LD2410C báo không có người vẫn quét khoảng 1 khung / 1,5 giây làm phương án dự phòng, tránh bị khóa bởi radar.
- YuNet phát hiện khuôn mặt; SFace tạo vector 128 chiều, so điểm cosine; yêu cầu nhiều khung xác nhận cùng ID và độ chênh giữa hai người gần nhất; không tự chấp nhận người lạ.
- Khi thêm ảnh, cần đủ **30 ảnh hợp lệ**, có khoảng cách tối thiểu 1 giây; người dùng cần tự thay đổi góc theo hướng dẫn, mã hiện tại chưa có thuật toán bắt buộc đủ từng góc bằng ước lượng pose.
- Kết quả phụ thuộc ảnh thật, góc lắp, lens và phân bố ảnh đào tạo; không có ngưỡng chung bảo đảm 100% không nhận sai.

## Lỗi thường gặp
- Server không chạy: `ENROLL_ERROR` / timeout 180 giây ở MAIN. Kiểm tra địa chỉ IP, firewall, SSID và khóa `device_key`.
- Không thấy ảnh: kiểm tra nguồn OV3660, PSRAM, chân camera, màn hình S3 serial và web preview.
- Không có tiến độ: đảm bảo server đã nhận và xử lý frame JPEG, trên preview chỉ có một người và khuôn mặt đủ rõ.
- Màu ảnh sai: kiểm tra ESP32 MAIN đúng `MADCTL=0xC0` và đã áp dụng `tft_wire_color()` khi chép pixel.
- S3 không gửi dữ liệu: kiểm tra TX/RX đấu chéo, chung GND, pin thật và baud 115200.
- S3 kết nối HTTP: server **không cần laptop chạy CUDA/GPU**; CPU đủ cho demo nhưng tốc độ tùy máy.

## Trạng thái kiểm thử
`pytest -q tests`: 11 bài đạt (Python, bao gồm mock API từ MAIN qua S3), cú pháp `app_main.c` đã được kiểm tra Clang với mock header. **Chưa** thực hiện build/nạp firmware bằng bộ toolchain thật ESP-IDF 5.2 và phần cứng thật trong môi trường này. Test khi nhận thiết bị: thành công đúng người / người lạ / dùng ảnh in chụp trước mặt / thiếu Wi-Fi / ngắt UART / cảm biến hiện diện thấp / xóa người đã có ảnh / tái đăng ký cùng ID.

## Quyền riêng tư
Khuôn mặt là dữ liệu sinh trắc học nhạy cảm. Hạn chế truy cập `LAPTOP_SERVER/data/`, sao lưu an toàn nếu có sự đồng ý, xóa khi không cần, không công khai máy chủ trên Internet. Server dùng HTTP nội bộ có khóa tĩnh, chưa hỗ trợ TLS, liveness, mã hóa UART hay chống giả mạo dữ liệu chuyên dụng.
