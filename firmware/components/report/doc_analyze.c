#include "doc_analyze.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* printf with a decimal comma */
static void fmt(char *b, int n, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, (size_t)n, f, ap);
    va_end(ap);
    /* decimal point -> comma, only between digits (sentence ends stay) */
    for (char *c = b; *c; c++)
        if (*c == '.' && c > b && c[-1] >= '0' && c[-1] <= '9' && c[1] >= '0' && c[1] <= '9') *c = ',';
}

static void add(doc_finding_t *out, int *k, int max, doc_flevel_t lv, const char *text)
{
    if (*k >= max) return;
    out[*k].level = lv;
    snprintf(out[*k].text, sizeof out[*k].text, "%s", text);
    (*k)++;
}

int doc_analyze(const doc_facts_t *f, char *verdict, int vn, doc_finding_t *out, int max)
{
    int k = 0;
    char t[400];
    const float pct = f->rated_ah > 0 ? f->ah * 100.0f / f->rated_ah : 0;

    /* 1. state of charge before the test (open-circuit voltage per cell) */
    if (f->v_before > 0 && f->cells > 0) {
        float vc = f->v_before / f->cells;
        /* rested open-circuit voltage: a full NMC cell settles from 4.20 to
         * ~4.13-4.17 V within hours after the charge, LFP to ~3.33-3.40 V */
        float full = f->lfp ? 3.33f : 4.13f, part = f->lfp ? 3.28f : 4.05f;
        if (f->v_before_loaded) {
            fmt(t, sizeof t, "Заряд перед тестом: напругу %.1f В виміряно вже під навантаженням - оцінка заряду неточна.",
                f->v_before);
            add(out, &k, max, DOC_F_NOTE, t);
        } else if (vc >= full) {
            fmt(t, sizeof t, "Заряд перед тестом: %.1f В (%.2f В/комірку) - батарея заряджена повністю.", f->v_before, vc);
            add(out, &k, max, DOC_F_OK, t);
        } else if (vc >= part) {
            fmt(t, sizeof t, "Заряд перед тестом: %.1f В (%.2f В/комірку) - батарея заряджена не до кінця (повний заряд %.2f В/комірку), частина ємності не була накопичена.",
                f->v_before, vc, f->lfp ? 3.40f : 4.20f);
            add(out, &k, max, DOC_F_CAUSE, t);
        } else {
            fmt(t, sizeof t, "Заряд перед тестом: %.1f В (%.2f В/комірку) - батарея недозаряджена (повний заряд %.2f В/комірку), виміряна ємність занижена.",
                f->v_before, vc, f->lfp ? 3.40f : 4.20f);
            add(out, &k, max, DOC_F_CAUSE, t);
        }
    }

    /* 2. cell balance; a BMS stop above the tester's threshold = weakest cell first */
    if (f->d_start >= 0 || f->d_end >= 0) {
        int s_ok = f->lfp ? 40 : 20, s_bad = f->lfp ? 80 : 40;
        int e_ok = f->lfp ? 100 : 50, e_bad = f->lfp ? 200 : 100;
        doc_flevel_t lv = DOC_F_OK;
        if (f->d_start > s_bad || f->d_end > e_bad) lv = DOC_F_CAUSE;
        else if (f->d_start > s_ok || f->d_end > e_ok) lv = DOC_F_NOTE;
        char a[24] = "-", b[24] = "-";
        if (f->d_start >= 0) snprintf(a, sizeof a, "%d", f->d_start);
        if (f->d_end >= 0) snprintf(b, sizeof b, "%d", f->d_end);
        fmt(t, sizeof t, "Баланс комірок: розбаланс на початку %s мВ, в кінці %s мВ - %s.", a, b,
            lv == DOC_F_OK ? "в нормі" : lv == DOC_F_NOTE ? "підвищений" : "значний, обмежує віддачу ємності");
        add(out, &k, max, lv, t);
    }

    /* 3. temperature: Li-ion capacity is rated at 25 C; colder cells give
     * roughly 0.4 %/C (NMC) or 0.6 %/C (LFP) less - an approximate model */
    if (f->temp_known) {
        float k_c = f->lfp ? 0.6f : 0.4f;
        float loss = f->temp_start < 25 ? (25 - f->temp_start) * k_c : 0;
        char heat[80] = "";
        if (f->temp_max_known) fmt(heat, sizeof heat, ", під час розряду до %.0f °C", f->temp_max);
        if (f->temp_start < 20 && pct < 100 && f->stop != DOC_STOP_RUNNING) {
            float at25 = loss < 50 ? f->ah / (1 - loss / 100) : f->ah;
            fmt(t, sizeof t, "Температура батареї на початку %.0f °C%s - нижче нормальних 20-25 °C: при такій температурі "
                "Li-ion віддає приблизно на %.0f %% менше; при 25 °C очікувалось би близько %.0f А·год.",
                f->temp_start, heat, loss, at25);
            add(out, &k, max, DOC_F_CAUSE, t);
        } else if (f->temp_start < 20) {
            fmt(t, sizeof t, "Температура батареї на початку %.0f °C%s - знижена (ємність при холоді менша приблизно на %.0f %%).",
                f->temp_start, heat, loss);
            add(out, &k, max, DOC_F_NOTE, t);
        } else {
            fmt(t, sizeof t, "Температура батареї: %.0f °C на початку%s - %s.", f->temp_start, heat,
                f->temp_max_known && f->temp_max > 45 ? "помітний нагрів" : "в нормі");
            add(out, &k, max, f->temp_max_known && f->temp_max > 45 ? DOC_F_NOTE : DOC_F_OK, t);
        }
    } else if (pct < 100 && f->stop != DOC_STOP_RUNNING) {
        add(out, &k, max, DOC_F_NOTE, "Температуру батареї не вимірювали (BMS не була підключена) - вплив температури "
            "на результат оцінити неможливо; при холоді в приміщенні ємність могла бути заниженою.");
    }

    /* 4. how the discharge ended */
    switch (f->stop) {
    case DOC_STOP_RUNNING:
        add(out, &k, max, DOC_F_NOTE, "Тест ще триває - результат проміжний.");
        break;
    case DOC_STOP_VSTOP:
        add(out, &k, max, DOC_F_OK, "Розряд проведено до напруги кінця розряду.");
        break;
    case DOC_STOP_BMS: {
        /* the BMS opens the load when its lowest cell reaches the limit */
        char avg[96] = "", cell[96] = "";
        if (f->cells > 0 && f->v_end > 0) fmt(avg, sizeof avg, " (у середньому %.2f В/комірку)", f->v_end / f->cells);
        if (f->low_cell > 0) snprintf(cell, sizeof cell, ", найнижча комірка - №%d", f->low_cell);
        if (f->v_end > 0) fmt(t, sizeof t, "Розряд завершив захист BMS батареї при %.1f В%s%s.", f->v_end, avg, cell);
        else fmt(t, sizeof t, "Розряд завершив захист BMS батареї%s.", cell);
        add(out, &k, max, DOC_F_OK, t);
        break;
    }
    case DOC_STOP_ALARM:
        fmt(t, sizeof t, "Тест перервано аварією тестера (%s) - ємність може бути занижена.", f->alarm_name ? f->alarm_name : "?");
        add(out, &k, max, DOC_F_CAUSE, t);
        break;
    case DOC_STOP_TIME:
        add(out, &k, max, DOC_F_CAUSE, "Тест зупинено за обмеженням часу - батарею розряджено не повністю, ємність занижена.");
        break;
    case DOC_STOP_MANUAL:
        add(out, &k, max, DOC_F_CAUSE, "Тест зупинено до досягнення порогу розряду - ємність може бути занижена.");
        break;
    }

    /* capacity reached: nothing "lowered" it - keep the facts as notes */
    if (pct >= 100 && f->stop != DOC_STOP_RUNNING)
        for (int i = 0; i < k; i++) if (out[i].level == DOC_F_CAUSE) out[i].level = DOC_F_NOTE;
    int causes = 0;
    for (int i = 0; i < k; i++) causes += out[i].level == DOC_F_CAUSE;
    if (f->stop == DOC_STOP_RUNNING)
        fmt(verdict, vn, "Проміжний результат: знято %.0f А·год (%.1f %% від номінальної %.0f А·год), тест триває.", f->ah, pct, f->rated_ah);
    else if (pct >= 100)
        fmt(verdict, vn, "Батарея відповідає заявленій ємності: %.0f А·год (%.1f %% від номінальної %.0f А·год).", f->ah, pct, f->rated_ah);
    else {
        fmt(verdict, vn, "Встановлена ємність %.0f А·год - %.1f %% від номінальної %.0f А·год (нижча на %.1f %%).%s",
            f->ah, pct, f->rated_ah, 100 - pct, causes ? " Що вплинуло на результат - нижче." : "");
        if (!causes)
            add(out, &k, max, DOC_F_NOTE, "Зовнішніх причин не виявлено: заряд, баланс комірок і температура в нормі - менша ємність ймовірно пов'язана зі станом самих комірок.");
    }
    return k;
}
