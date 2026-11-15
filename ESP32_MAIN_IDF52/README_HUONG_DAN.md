# ESP32 MAIN — Chấm công 3 phương thức / ESP-IDF 5.2

## Phạm vi
Bản tích hợp xây dựng từ **file main.c ST7735 RGB đã được người dùng thử thực tế** (`MADCTL=0xC0`, framebuffer RGB565 đảo byte). Các module RFID RC522, vân tay AS608, keypad, LD2410C, Wi-Fi/NTP và ghi Google Sheets được giữ theo cấu trúc firmware cũ. **Không thay đổi Google Apps Script hoặc công thức check-in/out / ca làm / tăng ca.**

## Điểm mới
- Menu quản trị: `ADD EMPLOYEE` → `DELETE EMPLOYEE` → `ENROLL FACE`.
- Thêm nhân viên: vẫn quét RFID và đăng ký, xác minh vân tay trên AS608 như logic cũ; không bắt buộc đăng ký mặt ngay.
- Sau này HR vào `ENROLL FACE`, nhập **mã nhân viên đã đăng ký RFID/vân tay**, nhấn C. ESP32 MAIN gửi `ENROLL_START:<id>` cho S3-CAM.
- S3-CAM liên hệ laptop để chụp 30 ảnh khoảng 1 ảnh/giây. Màn hình ESP32 MAIN hiển thị tiến độ và kết quả; khi nhận `ENROLL_DONE:<id>` mới bật quyền dùng FACE trong NVS.
- Chấm công FACE: S3 gửi RGB565 96×96 với CRC32 qua UART và sau đó gửi `FACE_ID:<id>`. MAIN xác thực CRC, ID còn có RFID và cờ FACE đã được đăng ký xong trước khi ghi chấm công lên Google Sheets. TFT hiển thị ảnh mặt trên nền xanh lá và ID nhân viên.
- Không nhận diện người lạ: MAIN hiện nền đỏ khi S3 gửi `FACE_UNKNOWN` (phía S3 có giới hạn tần suất).
- Khi xóa nhân viên: MAIN **xóa quyền FACE trước**, đồng thời lưu yêu cầu xóa dữ liệu mặt trên laptop trong NVS `face_del`, gửi lại mỗi 5 giây đến khi nhận `FACE_DELETED:<id>`. Bằng cách này, dữ liệu còn sót ở PC không đủ để chấm công sau khi đã xóa nhân viên trên MAIN.
- Tắt ứng dụng server hoặc ngắt Wi-Fi: RFID/vân tay giữ luồng chấm công cũ; FACE sẽ không hoạt động cho đến khi laptop hoạt động trở lại.

## Yêu cầu phần cứng
- ESP32 MAIN; RC522 SPI; AS608 UART2; keypad 4×4; LCD TFT ST7735 128×160; LD2410C.
- ESP32-S3-CAM OV3660 với sơ đồ chân camera ESP32-S3-EYE đã stream được trong thử nghiệm Arduino của người dùng.
- UART 3.3V, **chung GND**, đấu chéo:
  - ESP32 MAIN GPIO33 (TX UART1) → S3 GPIO1 (RX UART1)
  - ESP32 MAIN GPIO32 (RX UART1) ← S3 GPIO2 (TX UART1)
  - GND MAIN ↔ GND S3
- LƯU Ý: GPIO1/GPIO2 chỉ là **cấu hình mặc định trong dự án**. Kiểm tra xem trên bo S3 thực tế các chân này có được đưa ra header và không bị dùng cho ngoại vi khác hay không. Nếu không, đổi `FACE_S3_RX_GPIO` / `FACE_S3_TX_GPIO` trong dự án S3. **Không nối tín hiệu 5 V vào GPIO 3.3 V.**
- UART hai đầu 115200, 8N1.

## Nạp firmware
1. Sao lưu nguyên project ESP32 đang chạy và **sao lưu dữ liệu NVS** nếu có thể. Không dùng `erase-flash` vì sẽ xóa UID/thông tin cấu hình nhân viên.
2. Cài ESP-IDF **5.2.x** và ESP-IDF Tools for Windows (có thể dùng ESP-IDF Command Prompt hoặc ESP-IDF extension của VS Code).
3. Vào thư mục `ESP32_MAIN_IDF52`, chạy:
   ```sh
   idf.py set-target esp32
   idf.py build
   idf.py -p COMx flash monitor
   ```
   Thay `COMx` bằng cổng Serial thật.
4. Nếu có thông tin Wi-Fi hay Google Apps Script riêng trong firmware hiện tại, kiểm tra `main/wifi_time.c` và `main/google_sheet.c` trước khi build. Bản giao giữ các file từ code gốc, không tự ý thay khóa/URL.
5. Không nạp lại S3 cho đến khi hoàn tất kiểm thử RFID/vân tay và màn hình MAIN.

