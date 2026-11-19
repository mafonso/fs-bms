// Felicity Solar LiFePO4 BMS over BLE: polls "get dev real infor" every N
// seconds and publishes it as Prometheus /metrics, /api/bms, the 'bms'
// WebSocket topic and the `bms` console command. Protocol: bms_proto.c.
//
// One BLE session per poll: (scan once) -> connect -> discover -> subscribe ->
// write command -> reassemble JSON -> disconnect. Dropping the link between
// polls lets the FSolar app connect in the gaps (the BMS takes one central at
// a time) and makes reconnecting after a drop the normal path, not a special one.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "app_events.h"
#include "auth.h"
#include "settings.h"
#include "web.h"
#include "features.h"
#include "bms.h"

static const char *TAG = "bms";

void ble_store_config_init(void);   // NimBLE provides it but no header declares it

#define SCAN_MS        15000
#define CONNECT_MS     10000
#define OP_MS          5000
#define FRAME_MS       8000
#define PAIR_MS        15000
#define RAW_LOG_POLLS  3        // log the raw JSON of the first polls after boot at INFO
#define CHUNK_MAX      512
#define HISTORY_LEN    240      // good samples kept for the web charts (2 h at 30 s)

// Event bits set by NimBLE callbacks (host task), awaited by the bms task.
#define EV_SYNC       BIT0
#define EV_SCAN_DONE  BIT1
#define EV_CONNECT    BIT2
#define EV_DISCONNECT BIT3
#define EV_OP         BIT4
#define EV_FRAME      BIT5
#define EV_ENC        BIT6

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_lock;     // s_st, s_frame, s_msg/s_msg_raw
static TaskHandle_t s_task;

// ---- BLE link: written by callbacks, read by the bms task after an event bit ----
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile int s_op_rc, s_connect_rc, s_enc_rc;
static ble_addr_t s_pin;            // configured address (s_pinned)
static bool s_pinned;
static ble_addr_t s_peer;           // last BMS found; reused until a connect fails
static bool s_have_peer;
static bool s_found;
static char s_found_name[32];
static uint16_t s_svc_start, s_svc_end, s_tx_handle, s_rx_handle, s_rx_end, s_cccd;
static uint8_t s_tx_props;
static bool s_svc_found;
static uint16_t s_mtu;
static ble_uuid_any_t s_svc_uuid, s_tx_uuid, s_rx_uuid;
static bms_frame_t *s_frame;
// Chart history, one entry per good sample, under s_lock; ring in PSRAM.
typedef struct {
    float soc, power, delta_mv;
} hist_t;
static hist_t *s_hist;
static size_t s_hist_head, s_hist_count;

static cJSON *s_msg;                // last complete frame, handed to the bms task
static char *s_msg_raw;

// ---- Published state, under s_lock ----
static struct {
    const char *state;              // idle | scanning | connecting | reading | paused
    bool have_sample;
    bms_sample_t sample;
    int64_t sample_us;
    int rssi;
    bool have_info;
    bms_info_t info;
    char name[32];
    char addr[18];
    bool last_ok;
    uint32_t polls_ok, polls_err;
    char last_error[80];
    char *raw_real, *raw_basic;
    int64_t paused_until_us;
    uint32_t raw_logged;
    bool addr_pending;              // set_addr() ran; bms_task applies new_pin between sessions
    bool new_pinned;
    ble_addr_t new_pin;
} s_st = {.state = "idle"};

// ---------------------------------------------------------------------------
// Helpers

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void set_state(const char *st)
{
    lock();
    s_st.state = st;
    unlock();
}

static void addr_str(const ble_addr_t *a, char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2],
             a->val[1], a->val[0]);
}

static bool parse_addr(const char *s, ble_addr_t *a)
{
    unsigned b[6];
    char tail;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) {
            return false;
        }
        a->val[5 - i] = (uint8_t)b[i];
    }
    a->type = BLE_ADDR_PUBLIC;      // only val is compared; the scan tells us the real type
    return true;
}

static bool name_matches(const char *name, size_t len)
{
    return len >= 3 && (!strncmp(name, "F07", 3) || !strncmp(name, "F10", 3));
}

static uint32_t age_s(int64_t since_us)
{
    return (uint32_t)((esp_timer_get_time() - since_us) / 1000000);
}

// ---------------------------------------------------------------------------
// NimBLE callbacks (host task). Keep short; hand results over via s_ev.

static void on_chunk(const uint8_t *data, size_t len)
{
    cJSON *msg;
    lock();
    bms_frame_result_t r = bms_frame_feed(s_frame, data, len, &msg);
    if (r == BMS_FRAME_DONE) {
        cJSON_Delete(s_msg);
        free(s_msg_raw);
        s_msg = msg;
        s_msg_raw = strdup(s_frame->buf);
    }
    unlock();
    ESP_LOGD(TAG, "rx %u bytes: %.*s", (unsigned)len, (int)len, (const char *)data);
    if (r == BMS_FRAME_DONE) {
        xEventGroupSetBits(s_ev, EV_FRAME);
    } else if (r == BMS_FRAME_BAD_VER) {
        ESP_LOGW(TAG, "reply with CommVer != 1 ignored");
    } else if (r == BMS_FRAME_OVERFLOW) {
        ESP_LOGW(TAG, "reply longer than %d bytes dropped", BMS_FRAME_MAX);
    }
}

