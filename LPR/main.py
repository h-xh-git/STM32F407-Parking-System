from maix import camera, display, image, nn, app
import time

# ===================== 配置区域 =====================
UART_BAUD = 115200
MIN_PARKING_SECONDS = 30  # 演示场景出场判定：同一车牌入场后至少 30s 再被识别到才判为出场
DEBOUNCE_TIME = 2000      # 防抖时间（毫秒），同一车牌 2 秒内不重复识别
MAX_PLATE_RECORDS = 128   # 车牌状态记录上限，超出后清理最旧记录
MAX_DISPLAY_VEHICLES = 5  # 屏幕最多显示最近 5 辆在场车辆
ERROR_LOG_INTERVAL = 100  # 异常日志限频：每 100 帧最多打印 1 条
# ===================================================

# 加载OCR
try:
    ocr_detector = nn.PP_OCR("/root/models/pp_ocr.mud")
    print("[OK] OCR模型加载成功")
except Exception as e:
    print(f"[ERR] OCR模型加载失败: {e}")
    ocr_detector = None

# 摄像头配置
try:
    cam = camera.Camera(320, 240, image.Format.FMT_BGR888)
    disp = display.Display()
    print("[OK] 摄像头初始化成功")
except Exception as e:
    print(f"[ERR] 摄像头初始化失败: {e}")
    cam = None
    disp = None

# 状态管理（时间统一用 time.monotonic()，避免系统时间跳变影响防抖与出场判定）
last_plate = ""
last_time = 0.0
plate_records = {}  # 车牌 -> {'time': monotonic 秒, 'status': 'in'/'out'}，有界
recognition_enabled = True  # 启动后自动开始识别
frame_count = 0
last_error_frame = -ERROR_LOG_INTERVAL
uart_error_count = 0
uart_retry_count = 0


def log_error_limited(message):
    """异常日志限频：每 ERROR_LOG_INTERVAL 帧最多打印 1 条，避免刷屏拖慢主循环。"""
    global last_error_frame
    if frame_count - last_error_frame >= ERROR_LOG_INTERVAL:
        last_error_frame = frame_count
        print(message)


def trim_plate_records():
    """plate_records 是有界字典：超过 MAX_PLATE_RECORDS 条时清理最旧的记录。"""
    while len(plate_records) > MAX_PLATE_RECORDS:
        oldest = min(plate_records.items(), key=lambda item: item[1]['time'])[0]
        del plate_records[oldest]

# 省份列表
PROVINCES = ["京", "津", "沪", "渝", "冀", "豫", "云", "辽", "黑", "湘", "皖", "鲁", "新", "苏", "浙", "赣", "鄂", "桂", "甘", "晋", "蒙", "陕", "吉", "闽", "贵", "粤", "青", "藏", "川", "宁", "琼"]

def normalize_plate(text):
    """标准化车牌格式：过滤非法字符，要求 7/8 位、首字符为省份汉字、第 2 位为大写字母。

    返回标准化后的车牌；不满足规则时返回空串（上层据此丢弃识别结果）。
    """
    if not text:
        return ""

    result = ""
    for c in text:
        if '\u4e00' <= c <= '\u9fff':
            # 保留汉字（省份简称），其余非法字符全部过滤
            result += c
        elif c.isascii() and c.isalnum():
            # 字母统一转大写：中文车牌字母均为大写，避免 OCR 小写导致漏识别
            result += c.upper()

    if len(result) not in (7, 8):
        return ""
    if result[0] not in PROVINCES:
        return ""
    if not ('A' <= result[1] <= 'Z'):
        return ""
    return result

def is_valid_plate(text):
    """验证车牌有效性（规则与 normalize_plate 保持一致）"""
    return normalize_plate(text) != ""

# UART初始化
uart = None
uart_info = ""

uart_list = [
    ("/dev/ttyS0", "UART0")
]

maix_uart = None
try:
    from maix import uart as maix_uart
except ImportError:
    try:
        import uart as maix_uart
    except ImportError:
        maix_uart = None

if maix_uart:
    for dev, name in uart_list:
        try:
            uart = maix_uart.UART(dev, UART_BAUD)
            uart_info = f"{name} @ {dev}, {UART_BAUD} baud"
            print(f"[OK] 串口初始化成功: {uart_info}")
            break
        except Exception as e:
            print(f"[WARN] 串口 {name} ({dev}) 失败: {e}")

if not uart:
    print("[ERR] 所有串口都初始化失败！")

