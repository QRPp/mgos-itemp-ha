static struct mgos_jstore *jstore = NULL;
static mgos_timer_id jstore_tmr_id = MGOS_INVALID_TIMER_ID;

#define FNERR_JS(...) FNERR_AND(free(err), ##__VA_ARGS__)
#define FNERR_JS_RETF(...) FNERR_AND(free(err); return false, ##__VA_ARGS__)

static bool jstore_open() {
  if (jstore) return true;
  char *err;
  jstore = mgos_jstore_create(cfg->state, &err);
  if (jstore) return true;
  FNERR_JS_RETF("%s(%s): %s", "mgos_jstore_create", cfg->state, err);
}

static void jstore_save(void *opaque) {
  jstore_tmr_id = MGOS_INVALID_TIMER_ID;
  char *err;
  if (!mgos_jstore_save(jstore, cfg->state, &err))
    FNERR_JS("%s(%s): %s", "mgos_jstore_save", cfg->state, err);
}

static void jstore_save_delayed() {
  if (jstore_tmr_id) mgos_clear_timer(jstore_tmr_id);
  jstore_tmr_id = mgos_set_timer(cfg->delay * 1000, 0, jstore_save, NULL);
  if (!jstore_tmr_id) FNERR(CALL_FAILED(mgos_set_timer));
}
