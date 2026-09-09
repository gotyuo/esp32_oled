#!/bin/bash
# ESP 固件编译测试脚本
# 用法: ./test_compile.sh [esp32|esp8266|all]

set -e

FIRMWARE_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-all}"

echo "=========================================="
echo "ESP 固件编译测试"
echo "=========================================="
echo ""

# 检查 PlatformIO 是否安装
if ! command -v platformio &> /dev/null; then
    echo "❌ PlatformIO 未安装"
    echo "   安装方法: pip install platformio"
    echo "   或访问: https://platformio.org/install"
    exit 1
fi

# 检查 ESP32 工具链
check_toolchain() {
    local target=$1
    local platform=$2
    echo "检查 $platform 工具链..."
    
    cd "$FIRMWARE_DIR"
    
    if [ "$target" = "esp32" ]; then
        if platformio devtools --version 2>/dev/null | grep -q "espressif32"; then
            echo "   ✅ ESP32 工具链已就绪"
            return 0
        else
            echo "   ⚠️  ESP32 工具链未安装, 将在首次编译时自动下载"
            return 0
        fi
    elif [ "$target" = "esp8266" ]; then
        if platformio devtools --version 2>/dev/null | grep -q "espressif8266"; then
            echo "   ✅ ESP8266 工具链已就绪"
            return 0
        else
            echo "   ⚠️  ESP8266 工具链未安装, 将在首次编译时自动下载"
            return 0
        fi
    fi
}

# 编译 ESP32
compile_esp32() {
    echo ""
    echo "=========================================="
    echo "编译 ESP32 固件..."
    echo "=========================================="
    
    cd "$FIRMWARE_DIR"
    
    # 创建临时 PlatformIO 配置
    cat > platformio_esp32.ini << 'EOF'
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
upload_speed = 921600
monitor_speed = 115200
lib_deps =
    adafruit/Adafruit SSD1306@^2.5.3
    adafruit/Adafruit GFX Library@^1.11.5
    adafruit/DHT sensor library@^1.4.5
    adafruit/Adafruit BMP280 Unified Sensor@^2.3.1
    bblanchon/ArduinoJson@^6.21.3
    jeroen/vl53l1x@^3.0.0
build_flags =
    -D ARDUINO_USB_CDC_ON_BOOT=1
    -D FIRMWARE_VERSION=\"2.0.0\"
    -D FIRMWARE_BUILD=\"$(date +%Y-%m-%d)\"
    -D WIFI_SSID=\"YOUR_WIFI_SSID\"
    -D WIFI_PASSWORD=\"YOUR_WIFI_PASSWORD\"
    -D SERVER_HOST=\"192.168.68.119\"
    -D SERVER_PORT=12090
    -D AUTH_TOKEN=\"REPLACE_WITH_…_TOKEN\"
    -D DEVICE_ID=\"esp32-001\"
EOF
    
    echo "   编译中..."
    platformio run -c -e esp32 2>&1 | tail -20
    
    # 检查编译结果
    if [ -f ".pio/build/esp32/firmware.bin" ]; then
        local size=$(stat -c%s .pio/build/esp32/firmware.bin 2>/dev/null || stat -f%z .pio/build/esp32/firmware.bin 2>/dev/null)
        echo ""
        echo "   ✅ ESP32 编译成功"
        echo "   固件大小: ${size} bytes"
        echo "   固件路径: .pio/build/esp32/firmware.bin"
        return 0
    else
        echo ""
        echo "   ❌ ESP32 编译失败"
        return 1
    fi
}

# 编译 ESP8266
compile_esp8266() {
    echo ""
    echo "=========================================="
    echo "编译 ESP8266 固件..."
    echo "=========================================="
    
    cd "$FIRMWARE_DIR"
    
    # 创建临时 PlatformIO 配置
    cat > platformio_esp8266.ini << 'EOF'
[env:esp8266]
platform = espressif8266
board = esp12e
framework = arduino
upload_speed = 921600
monitor_speed = 115200
lib_deps =
    adafruit/Adafruit SSD1306@^2.5.3
    adafruit/Adafruit GFX Library@^1.11.5
    adafruit/DHT sensor library@^1.4.5
    bblanchon/ArduinoJson@^6.21.3
build_flags =
    -D FIRMWARE_VERSION=\"2.0.0\"
    -D FIRMWARE_BUILD=\"$(date +%Y-%m-%d)\"
    -D WIFI_SSID=\"YOUR_WIFI_SSID\"
    -D WIFI_PASSWORD=\"YOUR_WIFI_PASSWORD\"
    -D SERVER_HOST=\"192.168.68.119\"
    -D SERVER_PORT=12090
    -D AUTH_TOKEN=\"REPLACE_WITH_…_TOKEN\"
    -D DEVICE_ID=\"esp8266-001\"
EOF
    
    echo "   编译中..."
    platformio run -c -e esp8266 2>&1 | tail -20
    
    # 检查编译结果
    if [ -f ".pio/build/esp8266/firmware.bin" ]; then
        local size=$(stat -c%s .pio/build/esp8266/firmware.bin 2>/dev/null || stat -f%z .pio/build/esp8266/firmware.bin 2>/dev/null)
        echo ""
        echo "   ✅ ESP8266 编译成功"
        echo "   固件大小: ${size} bytes"
        echo "   固件路径: .pio/build/esp8266/firmware.bin"
        return 0
    else
        echo ""
        echo "   ❌ ESP8266 编译失败"
        return 1
    fi
}

