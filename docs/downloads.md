# Что скачать (VPN выключить)

Сервера Sipeed/Kendryte — китайские, через VPN часто тупят или режутся, поэтому качать с
**выключенным VPN**. Скачанное можно сложить на флешку, потом разложить по папкам репозитория.

Идём по пути **MaixPy (MicroPython)** — это самый быстрый способ поднять камеру, экран и Wi-Fi
без сборки C-тулчейна. C/SDK-вариант оставляем на потом (см. внизу, опционально).

## Обязательное (минимум для bring-up)

### 1. Драйвер USB-Serial (Windows)
Док прошивается/общается через USB-serial мост. Скачать оба, поставить нужный по факту
определения платы в Диспетчере устройств:
- **CH340/CH341** драйвер (WCH): https://www.wch-ic.com/downloads/CH341SER_EXE.html
- (если мост CH55x) **CH372/CH55x** — обычно тот же CH341SER подходит.
> Когда воткнёшь плату в USB — скажи, какой COM/чип появился в Диспетчере устройств, тогда
> зафиксируем точный драйвер.

### 2. MaixPy — прошивка (firmware .bin)
MicroPython для K210 с поддержкой камеры/LCD/KPU/Wi-Fi.
- Релизы: https://github.com/sipeed/MaixPy/releases
- Зеркало/последние bin: http://dl.sipeed.com/MAIX/MaixPy/release/master/
- Брать сборку **`maixpy_...minimum_with_ide_support.bin`** (с поддержкой IDE) или полную
  `maixpy_...full.bin`. Положить в `firmware/`.

### 3. kflash_gui — прошивальщик
GUI-утилита залить .bin в K210 по USB.
- https://github.com/sipeed/kflash_gui/releases (взять Windows-сборку, .zip/.exe).

### 4. MaixPy IDE (удобно, но не строго обязательно)
IDE с REPL и просмотром видеопотока с камеры — очень помогает на этапе bring-up.
- https://github.com/sipeed/MaixPy-IDE/releases (Windows installer).

## Желательное (для нейросетей/демо)

### 5. Готовые kmodel модели и примеры
Для проверки KPU (детекция лица/объектов):
- MaixHub model zoo: https://maixhub.com/model
- Примеры MaixPy (скрипты): в репозитории https://github.com/sipeed/MaixPy/tree/master/examples
  (можно скачать zip всего репо).
Скачанные `.kmodel` класть на microSD или в `firmware/models/`.

## Опционально (путь C/SDK, на будущее — НЕ сейчас)
Нужно только если откажемся от MaixPy и будем писать на C:
- Standalone SDK: https://github.com/kendryte/kendryte-standalone-sdk
- RISC-V тулчейн (kendryte-toolchain): https://github.com/kendryte/kendryte-gnu-toolchain/releases
- kflash (CLI): https://github.com/kendryte/kflash.py

## Куда раскладывать после скачивания
```text
firmware/maixpy_xxx.bin          — прошивка MaixPy
firmware/models/*.kmodel         — нейромодели (если качал)
（драйверы и IDE ставятся в систему, в репо не кладём）
```
`firmware/` и тяжёлые бинарники в git не коммитим (см. `.gitignore`).

## Короткий чек-лист
- [ ] CH341SER драйвер
- [ ] MaixPy `.bin` (с IDE support)
- [ ] kflash_gui (Windows)
- [ ] MaixPy IDE (опц.)
- [ ] пара `.kmodel` для теста KPU (опц.)