static void on_disc(const struct ble_gap_disc_desc *d)
{
    if (s_found) {
        return;
    }
    struct ble_hs_adv_fields f;
    char name[32] = "";
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 && f.name) {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, n);
        name[n] = '\0';
    }
    bool match = s_pinned ? !memcmp(d->addr.val, s_pin.val, 6) : name_matches(name, strlen(name));
    if (!match) {
        return;
    }
    s_found = true;
    s_peer = d->addr;
    snprintf(s_found_name, sizeof(s_found_name), "%s", name);
    char a[18];
    addr_str(&d->addr, a);
    ESP_LOGI(TAG, "found '%s' %s (type %u) %d dBm", name, a, d->addr.type, d->rssi);
    ble_gap_disc_cancel();          // cancelling reports no DISC_COMPLETE
    xEventGroupSetBits(s_ev, EV_SCAN_DONE);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        on_disc(&ev->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xEventGroupSetBits(s_ev, EV_SCAN_DONE);
        break;
    case BLE_GAP_EVENT_CONNECT:
        s_connect_rc = ev->connect.status;
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
        }
        xEventGroupSetBits(s_ev, EV_CONNECT);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupSetBits(s_ev, EV_DISCONNECT);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.attr_handle == s_rx_handle && ev->notify_rx.om) {
            static uint8_t buf[CHUNK_MAX];
            uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
            if (len > sizeof(buf)) {
                len = sizeof(buf);
            }
            if (os_mbuf_copydata(ev->notify_rx.om, 0, len, buf) == 0) {
                on_chunk(buf, len);
            }
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_enc_rc = ev->enc_change.status;
        xEventGroupSetBits(s_ev, EV_ENC);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        // MITM pairing. We are KeyboardOnly: type the passkey from `bms pin`.
        struct ble_sm_io io = {.action = ev->passkey.params.action};
        uint32_t pin;
        bool have_pin = settings_get_bms_pin(&pin) == ESP_OK;
        if (io.action == BLE_SM_IOACT_INPUT || io.action == BLE_SM_IOACT_DISP) {
            if (!have_pin) {
                ESP_LOGE(TAG, "BMS wants a passkey: set it with `bms pin <passkey>`");
                ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
                break;
            }
            io.passkey = pin;
        } else if (io.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(TAG, "numeric comparison %06" PRIu32 ": accepting", ev->passkey.params.numcmp);
            io.numcmp_accept = 1;
        } else {
            ESP_LOGW(TAG, "unsupported pairing action %d", io.action);
            break;
        }
        ESP_LOGI(TAG, "pairing: answering action %d", io.action);
        int rc = ble_sm_inject_io(ev->passkey.conn_handle, &io);
        if (rc != 0) {
            ESP_LOGW(TAG, "passkey inject failed (%d)", rc);
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The BMS lost our bond (reset, or re-paired with someone else): forget ours and pair again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGI(TAG, "BMS asked to pair again; old bond deleted");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void op_done(int status)
{
    s_op_rc = status;
    xEventGroupSetBits(s_ev, EV_OP);
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status == 0) {
        s_mtu = mtu;
    }
    op_done(err->status);
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        s_svc_found = true;
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
    } else {
        op_done(err->status == BLE_HS_EDONE ? (s_svc_found ? 0 : BLE_HS_ENOENT) : err->status);
    }
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status != 0) {
        op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
        return 0;
    }
    // Characteristics come in handle order: the one after RX bounds RX's descriptors.
    if (s_rx_handle && !s_rx_end && chr->def_handle > s_rx_handle) {
        s_rx_end = chr->def_handle - 1;
    }
    if (ble_uuid_cmp(&chr->uuid.u, &s_tx_uuid.u) == 0) {
        s_tx_handle = chr->val_handle;
        s_tx_props = chr->properties;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_rx_uuid.u) == 0) {
        s_rx_handle = chr->val_handle;
    }
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status != 0) {
        op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
        return 0;
    }
    if (!s_cccd && dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_cccd = dsc->handle;
    }
    return 0;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    op_done(err->status);
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "no BLE identity address (%d)", rc);
        return;
    }
    xEventGroupSetBits(s_ev, EV_SYNC);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (%d)", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Session (bms task)

// Wait for the GATT op started by `rc == 0` to finish. Use via OP(), which
// clears EV_OP before the call starts.
static int wait_op(int rc, uint32_t ms)
{
    if (rc != 0) {
        return rc;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_OP | EV_DISCONNECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(ms));
    if (b & EV_OP) {
        xEventGroupClearBits(s_ev, EV_OP);
        return s_op_rc;
    }
    return (b & EV_DISCONNECT) ? BLE_HS_ENOTCONN : BLE_HS_ETIMEOUT;
}
#define OP(call, ms) (xEventGroupClearBits(s_ev, EV_OP), wait_op((call), (ms)))

