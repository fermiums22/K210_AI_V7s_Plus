# K210 AI — верхний "умный" уровень для V7s Plus

Это репозиторий **верхней надстройки** мобильного домашнего ассистента на базе старого
робота-пылесоса V7s Plus. Нижний уровень (питание, моторы, энкодеры, датчики, STM32) живёт
в отдельном проекте `../V7s_Plus`. Здесь — мозги: камера, экран, Wi-Fi, нейросеть на K210 и
связь с домашним сервером.

## Главная цель всей связки K210 + ESP8285

Нужно сделать нормальный рабочий цикл разработки и прошивки для связки:

- **K210_AI_V7s_Plus** — прошивка K210 / Maix Dock / M1W.
- **K210_ESP_SPI_WIFI** — прошивка ESP8285, который должен быть Wi-Fi/SPI мостом для K210.

Финальная цель:

1. Собирать прошивку ESP8285 из репы `K210_ESP_SPI_WIFI`.
2. Через скрипт с ПК заливать файлы прошивки на SD-карту K210 по UART.
3. Скрипт должен:
   - получить доступ к SD через UART;
   - считать актуальный `flash.json`;
   - изменить в нём только нужную секцию прошивки ESP;
   - записать `.bin` и обновлённый `flash.json` обратно на SD;
   - перезагрузить K210;
   - логировать все этапы.
4. K210 после перезагрузки должен:
   - прочитать `flash.json`;
   - увидеть, что требуется прошить ESP;
   - сразу изменить config так, чтобы не прошиваться бесконечно при следующем старте;
   - прошить ESP;
   - залогировать результат `OK` или `FAIL`.

Критически важно: даже если прошивка ESP неудачная, K210 не должен уходить в вечный цикл прошивки.

Текущий ближайший этап — довести этот flow до стабильного результата `ESP flash result: OK` через SD UART service.

## Рабочий baseline K210

Текущий `main` основан на рабочей точке GC0328 VGA + SD + KSD:

```text
f8c8ba15e3e5dce7638c3599a2cabd3c73b12db5
```

Ожидаемый маркер камеры и SD:

```text
KSD:CAPTURE_OK capture.rgb565 614400 640 480 RGB565
```

## KSD server команды

KSD UART: `COM12`, `921600` baud.

Команды сервера:

```text
HELP
SELFTEST
LCD_TEST
AMP_TEST
SD_TEST
CAM_TEST
MIC_TEST
CAM_CAPTURE [path]
GET <path>
PUT <path> <size>
FORMAT_SD
FLASH_ESP
RUN_SPI
RESET
DONE
```

Самотест:

```bat
call run_k210_selftest.bat COM12
```

Ожидаемый ответ:

```text
KSD:TEST_BEGIN
KSD:TEST KSD_SERVER PASS command-loop
KSD:TEST LCD PASS color-text
KSD:TEST AUDIO_AMP PASS toggle
KSD:TEST SD_RW PASS 64-bytes
KSD:TEST CAMERA_RAM PASS 640x480-614400-RGB565
KSD:TEST MICROPHONE SKIP no-mic-capture-driver
KSD:TEST ESP_UART_SPI SKIP use-RUN_SPI-after-ESP-fw
KSD:TEST_END PASS
```

Захват камеры в файл на SD и чтение обратно:

```bat
call run_camera_capture.bat COM12
```

Любую отдельную команду можно вызвать так:

```bat
py -3 tools\ksd_cmd.py --port COM12 --baud 921600 --connect-timeout 30 --cmd HELP
```

`tools\ksd_cmd.py` возвращает errorlevel 1 для `FAIL`-ответов сервера и 0 для `PASS/OK/SKIP`.
`MICROPHONE` пока честно `SKIP`, потому что отдельного mic capture-драйвера в проекте нет.
`ESP_UART_SPI` в общем K210-самотесте тоже `SKIP`; его проверяем отдельно после прошивки ESP через `RUN_SPI`.

Работаем через `main`; экспериментальные ветки не нужны для текущего K210 self-test этапа.

## Где мы в общей архитектуре

```text
Home Assistant / сервер / OpenAI / TTS / STT      (Proxmox у Виктора)
        |
        | Wi-Fi
        v
ESP8285  <---- SPI/UART ---->  K210 + GC0328 + LCD + audio AMP + SD
        |
        v
нижний STM32 / моторы / питание / датчики
```
