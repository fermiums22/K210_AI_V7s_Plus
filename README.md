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

## Где мы в общей архитектуре

```text
Home Assistant / сервер / OpenAI / TTS / STT      (Proxmox у Виктора)
        ^
        | Wi-Fi / UART
        v
ЭТОТ РЕПО:  K210 Maix Dock — камера, экран, микрофон, динамик, Wi-Fi, нейросеть   <── верхний уровень
        ^
        | команды движения / телеметрия по UART
        v
V7s_Plus (STM32F071): питание, моторы, энкодеры, IR-бамперы, док-зарядка          <── нижний уровень
```

Цель не "пылесос". Цель — физическая сущность, которая ездит по дому, видит/слышит,
говорит и является интерфейсом к Home Assistant. K210 отвечает за восприятие и сеть,
STM32 — за движение.

Полная постановка задачи и состояние нижнего уровня: см. `../V7s_Plus/README.md`.

## Текущий режим разработки с ИИ / оператором

Работаем короткими итерациями:

1. ИИ правит код/скрипты прямо в GitHub и коммитит в `main`.
2. Виктор у себя делает `git pull`, запускает одну-две команды и сразу смотрит реальный вывод железа.
3. Если видно, что пошло не туда, Виктор сразу дропает попытку и присылает console output / лог.
4. ИИ не пытается "сам всё выполнить в фоне" и не держит длинные терминальные сессии. Это быстрее, потому что оператор видит железо, экран, COM-порты, reset/boot поведение и может остановить ошибочный путь раньше.
5. Скрипты должны быть разделены по смыслу: отдельно сборка, отдельно прошивка, отдельно UART upload/monitor. Не надо ради просмотра терминала пересобирать весь проект.

Правило для новых правок: build не должен менять tracked-файлы. Generated outputs должны лежать только в ignored директориях (`build/`, `out/`, `logs/`, `.pio/` и т.п.).

## Текущий K210 C-flow

Сейчас основной рабочий путь — C-прошивка K210, которая:

1. монтирует SD;
2. открывает короткое окно **UART SD service** по debug UART (`KSD1` protocol);
3. принимает от PC файлы ESP payload и `flash.json`;
4. по команде `RESET` перезагружается;
5. после reboot читает `flash.json`, disarm-ит one-shot job и прошивает ESP8285.

### Сборка K210

```bat
cd /d D:\w_space\K210_AI_V7s_Plus
git pull
build_k210.bat
```

Ожидаемый финал:

```text
OK: D:\w_space\K210_AI_V7s_Plus\build\robot_show.bin
```

`build_k210.bat` сам создаёт локальный `src\wifi_cfg.h`, если его нет. Этот файл gitignored, потому что там могут быть Wi-Fi креды.

### Прошивка K210

```bat
flash_k210.bat COM12
```

`COM12` заменить на реальный K210/CH340 COM-порт. Скрипт показывает доступные COM-порты перед запуском `kflash`.

На текущей плате работает DTR/RTS auto-reset/boot через `kflash -B dan`, поэтому руками BOOT/RESET обычно **не зажимать**. Ручной BOOT+RESET — только как fallback, если auto-reset реально упал на sync/timeout.

Успешная прошивка выглядит примерно так:

```text
Greeting Message Detected, Start Downloading ISP
Boot to Flashmode Successfully
Programming BIN: ... 100.0%
Rebooting...
OK: K210 flashed.
```

### Что значит `no host` на экране

`no host` / `UART upload: no host` — не ошибка. Это значит, что K210 после старта 5 секунд ждал PC-скрипт по UART SD service, не увидел `KSD1\n` и пошёл дальше в обычное приложение.

Для ESP flashing через K210 нужно запускать upload-скрипт с PC и затем нажать **RESET** на K210, чтобы PC успел попасть в это boot-window.

### Подготовка microSD для K210

Для текущего C/FatFs flow использовать **FAT32**, не exFAT. Практический безопасный вариант для больших карт — создать маленький раздел 4 GB FAT32 с меткой `K210SD`.

Проверка карты на ПК, только чтение:

```bat
cd /d D:\w_space\K210_AI_V7s_Plus
py -3 tools\check_sd_pc.py --drive E:
```

Ожидаемо для рабочей карты:

```text
fs    : FAT32
label : K210SD
```