static bool scan(void)
{
    struct ble_gap_disc_params p = {
        .passive = 0,               // active: the name may only be in the scan response
        .filter_duplicates = 1,
    };
    s_found = false;
    xEventGroupClearBits(s_ev, EV_SCAN_DONE);
    int rc = ble_gap_disc(s_own_addr_type, SCAN_MS, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan start failed (%d)", rc);
        return false;
    }
    if (!(xEventGroupWaitBits(s_ev, EV_SCAN_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(SCAN_MS + 2000)) &
          EV_SCAN_DONE)) {
        ble_gap_disc_cancel();
    }
    if (s_found) {
        s_have_peer = true;
        lock();
        if (s_found_name[0]) {
            snprintf(s_st.name, sizeof(s_st.name), "%s", s_found_name);
        }
        addr_str(&s_peer, s_st.addr);
        unlock();
    }
    return s_found;
}

// Send one command and wait for its reply. Returns the parsed reply (caller
// frees) and stores a copy of the raw JSON in *raw (caller frees).
static cJSON *request(const char *cmd, char **raw, char *err, size_t err_len)
{
    lock();
    bms_frame_reset(s_frame);
    cJSON_Delete(s_msg);
    free(s_msg_raw);
    s_msg = NULL;
    s_msg_raw = NULL;
    unlock();
    xEventGroupClearBits(s_ev, EV_FRAME);

    uint16_t len = strlen(cmd);
    uint16_t room = s_mtu - 3;
    int rc;
    // With response when the characteristic allows it (the BMS's TX is write-without-response).
    if (s_tx_props & BLE_GATT_CHR_PROP_WRITE) {
        if (len <= room) {
            rc = OP(ble_gattc_write_flat(s_conn, s_tx_handle, cmd, len, write_cb, NULL), OP_MS);
        } else {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(cmd, len);
            rc = om ? OP(ble_gattc_write_long(s_conn, s_tx_handle, 0, om, write_cb, NULL), OP_MS)
                    : BLE_HS_ENOMEM;
        }
    } else if (len <= room) {
        rc = ble_gattc_write_no_rsp_flat(s_conn, s_tx_handle, cmd, len);
    } else {
        rc = BLE_HS_EMSGSIZE;
    }
    if (rc != 0) {
        snprintf(err, err_len, "write failed (%d, MTU %u)", rc, s_mtu);
        return NULL;
    }

    EventBits_t b = xEventGroupWaitBits(s_ev, EV_FRAME | EV_DISCONNECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(FRAME_MS));
    lock();
    cJSON *msg = s_msg;
    *raw = s_msg_raw;
    s_msg = NULL;
    s_msg_raw = NULL;
    size_t partial = s_frame->len;
    unlock();
    if (!msg) {
        free(*raw);
        *raw = NULL;
        snprintf(err, err_len, "%s (got %u bytes)", (b & EV_DISCONNECT) ? "disconnected" : "no reply",
                 (unsigned)partial);
    }
    return msg;
}

