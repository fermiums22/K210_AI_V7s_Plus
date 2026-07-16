# K210 robot controller

> [!IMPORTANT]
> Разработка остановлена 16 июля 2026 года. Проект перенесён в
> `ESP32S3_V7s_Plus`: GoPro HERO12 передаёт видео на домашний сервер, а
> ESP32-S3 отвечает за команды, звук, OTA и связь со STM32. Связка
> K210 + ESP8285 потребовала слишком сложного межпроцессорного транспорта и
> не позволила быстро получить стабильные Wi-Fi, обновления, камеру и звук,
> необходимые для перехода к сценариям Home Assistant.

Основная прошивка K210 для связки с ESP8285. ESP является SPI master, K210 — slave.

## Структура

- `firmware_v2/kstream_slave` — рабочая прошивка контроллера и KSTREAM transport.
- `firmware_v2/recovery_esp` — восстановление ESP через UART из K210.
- `firmware_v2/common` — общие flash/boot/hash модули.
- `src` — модули робота: камера, экран, звук и SD.
- `protocol` — KSTREAM/KNET/KUPDATE.
- `lib`, `third_party`, `cmake`, `lds` — SDK и сборочная инфраструктура.
- `docs` — схема и архитектура.

## Команды

```bat
build_k210.bat
flash_k210.bat COM8
monitor_k210.bat COM8
```

Сборка использует Ninja и один каталог `build`. Повторная сборка без изменений занимает меньше секунды.

Для восстановления мёртвой ESP:

```bat
build_recovery_esp_v2.bat
```