def send_to_host(message):
    """发送消息到STM32；失败时重试一次，并统计失败/重试次数。"""
    global uart, uart_error_count, uart_retry_count
    if not uart:
        return False

    # 统一使用 UTF-8 编码（ASCII 是 UTF-8 的子集），保持 IN:车牌 / OUT:车牌 协议不变
    data = f"{message}\n".encode('utf-8')
    for attempt in range(2):
        try:
            uart.write(data)
            return True
        except Exception as e:
            if attempt == 0:
                uart_retry_count += 1
                log_error_limited(f"[WARN] UART发送失败，重试一次: {e}")
            else:
                uart_error_count += 1
                log_error_limited(f"[ERR] UART发送失败: {e}")
    return False

def handle_plate(raw_plate):
    """处理识别到的车牌：防抖后按 MIN_PARKING_SECONDS 判定入场/出场。"""
    global last_plate, last_time

    plate = normalize_plate(raw_plate)
    if not plate:
        return

    now = time.monotonic()

    # 防抖：同一车牌 DEBOUNCE_TIME 毫秒内不重复处理
    if plate == last_plate and (now - last_time) * 1000.0 < DEBOUNCE_TIME:
        print(f"[忽略] 重复识别: {plate}")
        return

    last_plate = plate
    last_time = now

    record = plate_records.get(plate)
    if record is None:
        plate_records[plate] = {'time': now, 'status': 'in'}
        trim_plate_records()
        print(f"IN: {plate}")
        send_to_host(f"IN:{plate}")
        return

    if record['status'] == 'in':
        elapsed = now - record['time']
        if elapsed >= MIN_PARKING_SECONDS:
            # 演示场景：同一车牌再次被识别且已超过最短停车时间，即判为出场
            plate_records[plate] = {'time': now, 'status': 'out'}
            print(f"OUT: {plate}")
            send_to_host(f"OUT:{plate}")
        else:
            remaining = int(MIN_PARKING_SECONDS - elapsed)
            print(f"{plate} in parking, need {remaining}s")
    else:
        plate_records[plate] = {'time': now, 'status': 'in'}
        print(f"IN: {plate}")
        send_to_host(f"IN:{plate}")

print("=" * 50)
print("=== 车牌识别系统 v4.1 (ASCII协议版) ===")
print(f"配置: 防抖={DEBOUNCE_TIME}ms, 最短停车={MIN_PARKING_SECONDS}秒")
print(f"串口: {uart_info if uart else '未初始化'}")
print("协议格式: IN:车牌 / OUT:车牌")
print("自动识别已启动...")
print("=" * 50)

# 主循环
while not app.need_exit():
    frame_count += 1
    try:
        if cam:
            img = cam.read()

            if recognition_enabled and ocr_detector and img:
                try:
                    results = ocr_detector.detect(img)
                    found_plates = []

                    for res in results:
                        text = res.char_str().strip()
                        if is_valid_plate(text):
                            plate = normalize_plate(text)
                            if plate and plate not in found_plates:
                                found_plates.append(plate)

                    if found_plates:
                        handle_plate(found_plates[0])
                        for plate in found_plates:
                            img.draw_string(5, 20, f"车牌: {plate}", image.COLOR_BLUE, 1.5)
                except Exception as e:
                    # 去掉裸 except: pass，异常打印限频，避免每帧刷屏
                    log_error_limited(f"[ERR] OCR识别异常: {e}")
            else:
                if img:
                    img.draw_string(5, 20, "无OCR模型", image.COLOR_RED, 1.5)

            if img and disp:
                status_y = 40
                # 屏幕最多显示最近 MAX_DISPLAY_VEHICLES 辆在场车辆
                parked = sorted(
                    ((plate, record) for plate, record in plate_records.items()
                     if record['status'] == 'in'),
                    key=lambda item: item[1]['time'],
                    reverse=True
                )[:MAX_DISPLAY_VEHICLES]
                for plate, _record in parked:
                    img.draw_string(5, status_y, f"{plate}: 场内", image.COLOR_GREEN, 1.2)
                    status_y += 25

                uart_status = "UART OK" if uart else "UART ERR"
                uart_color = image.COLOR_GREEN if uart else image.COLOR_RED
                img.draw_string(5, 220, f"{uart_status}", uart_color, 1.0)

                mode_text = "识别中" if recognition_enabled else "已停止"
                mode_color = image.COLOR_GREEN if recognition_enabled else image.COLOR_RED
                img.draw_string(200, 220, f"模式: {mode_text}", mode_color, 1.0)

                disp.show(img)

        time.sleep(0.01)

    except Exception as e:
        log_error_limited(f"[ERR] 主循环错误: {e}")
        time.sleep(1)

print("程序退出")