static bool session(char *err, size_t err_len)
{
    if (!s_have_peer) {
        set_state("scanning");
        if (!scan()) {
            snprintf(err, err_len, "no BMS found (%s)", s_pinned ? "pinned address" : "name F07*/F10*");
            return false;
        }
    }

    set_state("connecting");
    xEventGroupClearBits(s_ev, EV_CONNECT | EV_DISCONNECT | EV_OP | EV_FRAME | EV_ENC);
    s_connect_rc = -1;
    int rc = ble_gap_connect(s_own_addr_type, &s_peer, CONNECT_MS, NULL, gap_event, NULL);
    if (rc == 0) {
        xEventGroupWaitBits(s_ev, EV_CONNECT, pdTRUE, pdFALSE, pdMS_TO_TICKS(CONNECT_MS + 2000));
        rc = s_connect_rc;
    }
    if (rc != 0 || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        if (rc == -1) {
            ble_gap_conn_cancel();
        }
        s_have_peer = false;        // rescan next time: it may have moved to another address
        snprintf(err, err_len, "connect failed (%d)", rc);
        return false;
    }

    set_state("reading");
    bool ok = false;
    // The BMS sends a Security Request (AuthReq 0x0D: bonding, MITM, Secure
    // Connections) and refuses the CCCD write (ATT 0x05) until the link is
    // encrypted with an authenticated key, so pair/encrypt first. NimBLE may
    // already be answering its request, hence EALREADY.
    rc = ble_gap_security_initiate(s_conn);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        EventBits_t b = xEventGroupWaitBits(s_ev, EV_ENC | EV_DISCONNECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(PAIR_MS));
        rc = (b & EV_ENC) ? s_enc_rc : (b & EV_DISCONNECT) ? BLE_HS_ENOTCONN : BLE_HS_ETIMEOUT;
    }
    if (rc != 0) {
        uint32_t pin;
        bool have_pin = settings_get_bms_pin(&pin) == ESP_OK;
        // SMP errors from the peer are BLE_HS_ERR_SM_PEER_BASE (0x500) + reason.
        const char *why = !have_pin ? "passkey not set (bms pin <passkey>)"
                        : rc == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_CONFIRM_MISMATCH) ? "wrong passkey"
                        : rc == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_AUTHREQ) ? "BMS rejected our pairing parameters"
                        : rc == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_REPEATED) ? "BMS is refusing repeated pairing attempts; wait"
                        : "pairing/encryption failed";
        snprintf(err, err_len, "%s (%d)", why, rc);
        goto out;
    }
    // The pack usually starts the MTU exchange itself (247); ours then fails,
    // which is fine. Ask the stack for the MTU actually in force.
    if ((rc = OP(ble_gattc_exchange_mtu(s_conn, mtu_cb, NULL), OP_MS)) != 0) {
        ESP_LOGD(TAG, "MTU exchange: %d", rc);
    }
    s_mtu = ble_att_mtu(s_conn);
    if (s_mtu < 23) {
        s_mtu = 23;
    }

    s_svc_found = false;
    s_tx_handle = s_rx_handle = s_rx_end = s_cccd = 0;
    s_tx_props = 0;
    if ((rc = OP(ble_gattc_disc_svc_by_uuid(s_conn, &s_svc_uuid.u, svc_cb, NULL), OP_MS)) != 0) {
        snprintf(err, err_len, "service not found (%d)", rc);
        goto out;
    }
    if ((rc = OP(ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, chr_cb, NULL), OP_MS)) != 0 ||
        !s_tx_handle || !s_rx_handle) {
        snprintf(err, err_len, "characteristics not found (%d, tx %u rx %u)", rc, s_tx_handle, s_rx_handle);
        goto out;
    }
    if (!s_rx_end) {
        s_rx_end = s_svc_end;
    }
    if (s_rx_end > s_rx_handle &&
        (rc = OP(ble_gattc_disc_all_dscs(s_conn, s_rx_handle, s_rx_end, dsc_cb, NULL), OP_MS)) != 0) {
        snprintf(err, err_len, "descriptor discovery failed (%d)", rc);
        goto out;
    }
    if (!s_cccd) {
        snprintf(err, err_len, "RX has no CCCD");
        goto out;
    }
    static const uint8_t notify_on[2] = {1, 0};
    if ((rc = OP(ble_gattc_write_flat(s_conn, s_cccd, notify_on, 2, write_cb, NULL), OP_MS)) != 0) {
        snprintf(err, err_len, "subscribe failed (%d)", rc);
        goto out;
    }
    ESP_LOGI(TAG, "MTU %u, tx 0x%04x (props 0x%02x), rx 0x%04x, cccd 0x%04x", s_mtu, s_tx_handle,
             s_tx_props, s_rx_handle, s_cccd);

    lock();
    bool want_info = !s_st.have_info;
    unlock();
    char *raw = NULL;
    if (want_info) {
        char e[64];
        cJSON *msg = request(BMS_CMD_BASIC, &raw, e, sizeof(e));
        if (msg) {
            bms_info_t info;
            bms_decode_basic(msg, &info);
            ESP_LOGI(TAG, "device info: %s", raw);
            ESP_LOGI(TAG, "serial %s, model %s, sw %s, fw %s", info.serial, info.model, info.sw, info.fw);
            lock();
            s_st.info = info;
            s_st.have_info = true;
            free(s_st.raw_basic);
            s_st.raw_basic = raw;
            unlock();
            cJSON_Delete(msg);
        } else {
            ESP_LOGW(TAG, "device info: %s", e);   // not fatal; retried next poll
        }
    }

    cJSON *msg = request(BMS_CMD_REAL, &raw, err, err_len);
    if (!msg) {
        goto out;
    }
    bms_sample_t s;
    char derr[64];
    bool decoded = bms_decode_real(msg, &s, derr, sizeof(derr));
    cJSON_Delete(msg);

    int8_t rssi = 0;
    ble_gap_conn_rssi(s_conn, &rssi);
    lock();
    bool log_raw = s_st.raw_logged < RAW_LOG_POLLS || !decoded;
    s_st.raw_logged++;
    free(s_st.raw_real);
    s_st.raw_real = raw;            // kept even if it didn't decode, for `bms raw`
    if (decoded) {
        if (s_hist) {
            s_hist[s_hist_head] = (hist_t){s.soc, s.voltage * s.current, s.cell_delta * 1000.0f};
            s_hist_head = (s_hist_head + 1) % HISTORY_LEN;
            if (s_hist_count < HISTORY_LEN) {
                s_hist_count++;
            }
        }
        s_st.sample = s;
        s_st.sample_us = esp_timer_get_time();
        s_st.have_sample = true;
        s_st.rssi = rssi;
    }
    unlock();
    if (log_raw) {
        // Serial shows the whole line; the web log viewer cuts lines at 200 chars (see `bms raw`).
        ESP_LOGI(TAG, "raw: %s", raw);
    }
    if (!decoded) {
        snprintf(err, err_len, "decode: %s", derr);
        goto out;
    }
    ok = true;

out:
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        xEventGroupWaitBits(s_ev, EV_DISCONNECT, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    }
    return ok;
}

