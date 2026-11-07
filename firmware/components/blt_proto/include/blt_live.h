/*
 * Decoder for the BLT160 live block: RI, address 0, 192 bytes (read as three
 * 64-byte frames, like BltView). Field map: docs/BLT_REGISTERS.md section 3,
 * reversed from BltView.exe; little-endian. Pure C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLT_LIVE_LEN       0xC0   /* whole block */
#define BLT_LIVE_MIN_LEN   0x76   /* last field used by BltView ends at 0x75 */
#define BLT_LIVE_ID_OFF    0x76   /* battery serial / ID text, 48 chars */
#define BLT_LIVE_ID_LEN    48

/* RI[0x6B] */
typedef enum {
    BLT_ST_START = 0,
    BLT_ST_DISCHARGE = 1,
    BLT_ST_DISCHARGE_PAUSE = 2,
    BLT_ST_CHARGE = 3,
    BLT_ST_CHARGE_PAUSE = 4,
    BLT_ST_END = 5,
    BLT_ST_UNKNOWN6 = 6,          /* shown as "***" */
    BLT_ST_TEST = 7,
    BLT_ST_COLLAUDO = 8,          /* factory acceptance test */
    BLT_ST_PROGRAMMING = 9,
} blt_state_t;

/* RI[0x18] LED flags */
#define BLT_FLAG_CHARGE_ALARM     0x8000u
#define BLT_FLAG_CHARGING         0x4000u
#define BLT_FLAG_DISCHARGING      0x2000u
#define BLT_FLAG_DISCHARGE_ALARM  0x1000u

typedef struct {
    float    v_batt;            /* 0x02 /10 V   battery voltage            */
    float    v_load;            /* 0x04 /10 V   voltage across the load    */
    float    i_charge;          /* 0x06 /10 A                              */
    float    i_discharge;       /* 0x08 /10 A                              */
    float    temp_c[4];         /* 0x0A..0x10 s16 /10 degC (probe 1 = internal) */
    uint16_t flags;             /* 0x18 BLT_FLAG_*                          */
    uint8_t  outputs1, outputs2;/* 0x1A, 0x1B relays/stages/fans           */
    uint16_t rated_ah;          /* 0x1F programmed rated capacity, Ah      */
    uint8_t  rated_v;           /* 0x22 programmed rated voltage, V        */
    uint8_t  i_set;             /* 0x23 programmed discharge current, A    */
    uint8_t  rtc_s, rtc_m, rtc_h, rtc_d, rtc_mo, rtc_y;  /* 0x26..0x2B BCD -> binary */
    uint16_t t_charge_min;      /* 0x38 */
    uint16_t t_discharge_min;   /* 0x3A */
    uint16_t t_charge_pause_min;    /* 0x3C */
    uint16_t t_discharge_pause_min; /* 0x3E */
    uint8_t  n_charges;         /* 0x40 */
    uint8_t  n_discharges;      /* 0x41 cycle number */
    uint16_t ah_present;        /* 0x48 Ah */
    uint16_t ah_charged;        /* 0x4C Ah */
    uint16_t ah_discharged;     /* 0x4E Ah */
    uint8_t  efficiency_pct;    /* 0x52 (>= 0xFB = n/a) */
    float    v_min, v_max;      /* 0x53, 0x55 /100 (per cell? unverified) */
    int8_t   tmax_c[4];         /* 0x5B..0x5E */
    uint8_t  alarms[6];         /* 0x5F..0x64 bitmap, id = 8*byte + bit + 1 */
    uint8_t  state;             /* 0x6B blt_state_t */
    uint16_t test_no;           /* 0x70 */
    char     battery_id[17];    /* 0x76..0x85 ASCII serial, trailing spaces trimmed
                                   (copy of the RD 0x40 ID block, e.g. "202611070001";
                                   0x86..0xA5 is its 32-char text field) */
} blt_live_t;

/* Decode len bytes of the live block (needs >= BLT_LIVE_MIN_LEN). */
bool blt_live_decode(const uint8_t *blk, size_t len, blt_live_t *out);

/* Italian state text as BltView shows it ("BATTERIA IN SCARICA" ...). */
const char *blt_state_name_it(uint8_t state);

/* Number of active alarm bits; first active anomaly id in *first (0 if none). */
int blt_live_alarm_count(const blt_live_t *l, int *first);

#ifdef __cplusplus
}
#endif