Если Windows GUI не предлагает FAT32 для 64 GB microSD, использовать `diskpart` от администратора и выбирать диск вручную по размеру/USB-ридеру. Не запускать `FORMAT_SD` на K210, пока нет уверенности, что raw SD/mount стабилен.

### Проверка камеры с записью на SD

Команда KSD:

```text
CAM_CAPTURE cam/capture.rgb565
```

Ожидаемый ответ:

```text
KSD:CAPTURE_OK cam/capture.rgb565 <bytes> <w> <h> RGB565
```

PC-команда, которая отправляет `CAM_CAPTURE`, затем читает файл обратно с SD через KSD `GET` и сохраняет локальную копию в `logs\capture.rgb565`:

```bat
cd /d D:\w_space\K210_AI_V7s_Plus
py -3 tools\ksd_cmd.py --port COM12 --baud 921600 --cmd "CAM_CAPTURE cam/capture.rgb565" --get cam/capture.rgb565 --out logs\capture.rgb565
```

## Железо (этот уровень)

Идентифицировано по фото, см. [docs/hardware.md](docs/hardware.md):

- **Sipeed Maix Dock + модуль M1W** — K210 (dual-core 64-bit RISC-V @400 МГц + KPU
  нейроускоритель). Вариант **M1W** = со встроенным Wi-Fi (ESP8285) и разъёмом U.FL под антенну.
- **2.4" TFT дисплей** (шлейф `Sipeed024-H2441`, 240×320, контроллер типа ILI9341).
- **DVP-камера** на шлейфе `ZV-T01-GA4.2` (GC0328 / OV2640-класс).
- **microSD 64 GB** для моделей/логов.
- **Wi-Fi антенна** U.FL.
- Питание/прошивка по **micro-USB**.

## Что сейчас сделано

Плата поднята (bring-up в процессе):

- **C-прошивка K210:** собирается через `build_k210.bat`, прошивается через `flash_k210.bat COMx`.
- **UART SD service:** K210 на старте ждёт PC host и умеет `GET flash.json`, `PUT <file> <size>`, `RESET`, `CAM_CAPTURE`.
- **ESP flashing from SD:** K210-side flasher читает `flash.json`, disarm-ит one-shot job перед прошивкой и не уходит в вечный flash loop.
- **Дисплей:** работает через SDK LCD transport; диагностический экран показывает этапы boot/upload/flash.
- **microSD:** монтируется, используется для payload, `flash.json` и camera capture.
- **Старый MaixPy bring-up:** камера+LCD+IDE+WiFi+аудио были проверены ранее, но текущий рабочий путь для ESP flashing — C-прошивка.

## LCD-драйвер: откуда взят (НЕ переписывать вручную!)

> Заметка для будущих правок / ИИ: дисплей **уже работает** на официальном драйвере из
> Kendryte SDK. Не возвращайся к самописному bit-bang — он был тупиковой веткой.

- **Источник:** репозиторий `kendryte/kendryte-freertos-demo`, папка **`lcd/`** (это и есть
  сэмпл «lcd (RTOS)» из списка K210 SDK Samples в VisualGDB). Притащены файлы:
  - [src/lcd.c](src/lcd.c), [src/lcd.h](src/lcd.h) — примитивы рисования, **verbatim** из SDK
  - [src/jlt32009a.c](src/jlt32009a.c), [src/jlt32009a.h](src/jlt32009a.h) — транспорт
  - [src/font.h](src/font.h) — шрифт 8×16, **verbatim**
- **Транспорт — ключевой K210-трюк:** LCD висит на **SPI0 в OCTAL-режиме** (`SPI_FF_OCTAL` +
  `SPI_AITM_AS_FRAME_FORMAT`) — 8 линий D0–D7 работают как параллельная 8080-шина, SCLK = строб
  WR, CS = SPI0_SS3, а D/CX — отдельная нога на GPIOHS. Линии D0–D7 **внутри кристалла**
  захардкожены на DVP-пады; маршрутизирует их `sysctl_set_spi0_dvp_data(1)`, а **не**
  `fpioa_set_function` (см. комментарии в [src/pinout.h](src/pinout.h)).