static void log_sample(const bms_sample_t *s)
{
    char cells[BMS_MAX_CELLS * 7 + 1] = "", temps[BMS_MAX_TEMPS * 8 + 1] = "";
    size_t n = 0;
    for (int i = 0; i < s->n_cells; i++) {
        n += snprintf(cells + n, sizeof(cells) - n, "%s%.3f", i ? " " : "", s->cells[i]);
    }
    n = 0;
    for (int i = 0; i < s->n_temps; i++) {
        n += snprintf(temps + n, sizeof(temps) - n, "%s%.1f", i ? " " : "", s->temps[i]);
    }
    ESP_LOGI(TAG, "%.3f V %.1f A SoC %.2f %% %.1f/%.1f Ah warn 0x%" PRIx32 " fault 0x%" PRIx32,
             s->voltage, s->current, s->soc, s->remaining_ah, s->capacity_ah, s->warn, s->fault);
    ESP_LOGI(TAG, "%d cells, delta %.3f V (#%d %.3f .. #%d %.3f)", s->n_cells, s->cell_delta,
             s->cell_min_idx + 1, s->cell_min, s->cell_max_idx + 1, s->cell_max);
    ESP_LOGI(TAG, "cells V: %s", cells);
    ESP_LOGI(TAG, "temps C: %s", temps);
}

static cJSON *status_json(void);

static void poll_once(void)
{
    char err[80] = "";
    bool ok = session(err, sizeof(err));
    lock();
    s_st.last_ok = ok;
    if (ok) {
        s_st.polls_ok++;
        s_st.last_error[0] = '\0';
    } else {
        s_st.polls_err++;
        snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", err);
    }
    bms_sample_t s = s_st.sample;
    s_st.state = "idle";
    unlock();

    if (ok) {
        log_sample(&s);
    } else {
        ESP_LOGW(TAG, "poll failed: %s", err);
    }
    app_bms_event_t ev = {.ok = ok};
    app_event_post(APP_EVENT_BMS_POLLED, &ev, sizeof(ev));
    if (web_ws_client_count() > 0) {
        web_ws_broadcast("bms", status_json());
    }
}

static void bms_task(void *arg)
{
    xEventGroupWaitBits(s_ev, EV_SYNC, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "BLE ready; polling every %" PRIu32 " s", settings_get_bms_interval());
    for (;;) {
        int64_t start = esp_timer_get_time();
        lock();
        int64_t paused = s_st.paused_until_us - start;
        s_st.state = paused > 0 ? "paused" : "idle";
        if (s_st.addr_pending) {
            // Between sessions, so no scan callback is reading these.
            s_st.addr_pending = false;
            s_pinned = s_st.new_pinned;
            s_pin = s_st.new_pin;
            s_have_peer = false;
            s_st.have_info = false;
            s_st.name[0] = s_st.addr[0] = '\0';
        }
        unlock();
        if (paused <= 0) {
            poll_once();
        }
        // Keep the cadence from the start of the poll; `bms poll` wakes us early.
        int64_t wait_ms = (int64_t)settings_get_bms_interval() * 1000 - (esp_timer_get_time() - start) / 1000;
        if (paused > 0) {
            wait_ms = paused / 1000 + 1;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms > 100 ? wait_ms : 100));
    }
}

// ---------------------------------------------------------------------------
// Published views

bool bms_get_sample(bms_sample_t *out, uint32_t *age)
{
    lock();
    bool have = s_st.have_sample;
    if (have) {
        *out = s_st.sample;
        if (age) {
            *age = age_s(s_st.sample_us);
        }
    }
    unlock();
    return have;
}

static cJSON *status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    lock();
    cJSON_AddStringToObject(o, "state", s_st.state);
    cJSON_AddBoolToObject(o, "ok", s_st.last_ok);
    cJSON_AddNumberToObject(o, "polls_ok", s_st.polls_ok);
    cJSON_AddNumberToObject(o, "polls_err", s_st.polls_err);
    cJSON_AddStringToObject(o, "error", s_st.last_error);
    cJSON_AddNumberToObject(o, "interval_s", settings_get_bms_interval());
    int64_t paused = s_st.paused_until_us - esp_timer_get_time();
    cJSON_AddNumberToObject(o, "paused_s", paused > 0 ? (double)(paused / 1000000) : 0);
    cJSON_AddStringToObject(o, "name", s_st.name);
    cJSON_AddStringToObject(o, "addr", s_st.addr);
    uint32_t pin;
    cJSON_AddBoolToObject(o, "pin_set", settings_get_bms_pin(&pin) == ESP_OK);
    char pinned[SETTINGS_BMS_ADDR_MAX];
    settings_get_bms_addr(pinned);
    cJSON_AddStringToObject(o, "pinned_addr", pinned);
    if (s_st.have_info) {
        cJSON *i = cJSON_AddObjectToObject(o, "info");
        cJSON_AddStringToObject(i, "serial", s_st.info.serial);
        cJSON_AddStringToObject(i, "model", s_st.info.model);
        cJSON_AddStringToObject(i, "sw", s_st.info.sw);
        cJSON_AddStringToObject(i, "fw", s_st.info.fw);
    }
    if (s_st.have_sample) {
        const bms_sample_t *s = &s_st.sample;
        cJSON *d = cJSON_AddObjectToObject(o, "sample");
        cJSON_AddNumberToObject(d, "age_s", age_s(s_st.sample_us));
        cJSON_AddNumberToObject(d, "rssi", s_st.rssi);
        cJSON_AddNumberToObject(d, "voltage", s->voltage);
        cJSON_AddNumberToObject(d, "current", s->current);
        cJSON_AddNumberToObject(d, "power", s->voltage * s->current);
        cJSON_AddNumberToObject(d, "soc", s->soc);
        cJSON_AddNumberToObject(d, "remaining_ah", s->remaining_ah);
        cJSON_AddNumberToObject(d, "capacity_ah", s->capacity_ah);
        cJSON_AddItemToObject(d, "cells", cJSON_CreateFloatArray(s->cells, s->n_cells));
        cJSON_AddNumberToObject(d, "cell_min", s->cell_min);
        cJSON_AddNumberToObject(d, "cell_max", s->cell_max);
        cJSON_AddNumberToObject(d, "cell_delta", s->cell_delta);
        cJSON_AddItemToObject(d, "temps", cJSON_CreateFloatArray(s->temps, s->n_temps));
        cJSON_AddNumberToObject(d, "warn", s->warn);
        cJSON_AddNumberToObject(d, "fault", s->fault);
    }
    unlock();
    return o;
}

