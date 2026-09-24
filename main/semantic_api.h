/*
 * Semantic REST API — heat-pump state by MEANING, backed by the arctic-macon
 * library (macon_fields.h / macon_faults.h). No register numbers here.
 *
 *   GET   /api/fields          — field catalog (name, aliases, kind, unit, range, step, verified)
 *   GET   /api/state           — every field decoded, active faults, operation
 *   PATCH /api/state           — set fields { "outlet_water_temp": 42, "fan_on": true,
 *                                 "working_mode": "cooling" }; all-or-nothing, strict
 *   GET   /api/faults/catalog  — every fault id with code/label/severity/resolution/sites
 *   GET   /api/faults          — active faults
 *   POST  /api/faults          — { "code": "E19", "active": true } or { "site": N, "active": b }
 *   POST  /api/faults/clear    — clear every fault site (RUN indicator kept)
 *   GET/POST/DELETE /api/lease — advisory bench lease { "owner": "...", "ttl_s": 600 }
 */
#pragma once

#include "esp_http_server.h"
#include "cJSON.h"

namespace semantic_api {

// Register the endpoints above on a running server. Returns handler count.
int registerHandlers(httpd_handle_t server);

// Number of URI handlers registerHandlers() adds (for max_uri_handlers).
constexpr int HANDLER_COUNT = 11;

// Add { api_version, layout_fingerprint, catalog_fingerprint } for /api/status.
void addMaconInfo(cJSON *parent);

// Add the active-fault array (code/label/severity/site) to `parent[key]`.
void addActiveFaults(cJSON *parent, const char *key);

}  // namespace semantic_api
