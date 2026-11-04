#include "ble_hid.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* Not declared in the public header of every NimBLE version (ESP-IDF examples declare it the same way). */
extern void ble_store_config_init(void);

static const char *TAG = "ble_hid";

#define APPEARANCE_KEYBOARD 0x03C1

/* Standard keyboard with LED output report. 8-byte input report, no report ID. */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             /* Usage Page (Generic Desktop), Keyboard, Collection */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,             /*  Usage Page (Key Codes), E0..E7 modifiers */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  /* 8 x 1 bit input variable */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             /*  reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* 5 LEDs output */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             /*  LED padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, /* 6 keys */
    0xC0,
};
static const uint8_t HID_INFO[4] = { 0x11, 0x01, 0x00, 0x02 };   /* bcdHID 1.11, country 0, flags: normally connectable */
static const uint8_t REPORT_REF_INPUT[2] = { 0x00, 0x01 };       /* id 0, input */
static const uint8_t REPORT_REF_OUTPUT[2] = { 0x00, 0x02 };      /* id 0, output */

static ble_hid_config_t s_cfg;
static char s_name[32];
static uint8_t s_pnp[7];
static uint8_t s_protocol_mode = 1;   /* report protocol */
static uint8_t s_battery = 100;
static uint8_t s_own_addr_type;
static uint16_t s_input_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed, s_encrypted;
static bool s_started;

enum { ID_REPORT_MAP, ID_HID_INFO, ID_CTRL_POINT, ID_PROTO_MODE, ID_INPUT, ID_OUTPUT, ID_REF_IN, ID_REF_OUT,
       ID_PNP, ID_MANUF, ID_BATTERY };

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    intptr_t id = (intptr_t)arg;
    int rc = 0;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_READ_DSC:
        switch (id) {
        case ID_REPORT_MAP: rc = os_mbuf_append(ctxt->om, REPORT_MAP, sizeof(REPORT_MAP)); break;
        case ID_HID_INFO: rc = os_mbuf_append(ctxt->om, HID_INFO, sizeof(HID_INFO)); break;
        case ID_PROTO_MODE: rc = os_mbuf_append(ctxt->om, &s_protocol_mode, 1); break;
        case ID_INPUT: { static const uint8_t z[8] = { 0 }; rc = os_mbuf_append(ctxt->om, z, sizeof(z)); break; }
        case ID_OUTPUT: { static const uint8_t z[1] = { 0 }; rc = os_mbuf_append(ctxt->om, z, 1); break; }
        case ID_REF_IN: rc = os_mbuf_append(ctxt->om, REPORT_REF_INPUT, 2); break;
        case ID_REF_OUT: rc = os_mbuf_append(ctxt->om, REPORT_REF_OUTPUT, 2); break;
        case ID_PNP: rc = os_mbuf_append(ctxt->om, s_pnp, sizeof(s_pnp)); break;
        case ID_MANUF: rc = os_mbuf_append(ctxt->om, "DIY", 3); break;
        case ID_BATTERY: rc = os_mbuf_append(ctxt->om, &s_battery, 1); break;
        default: return BLE_ATT_ERR_UNLIKELY;
        }
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (id == ID_PROTO_MODE) {
            uint8_t v;
            if (OS_MBUF_PKTLEN(ctxt->om) == 1 && os_mbuf_copydata(ctxt->om, 0, 1, &v) == 0) s_protocol_mode = v;
        }
        return 0;   /* control point and LED output report: accept and ignore */
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

#define ENC_READ  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {   /* HID service */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = access_cb, .arg = (void *)ID_HID_INFO,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = access_cb, .arg = (void *)ID_REPORT_MAP,
              .flags = ENC_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = access_cb, .arg = (void *)ID_CTRL_POINT,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = access_cb, .arg = (void *)ID_PROTO_MODE,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = access_cb, .arg = (void *)ID_INPUT,
              .val_handle = &s_input_handle,
              .flags = ENC_READ | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = access_cb, .arg = (void *)ID_REF_IN },
                  { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = access_cb, .arg = (void *)ID_OUTPUT,
              .flags = ENC_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = access_cb, .arg = (void *)ID_REF_OUT },
                  { 0 } } },
            { 0 } },
    },
    {   /* Device information */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = access_cb, .arg = (void *)ID_MANUF,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = access_cb, .arg = (void *)ID_PNP,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 } },
    },
    {   /* Battery (static 100 %, USB powered) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = access_cb, .arg = (void *)ID_BATTERY,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 } },
    },
    { 0 },
};

/* ------------------------------------------------------------------ */
static void start_advertising(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            s_encrypted = false;
            ESP_LOGI(TAG, "connected");
            struct ble_gap_upd_params p = {
                .itvl_min = s_cfg.conn_itvl_min, .itvl_max = s_cfg.conn_itvl_max,
                .latency = s_cfg.conn_latency, .supervision_timeout = s_cfg.conn_timeout,
            };
            ble_gap_update_params(s_conn, &p);
        } else {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_encrypted = false;
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_encrypted = (event->enc_change.status == 0);
        ESP_LOGI(TAG, "encryption %s (status=%d)", s_encrypted ? "on" : "off", event->enc_change.status);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_input_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "input report notifications %s", s_subscribed ? "enabled" : "disabled");
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Host lost its key (e.g. device removed in Windows): drop the old bond and pair again. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", (unsigned)event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = APPEARANCE_KEYBOARD;
    f.appearance_is_present = 1;
    f.uuids16 = &hid_uuid;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = s_cfg.adv_itvl_min;
    p.itvl_max = s_cfg.adv_itvl_max;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc && rc != BLE_HS_EALREADY) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    ESP_LOGI(TAG, "advertising as \"%s\"", s_name);
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset, reason=%d", reason); }

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    start_advertising();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_hid_start(const ble_hid_config_t *cfg)
{
    if (!cfg || !cfg->device_name) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_cfg = *cfg;
    strncpy(s_name, cfg->device_name, sizeof(s_name) - 1);
    s_pnp[0] = 0x02;   /* vendor id source: USB-IF */
    s_pnp[1] = (uint8_t)(cfg->pnp_vendor_id & 0xFF);  s_pnp[2] = (uint8_t)(cfg->pnp_vendor_id >> 8);
    s_pnp[3] = (uint8_t)(cfg->pnp_product_id & 0xFF); s_pnp[4] = (uint8_t)(cfg->pnp_product_id >> 8);
    s_pnp[5] = (uint8_t)(cfg->pnp_version & 0xFF);    s_pnp[6] = (uint8_t)(cfg->pnp_version >> 8);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err)); return err; }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   /* Just Works */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_device_appearance_set(APPEARANCE_KEYBOARD);

    int rc = ble_gatts_count_cfg(GATT_SVCS);
    if (rc) { ESP_LOGE(TAG, "gatts_count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc) { ESP_LOGE(TAG, "gatts_add_svcs rc=%d", rc); return ESP_FAIL; }

    ble_store_config_init();
    if (cfg->clear_bonds_on_boot) { ble_store_clear(); ESP_LOGW(TAG, "all bonds cleared"); }

    nimble_port_freertos_init(host_task);
    s_started = true;
    return ESP_OK;
}

bool ble_hid_ready(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed && s_encrypted; }

bool ble_hid_send_report(const uint8_t report[8])
{
    if (!ble_hid_ready()) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, 8);
    if (!om) return false;
    return ble_gatts_notify_custom(s_conn, s_input_handle, om) == 0;
}

void ble_hid_clear_bonds(void) { ble_store_clear(); }