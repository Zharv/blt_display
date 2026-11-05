/*
 * Render a report and print it over IPP to the Wi-Fi printer.
 */
#pragma once

#include "report.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render d into a raster, encode URF, and POST an IPP Print-Job to the printer.
 * Blocks for the duration of the HTTP exchange; call from its own task. */
esp_err_t print_job_send(const report_data_t *d);

#ifdef __cplusplus
}
#endif