// Prometheus text exposition. Values are left out once the sample is older
// than 3 poll intervals (min 3 min), so graphs show a gap instead of a flat line.
typedef struct {
    char *buf;
    size_t len, cap;
} sbuf_t;

static void out(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(sbuf_t *b, const char *fmt, ...)
{
    if (b->len >= b->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len = (n > 0 && b->len + n < b->cap) ? b->len + n : b->cap;
}

static void gauge(sbuf_t *b, const char *name, const char *help, double v)
{
    out(b, "# HELP %s %s\n# TYPE %s gauge\n%s %g\n", name, help, name, name, v);
}

// Label values here are serials, versions and BLE names; drop anything that would need escaping.
static void label_safe(char *dst, const char *src, size_t len)
{
    size_t j = 0;
    for (; *src && j + 1 < len; src++) {
        if (*src != '"' && *src != '\\' && *src != '\n') {
            dst[j++] = *src;
        }
    }
    dst[j] = '\0';
}

static esp_err_t metrics_get(httpd_req_t *req)
{
    sbuf_t b = {.cap = 6144};
    b.buf = heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b.buf && !(b.buf = malloc(b.cap))) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    uint32_t interval = settings_get_bms_interval();
    uint32_t max_age = interval * 3 > 180 ? interval * 3 : 180;

    lock();
    gauge(&b, "bms_up", "1 if the last BLE poll of the BMS succeeded", s_st.last_ok);
    out(&b, "# HELP bms_polls_total BLE polls of the BMS by result\n# TYPE bms_polls_total counter\n"
            "bms_polls_total{result=\"ok\"} %" PRIu32 "\nbms_polls_total{result=\"error\"} %" PRIu32 "\n",
        s_st.polls_ok, s_st.polls_err);
    gauge(&b, "bms_poll_interval_seconds", "Configured poll interval", interval);
    if (s_st.have_info || s_st.addr[0]) {
        char name[32], serial[32], model[16], sw[16], fw[16];
        label_safe(name, s_st.name, sizeof(name));
        label_safe(serial, s_st.info.serial, sizeof(serial));
        label_safe(model, s_st.info.model, sizeof(model));
        label_safe(sw, s_st.info.sw, sizeof(sw));
        label_safe(fw, s_st.info.fw, sizeof(fw));
        out(&b, "# HELP bms_info BMS identity\n# TYPE bms_info gauge\n"
                "bms_info{name=\"%s\",address=\"%s\",serial=\"%s\",model=\"%s\",sw_version=\"%s\",fw_version=\"%s\"} 1\n",
            name, s_st.addr, serial, model, sw, fw);
    }
    if (s_st.have_sample) {
        uint32_t age = age_s(s_st.sample_us);
        gauge(&b, "bms_sample_age_seconds", "Seconds since the last good sample", age);
        if (age <= max_age) {
            const bms_sample_t *s = &s_st.sample;
            gauge(&b, "bms_voltage_volts", "Pack voltage", s->voltage);
            gauge(&b, "bms_current_amperes", "Pack current, positive while charging", s->current);
            gauge(&b, "bms_power_watts", "Pack power (voltage x current)", s->voltage * s->current);
            gauge(&b, "bms_state_of_charge_percent", "State of charge", s->soc);
            gauge(&b, "bms_remaining_capacity_amp_hours", "Remaining charge", s->remaining_ah);
            gauge(&b, "bms_full_capacity_amp_hours", "Full capacity (BatsocList[0][2])", s->capacity_ah);
            gauge(&b, "bms_cell_count", "Cells reporting a voltage", s->n_cells);
            out(&b, "# HELP bms_cell_voltage_volts Cell voltage\n# TYPE bms_cell_voltage_volts gauge\n");
            for (int i = 0; i < s->n_cells; i++) {
                out(&b, "bms_cell_voltage_volts{cell=\"%d\"} %.3f\n", i + 1, s->cells[i]);
            }
            gauge(&b, "bms_cell_voltage_min_volts", "Lowest cell voltage", s->cell_min);
            gauge(&b, "bms_cell_voltage_max_volts", "Highest cell voltage", s->cell_max);
            gauge(&b, "bms_cell_voltage_delta_volts", "Highest minus lowest cell voltage", s->cell_delta);
            out(&b, "# HELP bms_temperature_celsius BMS temperature sensor\n"
                    "# TYPE bms_temperature_celsius gauge\n");
            for (int i = 0; i < s->n_temps; i++) {
                out(&b, "bms_temperature_celsius{sensor=\"%d\"} %.1f\n", i + 1, s->temps[i]);
            }
            gauge(&b, "bms_warning_code", "Bwarn bit field (0: none)", s->warn);
            gauge(&b, "bms_fault_code", "Bfault bit field (0: none)", s->fault);
            gauge(&b, "bms_problem_code", "Bwarn + Bfault", (double)s->problem);
            gauge(&b, "bms_rssi_dbm", "BLE signal strength during the last poll", s_st.rssi);
        }
    }
    unlock();

    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, b.buf, b.len);
    free(b.buf);
    return err;
}

