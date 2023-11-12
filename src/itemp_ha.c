#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <mgos_homeassistant.h>
#include <mgos_jstore.h>
#include <mgos_mqtt.h>

#include <mgos-cc1101.h>
#include <mgos-helpers/log.h>
#include <mgos-helpers/mem.h>
#include <mgos-helpers/time.h>
#include <mgos-helpers/tmr.h>
#include <mgos-itemp.h>

struct num_mqtt {
  const char *path;
  float val;
};

static void num_mqtt_cb(void *ud, const char *name, size_t name_len,
                        const char *path, const struct json_token *token) {
  struct num_mqtt *inm = ud;
  if (token->type != JSON_TYPE_NUMBER || strcmp(path, inm->path)) return;
  inm->val = atof(token->ptr);
}

static void num_mqtt(const char *msg, int msg_len, void *ud) {
  if (json_walk(msg, msg_len, num_mqtt_cb, ud) < 0)
    FNERR_RET(, CALL_FAILED(json_walk));
}

#define ITHV(temp) ((int) (temp * 2))
#define ITHT(temp) (temp / 2.0)

#define ITHT_COMFORT 22
#define ITHT_ECO 17
#define ITHT_MAX 29.5
#define ITHT_MAX_ARG 63.5
#define ITHT_MIN 5
#define ITHT_MIN_ARG -64
#define ITHT_NORMAL 21

enum itemp_ha_valve_temp {
  ITHV_COMFORT = ITHV(ITHT_COMFORT),
  ITHV_ECO = ITHV(ITHT_ECO),
  ITHV_MAX = ITHV(ITHT_MAX),
  ITHV_MIN = ITHV(ITHT_MIN),
  ITHV_NORMAL = ITHV(ITHT_NORMAL),
  ITHV_TO_MAX = ITHV(ITHT_MAX_ARG),
  ITHV_TO_MIN = ITHV(ITHT_MIN_ARG)
};

struct itemp_ha {
  struct mgos_homeassistant_object *o;  // Linked HA object
  const char *name;                     // Object name
  uint32_t src;                         // Remote control ID (3-byte)
  mgos_timer_id tmr;                    // Periodic operations

  struct {                         // HA climate control
    bool mode;                     // Boiler should be on
    float temp;                    // Target temperature
    mgos_jstore_item_hnd_t state;  // JSON store handle to persisted state
  } st;

  struct {             // Latest temperature reading
    const char *path;  // JSON path to value in temp_t MQTT msg
    float temp;        // The value
    int64_t when;      // Last update timestamp
  } last;

  struct {              // Physical valve setting
    int8_t new;         // ITHV(temperature) to set
    int8_t now;         // ITHV(temperature) set last
    mgos_timer_id tmr;  // Delayed issuing of RF commands
    int64_t when;       // Last RF command timestamp
  } valve;
};

static float ith_free_heat_C = NAN;  // Temperature of heat that may go to waste

static void ith_free_heat_mqtt(struct mg_connection *nc, const char *topic,
                               int topic_len, const char *msg, int msg_len,
                               void *ud) {
  struct num_mqtt inm = {.path = ud, .val = NAN};
  num_mqtt(msg, msg_len, &inm);
  ith_free_heat_C = inm.val;
}

#define ITH_TEMP_AGE_MAX 600
static bool ith_last_too_old(const struct itemp_ha *ith) {
  return MGOS_OLDER_THAN_S(ith->last.when, ITH_TEMP_AGE_MAX);
}

static const char *ith_st_mode_s(const struct itemp_ha *ith) {
  return ith->st.mode ? "heat" : "off";
}

#include "itemp_ha_jstore.h"
#include "itemp_ha_valve.h"

static bool ith_ha_set_temp(struct itemp_ha *ith, float temp) {
  if (temp < ITHV_MIN && temp > ITHV_MAX)
    FNERR_RETF("%.1f: temp out of range", temp);
  ith->st.temp = temp;
  return true;
}

#define ITH_STATE_FMT "{temp:%f}"
static bool ith_state_parse(struct itemp_ha *ith, const struct mg_str state) {
  float temp = NAN;
  json_scanf(state.p, state.len, ITH_STATE_FMT, &temp);
  if (!isnan(temp)) ith_ha_set_temp(ith, temp);
  return true;
}

static bool ith_state_read(struct itemp_ha *ith) {
  char *err;
  struct mg_str state;
  ith->st.state = MGOS_JSTORE_REF_TYPE_INVALID;
  if (mgos_jstore_item_get(jstore, MGOS_JSTORE_REF_BY_ID(mg_mk_str(ith->name)),
                           NULL, &state, &ith->st.state, NULL, &err))
    return ith_state_parse(ith, state);
  FNERR_JS_RETF("%s(%s): %s", "mgos_jstore_item_get", ith->name, err);
}

