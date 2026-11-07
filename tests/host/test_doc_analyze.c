/* Host test for the protocol conclusions (components/report/doc_analyze). */
#include "doc_analyze.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static doc_facts_t good(void)
{
    doc_facts_t f;
    memset(&f, 0, sizeof f);
    f.ah = 306; f.rated_ah = 300; f.cells = 20;
    f.v_before = 83.6f; f.temp_known = true; f.temp_start = 21; f.temp_max_known = true; f.temp_max = 27;
    f.d_start = 12; f.d_end = 35; f.stop = DOC_STOP_VSTOP; f.v_end = 54.0f; f.v_stop = 54.0f;
    return f;
}

static int count(const doc_finding_t *o, int n, doc_flevel_t lv)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += o[i].level == lv;
    return c;
}

static bool has(const doc_finding_t *o, int n, const char *s)
{
    for (int i = 0; i < n; i++) if (strstr(o[i].text, s)) return true;
    return false;
}

int main(void)
{
    char v[256];
    doc_finding_t o[DOC_FINDINGS_MAX];

    doc_facts_t f = good();
    int n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(strstr(v, "відповідає") != NULL, "full capacity verdict: %s", v);
    CHECK(count(o, n, DOC_F_CAUSE) == 0, "no causes on a good battery");
    CHECK(has(o, n, "4,18 В/комірку") && has(o, n, "заряджена повністю"), "charge line with decimal comma");

    /* 96 %: under-charged (82.0 V / 20 = 4.10 V/cell) */
    f = good(); f.ah = 288; f.v_before = 82.0f;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(strstr(v, "96,0 %") && strstr(v, "нижча на 4,0 %"), "96%% verdict: %s", v);
    CHECK(has(o, n, "заряджена не до кінця") && count(o, n, DOC_F_CAUSE) == 1, "under-charge is the cause");

    /* 96 %: cold */
    f = good(); f.ah = 288; f.temp_start = 6;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "приблизно на 8 %") && count(o, n, DOC_F_CAUSE) == 1, "cold is the cause");

    /* 96 % in a cool workshop (12 C): ~5 % from the cold, ~304 Ah at 25 C */
    f = good(); f.ah = 288; f.temp_start = 12;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "на 5 %") && has(o, n, "близько 304 А·год"), "cool workshop estimate");
    CHECK(count(o, n, DOC_F_CAUSE) == 1, "temperature counted as a cause");

    /* the same from the room temperature typed in (no BMS) */
    f = good(); f.ah = 288; f.temp_start = 12; f.temp_room = true; f.temp_max_known = false;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "Температура в приміщенні на початку 12") && count(o, n, DOC_F_CAUSE) == 1, "room temperature wording");

    /* same at full capacity: only a note */
    f = good(); f.temp_start = 12;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(count(o, n, DOC_F_CAUSE) == 0 && has(o, n, "знижена"), "cold note at full capacity");

    /* 96 % without a temperature: says so */
    f = good(); f.ah = 288; f.temp_known = false; f.temp_max_known = false;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "не вимірювали"), "unknown temperature mentioned");

    /* 96 %: BMS stopped at 59 V, tester threshold 54 V, big end imbalance */
    f = good(); f.ah = 288; f.stop = DOC_STOP_BMS; f.v_end = 59.0f; f.d_end = 140; f.low_cell = 12;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "найнижча комірка - №12") && has(o, n, "2,95 В/комірку"), "BMS stop: avg per cell + lowest cell");
    CHECK(!has(o, n, "54,0"), "no tester threshold in the text");
    CHECK(has(o, n, "значний"), "end imbalance flagged");
    CHECK(count(o, n, DOC_F_CAUSE) == 1, "imbalance is the cause (%d)", count(o, n, DOC_F_CAUSE));

    /* 96 % with nothing wrong: cells themselves */
    f = good(); f.ah = 288;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "станом самих комірок"), "no external cause note");
    for (int i = 0; i < n; i++) printf("  [%d] %s\n", o[i].level, o[i].text);

    /* stopped early by the operator / time */
    f = good(); f.ah = 200; f.stop = DOC_STOP_MANUAL;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "до досягнення порогу"), "manual stop");
    f.stop = DOC_STOP_TIME;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "обмеженням часу"), "time limit");

    /* running */
    f = good(); f.ah = 150; f.stop = DOC_STOP_RUNNING;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(strstr(v, "Проміжний") != NULL, "interim verdict");

    /* unknown data: no charge / balance / temperature lines, no crash */
    memset(&f, 0, sizeof f);
    f.ah = 290; f.rated_ah = 300; f.d_start = f.d_end = -1; f.stop = DOC_STOP_VSTOP;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(n >= 1 && n <= DOC_FINDINGS_MAX, "unknowns: %d findings", n);

    /* LFP limits: 3.30 V/cell is "not fully" for LFP, 60 mV end imbalance fine */
    f = good(); f.lfp = true; f.cells = 16; f.v_before = 16 * 3.31f; f.d_start = 30; f.d_end = 60;
    n = doc_analyze(&f, v, sizeof v, o, DOC_FINDINGS_MAX);
    CHECK(has(o, n, "не до кінця"), "LFP partial charge");
    CHECK(has(o, n, "в нормі."), "LFP balance ok");

    printf("test_doc_analyze: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