static esp_err_t api_get(httpd_req_t *req)
{
    return web_send_json(req, status_json());
}

// {interval_s, soc: [...], power: [...], delta_mv: [...]}, oldest first.
static esp_err_t history_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "interval_s", settings_get_bms_interval());
    cJSON_AddNumberToObject(o, "len", HISTORY_LEN);
    cJSON *soc = cJSON_AddArrayToObject(o, "soc");
    cJSON *pw = cJSON_AddArrayToObject(o, "power");
    cJSON *dm = cJSON_AddArrayToObject(o, "delta_mv");
    lock();
    size_t start = s_hist_count < HISTORY_LEN ? 0 : s_hist_head;
    for (size_t i = 0; s_hist && i < s_hist_count; i++) {
        const hist_t *h = &s_hist[(start + i) % HISTORY_LEN];
        cJSON_AddItemToArray(soc, cJSON_CreateNumber(roundf(h->soc * 100) / 100));
        cJSON_AddItemToArray(pw, cJSON_CreateNumber(roundf(h->power)));
        cJSON_AddItemToArray(dm, cJSON_CreateNumber(roundf(h->delta_mv)));
    }
    unlock();
    return web_send_json(req, o);
}

// {"real": <last raw reply>, "basic": <device info reply>}, as the BMS sent them.
static esp_err_t raw_get(httpd_req_t *req)
{
    lock();
    char *real = s_st.raw_real ? strdup(s_st.raw_real) : NULL;
    char *basic = s_st.raw_basic ? strdup(s_st.raw_basic) : NULL;
    unlock();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"real\":");
    httpd_resp_sendstr_chunk(req, real ? real : "null");
    httpd_resp_sendstr_chunk(req, ",\"basic\":");
    httpd_resp_sendstr_chunk(req, basic ? basic : "null");
    httpd_resp_sendstr_chunk(req, "}\n");
    free(real);
    free(basic);
    return httpd_resp_sendstr_chunk(req, NULL);
}

// ---------------------------------------------------------------------------
// Control, shared by the HTTP API and the console. Return NULL or an error message.

static void poll_now(void)
{
    lock();
    s_st.paused_until_us = 0;
    unlock();
    xTaskNotifyGive(s_task);
}

static const char *set_interval(int seconds)
{
    if (settings_set_bms_interval(seconds) != ESP_OK) {
        return "interval must be 10..3600 s";
    }
    xTaskNotifyGive(s_task);
    return NULL;
}

static const char *set_addr(const char *addr)
{
    ble_addr_t a = {0};
    if (addr[0] && !parse_addr(addr, &a)) {
        return "address must look like AA:BB:CC:DD:EE:FF";
    }
    char norm[18] = "";
    if (addr[0]) {
        addr_str(&a, norm);
    }
    if (settings_set_bms_addr(norm) != ESP_OK) {
        return "could not save the address";
    }
    lock();
    s_st.addr_pending = true;
    s_st.new_pinned = addr[0] != '\0';
    s_st.new_pin = a;
    unlock();
    xTaskNotifyGive(s_task);
    return NULL;
}

// Pairing passkey. Stored in NVS only; never in the build. "-" removes it.
static const char *set_pin(const char *pin)
{
    if (!strcmp(pin, "-")) {
        return settings_set_bms_pin(NULL) == ESP_OK ? NULL : "could not clear the passkey";
    }
    char *end;
    unsigned long v = strtoul(pin, &end, 10);
    if (*end || !*pin || strlen(pin) > 6 || v > 999999) {
        return "passkey must be up to 6 digits";
    }
    uint32_t p = v;
    if (settings_set_bms_pin(&p) != ESP_OK) {
        return "could not save the passkey";
    }
    lock();
    s_st.addr_pending = true;       // re-pair from scratch on the next poll
    s_st.new_pinned = s_pinned;
    s_st.new_pin = s_pin;
    unlock();
    xTaskNotifyGive(s_task);
    return NULL;
}

