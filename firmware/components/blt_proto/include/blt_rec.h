/*
 * BLT160 recorder (docs/BLT_RECORDER.md): cycle records (a 0xC0 snapshot of
 * the live block, also valid for the live block itself) and graph pages
 * (0x80 B: BCD time + 20 samples of tag/V/I). Pure C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLT_REC_LEN      0xC0
#define BLT_PAGE_LEN     0x80
#define BLT_PAGE_SAMPLES 20
#define BLT_REC_ADDR(n)  (0xF00u + ((uint32_t)(n) % 150u) * 0xC0u)
#define BLT_PAGE_ADDR(p) (0x10000u + ((uint32_t)(p) % 250u) * 0x80u)

typedef struct {
    uint8_t  mode;              /* 0x1E work mode (0 = TEST efficiency) */
    uint16_t rated_ah;          /* 0x1F */
    uint8_t  rated_v;           /* 0x22 */
    uint8_t  discharge_a;       /* 0x23 */
    uint8_t  thr_cv, stop_cv;   /* 0x24, 0x25: cV per 2-V cell */
    uint8_t  snap_y, snap_mo, snap_d, snap_h, snap_mi;   /* 0x26..0x2B snapshot RTC */
    uint8_t  dis_y, dis_mo, dis_d, dis_h, dis_mi;        /* 0x2C..0x30 discharge start (0 = none) */
    uint16_t t_dis_min;         /* 0x3A */
    uint16_t t_dis_pause_min;   /* 0x3E */
    uint8_t  cycle;             /* 0x41 */
    uint16_t ah_start, ah_end;  /* 0x42, 0x44 */
    uint16_t ah_dis;            /* 0x4E */
    uint8_t  efficiency;        /* 0x52 (>= 0xFB n/a) */
    uint16_t v_min_raw;         /* 0x53 /100 V per 2-V cell */
    int8_t   t_card_max;        /* 0x5B */
    uint8_t  mem_alarms[6];     /* 0x65..0x6A */
    uint8_t  state;             /* 0x6B */
    uint8_t  n_active;          /* 0x6D */
    uint16_t pr_cycle;          /* 0x6E */
    uint16_t pr_test;           /* 0x70 */
    uint32_t pr_page;           /* 0x72 */
    char     battery_id[17];    /* 0x76 */
    char     note[33];          /* 0x86 */
    uint16_t t_limit_min;       /* 0xA7 (6000 = unlimited) */
    uint8_t  graph_min;         /* 0xB1 */
} blt_rec_t;

bool blt_rec_decode(const uint8_t *b, size_t len, blt_rec_t *out);

/* discharge stop voltage of the record, V (rated * stop_cv / 200) */
float blt_rec_stop_v(const blt_rec_t *r);
/* number of memorised alarms */
int blt_rec_alarm_count(const blt_rec_t *r);
/* id of the first memorised alarm (1..48), 0 = none */
int blt_rec_alarm_first(const blt_rec_t *r);
/* Ukrainian name of a tester anomaly id (BLT_REGISTERS.md), "" if unnamed */
const char *blt_alarm_name_ua(int id);

typedef struct {
    uint8_t  y, mo, d, h, mi;   /* time of sample 0 */
    uint8_t  interval_min;      /* 0x7F */
    int      n;                 /* valid samples (stops at V = 0xFFFF) */
    uint16_t tag[BLT_PAGE_SAMPLES];
    float    v[BLT_PAGE_SAMPLES];
    float    i[BLT_PAGE_SAMPLES];   /* negative = discharge */
} blt_page_t;

bool blt_page_decode(const uint8_t *b, size_t len, blt_page_t *out);

/* minutes since 2000-01-01 00:00 (for differences between record/page times) */
int32_t blt_minutes(uint8_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi);

#ifdef __cplusplus
}
#endif