- **Доступ через device-manager:** `io_open("/dev/spi0")` / `io_open("/dev/gpio0")` +
  `spi_get_device` / `spi_dev_*` / `spi_dev_fill`. Реализация драйверов — в `lib/bsp/device/`
  и `lib/freertos/`, уже в дереве. SDK скачивать не нужно — он вендорнут целиком.
- **Что адаптировано локально** (в апстрим-сэмпле этого нет, т.к. он полагается на
  board-config `g_fpioa_cfg`/`bsp_pin_setup()`, который тут не подключён): в `tft_hard_init()`
  добавлены FPIOA-разводка, `sysctl_set_spi0_dvp_data(1)`, аппаратный reset по RST и включение
  подсветки. Распиновка вынесена в [src/project_cfg.h](src/project_cfg.h).
- **API для прикладного кода:** `lcd_init()`, `lcd_clear(color)`,
  `lcd_draw_picture(x,y,w,h,ptr)` (RGB565, по 2 пикселя в `uint32`), `lcd_draw_rectangle`,
  `lcd_draw_string`. Старого API (`lcd_fill`, `lcd_set_window`, `rgb()`) **больше нет**.
- **Оговорка по контроллеру:** init-таблица в `lcd_init()` минимальна (SW reset → sleep off →
  COLMOD 0x55 → MADCTL → display on) и универсальна для ILI9341/ST7789/NT35310. Если поедут
  цвета (BGR/RGB) или ориентация — правится **только** байтом MADCTL, транспорт трогать не надо.

## Аудио и Wi-Fi (C-прошивка)

- **Аудио** — [src/audio.c](src/audio.c): I2S0 TX → PT8211 DAC → PAM8403. Device-manager
  (`/dev/i2s0`, `i2s_config_as_render` + `io_write`). DAC висит на пад-линии **OUT_D1**
  (IO34, не D0!), поэтому маска каналов `0x0C`. Усилитель глушится в простое ([src/amp.c](src/amp.c),
  MUTE/SHDN на IO9/IO10). Выравнивание I2S — `I2S_AM_RIGHT` (PT8211 right-justified).
- **Wi-Fi** — [src/wifi.c](src/wifi.c): ESP8285 по AT поверх UART2 (`/dev/uart2`),
  `EN`-power-cycle на IO8 → `AT` → `CWMODE=1` → `CWJAP` → `CIFSR`, IP выводится на LCD.
  - ⚠️ **Распиновка UART зеркальна силкскрину:** IO6 = `UART2_RX` (K210 RX), IO7 = `UART2_TX`
    (K210 TX). Метки «TX/RX» на плате — со стороны ESP. См. [src/pinout.h](src/pinout.h).
  - ⚠️ **Имя устройства:** hw UART2 = `/dev/uart2` (драйверы `uart0/1/2` — это UART1/2/3).
- **Креды Wi-Fi:** в [src/wifi_cfg.h](src/wifi_cfg.h) — **gitignored**. `build_k210.bat`
  создаёт пустой локальный файл автоматически; если AT Wi-Fi mode нужен, вписать SSID/пароль руками.

## Ближайшие цели

1. Довести ESP8285 flashing через K210 SD UART до стабильного `ESP flash result: OK`.
2. Проверить, что `flash.json` disarm-ится до прошивки и не создаёт вечный flash loop.
3. Проверить ESP boot после прошивки: UART log, SPI slave ready, Wi-Fi/TCP server.
4. Потом возвращаться к Wi-Fi/SPI receiver и Home Assistant integration.

## Что скачать из интернета

См. **[docs/downloads.md](docs/downloads.md)** — там список ссылок и файлов, которые надо
скачать с выключенным VPN (сервера Sipeed/китайские). Флешку использовать как промежуточное
хранилище.

## Структура репозитория

```text
README.md            — этот файл, цели и контекст
docs/
  hardware.md        — описание железа и фотографий
  downloads.md       — чек-лист: что скачать (firmware, драйверы, тулчейн)
  photos/            — фото платы и стенда (см. photos/README.md)
firmware/            — сюда кладём скачанные .bin прошивки MaixPy/kmodel (в git не коммитим)
src/                 — K210 C-код
tools/               — PC-утилиты для UART/SD диагностики
build_k210.bat       — сборка K210 firmware
flash_k210.bat       — прошивка K210 через kflash + DTR/RTS auto-reset
build/               — generated build output, ignored
```
