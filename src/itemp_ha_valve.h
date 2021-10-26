static void ith_valve_update(struct itemp_ha *ith, bool force);

static void ith_valve_cmd_cb(void *opaque) {
  struct mgos_cc1101_tx_op *op = opaque;
  if (op->err) {
    struct itemp_ha *ith = op->req.opaque;
    FNERR("%s: RF TX err %d, retrying", ith->name, op->err);
    ith_valve_update(ith, true);
  }
  free(op);
}

#define TRY_CMD_GT(cmd, arg) \
  TRY_GT(mgos_itemp_send_cmd, ith->src, cmd, arg, 700000, ith_valve_cmd_cb, ith)
static void ith_valve_cmd(void *opaque) {
  struct itemp_ha *ith = opaque;
  uint8_t temp = ith->valve.new;
  ith->valve.tmr = MGOS_INVALID_TIMER_ID;
  if (temp <= (ITHV_MIN + ITHV_ECO) / 2) {
    TRY_CMD_GT(ITCMD_ADJUST, ITHV_TO_MIN);
    if (temp > ITHV_MIN) TRY_CMD_GT(ITCMD_ADJUST, temp - ITHV_MIN);
  } else if (temp <= (ITHV_ECO + ITHV_COMFORT) / 2) {
    TRY_CMD_GT(ITCMD_SETBACK, 0);
    if (temp != ITHV_ECO) TRY_CMD_GT(ITCMD_ADJUST, temp - ITHV_ECO);
  } else if (temp <= (ITHV_COMFORT + ITHV_MAX) / 2) {
    TRY_CMD_GT(ITCMD_COMFORT, 0);
    if (temp != ITHV_COMFORT) TRY_CMD_GT(ITCMD_ADJUST, temp - ITHV_COMFORT);
  } else {
    TRY_CMD_GT(ITCMD_ADJUST, ITHV_TO_MAX);
    if (temp < ITHV_MAX) TRY_CMD_GT(ITCMD_ADJUST, temp - ITHV_MAX);
  }
  ith->valve.now = temp;
  ith->valve.when = mgos_uptime_micros();
  return;
err:
  ith_valve_update(ith, true);
}

static bool ith_valve_set(struct itemp_ha *ith, bool heat, uint8_t vt) {
  bool ret = heat != ith->st.mode || vt != ith->valve.new;
  ith->st.mode = heat;
  ith->valve.new = vt;
  return ret;
}

static bool ith_valve_off(struct itemp_ha *ith) {
  return ith_valve_set(ith, false, ITHV_MIN);
}

static bool ith_valve_on(struct itemp_ha *ith, bool heat) {
  return ith_valve_set(ith, heat, ITHV(ith->st.temp));
}

#define ITH_VALVE_UPD_MAX 3600
static bool ith_valve_too_old(const struct itemp_ha *ith) {
  return MGOS_OLDER_THAN_S(ith->valve.when, ITH_VALVE_UPD_MAX);
}

static void ith_valve_update(struct itemp_ha *ith, bool force) {
  MGOS_TMR_STOP(ith->valve.tmr);
  if (!force && ith->valve.new == ith->valve.now) return;
  TRY_RET(, MGOS_TMR_SET, ith->valve.tmr,
          mgos_sys_config_get_itemp_ha_delay() * 1000, 0, ith_valve_cmd, ith);
}

#define ITH_TEMP_HYST 0.5
static bool ith_valve_eval(struct itemp_ha *ith) {
  bool ret;
  if (isnan(ith->last.temp))         // Temp stale
    ret = ith_valve_on(ith, false);  // Set target temp, need no heat
  else {
    float diff = ith->last.temp - ith->st.temp;
    float thr = ith->st.mode ? ITH_TEMP_HYST    // Heat till hyst above,
                             : -ITH_TEMP_HYST;  // delay heat till hyst below
    ret = diff > thr ? ith_valve_off(ith) : ith_valve_on(ith, true);
  }
  ith_valve_update(ith, false);
  return ret;
}