## Dùng keypad
- `*`: vào đăng nhập admin PIN như trước, `C`: xác nhận.
- Tại menu: `A`: đổi lần lượt **ADD → DELETE → ENROLL FACE**; `C`: chọn; `B`: quay lại/thoát; `#`: thoát về chấm công.
- Chọn `ADD EMPLOYEE`: nhập ID rồi `C`, quét thẻ mới và làm các bước AS608 như bản cũ. Sau hoàn tất, RFID và vân tay đã chấm công được mà không cần gương mặt.
- Chọn `ENROLL FACE`: nhập ID đã tồn tại; `C`: gửi yêu cầu đăng ký mặt tới laptop. Nhìn thẳng, quay trái/phải, ngẩng/cúi theo hướng dẫn trên web cho đến đủ 30 ảnh. Có thể `B` hoặc `#` để hủy.
- Chọn `DELETE EMPLOYEE`: nhập ID; `C`: xóa thông tin RFID và AS608 như trước, đồng thời vô hiệu hóa FACE tại MAIN và yêu cầu server xóa mặt.
- Nếu hiển thị `SYNC FACE DELETE`: đợi S3 và laptop khởi chạy để nhận ACK xóa cũ rồi thử lại. Đây là **khóa an toàn** khi tái sử dụng cùng ID.

## Giao thức UART1
- MAIN → S3: `PRESENCE:0/1`, `ENROLL_START:<id>`, `ENROLL_CANCEL:<id>`, `FACE_DELETE:<id>`.
- S3 → MAIN: `CAM_READY`, `ENROLL_STARTED:<id>`, `ENROLL_PROGRESS:<id>:<n>`, `ENROLL_DONE:<id>`, `ENROLL_ERROR:<id>`, `FACE_DELETED:<id>`, `FACE_UNKNOWN`.
- S3 → MAIN khi thành công: `FACEIMG:<id>:96:96:18432:<CRC32_HEX>\n` + **18.432 byte RGB565 little-endian** + `FACE_ID:<id>\n`.
- MAIN chỉ cấp quyền chấm công FACE nếu bản tin ảnh có CRC32 hợp lệ, ID hai bản tin khớp, không quá cũ, ID còn tồn tại, và cờ NVS FACE đã bật.
- ST7735: `MADCTL (0x36)=0xC0`, `COLMOD (0x3A)=0x05`; dữ liệu màu gửi MSB trước. Sau `tx_color`, gửi NOP 0x00 để đợi DMA hoàn tất trước khi nhả mutex (theo cơ chế hàng đợi SPI của ESP-IDF 5.2).

## Bài test nên thực hiện với bo thật
1. Giữ nguyên NVS hiện có; quét RFID, vân tay cũ, thử thẻ lạ/vân tay lạ; quan sát màu xanh/đỏ và dòng dữ liệu Google Sheets.
2. Đăng ký người mới bằng RFID+AS608, **chưa đăng ký FACE**: hai phương thức cũ vẫn hoạt động; `FACE_ID` cùng số ID phải **bị từ chối**.
3. Khởi động laptop server và S3, vào `ENROLL FACE`, nhập ID đã đăng ký. Kiểm tra số ảnh trên màn và web, sau đó `FACE ENROLLED`.
4. Chấm công qua gương mặt: một ảnh mặt 96×96 trên LCD với màu đúng, một sự kiện FACE được ghi lên Google Sheets; không gửi bản ghi trùng cho một lần xác nhận.
5. Ngắt Wi-Fi hoặc tắt laptop trong lúc đăng ký: hệ thống báo lỗi/timeout, không bật FACE; RFID và vân tay vẫn hoạt động.
6. Xóa nhân viên khi laptop tắt: quét lại mặt không được chấm công, sau khi laptop bật lại kiểm tra xóa trên giao diện quản trị. Thử tái sử dụng ID chỉ sau khi nhận ACK.
7. Hủy đăng ký mặt khi chưa đủ 30 ảnh; thử khởi động lại; kiểm tra không vô tình bật FACE.
8. Kiểm tra heap, task stack, LCD, RC522 sau nhiều lần nhận ảnh liên tiếp.

## Bảo mật / giới hạn
- Webcam không có cơ chế liveness/anti-spoofing chuyên dụng: ảnh hoặc video của người hợp lệ có thể vượt qua CNN trong một số điều kiện. Đừng dùng FACE làm cơ chế mở khóa bảo vệ duy nhất.
- `CRC32` chống lỗi truyền UART, **không phải xác thực mật mã**. Ai có quyền truy cập vật lý UART vẫn có thể giả mạo dữ liệu nếu không bổ sung MAC/challenge.
- Đường từ S3 tới laptop hiện dùng HTTP trong mạng LAN; tránh Wi-Fi khách hoặc mạng công cộng.
- PIN quản trị mặc định của bản gốc có thể còn là `1234`; cần thay PIN/đặt giới hạn tiếp cận trước khi triển khai thực tế.
- Server/laptop cần luôn chạy để FACE hoạt động. Ảnh và vector đặc trưng chứa dữ liệu sinh trắc học nhạy cảm; cần giới hạn quyền truy cập và có quy trình xóa.

## Tính xác nhận
Đã chạy kiểm thử Python (11 bài) trên module laptop, static C syntax check bằng Clang với mock header. **Chưa** thể thực hiện `idf.py build`, nạp lên thiết bị và thử toàn hệ thống ở môi trường bàn giao. Không có ai có thể bảo đảm tuyệt đối firmware chạy đúng mà chưa test trên đúng bo của bạn. Nếu firmware không build, hãy gửi **toàn bộ log lỗi đầu tiên**, không thay ngẫu nhiên thư viện hoặc chân GPIO.

## Khôi phục nhanh
Nếu xảy ra sự cố, dùng nguyên project `esp32main(2).zip` hoặc bản RGB đã chạy ổn của bạn để nạp lại; **không xóa NVS**. Firmware MAIN mới dùng namespace `face` và `face_del` độc lập với `users` và `cfg` hiện tại.
