# LG Magic Remote (MR20) — Драйвер для Linux и нативные утилиты на C

**Язык:** [English🇬🇧](README.md) **Русский🇷🇺**

![LG Magic Remote](images/lg_magic_remote.png)

## Обзор

Этот проект — **нативный C-преемник** оригинального проекта LG Magic
Remote. Драйвер ядра Linux для пульта MR20 (Bluetooth HID-устройство
`000f:3412`) сохранён как основа, а оригинальный инструментарий на Python
(`numpy`, `scipy`, `pyqtgraph`, `python-evdev`, `ahrs`, …) **полностью
заменён одним бинарником на C: `lg-magic`**.

Что это значит на практике:

- **Ноль зависимостей во время выполнения** — один бинарник, собранный
  только с libc/libm. Ни Python, ни pip, ни GUI-библиотек.
- **Один инструмент — шесть задач** — чтение IMU, анализатор HID,
  калибровка, конвертация blob-файла, конфигурация и интерактивный мастер
  настройки.
- **Побайтовый паритет** с оригинальными скриптами (проверяется по
  эталонным данным, сгенерированным Python-реализациями — см. TESTING.md).
- **Полноценная упаковка** — .deb-пакет `lg-magic-dkms` с DKMS,
  udev-правилами и CI/CD на GitHub Actions (сборка, тесты, релизы).

Оригинальные Python-скрипты остаются в `scripts/` только для справки.

## Возможности

### Модуль ядра (`lg_magic.ko`)

- Полное отображение кнопок (питание, цифры, навигация, медиа, цветные
  кнопки) и колесо прокрутки
- Гироскопический airmouse с калибровкой bias/scale и фильтром нижних
  частот — полностью в пространстве ядра
- Отдельное evdev-устройство `LG Magic Remote IMU` с сырыми 6-осевыми
  данными (акселерометр + гироскоп) и аппаратным счётчиком
- Калибровка загружается из `/lib/firmware` (по MAC-адресу пульта, с
  общим fallback-файлом)

### Бинарник `lg-magic`

| Подкоманда | Назначение |
|---|---|
| `lg-magic analyze` | Разбор HIDRAW-пакетов пульта (заменяет `lg_magic.py`) |
| `lg-magic imu` | Чтение IMU через evdev: сырые данные, запись `--csv`, углы `--ahrs`, куб `--cube` в терминале, airmouse `--mouse` через uinput |
| `lg-magic calibrate` | Калибровка акселерометра (Левенберг–Марквардт) / гироскопа из CSV-записи |
| `lg-magic calib2bin` | Конвертация калибровочного JSON в 32-байтовый blob для ядра |
| `lg-magic config` | Просмотр / изменение конфигурации (JSON-файлы, см. ниже) |
| `lg-magic setup` | Интерактивный мастер: настройка, калибровка, установка — под ключ |

## Требования

- **Linux** с работающим ядром
- **DKMS** и **заголовки ядра** — для сборки модуля (устанавливаются
  автоматически из .deb)
- **gcc + make** — только при сборке из исходников

Самим утилитам во время выполнения нужны только libc/libm.

## Установка

### 1. Пакет из релиза (рекомендуется)

