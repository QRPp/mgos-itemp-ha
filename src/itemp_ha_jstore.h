static struct mgos_jstore *jstore = NULL;
static mgos_timer_id jstore_tmr_id = MGOS_INVALID_TIMER_ID;

#define FNERR_JS(...) FNERR_AND(free(err), ##__VA_ARGS__)
#define FNERR_JS_RETF(...) FNERR_AND(free(err); return false, ##__VA_ARGS__)

static bool jstore_open() {
  if (jstore) return true;
  const char *ihsf = mgos_sys_config_get_itemp_ha_state();
  char *err;
  jstore = mgos_jstore_create(ihsf, &err);
  if (jstore) return true;
  FNERR_JS_RETF("%s(%s): %s", "mgos_jstore_create", ihsf, err);
}

static void jstore_save(void *opaque) {
  jstore_tmr_id = MGOS_INVALID_TIMER_ID;
  char *err;
  const char *ihsf = mgos_sys_config_get_itemp_ha_state();
  if (!mgos_jstore_save(jstore, ihsf, &err))
    FNERR_JS("%s(%s): %s", "mgos_jstore_save", ihsf, err);
}

static void jstore_save_delayed() {
  if (jstore_tmr_id) mgos_clear_timer(jstore_tmr_id);
  jstore_tmr_id = mgos_set_timer(mgos_sys_config_get_itemp_ha_delay() * 1000, 0,
                                 jstore_save, NULL);
  if (!jstore_tmr_id) FNERR(CALL_FAILED(mgos_set_timer));
}
