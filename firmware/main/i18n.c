#include "i18n.h"

#include "nvs.h"

static lang_t s_lang = LANG_UA;

static const char *const STR[S__COUNT][2] = {
    [S_TAB_ONLINE]      = { "Онлайн", "Online" },
    [S_TAB_TEST]        = { "Тест", "Test" },
    [S_TAB_ARCHIVE]     = { "Архів", "Archive" },
    [S_TAB_BMS]         = { "BMS", "BMS" },
    [S_TAB_PRINTER]     = { "Принтер", "Printer" },
    [S_TAB_SETTINGS]    = { "Налаштування", "Settings" },
    [S_PRINT_REPORT]    = { "Друк звіту", "Print report" },

    [S_ST_DISCHARGE]    = { "Розряд", "Discharge" },
    [S_ST_DONE]         = { "Завершено", "Done" },
    [S_ST_REST]         = { "Пауза", "Rest" },
    [S_ST_IDLE]         = { "Очікування", "Idle" },

    [S_TESTER]          = { "Тестер BLT", "BLT tester" },
    [S_SIMULATED]       = { "симуляція", "simulated" },
    [S_LIVE]            = { "наживо", "live" },
    [S_USB_LINK]        = { "USB · дані не читаються", "USB · not reading" },
    [S_NO_TESTER]       = { "Немає тестера", "No tester" },
    [S_CAPACITY]        = { "Ємність", "Capacity" },
    [S_ENERGY]          = { "Енергія", "Energy" },
    [S_TIME]            = { "Час", "Time" },
    [S_TEMP]            = { "Темп.", "Temp." },

    [S_U_V]             = { "В", "V" },
    [S_U_A]             = { "А", "A" },
    [S_U_AH]            = { "А·год", "Ah" },
    [S_U_WH]            = { "Вт·год", "Wh" },
    [S_U_MV]            = { "мВ", "mV" },
    [S_U_MIN]           = { "хв", "min" },

    [S_BMS_NO_LINK]     = { "немає зв'язку", "no link" },
    [S_CELL_DELTA]      = { "Δ комірок", "Cell Δ" },
    [S_CELLS]           = { "Комірки", "Cells" },
    [S_BALANCING]       = { "баланс", "balancing" },

    [S_CH_DISCHARGE]    = { "Розряд", "Discharge" },
    [S_CH_IMBALANCE]    = { "Розбаланс", "Imbalance" },
    [S_CH_U_TESTER]     = { "U тестера", "Tester U" },
    [S_CH_U_BMS]        = { "U BMS", "BMS U" },
    [S_CH_I]            = { "I", "I" },
    [S_CH_DELTA]        = { "Δ", "Δ" },
    [S_CH_UP]           = { "вгору", "high" },
    [S_CH_DOWN]         = { "вниз", "low" },
    [S_CH_OTHERS]       = { "решта", "others" },
    [S_CH_DELTA_NOW]    = { "Δ %d мВ  ·  макс %d мВ", "Δ %d mV  ·  max %d mV" },
    [S_CH_DEV]          = { "мВ від середнього", "mV from average" },
    [S_CELL_TAP]        = { "тап — виділити на графіку", "tap to highlight" },

    [S_PR_PRINTING]     = { "друк…", "printing…" },
    [S_PR_OK]           = { "надруковано", "printed" },
    [S_PR_NOT_FOUND]    = { "принтер не знайдено", "printer not found" },
    [S_PR_NO_WIFI]      = { "немає Wi-Fi", "no Wi-Fi" },
    [S_PR_FAILED]       = { "помилка друку", "print failed" },

    [S_ARCHIVE_EMPTY]   = { "Тут з'являться збережені тести", "Saved tests will appear here" },

    [S_SET_WIFI]        = { "Wi-Fi", "Wi-Fi" },
    [S_SET_LANGUAGE]    = { "Мова інтерфейсу", "Interface language" },
    [S_SET_LANG_NOTE]   = { "інтерфейс перезапуститься", "the interface restarts" },
    [S_SET_TESTER]      = { "Тестер BLT (USB)", "BLT tester (USB)" },
    [S_SET_TESTER_POLL] = { "Читати дані тестера", "Read tester data" },
    [S_SET_TESTER_NOTE] = { "Лише читання (RI раз на 0,9 с, як BLTView). Команди керування тестеру не надсилаються.",
                            "Read-only (RI every 0.9 s, like BLTView). No control commands are sent to the tester." },
    [S_SCAN]            = { "Пошук", "Scan" },
    [S_CONNECT]         = { "Підключити", "Connect" },
    [S_PASSWORD]        = { "пароль", "password" },
    [S_NETWORK]         = { "Мережа", "Network" },
    [S_NETWORK_NONE]    = { "Мережа: не вибрано", "Network: none" },
    [S_WIFI_HINT]       = { "натисни «Пошук», щоб побачити мережі", "tap Scan to list networks" },
    [S_WIFI_SCANNING]   = { "пошук…", "scanning…" },
    [S_WIFI_FOUND]      = { "знайдено мереж: %d", "found %d networks" },
    [S_WIFI_PICK_FIRST] = { "спершу обери мережу", "pick a network first" },
    [S_WIFI_CONNECTING] = { "підключення до %s…", "connecting to %s…" },

    [S_PRN_SEARCH]      = { "Пошук", "Search" },
    [S_PRN_HINT]        = { "«Пошук» або введи IP праворуч", "Search, or enter the IP on the right" },
    [S_PRN_SEARCHING]   = { "шукаю принтери…", "searching for printers…" },
    [S_PRN_NONE]        = { "через mDNS не знайдено — введи IP", "none found via mDNS - enter the IP" },
    [S_PRN_FOUND]       = { "знайдено: %d — торкнись, щоб вибрати", "found %d - tap to select" },
    [S_PRN_SAVED]       = { "збережено: %s (%s:%u)", "saved: %s (%s:%u)" },
    [S_PRN_CURRENT]     = { "поточний: %s (%s)", "current: %s (%s)" },
    [S_PRN_MANUAL]      = { "IP принтера вручну:", "Printer IP manually:" },
    [S_PRN_USE_IP]      = { "Використати IP", "Use this IP" },
    [S_PRN_ENTER_IP]    = { "спершу введи IP", "enter an IP first" },
    [S_PRN_DIRECT]      = { "У мережі Wi-Fi Direct (DIRECT-…) принтером автоматично стає точка доступу.", "On a Wi-Fi Direct network (DIRECT-…) the access point is used as the printer." },

    [S_BMS_TITLE]       = { "BMS по Bluetooth", "BMS over Bluetooth" },
    [S_BMS_HINT]        = { "Натисніть «Пошук» і виберіть BMS (розпізнані JK і Daly — зверху)", "Tap Scan and pick the BMS (recognised JK and Daly listed first)" },
    [S_BMS_SCANNING]    = { "пошук 6 с…", "scanning for 6 s…" },
    [S_BMS_FOUND]       = { "знайдено: %d — торкнись своєї BMS", "found %d - tap your BMS" },
    [S_BMS_NOTHING]     = { "нічого не знайдено — BMS не спить?", "nothing found - is the BMS awake?" },
    [S_BMS_CONNECTING]  = { "підключення до %s…", "connecting to %s…" },
    [S_BMS_NOT_READY]   = { "Bluetooth ще не готовий", "Bluetooth not ready yet" },
    [S_BMS_FORGOT]      = { "від'єднано — можна вибрати іншу BMS", "disconnected — pick another BMS" },
    [S_DISCONNECT]      = { "Від'єднати", "Disconnect" },

    [S_BLE_OFF]         = { "Bluetooth вимкнено", "Bluetooth off" },
    [S_BLE_IDLE]        = { "не підключено", "not connected" },
    [S_BLE_SCANNING]    = { "пошук…", "scanning…" },
    [S_BLE_CONNECTING]  = { "підключення…", "connecting…" },
    [S_BLE_CONNECTED]   = { "підключено, чекаю даних", "connected, waiting for data" },
    [S_BLE_STREAMING]   = { "дані надходять", "receiving data" },
    [S_BLE_ERROR]       = { "збій зв'язку, повторюю", "link error, retrying" },
    [S_PREP_TITLE]      = { "Підготовка тесту на тестері BLT", "Prepare the BLT tester for a test" },
    [S_PREP_STATE]      = { "Стан тестера", "Tester state" },
    [S_PREP_NOW]        = { "Зараз у тестері", "On the tester now" },
    [S_PREP_SERIAL]     = { "Серійний номер батареї", "Battery serial number" },
    [S_PREP_NEXT_SN]    = { "Наступний номер", "Next number" },
    [S_PREP_AH]         = { "Запланована ємність, А·год", "Planned capacity, Ah" },
    [S_PREP_WRITE]      = { "Записати в тестер", "Write to the tester" },
    [S_PREP_WRITING]    = { "записую…", "writing…" },
    [S_PREP_NOTE]       = { "Записуються лише серійний номер і ємність (Rated Capacity), і лише коли тестер у стані START - тест ще не запущено. Після запису значення читаються назад і звіряються.",
                            "Only the serial number and the rated capacity are written, and only while the tester is in START (no test running). The values are read back and compared." },
    [S_PREP_NO_TESTER]  = { "тестер не відповідає", "tester not responding" },
    [S_PREP_BUSY]       = { "тест іде - запис лише в стані START", "a test is running - writing only in START" },
    [S_PREP_READ_FAIL]  = { "не вдалося прочитати блоки тестера", "could not read the tester blocks" },
    [S_PREP_BAD_SERIAL] = { "серійний номер: 1-16 цифр", "serial number: 1-16 digits" },
    [S_PREP_BAD_AH]     = { "ємність: 1-9999 А·год", "capacity: 1-9999 Ah" },
    [S_PREP_SAME]       = { "у тестері вже ці значення", "the tester already has these values" },
    [S_PREP_WRITE_FAIL] = { "помилка запису", "write failed" },
    [S_PREP_OK]         = { "записано і перевірено", "written and verified" },
    [S_HS_TITLE]      = { "Стан батареї", "Battery status" },
    [S_HS_IDLE]       = { "Тест не йде", "No test running" },
    [S_HS_OK]         = { "Норма", "OK" },
    [S_HS_WARN]       = { "Придивіться", "Check it" },
    [S_HS_STOP]       = { "Зупиніть тест", "Stop the test" },
    [S_HR_IDLE]       = { "вердикт - під час розряду", "judged during discharge" },
    [S_HR_OK]         = { "усе в межах", "all within limits" },
    [S_HR_NO_BMS]     = { "немає даних BMS", "no BMS data" },
    [S_HR_ALARM]      = { "аварія на тестері", "tester alarm" },
    [S_HR_CELL_LOW]   = { "комірка №%d: %.2f В", "cell #%d: %.2f V" },
    [S_HR_DELTA]      = { "розбаланс %.0f мВ, найнижча №%d", "imbalance %.0f mV, lowest #%d" },
    [S_HR_DELTA_GROW] = { "розбаланс росте: з %.0f до %.0f мВ, №%d", "imbalance growing: %.0f to %.0f mV, #%d" },
    [S_HR_CURRENT]    = { "струм %.1f А, задано %.0f А", "current %.1f A, set %.0f A" },
    [S_HR_VDROP]      = { "різниця U тестер/BMS %.1f В", "tester/BMS voltage gap %.1f V" },
    [S_HR_TEMP]       = { "батарея %.0f °C (+%.0f від старту)", "battery %.0f °C (+%.0f since start)" },
    [S_HR_MOS]        = { "MOS у BMS %.0f °C", "BMS MOS %.0f °C" },
    [S_HG_DELTA]      = { "Розбаланс", "Imbalance" },
    [S_HG_CELL]       = { "Мін. комірка", "Lowest cell" },
    [S_HG_CURR]       = { "Струм", "Current" },
    [S_HG_TEMP]       = { "Температура", "Temperature" },
    [S_SET_CHEM]      = { "Хімія батареї", "Battery chemistry" },
    [S_SET_CHEM_HINT] = { "Пороги стану батареї (розбаланс, мін. комірка)", "Battery-status limits (imbalance, lowest cell)" },
    [S_CHEM_AUTO]     = { "Авто", "Auto" },
};

void i18n_init(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "lang", &v) == ESP_OK && v <= LANG_EN) s_lang = (lang_t)v;
        nvs_close(h);
    }
}

lang_t i18n_lang(void) { return s_lang; }

void i18n_save(lang_t lang)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "lang", (uint8_t)lang);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *tr(str_id_t id)
{
    if ((unsigned)id >= S__COUNT) return "?";
    const char *s = STR[id][s_lang];
    return s ? s : STR[id][LANG_EN];
}
