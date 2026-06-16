# ESP32-S31-Korvo-1 FT8/FT4 Receiver

基于ESP32-S31-Korvo-1 的FT8/FT4 接收解码器

## 当前功能

- 板载麦克风以 16 kHz / 16-bit PCM 采集音频
- 使用 `kgoba/ft8_lib` 处理 FT8/FT4 瀑布数据并解码
- 触屏切换 FT8/FT4,查看音频电平,时隙进度和最近解码
- 保留触屏 Wi-Fi 扫描与连接页面
- Wi-Fi 连接后通过 NTP 同步 UTC 时间
- PSK Reporter 配置入口默认关闭

## 构建与烧录

```powershell
idf.py build
idf.py -p COM31 flash monitor
```

## 上游依赖
- ESP-IDF: [espressif/esp-idf](https://github.com/espressif/esp-idf)
- ESP32-S31-Korvo-1 BSP: [espressif/esp-bsp/bsp/esp32-s31-korvo-1](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32-s31-korvo-1)
- ESP BSP 公共组件: [espressif/esp-bsp](https://github.com/espressif/esp-bsp)
- ESP-IoT-Solution 组件: [espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution)
- ESP-ADF `esp_codec_dev`: [espressif/esp-adf/components/esp_codec_dev](https://github.com/espressif/esp-adf/tree/master/components/esp_codec_dev)
- ESP Video 组件: [espressif/esp-video-components](https://github.com/espressif/esp-video-components)
- ESP USB 组件: [espressif/esp-usb](https://github.com/espressif/esp-usb)
- LVGL: [lvgl/lvgl](https://github.com/lvgl/lvgl)
- FT8/FT4 解码库: [kgoba/ft8_lib](https://github.com/kgoba/ft8_lib)