static void pause_for(int minutes)
{
    lock();
    s_st.paused_until_us = minutes > 0 ? esp_timer_get_time() + (int64_t)minutes * 60 * 1000000 : 0;
    unlock();
    xTaskNotifyGive(s_task);
}

static esp_err_t api_post(httpd_req_t *req)
{
    if (!web_check_csrf(req) || !web_require_admin(req)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 256);
    if (!body) {
        return ESP_OK;
    }
    const char *e = NULL;
    const cJSON *v;
    if (!e && cJSON_IsNumber(v = cJSON_GetObjectItem(body, "interval_s"))) {
        e = set_interval(v->valueint);
    }
    if (!e && cJSON_IsString(v = cJSON_GetObjectItem(body, "addr"))) {
        e = set_addr(v->valuestring);
    }
    if (!e && cJSON_IsString(v = cJSON_GetObjectItem(body, "pin"))) {
        e = set_pin(v->valuestring[0] ? v->valuestring : "-");   // write-only; never returned
    }
    if (!e && cJSON_IsNumber(v = cJSON_GetObjectItem(body, "pause_min"))) {
        pause_for(v->valueint);
    }
    if (!e && cJSON_IsTrue(cJSON_GetObjectItem(body, "poll"))) {
        poll_now();
    }
    cJSON_Delete(body);
    if (e) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, e);
        return ESP_OK;
    }
    return web_send_json(req, status_json());
}

static void on_ws_request(cJSON *data)
{
    web_ws_broadcast("bms", status_json());
}

static void bms_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/metrics", .method = HTTP_GET, .handler = metrics_get},
        {.uri = "/api/bms", .method = HTTP_GET, .handler = api_get},
        {.uri = "/api/bms", .method = HTTP_POST, .handler = api_post},
        {.uri = "/api/bms/raw", .method = HTTP_GET, .handler = raw_get},
        {.uri = "/api/bms/history", .method = HTTP_GET, .handler = history_get},
    };
    web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}

static int cmd_bms(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    const char *e = NULL;
    if (!strcmp(sub, "status")) {
        cJSON *o = status_json();
        char *s = cJSON_Print(o);
        printf("%s\n", s);
        cJSON_free(s);
        cJSON_Delete(o);
    } else if (!strcmp(sub, "raw")) {
        lock();
        printf("real:  %s\nbasic: %s\n", s_st.raw_real ? s_st.raw_real : "(none yet)",
               s_st.raw_basic ? s_st.raw_basic : "(none yet)");
        unlock();
    } else if (!strcmp(sub, "poll")) {
        poll_now();
        printf("polling\n");
    } else if (!strcmp(sub, "interval") && argc == 3) {
        e = set_interval(atoi(argv[2]));
    } else if (!strcmp(sub, "addr") && argc == 3) {
        e = set_addr(strcmp(argv[2], "-") ? argv[2] : "");
    } else if (!strcmp(sub, "pin") && argc == 3) {
        e = set_pin(argv[2]);
    } else if (!strcmp(sub, "pause") && argc == 3) {
        pause_for(atoi(argv[2]));
    } else {
        printf("usage: bms [status|raw|poll|interval <s>|addr <AA:BB:CC:DD:EE:FF>|addr -|pin <passkey>|pin -|pause <min>]\n");
        return 1;
    }
    if (e) {
        printf("%s\n", e);
        return 1;
    }
    return 0;
}

static void bms_console(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "bms",
        .help = "Battery BMS: status | raw | poll | interval <s> | addr <mac>|- | pin <passkey>|- | pause <min> (0 resumes)",
        .func = cmd_bms,
    };
    esp_console_cmd_register(&cmd);
}

// ---------------------------------------------------------------------------

static esp_err_t bms_init(void)
{
    s_ev = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    s_frame = heap_caps_malloc(sizeof(*s_frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) {
        s_frame = malloc(sizeof(*s_frame));
    }
    if (!s_ev || !s_lock || !s_frame) {
        return ESP_ERR_NO_MEM;
    }
    bms_frame_reset(s_frame);
    // Charts are optional: without PSRAM there is just no history.
    s_hist = heap_caps_calloc(HISTORY_LEN, sizeof(hist_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ble_uuid_from_str(&s_svc_uuid, BMS_SVC_UUID) || ble_uuid_from_str(&s_tx_uuid, BMS_TX_UUID) ||
        ble_uuid_from_str(&s_rx_uuid, BMS_RX_UUID)) {
        return ESP_ERR_INVALID_ARG;
    }
    char addr[SETTINGS_BMS_ADDR_MAX];
    if (settings_get_bms_addr(addr) == ESP_OK && addr[0] && parse_addr(addr, &s_pin)) {
        s_pinned = true;
        ESP_LOGI(TAG, "pinned to %s", addr);
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // Authenticated (MITM) pairing with the passkey from `bms pin`, bonded so
    // later sessions only need to encrypt. The BMS rejects Just Works (SMP 0x03).
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    web_ws_on("bms", on_ws_request);
    if (xTaskCreate(bms_task, "bms", 6144, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const feature_t bms_feature = {
    .name = "bms",
    .init = bms_init,
    .register_routes = bms_routes,
    .register_console = bms_console,
};
