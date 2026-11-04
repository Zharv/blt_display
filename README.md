# blt-esp-display

Автономний тач-дисплей для тестерів-розрядників акумуляторів **Alfa Progetti BLT160 / SmartIC**
на платі Guition **JC8012P4A1** (ESP32-P4, 10.1" 1280×800). Повторює корисне з Windows-софту
**BLTView**, але під тач-панель, і додатково:

- підключається до **BMS по Bluetooth LE** (JK / JBD / Daly) — на одному екрані показники тестера
  і BMS разом: графік розряду, температури, **розбаланс** комірок;
- **друкує звіт і розрядний графік на Wi-Fi-принтер** (HP Smart Tank 581, по IPP / AirPrint).

Одне тестування = одна батарея (один канал тестера + одне BLE-з'єднання). ПК і Access-база не потрібні.

```
 Тестер ──USB-CDC──► ESP32-P4 ◄──BLE── BMS
                      ├─► тач-UI (графік розряду, комірки, темп.)
                      └─► Wi-Fi ─IPP─► принтер
```

Повний опис — [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Протокол тестера —
[docs/BLT_PROTOCOL.md](docs/BLT_PROTOCOL.md) (відреверсений з BltView.exe 2.6.16).

## Структура

| Шлях | Що |
|---|---|
| `firmware/` | прошивка, ESP-IDF 5.5 + LVGL 9.x |
| `firmware/components/blt_proto` | протокол тестера: збірка кадрів, валідація/розбір відповіді (чистий C) |
| `firmware/components/bms` | нормалізований `bms_data_t` + драйвери `bms_jbd` (є), `bms_jk`/`bms_daly` (TODO) |
| `firmware/main` | USB-CDC, BLE-менеджер, модель сесії, сховище, UI, друк (TODO) |
| `tools/blt_client.py` | той самий протокол на Python (клієнт + оракул для тестів) |
| `tools/blt_sim.py` | симулятор тестера: синтетичний розряд поверх реального фреймінгу |
| `tests/` | host-тести парсерів (MSVC `cl.exe` або будь-який C-компілятор) |
| `docs/` | архітектура + протокол |

## Host-тести (без заліза)

```bash
powershell -ExecutionPolicy Bypass -File tests\run_host_tests.ps1
```

Статус: `blt_proto` 40/0 (звірено з Python-оракулом і двома контрольними кадрами),
`bms_jbd` 19/0 (запит звірено з публічним кадром; поля відповіді — за специфікацією,
чекають звірки з реальним дампом).

Демо потоку даних через симулятор тестера:

```bash
python tools\blt_sim.py
```

## Збірка прошивки

ESP-IDF v5.5 (на цьому ПК у `D:\Espressif\esp-idf-v5.5`):

```
. .\idf-env.ps1
cd firmware
idf.py set-target esp32p4
idf.py build
idf.py -p COM<n> flash monitor
```

> Каркас прошивки (`firmware/main`, BSP, USB/BLE/UI) ще в роботі — зараз готові й протестовані
> залізо-незалежні компоненти протоколу та симулятори.

## Застереження до заліза

- прошивка-slave на ESP32-C6 має бути **з BT** (ESP-Hosted ≥ 2.4.2);
- USB-host порт має бути OTG і давати/терпіти VBUS (звірити по схемі плати);
- формат друку принтера (`image/urf` чи PDF) звірити через його EWS.