# 检查 API 端点
check_api_endpoints() {
    echo ""
    echo "=========================================="
    echo "检查 API 端点..."
    echo "=========================================="
    
    local errors=0
    
    # 检查 ESP32
    echo ""
    echo "ESP32 platform_client.cpp:"
    grep -q '"/api/ingest"' "$FIRMWARE_DIR/esp32/platform_client.cpp" && echo "   ✅ /api/ingest" || { echo "   ❌ /api/ingest 缺失"; ((errors++)); }
    grep -q '"/api/ota/list"' "$FIRMWARE_DIR/esp32/platform_client.cpp" && echo "   ✅ /api/ota/list" || { echo "   ❌ /api/ota/list 缺失"; ((errors++)); }
    grep -q '"/api/devices"' "$FIRMWARE_DIR/esp32/platform_client.cpp" && echo "   ✅ /api/devices" || { echo "   ❌ /api/devices 缺失"; ((errors++)); }
    
    # 检查 ESP8266
    echo ""
    echo "ESP8266 platform_client.cpp:"
    grep -q '"/api/ingest"' "$FIRMWARE_DIR/esp8266/platform_client.cpp" && echo "   ✅ /api/ingest" || { echo "   ❌ /api/ingest 缺失"; ((errors++)); }
    grep -q '"/api/ota/list"' "$FIRMWARE_DIR/esp8266/platform_client.cpp" && echo "   ✅ /api/ota/list" || { echo "   ❌ /api/ota/list 缺失"; ((errors++)); }
    grep -q '"/api/ota/push/"' "$FIRMWARE_DIR/esp8266/platform_client.cpp" && echo "   ✅ /api/ota/push/" || { echo "   ❌ /api/ota/push/ 缺失"; ((errors++)); }
    
    if [ $errors -eq 0 ]; then
        echo ""
        echo "   ✅ 所有 API 端点正确"
        return 0
    else
        echo ""
        echo "   ❌ 发现 $errors 个错误端点"
        return 1
    fi
}

# 检查引脚配置
check_pins() {
    echo ""
    echo "=========================================="
    echo "检查引脚配置..."
    echo "=========================================="
    
    local errors=0
    
    # ESP32
    echo ""
    echo "ESP32 引脚:"
    grep -q '#define OLED_SDA_PIN.*21' "$FIRMWARE_DIR/esp32/envmon_esp32.h" && echo "   ✅ OLED SDA = 21" || { echo "   ❌ OLED SDA 错误"; ((errors++)); }
    grep -q '#define OLED_SCL_PIN.*22' "$FIRMWARE_DIR/esp32/envmon_esp32.h" && echo "   ✅ OLED SCL = 22" || { echo "   ❌ OLED SCL 错误"; ((errors++)); }
    grep -q '#define TEMP_PIN.*4' "$FIRMWARE_DIR/esp32/envmon_esp32.h" && echo "   ✅ DHT22 = GPIO4" || { echo "   ❌ DHT22 引脚错误"; ((errors++)); }
    
    # ESP8266
    echo ""
    echo "ESP8266 引脚:"
    grep -q '#define OLED_SDA.*4' "$FIRMWARE_DIR/esp8266/envmon_esp8266.h" && echo "   ✅ OLED SDA = 4" || { echo "   ❌ OLED SDA 错误"; ((errors++)); }
    grep -q '#define OLED_SCL.*5' "$FIRMWARE_DIR/esp8266/envmon_esp8266.h" && echo "   ✅ OLED SCL = 5" || { echo "   ❌ OLED SCL 错误"; ((errors++)); }
    grep -q '#define TEMP_PIN.*3' "$FIRMWARE_DIR/esp8266/envmon_esp8266.h" && echo "   ✅ DHT11 = GPIO3" || { echo "   ❌ DHT11 引脚错误"; ((errors++)); }
    
    if [ $errors -eq 0 ]; then
        echo ""
        echo "   ✅ 所有引脚配置正确"
        return 0
    else
        echo ""
        echo "   ❌ 发现 $errors 个引脚错误"
        return 1
    fi
}

# 主逻辑
if [ "$TARGET" = "all" ]; then
    check_toolchain esp32 espressif32
    check_toolchain esp8266 espressif8266
    compile_esp32 || true
    compile_esp8266 || true
    check_api_endpoints
    check_pins
elif [ "$TARGET" = "esp32" ]; then
    check_toolchain esp32 espressif32
    compile_esp32
    check_api_endpoints
    check_pins
elif [ "$TARGET" = "esp8266" ]; then
    check_toolchain esp8266 espressif8266
    compile_esp8266
    check_api_endpoints
    check_pins
else
    echo "用法: $0 [esp32|esp8266|all]"
    exit 1
fi

echo ""
echo "=========================================="
echo "测试完成"
echo "=========================================="