Скачайте `lg-magic-dkms_1.0-1_amd64.deb` из последнего
[GitHub Release](https://github.com/sirfragles/lg-magic/releases):

```bash
sudo apt install ./lg-magic-dkms_1.0-1_amd64.deb
```

Пакет устанавливает `/usr/bin/lg-magic`, udev-правило и регистрирует модуль
в DKMS — тот собирает `lg_magic.ko` под ваше ядро и пересобирает его при
обновлениях ядра.

### 2. Сборка из исходников

```bash
make              # модуль ядра + бинарник lg-magic
make check        # сборка утилит и полный прогон тестов
sudo make install # /usr/bin/lg-magic + /etc/udev/rules.d/51-lgimu.rules
sudo modprobe lg_magic
```

### 3. Ручная установка через DKMS

```bash
sudo mkdir -p /usr/src/lg-magic-1.0
sudo cp Makefile dkms.conf COPYING /usr/src/lg-magic-1.0/
sudo cp -r kernel include /usr/src/lg-magic-1.0/
sudo dkms add -m lg-magic -v 1.0
sudo dkms build -m lg-magic -v 1.0
sudo dkms install -m lg-magic -v 1.0
# DKMS собирает только модуль — утилиты ставятся отдельно:
make tools && sudo make install
```

## Быстрый старт

После установки запустите мастер — он проведёт через параметры модуля,
калибровку акселерометра и гироскопа, установку blob-файла прошивки и тест
airmouse:

```bash
sudo lg-magic setup
```

Шаги мастера:

1. **Проверка окружения** — root, загруженный модуль, обнаруженные
   устройства (включая `/dev/uinput`)
2. **Параметры модуля** — запись `/etc/modprobe.d/lg-magic.conf`
   (`imu_evdev=1 airmouse=1`) и перезагрузка модуля
3. **Калибровка акселерометра** — «медленно вращайте пульт во всех осях»
   (запись 20 с, подгонка Левенберга–Марквардта, проверка качества)
4. **Калибровка гироскопа** — «положите пульт и не трогайте»
   (запись 10 с, средний bias)
5. **Калибровочный JSON** — записывается в `/etc/lg-magic/calib.json`
6. **Blob прошивки** — `lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` для MAC-адреса
   вашего пульта (+ fallback `lg_magic_calib.bin`) в `/lib/firmware/`
7. **Перезагрузка модуля** — с проверкой в `dmesg` («Loading LG Magic
   calibration»)
8. **Тест airmouse** — «двигайте пультом, Ctrl+C завершает»
9. **Пользовательская конфигурация** — `~/.config/lg-magic/config.json`
   с путём калибровки и настройками airmouse
10. **Итоги** — что сделано и как повторить или откатить

`--non-interactive` принимает значения по умолчанию (для скриптов).

## Использование

Подробности: `lg-magic --help` или `lg-magic <подкоманда> --help`.

```bash
# Анализатор HIDRAW-пакетов (авто-поиск пульта по VID/PID 000f:3412)
lg-magic analyze                        # или --device /dev/hidrawN / --list

# Сырые данные IMU (авто-поиск evdev-устройства «IMU»)
lg-magic imu

# Запись сырых сэмплов для калибровки
lg-magic imu --csv samples.csv --duration 20

# Углы ориентации (Madgwick AHRS) / куб в терминале (подразумевает --ahrs)
lg-magic imu --calib calib.json --ahrs
lg-magic imu --calib calib.json --cube

# Airmouse через uinput (нужно udev-правило + группа input, либо root)
lg-magic imu --calib calib.json --mouse

# Калибровка из записи
lg-magic calibrate samples.csv calib_accel.json --accel
lg-magic calibrate samples.csv calib_gyro.json --gyro

# 32-байтовый blob прошивки
lg-magic calib2bin calib.json lg_magic_calib.bin --alpha 0.2 --mouse_k 0.5
sudo cp lg_magic_calib.bin /lib/firmware/

# Конфигурация
lg-magic config                    # эффективная конфигурация
lg-magic config set mouse_k 0.5    # сохранить в ~/.config/lg-magic/config.json
lg-magic config path               # расположение файлов конфигурации
```

### Конфигурация

Приоритет: встроенные значения по умолчанию < `/etc/lg-magic/config.json` <
`~/.config/lg-magic/config.json` < `--config FILE` < флаги CLI.

| Ключ | Тип | По умолч. | Значение |
|---|---|---|---|
| `imu_device` | строка | автопоиск | путь evdev к устройству IMU |
| `hidraw_device` | строка | автопоиск | путь hidraw к пульту |
| `default_calib` | строка | — | калибровочный JSON для `--calib` |
| `lpf_alpha` | число | 0.2 | фильтр нижних частот для `--mouse` |
| `mouse_scale` | число | 30.0 | скорость курсора для `--mouse` |
| `madgwick_beta` | число | 0.1 | усиление фильтра Маджвика |
| `alpha` | число | 0.2 | LPF airmouse (записывается в blob) |
| `mouse_k` | число | 0.5 | чувствительность airmouse (записывается в blob) |
| `gyro_scale_default` | число | 0.07 | рекомендуемый масштаб гироскопа (см. калибровку) |

### Ручная калибровка (без мастера)

1. **Запишите** сырые сэмплы: `lg-magic imu --csv samples.csv`
2. **Акселерометр** (во время записи медленно вращайте пульт во всех
   осях): `lg-magic calibrate samples.csv calib_accel.json --accel`
3. **Гироскоп** (пульт лежит неподвижно):
   `lg-magic calibrate samples.csv calib_gyro.json --gyro`
4. **Объедините** секции `accel` и `gyro` в один JSON; задайте
   `gyro.scale` (разумное значение — около `0.07`, см. `gyro_scale_default`)
5. **Сконвертируйте и установите**: `lg-magic calib2bin calib.json …` +
   копия в `/lib/firmware/` (см. выше), затем
   `sudo modprobe -r lg_magic && sudo modprobe lg_magic`

## Параметры модуля

| Параметр | Значения | Описание |
|---|---|---|
| `airmouse` | 0/1 | Включить airmouse |
| `airmouse_threshold` | целое | Порог гироскопа для активации управления курсором (по умолч. 300) |
| `imu_evdev` | 0/1 | Показывать сырые данные IMU отдельным evdev-устройством |
| `debug` | 0–2 | Подробность логов (0 = тихо … 2 = подробно) |

```bash
# При загрузке
sudo modprobe lg_magic airmouse=1 airmouse_threshold=300 imu_evdev=1 debug=1
# Или постоянно в /etc/modprobe.d/lg-magic.conf (мастер пишет этот файл)
# Или на лету через sysfs
echo 0 > /sys/module/lg_magic/parameters/debug
```

## Расположение файлов

| Путь | Содержимое |
|---|---|
| `/usr/bin/lg-magic` | бинарник утилит |
| `/lib/modules/$(uname -r)/kernel/drivers/input/misc/lg_magic.ko` | модуль (через DKMS) |
| `/usr/src/lg-magic-1.0/` | дерево исходников DKMS |
| `/etc/udev/rules.d/51-lgimu.rules` | udev-правила (IMU evdev, hidraw, uinput) |
| `/etc/modprobe.d/lg-magic.conf` | параметры модуля (пишет мастер) |
| `/etc/lg-magic/calib.json` | калибровочный JSON (по умолчанию мастера) |
| `/lib/firmware/lg_magic_calib.bin` | blob калибровки, общий fallback |
| `/lib/firmware/lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` | blob калибровки для конкретного пульта (BT MAC) |
| `/etc/lg-magic/config.json`, `~/.config/lg-magic/config.json` | файлы конфигурации |

## Разработка

```bash
make              # модуль + утилиты
make check        # юнит-тесты, тесты паритета, smoke-тесты CLI (см. TESTING.md)
make clean
```

Набор тестов (195 проверок) покрывает переносимое ядро — JSON, матричную и
кватернионную математику, round-trip CSV, фильтр Маджвика по эталонной
трассе, подгонку Левенберга–Марквардта (включая кросс-проверку
аналитического якобиана численным), побайтовое совпадение blob-файла с
`struct.pack` из Python и конфигурацию. Linux-интеграция (uinput
end-to-end) и проверки упаковки выполняются в CI. Подробности:
[TESTING.md](TESTING.md).

CI запускается на каждый push и pull request (Ubuntu + macOS): сборка,
тесты, сборка `.deb`, установка через DKMS, выгрузка артефакта. Тег релиза
(`v*`) собирает `.deb` и прикрепляет его к GitHub Release.

### Связь с оригинальными Python-скриптами

C-утилиты точно повторяют поведение оригинальных скриптов — включая формат
CSV, матрицу выравнивания, константы фильтров и layout blob-файла — и
проверяются по эталонным данным, сгенерированным Python-реализациями.
Несколько осознанных, задокументированных улучшений:

- `lg-magic analyze` ищет пульт автоматически по VID/PID вместо
  зашитого `/dev/hidraw7`
- `lg-magic imu --cube` подразумевает `--ahrs` (в Python один `--cube`
  показывал статичный куб)
- калибровка только `--gyro` записывает единичную коррекцию
  акселерометра вместо пустых массивов (пустые массивы ломали `--ahrs`)
- `--duration` и `--print-calib` — расширения

Скрипты остаются в `scripts/` только для справки и генерации эталонных
данных; они больше не входят в поддерживаемый рабочий процесс.

## Структура проекта

```
├── kernel/            # модуль ядра (lg_magic.ko)
├── include/           # lg_magic_calib.h — структура калибровки, общая
│                      #   дословно для ядра и userspace
├── tools/
│   ├── src/           # бинарник lg-magic (multi-call, только libc/libm)
│   ├── include/       # внутренние заголовки
│   └── tests/         # юнит- / паритет- / smoke-тесты
├── testdata/          # эталонные данные (сгенерированы Python-скриптами)
├── debian/            # упаковка (lg-magic-dkms, DKMS через dh_dkms)
├── scripts/           # оригинальные Python-утилиты (устаревшие, справка)
├── dkms.conf          # конфигурация DKMS (только модуль)
├── Makefile           # верхнеуровневая сборка
└── .github/workflows/ # CI + автоматизация релизов
```

## Совместимость

- **Протестировано с**: LG Magic Remote MR20
- **Версии ядра**: 4.15+ (протестировано на 6.11)
- **Архитектуры**: .deb и DKMS собираются под текущее ядро
  (x86_64/arm64, little-endian)

## Лицензия

GPL-2.0-or-later — как у ядра Linux. Порт алгоритма Маджвика (AHRS) в
`tools/src/madgwick.c` основан на public-domain реализации С. Маджвика
(x-io.co.uk).

Copyright © 2025 [Ilya Chelyadin]. Проект не связан с LG Electronics.
