/*
 * Semantic REST API — implementation. See semantic_api.h for the endpoints.
 */
#include "semantic_api.h"
#include "tuya_state.h"
#include "macon_fields.h"
#include "macon_faults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <initializer_list>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

using namespace arctic;

static const char *TAG = "sem";

namespace semantic_api {

namespace {

// ---------------------------------------------------------------- helpers

esp_err_t sendJson(httpd_req_t *req, cJSON *json) {
    char *str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, str ? str : "{}");
    free(str);
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t sendError(httpd_req_t *req, const char *status, const char *msg) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", msg);
    httpd_resp_set_status(req, status);
    return sendJson(req, json);
}

esp_err_t badRequest(httpd_req_t *req, const char *msg) {
    return sendError(req, "400 Bad Request", msg);
}

// Parse the JSON body (caller deletes). nullptr on empty / oversized / invalid.
cJSON *readJson(httpd_req_t *req, int max_len = 4096) {
    const int len = req->content_len;
    if (len <= 0 || len > max_len) return nullptr;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return nullptr;
    int total = 0;
    while (total < len) {
        const int r = httpd_req_recv(req, buf + total, len - total);
        if (r <= 0) { free(buf); return nullptr; }
        total += r;
    }
    buf[len] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

const char *kindName(MaconFieldKind k) {
    switch (k) {
        case MaconFieldKind::Number:             return "number";
        case MaconFieldKind::Flag:               return "flag";
        case MaconFieldKind::WorkingMode:        return "working_mode";
        case MaconFieldKind::OperatingDirection: return "operating_direction";
    }
    return "unknown";
}

void addFieldValue(cJSON *obj, const MaconState &s, const MaconFieldDesc &d) {
    int32_t v = 0;
    if (!macon_field_get(s, d, &v)) {
        cJSON_AddNullToObject(obj, d.name);
        return;
    }
    switch (d.kind) {
        case MaconFieldKind::Number:
            cJSON_AddNumberToObject(obj, d.name, v);
            break;
        case MaconFieldKind::Flag:
            cJSON_AddBoolToObject(obj, d.name, v != 0);
            break;
        case MaconFieldKind::WorkingMode: {
            const char *k = macon_working_mode_key(static_cast<MaconWorkingMode>(v));
            if (k) cJSON_AddStringToObject(obj, d.name, k); else cJSON_AddNullToObject(obj, d.name);
            break;
        }
        case MaconFieldKind::OperatingDirection: {
            const char *k = macon_direction_key(static_cast<MaconMode>(v));
            if (k) cJSON_AddStringToObject(obj, d.name, k); else cJSON_AddNullToObject(obj, d.name);
            break;
        }
    }
}

cJSON *faultJson(const MaconFault &f) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "code", f.code);
    cJSON_AddStringToObject(o, "label", f.label);
    cJSON_AddStringToObject(o, "severity", macon_fault_severity_name(f.severity));
    cJSON_AddNumberToObject(o, "site", f.site);
    cJSON_AddNumberToObject(o, "id", static_cast<int>(f.id));
    return o;
}

void addFaultsFromState(cJSON *parent, const char *key, const MaconState &s) {
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    MaconFault out[40];
    const size_t n = macon_decode_faults(s.fault_run, s.fault_ee, s.fault_comp,
                                         s.fault_elec, s.fault_ref, out, 40);
    for (size_t i = 0; i < n; ++i) cJSON_AddItemToArray(arr, faultJson(out[i]));
}

// Translate one JSON value into the int the library expects for `d`.
// Returns false with `why` filled on a type error.
bool jsonToFieldValue(const MaconFieldDesc &d, const cJSON *v, int32_t *out,
                      char *why, size_t why_len) {
    switch (d.kind) {
        case MaconFieldKind::Number:
            if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble)) {
                snprintf(why, why_len, "field '%s' needs an integer", d.name);
                return false;
            }
            *out = static_cast<int32_t>(v->valuedouble);
            return true;
        case MaconFieldKind::Flag:
            if (cJSON_IsBool(v)) { *out = cJSON_IsTrue(v) ? 1 : 0; return true; }
            if (cJSON_IsNumber(v) && (v->valueint == 0 || v->valueint == 1)) {
                *out = v->valueint;
                return true;
            }
            snprintf(why, why_len, "field '%s' needs a boolean", d.name);
            return false;
        case MaconFieldKind::WorkingMode: {
            const MaconWorkingMode m = cJSON_IsString(v)
                ? macon_working_mode_from_key(v->valuestring) : MaconWorkingMode::Unknown;
            if (m == MaconWorkingMode::Unknown) {
                snprintf(why, why_len, "field '%s' needs one of cooling, floor_heating, "
                         "fan_coil_heating, hot_water, auto", d.name);
                return false;
            }
            *out = static_cast<int32_t>(m);
            return true;
        }
        case MaconFieldKind::OperatingDirection: {
            const MaconMode m = cJSON_IsString(v)
                ? macon_direction_from_key(v->valuestring) : MaconMode::Unknown;
            if (m == MaconMode::Unknown) {
                snprintf(why, why_len, "field '%s' needs heating or cooling", d.name);
                return false;
            }
            *out = static_cast<int32_t>(m);
            return true;
        }
    }
    snprintf(why, why_len, "field '%s' is not settable", d.name);
    return false;
}