static bool ith_state_render(const struct itemp_ha *ith, char *buf, size_t bsz,
                             struct mg_str *state) {
  struct json_out out = JSON_OUT_BUF(buf, bsz);
  int sz = json_printf(&out, ITH_STATE_FMT, ith->st.temp);
  if (sz < 0) FNERR_RETF(CALL_FAILED(json_printf));
  if (sz >= bsz) FNERR_RETF("%s(): %s", "json_printf", "overflow");
  *state = mg_mk_str_n(buf, sz);
  return true;
}

static void ith_state_update(const struct itemp_ha *ith) {
  struct mg_str state;
  TRY_RET(, ith_state_render, ith, alloca(32), 32, &state);
  char *err;
  if (!mgos_jstore_item_edit(jstore, MGOS_JSTORE_REF_BY_HND(ith->st.state),
                             state, MGOS_JSTORE_OWN_COPY, &err))
    FNERR_JS("%s(%s): %s", "mgos_jstore_item_edit", ith->name, err);
  jstore_save_delayed();
}

static bool ith_state_write(struct itemp_ha *ith) {
  struct mg_str state;
  TRY_RETF(ith_state_render, ith, alloca(32), 32, &state);
  char *err = NULL;
  mgos_jstore_item_add(jstore, mg_mk_str(ith->name), state,
                       MGOS_JSTORE_OWN_COPY, MGOS_JSTORE_OWN_COPY,
                       &ith->st.state, NULL, &err);
  if (!err) return true;
  FNERR_JS_RETF("%s(%s): %s", "mgos_jstore_item_add", ith->name, err);
}

static bool ith_tmr_restart(struct itemp_ha *ith);

#define ITH_CMD_FMT "{temp:%f}"
static void ith_cmd(struct mgos_homeassistant_object *o, const char *arg,
                    const int arg_sz) {
  float temp;
  if (json_scanf(arg, arg_sz, ITH_CMD_FMT, &temp) != 1)
    FNERR_RET(, "%s is required, got %.*s", "temp", arg_sz, arg);

  struct itemp_ha *ith = o->user_data;
  if (ith_ha_set_temp(ith, temp)) {
    ith_state_update(ith);
    ith_valve_eval(ith);
    ith_tmr_restart(ith);
  }
}

#define ITH_STATUS_FMT_ "mode:%Q,temp:%.1f,attr:{prc:%.1f}"
static void ith_status(struct mgos_homeassistant_object *o,
                       struct json_out *out) {
  struct itemp_ha *ith = o->user_data;
  json_printf(out, ITH_STATUS_FMT_, ith_st_mode_s(ith), ith->st.temp,
              ITHT(ith->valve.new));
}

static bool ith_last_set(struct itemp_ha *ith, float temp) {
  ith->last.temp = temp;
  ith->last.when = mgos_uptime_micros();
  return ith_valve_eval(ith);
}

static void ith_last_mqtt(struct mg_connection *nc, const char *topic,
                          int topic_len, const char *msg, int msg_len,
                          void *ud) {
  struct itemp_ha *ith = ud;
  struct num_mqtt inm = {.path = ith->last.path, .val = NAN};
  num_mqtt(msg, msg_len, &inm);
  if (isnan(inm.val)) return;
  if (ith_last_set(ith, inm.val)) ith_tmr_restart(ith);
}

static void ith_tmr(void *opaque) {
  struct itemp_ha *ith = opaque;
  if (ith_last_too_old(ith))
    ith_last_set(ith, NAN);
  else if (ith_valve_too_old(ith))
    ith_valve_update(ith, true);
  mgos_homeassistant_object_send_status(ith->o);
}

static bool ith_tmr_restart(struct itemp_ha *ith) {
  TRY_RETF(MGOS_TMR_RESET, ith->tmr,
           mgos_sys_config_get_itemp_ha_period() * 1000,
           MGOS_TIMER_REPEAT | MGOS_TIMER_RUN_NOW, ith_tmr, ith);
  return true;
}

#define OBJ_ARG_FMT "{name:%Q,src:%H,temp_path:%Q,temp_t:%Q,temp_tpl:%Q}"
#define OBJ_ARG_TEMP_PATH_DFLT ".temperature"
#define OBJ_ARG_TEMP_TPL_DFLT "{{value_json.temperature}}"
#define OBJ_CMD(cmd)                                          \
  OBJ_T_TPL_FMT, #cmd, "_cmd_t", OBJ_CMD_T, #cmd, "_cmd_tpl", \
      OBJ_CMD_TPL_BEGIN, #cmd, OBJ_CMD_TPL_END
