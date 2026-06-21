# Что скачать (VPN выключить)

> **СТАТУС 2026-06-21:** почти всё уже скачано автоматически — см.
> [`../downloads/README.md`](../downloads/README.md). Прошивки в `firmware/`, kflash_gui и
> драйвер в `downloads/`. **Вручную осталась только MaixPy IDE** (на dl.sipeed.com большие
> файлы под капчей): браузером с https://dl.sipeed.com/MAIX/MaixPy/ide → `v0.2.5` →
> `maixpy-ide-windows-0.2.5.exe`. IDE не обязательна для прошивки.
>
> Точная версия прошивки для K210: **v0.6.3_2_gd8901fd22** (последняя в ветке master).

Сервера Sipeed/Kendryte — китайские, через VPN часто тупят или режутся, поэтому качать с
**выключенным VPN**. Скачанное можно сложить на флешку, потом разложить по папкам репозитория.

Идём по пути **MaixPy (MicroPython)** — это самый быстрый способ поднять камеру, экран и Wi-Fi
без сборки C-тулчейна. C/SDK-вариант оставляем на потом (см. внизу, опционально).

## ⚠️ Внимание: не то качать
Не бери **MaixPy v4.x** и образы **MaixCam** (`maixcam-...img.xz`) — это новая платформа, **не K210**.
Для нашего K210 нужна старая ветка **MaixPy-v1 (v0.6.3)**. Точные файлы ниже.

## Обязательное (минимум для bring-up) — точные имена

### 1. Драйвер USB-Serial (Windows) — `CH341SER.ZIP`
Док прошивается через USB-serial мост. Скачать **`CH341SER.ZIP`** (внутри `CH341SER.EXE` — запустить, Install):
- https://www.wch-ic.com/downloads/CH341SER_EXE.html
> Если после установки плата в Диспетчере устройств определится другим чипом (не CH340) —
> скажи мне какой COM/чип, подберём драйвер. Но начинать с CH341SER.

### 2. MaixPy K210 — прошивка (один `.bin`, не архив)
Папка: **https://dl.sipeed.com/MAIX/MaixPy/release/master/**
Зайти в папку версии **`v0.6.3`** (последняя для K210) и скачать файл, имя которого
заканчивается на **`_minimum_with_ide_support.bin`** — т.е. вида:
```
maixpy_v0.6.3_<хеш>_minimum_with_ide_support.bin
```
(в имени есть хеш коммита — бери именно вариант с суффиксом `_minimum_with_ide_support`).
Положить в `firmware/`. Релиз-страница для сверки версии: https://github.com/sipeed/MaixPy-v1/releases

### 3. kflash_gui — прошивальщик → `kflash_gui_v1.8.2_windows.7z`
GUI залить .bin в K210. **Это .7z** — нужен распаковщик (см. п.5):
- https://github.com/sipeed/kflash_gui/releases/download/v1.8.2/kflash_gui_v1.8.2_windows.7z

### 4. MaixPy IDE → `maixpy-ide-windows-0.2.4.0.exe`
IDE с REPL и видеопотоком с камеры — сильно помогает на bring-up. Это **.exe-инсталлятор**:
- Папка: https://dl.sipeed.com/MAIX/MaixPy/ide/  → версия `v0.2.4` → `maixpy-ide-windows-0.2.4.0.exe`

### 5. 7-Zip (чтобы открыть .7z из п.3) — `7z????-x64.exe`
Глобальный сайт, VPN не нужен: https://www.7-zip.org/ (если 7-Zip уже стоит — пропусти).

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

## Короткий чек-лист (точные файлы)
- [ ] `CH341SER.ZIP` — драйвer USB-serial
- [ ] `maixpy_v0.6.3_*_minimum_with_ide_support.bin` — прошивка K210 (из папки release/master/v0.6.3)
- [ ] `kflash_gui_v1.8.2_windows.7z` — прошивальщик
- [ ] `maixpy-ide-windows-0.2.4.0.exe` — IDE (опц., но удобно)
- [ ] `7z*-x64.exe` — если 7-Zip ещё не стоит (для распаковки kflash_gui)
- [ ] пара `.kmodel` для теста KPU (опц.)