// ---------------------------------------------------------------- handlers

esp_err_t handleFields(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "api_version", MACON_SEMANTIC_API_VERSION);
    cJSON *arr = cJSON_AddArrayToObject(json, "fields");
    for (size_t i = 0; i < MACON_FIELDS_COUNT; ++i) {
        const MaconFieldDesc &d = MACON_FIELDS[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", d.name);
        cJSON *al = cJSON_AddArrayToObject(o, "aliases");
        for (const char *a : d.aliases) if (a) cJSON_AddItemToArray(al, cJSON_CreateString(a));
        cJSON_AddStringToObject(o, "kind", kindName(d.kind));
        if (d.unit) cJSON_AddStringToObject(o, "unit", d.unit);
        if (d.kind == MaconFieldKind::Number) {
            cJSON_AddNumberToObject(o, "min", d.min);
            cJSON_AddNumberToObject(o, "max", d.max);
            cJSON_AddNumberToObject(o, "step", d.step);
        } else if (d.kind == MaconFieldKind::WorkingMode) {
            cJSON *opt = cJSON_AddArrayToObject(o, "options");
            for (const char *k : { "cooling", "floor_heating", "fan_coil_heating", "hot_water", "auto" })
                cJSON_AddItemToArray(opt, cJSON_CreateString(k));
        } else if (d.kind == MaconFieldKind::OperatingDirection) {
            cJSON *opt = cJSON_AddArrayToObject(o, "options");
            for (const char *k : { "heating", "cooling" })
                cJSON_AddItemToArray(opt, cJSON_CreateString(k));
        }
        cJSON_AddBoolToObject(o, "verified", d.verified);
        cJSON_AddItemToArray(arr, o);
    }
    return sendJson(req, json);
}

esp_err_t handleGetState(httpd_req_t *req) {
    MaconState s{};
    tuya_state::decode(&s);
    cJSON *json = cJSON_CreateObject();
    cJSON *fields = cJSON_AddObjectToObject(json, "fields");
    for (size_t i = 0; i < MACON_FIELDS_COUNT; ++i) addFieldValue(fields, s, MACON_FIELDS[i]);
    cJSON_AddStringToObject(json, "operation", operation_name(decode_operation(s)));
    cJSON_AddBoolToObject(json, "compressor_running", s.compressor_freq > 0);
    addFaultsFromState(json, "faults", s);
    return sendJson(req, json);
}

esp_err_t handlePatchState(httpd_req_t *req) {
    cJSON *json = readJson(req);
    if (!json || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return badRequest(req, "body must be a JSON object of field: value");
    }
    char why[160] = "";
    bool ok = true;
    int applied = 0;
    {
        tuya_state::Access a;
        // Stage on a copy so a bad field leaves the served state untouched.
        MaconImage staged = a.image();
        const cJSON *item = nullptr;
        cJSON_ArrayForEach(item, json) {
            const MaconFieldDesc *d = macon_field_find(item->string);
            if (!d) {
                snprintf(why, sizeof(why), "unknown field '%s' (see GET /api/fields)", item->string);
                ok = false;
                break;
            }
            int32_t v = 0;
            if (!jsonToFieldValue(*d, item, &v, why, sizeof(why))) { ok = false; break; }
            const MaconSetResult r = macon_field_set(staged, *d, v);
            if (r != MaconSetResult::Ok) {
                if (d->kind == MaconFieldKind::Number) {
                    snprintf(why, sizeof(why), "field '%s'=%ld out of range [%ld, %ld] step %ld",
                             d->name, (long)v, (long)d->min, (long)d->max, (long)d->step);
                } else {
                    snprintf(why, sizeof(why), "field '%s' rejected", d->name);
                }
                ok = false;
                break;
            }
            ++applied;
        }
        if (ok) a.image() = staged;
    }
    cJSON_Delete(json);
    if (!ok) return badRequest(req, why);
    ESP_LOGI(TAG, "PATCH /api/state: %d field(s)", applied);
    return handleGetState(req);
}

