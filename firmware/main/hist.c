#include "hist.h"

#include <string.h>
#include "esp_heap_caps.h"

static hist_pt_t *s_buf;
static int s_n;
static int s_stride = 1;   /* keep every s_stride-th sample */
static int s_skip;

void hist_init(void)
{
    if (!s_buf) s_buf = heap_caps_calloc(HIST_MAX, sizeof(hist_pt_t), MALLOC_CAP_SPIRAM);
    hist_reset();
}

void hist_reset(void)
{
    s_n = 0;
    s_stride = 1;
    s_skip = 0;
}

void hist_add(const online_sample_t *s)
{
    if (!s_buf) return;
    if (++s_skip < s_stride) return;
    s_skip = 0;

    if (s_n == HIST_MAX) {             /* thin to half, keep the whole span */
        for (int i = 0; i < HIST_MAX / 2; i++) s_buf[i] = s_buf[i * 2];
        s_n = HIST_MAX / 2;
        s_stride *= 2;
    }
    hist_pt_t *p = &s_buf[s_n++];
    p->t_s = s->elapsed_s;
    p->v_t = s->voltage_v;
    p->i_t = s->current_a;
    p->v_b = s->bms_present ? s->bms_pack_v : 0;
    p->i_b = s->bms_present ? s->bms_current_a : 0;
    p->n = s->bms_present ? s->cell_count : 0;
    if (p->n > UI_MAX_CELLS) p->n = UI_MAX_CELLS;
    memcpy(p->cell, s->cell_mv, p->n * sizeof(uint16_t));
}

int hist_count(void) { return s_n; }

const hist_pt_t *hist_at(int i)
{
    return (s_buf && i >= 0 && i < s_n) ? &s_buf[i] : NULL;
}

int hist_prepend_recorder(const uint16_t *t_min, const float *v, const float *i_abs, int n)
{
    if (!s_buf || n <= 0) return 0;
    uint32_t first = s_n ? s_buf[0].t_s : UINT32_MAX;
    int m = 0;
    while (m < n && (uint32_t)t_min[m] * 60u < first) m++;
    int room = HIST_MAX - s_n - 1;
    if (m == 0 || room <= 0) return 0;
    int step = (m + room - 1) / room;
    int k = (m + step - 1) / step;
    memmove(s_buf + k, s_buf, (size_t)s_n * sizeof(hist_pt_t));
    for (int j = 0; j < k; j++) {
        int src = j * step;
        hist_pt_t *p = &s_buf[j];
        memset(p, 0, sizeof *p);
        p->t_s = (uint32_t)t_min[src] * 60u;
        p->v_t = v[src];
        p->i_t = -i_abs[src];
    }
    s_n += k;
    return k;
}

int hist_prepend_points(const hist_pt_t *pts, int n)
{
    if (!s_buf || n <= 0) return 0;
    uint32_t first = s_n ? s_buf[0].t_s : UINT32_MAX;
    int m = 0;
    while (m < n && pts[m].t_s < first) m++;
    int room = HIST_MAX - s_n - 1;
    if (m == 0 || room <= 0) return 0;
    int step = (m + room - 1) / room;
    int k = (m + step - 1) / step;
    memmove(s_buf + k, s_buf, (size_t)s_n * sizeof(hist_pt_t));
    for (int j = 0; j < k; j++) s_buf[j] = pts[j * step];
    s_n += k;
    return k;
}
