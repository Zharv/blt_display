#include "blt_rec.h"

#include <string.h>

static uint8_t bcd(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint16_t u16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

static void ascii(char *out, const uint8_t *b, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == 0) break;
        out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

bool blt_rec_decode(const uint8_t *b, size_t len, blt_rec_t *o)
{
    if (!b || !o || len < BLT_REC_LEN) return false;
    memset(o, 0, sizeof *o);
    o->mode = b[0x1E];
    o->rated_ah = u16(b + 0x1F);
    o->rated_v = b[0x22];
    o->discharge_a = b[0x23];
    o->thr_cv = b[0x24];
    o->stop_cv = b[0x25];
    o->snap_mi = bcd(b[0x27]); o->snap_h = bcd(b[0x28]);
    o->snap_d = bcd(b[0x29]); o->snap_mo = bcd(b[0x2A]); o->snap_y = bcd(b[0x2B]);
    o->dis_mi = bcd(b[0x2C]); o->dis_h = bcd(b[0x2D]);
    o->dis_d = bcd(b[0x2E]); o->dis_mo = bcd(b[0x2F]); o->dis_y = bcd(b[0x30]);
    o->t_dis_min = u16(b + 0x3A);
    o->t_dis_pause_min = u16(b + 0x3E);
    o->cycle = b[0x41];
    o->ah_start = u16(b + 0x42);
    o->ah_end = u16(b + 0x44);
    o->ah_dis = u16(b + 0x4E);
    o->efficiency = b[0x52];
    o->v_min_raw = u16(b + 0x53);
    o->t_card_max = (int8_t)b[0x5B];
    memcpy(o->mem_alarms, b + 0x65, 6);
    o->state = b[0x6B];
    o->n_active = b[0x6D];
    o->pr_cycle = u16(b + 0x6E);
    o->pr_test = u16(b + 0x70);
    o->pr_page = (uint32_t)b[0x72] | ((uint32_t)b[0x73] << 8) | ((uint32_t)b[0x74] << 16) | ((uint32_t)b[0x75] << 24);
    ascii(o->battery_id, b + 0x76, 16);
    ascii(o->note, b + 0x86, 32);
    o->t_limit_min = u16(b + 0xA7);
    o->graph_min = b[0xB1];
    return true;
}

float blt_rec_stop_v(const blt_rec_t *r)
{
    return r->rated_v * r->stop_cv / 200.0f;
}

int blt_rec_alarm_count(const blt_rec_t *r)
{
    int n = 0;
    for (int i = 0; i < 6; i++)
        for (int b = 0; b < 8; b++) n += (r->mem_alarms[i] >> b) & 1;
    return n;
}

bool blt_page_decode(const uint8_t *b, size_t len, blt_page_t *o)
{
    if (!b || !o || len < BLT_PAGE_LEN) return false;
    memset(o, 0, sizeof *o);
    o->mi = bcd(b[1]); o->h = bcd(b[2]);
    o->d = bcd(b[4]); o->mo = bcd(b[5]); o->y = bcd(b[6]);
    o->interval_min = b[0x7F];
    for (int k = 0; k < BLT_PAGE_SAMPLES; k++) {
        const uint8_t *s = b + 7 + 6 * k;
        uint16_t v = u16(s + 2);
        if (v == 0xFFFF) break;
        o->tag[k] = u16(s);
        o->v[k] = v / 10.0f;
        o->i[k] = (int16_t)u16(s + 4) / 10.0f;
        o->n++;
    }
    return true;
}

int32_t blt_minutes(uint8_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi)
{
    static const uint16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (mo < 1 || mo > 12) mo = 1;
    int32_t days = (int32_t)y * 365 + (y + 3) / 4 + cum[mo - 1] + (d ? d - 1 : 0);
    if (mo > 2 && (y % 4) == 0) days++;
    return days * 1440 + h * 60 + mi;
}

const char *blt_alarm_name_ua(int id)
{
    switch (id) {
    case 1:  return "несправна EEPROM";
    case 2:  return "несправний годинник RTC";
    case 3:  return "несправний внутр. термометр";
    case 4:  return "розряджена батарейка RTC";
    case 5:  return "немає живлення RS485";
    case 6:  return "помилка зв'язку RS485";
    case 7:  return "помилка зв'язку RS232";
    case 8:  return "помилка зв'язку USB";
    case 9:  return "вентилятор повільний";
    case 10: return "вентилятор зупинився";
    case 11: return "висока внутр. температура";
    case 12: return "запобіжник AP208";
    case 13: return "вентилятор на макс. швидкості";
    case 14: return "низький опір вставки";
    case 15: return "похибка опорної напруги";
    case 17: return "обрив датчика T батареї";
    case 18: return "КЗ датчика T батареї";
    case 19: return "обрив датчика T радіатора";
    case 20: return "КЗ датчика T радіатора";
    case 21: return "обрив дод. датчика T";
    case 22: return "КЗ дод. датчика T";
    case 23: return "обрив аналог. датчика";
    case 24: return "аналог. датчик поза шкалою";
    case 25: return "запобіжник батареї";
    case 26: return "Uном поза діапазоном";
    case 27: return "висока напруга батареї";
    case 28: return "напруга кінця розряду";
    case 29: return "переплутана полярність";
    case 30: return "великий струм секцій";
    case 31: return "макс. опір";
    case 32: return "велика потужність секцій";
    case 33: return "запобіжник заряду";
    case 35: return "тайм-аут заряду";
    case 41: return "тест зупинено батареєю";
    default: return "";
    }
}

int blt_rec_alarm_first(const blt_rec_t *r)
{
    for (int i = 0; i < 6; i++)
        for (int b = 0; b < 8; b++)
            if ((r->mem_alarms[i] >> b) & 1) return 8 * i + b + 1;
    return 0;
}