esp_err_t handleFaultCatalog(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(json, "faults");
    for (uint16_t i = 0; i < static_cast<uint16_t>(MaconFaultId::Count); ++i) {
        const MaconFaultId id = static_cast<MaconFaultId>(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", i);
        cJSON_AddStringToObject(o, "code", macon_code_for_fault_id(id));
        cJSON_AddStringToObject(o, "label", macon_label_for_fault_id(id));
        cJSON_AddStringToObject(o, "severity",
                                macon_fault_severity_name(macon_severity_for_fault_id(id)));
        cJSON_AddStringToObject(o, "resolution", macon_fault_resolution(id));
        cJSON *sites = cJSON_AddArrayToObject(o, "sites");
        MaconFaultSiteId ss[4];
        const size_t n = macon_fault_sites_for_id(id, ss, 4);
        for (size_t k = 0; k < n && k < 4; ++k) {
            const MaconFaultBit *fb = macon_fault_bit_for_site(ss[k]);
            cJSON *so = cJSON_CreateObject();
            cJSON_AddNumberToObject(so, "site", ss[k]);
            if (fb) {
                cJSON_AddStringToObject(so, "label", fb->label);
                cJSON_AddStringToObject(so, "severity", macon_fault_severity_name(fb->severity));
            }
            cJSON_AddItemToArray(sites, so);
        }
        cJSON_AddItemToArray(arr, o);
    }
    return sendJson(req, json);
}

esp_err_t handleGetFaults(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    addActiveFaults(json, "faults");
    return sendJson(req, json);
}

esp_err_t handlePostFault(httpd_req_t *req) {
    cJSON *json = readJson(req);
    if (!json) return badRequest(req, "body must be JSON");
    const cJSON *code = cJSON_GetObjectItem(json, "code");
    const cJSON *site = cJSON_GetObjectItem(json, "site");
    const cJSON *act  = cJSON_GetObjectItem(json, "active");
    const bool active = act ? cJSON_IsTrue(act) : true;
    if (act && !cJSON_IsBool(act)) {
        cJSON_Delete(json);
        return badRequest(req, "'active' must be a boolean");
    }
    char why[96] = "";
    bool ok = false;
    if (cJSON_IsString(code) && !site) {
        const MaconFaultId id = macon_fault_id_from_code(code->valuestring);
        if (id == MaconFaultId::Unknown) {
            snprintf(why, sizeof(why), "unknown fault code '%s' (see GET /api/faults/catalog)",
                     code->valuestring);
        } else {
            tuya_state::Access a;
            a.image().set_fault(id, active);
            ok = true;
        }
    } else if (cJSON_IsNumber(site) && !code) {
        tuya_state::Access a;
        ok = a.image().set_fault_site(static_cast<MaconFaultSiteId>(site->valueint), active);
        if (!ok) snprintf(why, sizeof(why), "unknown fault site %d", site->valueint);
    } else {
        snprintf(why, sizeof(why), "give exactly one of 'code' (string) or 'site' (number)");
    }
    cJSON_Delete(json);
    if (!ok) return badRequest(req, why);
    return handleGetFaults(req);
}

esp_err_t handleClearFaults(httpd_req_t *req) {
    {
        tuya_state::Access a;
        a.image().clear_faults();
    }
    return handleGetFaults(req);
}

// ---------------------------------------------------------------- bench lease
// Advisory: CI and humans claim the bench before driving it so two runs can't
// fight over the same simulator. Not enforced on the other endpoints.

char    s_lease_owner[64] = "";
int64_t s_lease_expiry_us = 0;

bool leaseHeld(int64_t now) { return s_lease_owner[0] && now < s_lease_expiry_us; }

cJSON *leaseJson(int64_t now) {
    cJSON *o = cJSON_CreateObject();
    const bool held = leaseHeld(now);
    cJSON_AddBoolToObject(o, "held", held);
    if (held) {
        cJSON_AddStringToObject(o, "owner", s_lease_owner);
        cJSON_AddNumberToObject(o, "remaining_s", (double)((s_lease_expiry_us - now) / 1000000));
    }
    return o;
}

esp_err_t handleGetLease(httpd_req_t *req) {
    return sendJson(req, leaseJson(esp_timer_get_time()));
}

esp_err_t handlePostLease(httpd_req_t *req) {
    cJSON *json = readJson(req);
    const cJSON *owner = json ? cJSON_GetObjectItem(json, "owner") : nullptr;
    const cJSON *ttl   = json ? cJSON_GetObjectItem(json, "ttl_s") : nullptr;
    if (!cJSON_IsString(owner) || !owner->valuestring[0]) {
        cJSON_Delete(json);
        return badRequest(req, "need { \"owner\": \"...\", \"ttl_s\": N }");
    }
    const int64_t now = esp_timer_get_time();
    int ttl_s = cJSON_IsNumber(ttl) ? ttl->valueint : 600;
    if (ttl_s < 1) ttl_s = 1;
    if (ttl_s > 7200) ttl_s = 7200;
    if (leaseHeld(now) && strcmp(s_lease_owner, owner->valuestring) != 0) {
        cJSON_Delete(json);
        cJSON *resp = leaseJson(now);
        cJSON_AddStringToObject(resp, "error", "bench lease held by another owner");
        httpd_resp_set_status(req, "409 Conflict");
        return sendJson(req, resp);
    }
    snprintf(s_lease_owner, sizeof(s_lease_owner), "%s", owner->valuestring);
    s_lease_expiry_us = now + (int64_t)ttl_s * 1000000;
    cJSON_Delete(json);
    ESP_LOGI(TAG, "lease -> %s (%ds)", s_lease_owner, ttl_s);
    return sendJson(req, leaseJson(now));
}

esp_err_t handleDeleteLease(httpd_req_t *req) {
    cJSON *json = readJson(req);
    const cJSON *owner = json ? cJSON_GetObjectItem(json, "owner") : nullptr;
    const cJSON *force = json ? cJSON_GetObjectItem(json, "force") : nullptr;
    const int64_t now = esp_timer_get_time();
    const bool mine = cJSON_IsString(owner) && strcmp(owner->valuestring, s_lease_owner) == 0;
    if (leaseHeld(now) && !mine && !cJSON_IsTrue(force)) {
        cJSON_Delete(json);
        cJSON *resp = leaseJson(now);
        cJSON_AddStringToObject(resp, "error", "not the lease owner (pass force: true to break it)");
        httpd_resp_set_status(req, "409 Conflict");
        return sendJson(req, resp);
    }
    s_lease_owner[0] = '\0';
    s_lease_expiry_us = 0;
    cJSON_Delete(json);
    return sendJson(req, leaseJson(now));
}

}  // namespace

