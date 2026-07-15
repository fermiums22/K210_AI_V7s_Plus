# K210 robot controller

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