#define OBJ_CMD_T "~/cmd"
#define OBJ_CMD_TPL_BEGIN "{{\{'"
#define OBJ_CMD_TPL_END "':value}|to_json}}"
#define OBJ_JSON_FMT                                             \
  "ic:%Q,temp_unit:%Q,temp_step:0.5,min_temp:%.1f,max_temp:%.1f" \
  ",curr_temp_t:%Q,curr_temp_tpl:%Q,modes:[heat,off]"            \
  ",json_attr_t:%Q,json_attr_tpl:\"%s%s%s\""
#define OBJ_JSON_TPL_MID "attr|tojson"
#define OBJ_STAT(stat)                                             \
  OBJ_T_TPL_FMT, #stat, "_stat_t", OBJ_STAT_T, #stat, "_stat_tpl", \
      OBJ_STAT_TPL_BEGIN, #stat, OBJ_STAT_TPL_END
#define OBJ_STAT_T "~"
#define OBJ_STAT_TPL_BEGIN "{{value_json."
#define OBJ_STAT_TPL_END "}}"
#define OBJ_T_TPL_FMT ",\"%s%s\":%Q,\"%s%s\":\"%s%s%s\""
static bool ith_obj_fromjson(struct mgos_homeassistant *ha,
                             struct json_token v) {
  struct itemp_ha *ith = NULL;
  bool ok = false;
  char *name = NULL, *src_s = NULL, *temp_path = NULL, *temp_t = NULL,
       *temp_tpl = NULL;
  int src_sz;
  json_scanf(v.ptr, v.len, OBJ_ARG_FMT, &name, &src_sz, &src_s, &temp_path,
             &temp_t, &temp_tpl);
  if (!src_s) FNERR_GT("%s is required", "src");
  if (src_sz != 3) FNERR_GT("src must be 3 bytes long");
  if (!temp_t) FNERR_GT("%s is required", "temp_t");

  ith = TRY_MALLOC_OR(goto err, ith);
  ith->src = src_s[0] << 16 | src_s[1] << 8 | src_s[2];
  if (!name && asprintf(&name, "itemp_%06X", ith->src) < 0)
    FNERR_GT("error generating %s", "name");
  ith->name = name;

  ith->st.temp = ITHT_NORMAL;
  if (!ith_state_read(ith)) TRY_GT(ith_state_write, ith);

  ith->last.path = temp_path ?: OBJ_ARG_TEMP_PATH_DFLT;
  ith->last.temp = NAN;
  ith->last.when = 0;

  const size_t json_sz = 1024;
  char *json = alloca(json_sz);
  struct json_out jout = JSON_OUT_BUF(json, json_sz);
  json_printf(&jout, OBJ_JSON_FMT, "mdi:radiator", "C", (float) ITHT_MIN,
              (float) ITHT_MAX, temp_t, temp_tpl ?: OBJ_ARG_TEMP_TPL_DFLT,
              OBJ_STAT_T, OBJ_STAT_TPL_BEGIN, OBJ_JSON_TPL_MID,
              OBJ_STAT_TPL_END);
  json_printf(&jout, OBJ_STAT(mode));
  json_printf(&jout, OBJ_CMD(temp));
  json_printf(&jout, OBJ_STAT(temp));

  ith->o = mgos_homeassistant_object_add(ha, name, COMPONENT_CLIMATE, json,
                                         ith_status, ith);
  if (!ith->o) FNERR_GT("%s() failed", "mgos_homeassistant_object_add");
  TRY_GT(mgos_homeassistant_object_add_cmd_cb, ith->o, NULL, ith_cmd);
  ith->o->config_sent = false;
  TRY_GT(ith_tmr_restart, ith);
  mgos_mqtt_sub(temp_t, ith_last_mqtt, ith);
  ith->valve.now = ITHV_MAX;
  ith->valve.when = 0;
  ith_valve_eval(ith);
  ok = true;

err:
  if (!ok && ith && ith->o) mgos_homeassistant_object_remove(&ith->o);
  if (!ok && ith) free(ith);
  if (!ok && name) free(name);
  if (src_s) free(src_s);
  if (!ok && temp_path) free(temp_path);
  if (temp_t) free(temp_t);
  if (temp_tpl) free(temp_tpl);
  return ok;
}

bool mgos_itemp_ha_init(void) {
  const struct mgos_config_itemp_ha *cfg = mgos_sys_config_get_itemp_ha();
  if (!cfg->enable) return true;
  TRY_RETF(jstore_open);
  TRY_RETF(mgos_homeassistant_register_provider, "itemp", ith_obj_fromjson,
           NULL);
  const struct mgos_config_itemp_ha_free_heat *ft = &cfg->free_heat;
  if (ft->t && *ft->t && ft->tpl)
    mgos_mqtt_sub(ft->t, ith_free_heat_mqtt, (void *) cfg->free_heat.tpl);
  return true;
}