void addMaconInfo(cJSON *parent) {
    cJSON *m = cJSON_AddObjectToObject(parent, "macon");
    char buf[12];
    cJSON_AddNumberToObject(m, "api_version", MACON_SEMANTIC_API_VERSION);
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)macon_layout_fingerprint());
    cJSON_AddStringToObject(m, "layout_fingerprint", buf);
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)macon_catalog_fingerprint());
    cJSON_AddStringToObject(m, "catalog_fingerprint", buf);
}

void addActiveFaults(cJSON *parent, const char *key) {
    MaconState s{};
    tuya_state::decode(&s);
    addFaultsFromState(parent, key, s);
}

int registerHandlers(httpd_handle_t server) {
    const httpd_uri_t uris[] = {
        { "/api/fields",         HTTP_GET,    handleFields,       nullptr },
        { "/api/state",          HTTP_GET,    handleGetState,     nullptr },
        { "/api/state",          HTTP_PATCH,  handlePatchState,   nullptr },
        { "/api/faults/catalog", HTTP_GET,    handleFaultCatalog, nullptr },
        { "/api/faults/clear",   HTTP_POST,   handleClearFaults,  nullptr },
        { "/api/faults",         HTTP_GET,    handleGetFaults,    nullptr },
        { "/api/faults",         HTTP_POST,   handlePostFault,    nullptr },
        { "/api/lease",          HTTP_GET,    handleGetLease,     nullptr },
        { "/api/lease",          HTTP_POST,   handlePostLease,    nullptr },
        { "/api/lease",          HTTP_DELETE, handleDeleteLease,  nullptr },
    };
    static_assert(sizeof(uris) / sizeof(uris[0]) <= HANDLER_COUNT, "bump HANDLER_COUNT");
    int n = 0;
    for (const auto &u : uris) {
        if (httpd_register_uri_handler(server, &u) == ESP_OK) ++n;
        else ESP_LOGE(TAG, "failed to register %s", u.uri);
    }
    return n;
}

}  // namespace semantic_api
