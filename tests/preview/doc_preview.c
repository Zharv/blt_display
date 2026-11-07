/* Host preview of the printed protocol: renders the A4 page to a PGM.
 *   doc_preview <font.ttf> <out.pgm> */
#include "doc.h"
#include "rtext.h"
#include "urf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: doc_preview font.ttf out.pgm\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *ttf = malloc((size_t)n);
    fread(ttf, 1, (size_t)n, f); fclose(f);
    if (!rtext_init(ttf, (size_t)n)) { fprintf(stderr, "bad font\n"); return 1; }

    /* a discharge like the one on the tester today: 100 A, 20S NMC */
    static uint16_t t[200]; static float v[200], i[200];
    int k = 0;
    for (int m = 0; m <= 183; m += 1, k++) {
        float x = m / 183.0f;
        t[k] = (uint16_t)m;
        v[k] = 83.6f - 1.2f * (1 - expf(-m / 4.0f)) - 10.5f * x - 6.5f * powf(x, 8);
        i[k] = m == 0 ? 0 : 100.0f;
    }
    static doc_kv_t info[] = {
        { "Тестер", "BLT160" }, { "Серійний № батареї", "202611070001" },
        { "Номінальна напруга", "72 В" }, { "Номінальна ємність", "300 А·год" },
        { "Струм розряду", "100 А" }, { "Напруга кінця розряду", "56,0 В" },
        { "Початок розряду", "07.11.2026 11:31" }, { "Час розряду", "3:03" },
        { "Знята ємність", "306 А·год (102,0 %)" }, { "Енергія", "22 410 Вт·год" },
        { "Мін. напруга", "56,0 В" }, { "Макс. темп. тестера", "38 °C" },
        { "BMS", "JK · 20S" }, { "Комірки (кінець)", "2,80…2,83 В" },
        { "Розбаланс макс.", "26 мВ" }, { "Аварії тестера", "немає" },
    };
    doc_protocol_t d = {
        .company = "ТОВ \"ЗАВОД АКУМУЛЯТОРНИХ СИСТЕМ ЖАРВ\"",
        .company_code = "45745311",
        .address = { "Україна, 07400, Київська обл.,",
                     "м. Бровари, вул. Петлюри Симона, будинок 11Г, кімната 43" },
        .signer_title = "Директор ТОВ \"ЗАС ЖАРВ\"",
        .signer_name = "Жалінський Роман",
        .equipment = "Електронне навантаження BLT160",
        .number = "07112026-1", .date = "07.11.2026", .serial = "202611070001",
        .v_before = 83.8f, .temp_start = 21, .discharge_a = 100,
        .capacity_ah = 306, .discharge_min = 183, .in_progress = false,
        .info = info, .info_n = sizeof info / sizeof info[0],
        .t_min = t, .v = v, .i = i, .n = k, .start_h = 11, .start_m = 31, .v_stop = 56.0f,
    };
    /* argv[3]: a PROTO dump from the device (DOCK/DOCI/DOCS log lines) */
    static doc_kv_t dinfo[16];
    static uint16_t dt[5000]; static float dv[5000], di[5000];
    static char line[512];
    if (argc > 3 && (f = fopen(argv[3], "rb")) != NULL) {
        int ni = 0, ns = 0;
        d.in_progress = false;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *p;
            if ((p = strstr(line, "DOCS ")) != NULL && ns < 5000) {
                unsigned tt; float vv, ii;
                if (sscanf(p + 5, "%u %f %f", &tt, &vv, &ii) == 3) { dt[ns] = (uint16_t)tt; dv[ns] = vv; di[ns] = ii; ns++; }
            } else if ((p = strstr(line, "DOCI ")) != NULL && ni < 16) {
                char *bar = strchr(p + 5, '|');
                if (!bar) continue;
                *bar = 0;
                dinfo[ni].label = strdup(p + 5);
                snprintf(dinfo[ni].value, sizeof dinfo[ni].value, "%s", bar + 1);
                ni++;
            } else if ((p = strstr(line, "DOCK ")) != NULL) {
                char *k = p + 5, *bar = strchr(k, '|');
                if (!bar) continue;
                *bar = 0;
                char *v = bar + 1;
                if (!strcmp(k, "number")) snprintf(d.number, sizeof d.number, "%s", v);
                else if (!strcmp(k, "date")) snprintf(d.date, sizeof d.date, "%s", v);
                else if (!strcmp(k, "serial")) snprintf(d.serial, sizeof d.serial, "%s", v);
                else if (!strcmp(k, "v_before")) d.v_before = (float)atof(v);
                else if (!strcmp(k, "temp_start")) d.temp_start = (float)atof(v);
                else if (!strcmp(k, "discharge_a")) d.discharge_a = atoi(v);
                else if (!strcmp(k, "capacity_ah")) d.capacity_ah = (float)atof(v);
                else if (!strcmp(k, "discharge_min")) d.discharge_min = (uint32_t)atoi(v);
                else if (!strcmp(k, "in_progress")) d.in_progress = atoi(v) != 0;
                else if (!strcmp(k, "start")) sscanf(v, "%d:%d", &d.start_h, &d.start_m);
                else if (!strcmp(k, "v_stop")) d.v_stop = (float)atof(v);
            }
        }
        fclose(f);
        d.info = dinfo; d.info_n = ni;
        d.t_min = dt; d.v = dv; d.i = di; d.n = ns;
    }
    static uint8_t px[DOC_A4_W * DOC_A4_H];
    raster_t r;
    raster_init(&r, px, DOC_A4_W, DOC_A4_H);
    raster_clear(&r, 255);
    doc_protocol_render(&d, &r);
    printf("URF size: %zu bytes\n", urf_encode(&r, 300, NULL, 0));
    f = fopen(argv[2], "wb");
    fprintf(f, "P5\n%d %d\n255\n", DOC_A4_W, DOC_A4_H);
    fwrite(px, 1, sizeof px, f);
    fclose(f);
    return 0;
}
