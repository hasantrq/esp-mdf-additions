/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "mwifi.h"
#include "miniz.h"

typedef struct {
    uint32_t magic; /**< Filter duplicate packets */
    struct {
        uint32_t transmit_self : 1;  /**< Whether the forwarded packet is for yourself */
        uint32_t transmit_all  : 1;  /**< Whether to send packages to all devices */
        uint32_t transmit_num  : 10; /**< Number of destination devices forwarded */
        uint32_t total_size    : 12; /**< Total length of the packet */
        uint32_t packet_seq    : 4;  /**< Serial number of the packet */
    };
    mwifi_data_type_t type;     /**< The type of data */
} __attribute__((packed)) mwifi_data_head_t;

static const char *TAG           = "mwifi";
static bool g_mwifi_inited_flag  = false;
static bool mwifi_connected_flag = false;
static bool g_mwifi_started_flag = false;
static mwifi_config_t *g_ap_config  = NULL;
static mwifi_init_config_t *g_init_config = NULL;

bool mwifi_is_started()
{
    return g_mwifi_started_flag;
}

bool mwifi_is_connected()
{
    return mwifi_connected_flag;
}

mdf_err_t mwifi_scan(const mwifi_config_t *config, wifi_ap_record_t *ap)
{
    MDF_PARAM_CHECK(config);
    MDF_PARAM_CHECK(ap);

    int ie_len         = 0;
    int8_t rssi_best   = -120;
    mesh_assoc_t assoc = {0x0};
    bool parent_found  = false;
    uint16_t ap_number = 0;
    wifi_ap_record_t ap_record     = {0x0};
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 300,
    };

    ESP_ERROR_CHECK(esp_mesh_set_self_organized(false, false));

    for (int i = 0; !parent_found && i < 2; ++i) {
        esp_wifi_scan_stop();
        esp_wifi_scan_start(&scan_config, true);
        esp_wifi_scan_get_ap_num(&ap_number);
        MDF_ERROR_CHECK(ap_number <= 0, ESP_FAIL, "esp_wifi_scan_get_ap_num");

        MDF_LOGD("Get number of APs found, number: %d", ap_number);

        for (int i = 0; i < ap_number; i++) {
            ie_len = 0;
            memset(&ap_record, 0, sizeof(wifi_ap_record_t));
            memset(&assoc, 0, sizeof(mesh_assoc_t));

            esp_mesh_scan_get_ap_ie_len(&ie_len);
            esp_mesh_scan_get_ap_record(&ap_record, &assoc);

            if (ie_len == sizeof(assoc)) {
                if (assoc.mesh_type != MESH_IDLE && assoc.layer_cap
                        && assoc.assoc < assoc.assoc_cap && ap_record.rssi > -85
                        && !memcmp(config->mesh_id, assoc.mesh_id, sizeof(config->mesh_id))
                        && !(assoc.flag & MESH_ASSOC_FLAG_NETWORK_FREE)) {
                    parent_found = true;
                    memcpy(ap, &ap_record, sizeof(wifi_ap_record_t));

                    MDF_LOGD("Mesh, ssid: %s, layer: %d/%d, assoc: %d/%d, %d, " MACSTR
                             ", channel: %u, rssi: %d, ID: " MACSTR ", IE: %s",
                             ap_record.ssid, assoc.layer, assoc.layer_cap, assoc.assoc,
                             assoc.assoc_cap, assoc.layer2_cap, MAC2STR(ap_record.bssid),
                             ap_record.primary, ap_record.rssi, MAC2STR(assoc.mesh_id),
                             assoc.encrypted ? "Encrypted" : "Unencrypted");
                }
            } else if ((strlen((char *)ap_record.ssid) && !strcmp(config->router_ssid, (char *)ap_record.ssid))
                       && ap_record.rssi > rssi_best) {
                MDF_LOGV("Router, ssid: %s, bssid: " MACSTR ", channel: %u, rssi: %d",
                         ap_record.ssid, MAC2STR(ap_record.bssid),
                         ap_record.primary, ap_record.rssi);

                parent_found = true;
                rssi_best    = ap_record.rssi;
                memcpy(ap, &ap_record, sizeof(wifi_ap_record_t));
            } else {
                MDF_LOGV("(%d : %s), " MACSTR ", channel: %u, rssi: %d", i,
                         ap_record.ssid, MAC2STR(ap_record.bssid), ap_record.primary,
                         ap_record.rssi);
            }
        }

        /**
         * @brief Get hidden router
         */
        if (!parent_found && strlen(config->router_ssid) > 0) {
            scan_config.ssid      = (uint8_t *)config->router_ssid;
            scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        }
    }

    if (config->mesh_type == MESH_ROOT) {
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
    } else {
        ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
    }

    MDF_ERROR_CHECK(!parent_found, MDF_FAIL, "Router and devices not found");

    return ESP_OK;
}

static void esp_mesh_event_cb(mesh_event_t event)
{
    MDF_LOGD("esp_mesh_event_cb event.id: %d", event.id);
    static int s_disconnected_count = 0;

    switch (event.id) {
        case MESH_EVENT_PARENT_CONNECTED:
            MDF_LOGI("Parent is connected");
            mwifi_connected_flag = true;

            if (esp_mesh_is_root()) {
                tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
            } else {
                tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
            }

            break;

        case MESH_EVENT_ROOT_LOST_IP:
            MDF_LOGI("Root loses the IP address");
            // esp_wifi_connect();
            break;

        case MESH_EVENT_PARENT_DISCONNECTED:
            MDF_LOGI("Parent is disconnected, reason: %d", event.info.disconnected.reason);
            mwifi_connected_flag = false;

            /**
             * @brief The root node cannot find the router and only reports the disconnection.
             */
            if (s_disconnected_count++ > 30) {
                s_disconnected_count = 0;
                mdf_event_loop_send(MDF_EVENT_MWIFI_NO_PARENT_FOUND, NULL);
            }

            break;

        case MESH_EVENT_STARTED:
            MDF_LOGI("MESH is started");
            s_disconnected_count = 0;
            g_mwifi_started_flag = true;
            tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
            tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
            break;

        case MESH_EVENT_STOPPED:
            MDF_LOGI("MESH is stopped");
            g_mwifi_started_flag = false;
            mwifi_connected_flag = false;
            tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
            tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);
            break;

        case MDF_EVENT_MWIFI_ROUTING_TABLE_ADD:
            MDF_LOGI("Routing table is changed by adding newly joined children add_num: %d, total_num: %d",
                     event.info.routing_table.rt_size_change,
                     event.info.routing_table.rt_size_new);
            break;

        case MDF_EVENT_MWIFI_ROUTING_TABLE_REMOVE:
            MDF_LOGI("Routing table is changed by removing leave children remove_num: %d, total_num: %d",
                     event.info.routing_table.rt_size_change,
                     event.info.routing_table.rt_size_new);
            break;

        default:
            break;
    }

    mdf_event_loop_send(event.id, NULL);
}

mdf_err_t mwifi_init(mwifi_init_config_t *config)
{
    MDF_PARAM_CHECK(config);
    MDF_ERROR_CHECK(g_mwifi_inited_flag, MDF_ERR_MWIFI_INITED, "Mwifi has been initialized");

    g_init_config = MDF_CALLOC(1, sizeof(mwifi_init_config_t));

    memcpy(g_init_config, config, sizeof(mwifi_init_config_t));
    g_mwifi_inited_flag = true;

    ESP_ERROR_CHECK(esp_mesh_init());

    return MDF_OK;
}

void mwifi_print_config()
{
    mesh_cfg_t cfg                     = {0};
    mesh_attempts_t attempts           = {0};
    int beacon_interval                = 0;
    mesh_switch_parent_t switch_parent = {0};

    ESP_ERROR_CHECK(esp_mesh_get_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_get_attempts(&attempts));
    ESP_ERROR_CHECK(esp_mesh_get_switch_parent_paras(&switch_parent));

    MDF_LOGI("**************** Root config ****************");
    MDF_LOGI("vote_percentage       : %0.2f", esp_mesh_get_vote_percentage());
    MDF_LOGI("vote_max_count        : %d", attempts.vote);
    MDF_LOGI("backoff_rssi          : %d", switch_parent.backoff_rssi);
    MDF_LOGI("scan_min_count        : %d", attempts.scan);
    MDF_LOGI("attempt_count         : %d", attempts.fail);
    MDF_LOGI("monitor_ie_count      : %d", attempts.monitor_ie);
    MDF_LOGI("root_healing_ms       : %d", esp_mesh_get_root_healing_delay());
    MDF_LOGI("root_conflicts_enable : %d", esp_mesh_is_root_conflicts_allowed());
    MDF_LOGI("fix_root_enable       : %d", esp_mesh_is_root_fixed());

    MDF_LOGI("****************  Capacity   ****************");
    MDF_LOGI("max_layer             : %d", esp_mesh_get_max_layer());
    MDF_LOGI("max_connection        : %d", cfg.mesh_ap.max_connection);
    MDF_LOGI("capacity_num          : %d", esp_mesh_get_capacity_num());

    MDF_LOGI("****************  Stability  ****************");
    MDF_LOGI("assoc_expire_ms       : %d", esp_mesh_get_ap_assoc_expire() * 1000);
    ESP_ERROR_CHECK(esp_mesh_get_beacon_interval(&beacon_interval));
    MDF_LOGI("beacon_interval_ms    : %d", beacon_interval);
    MDF_LOGI("passive_scan_ms       : %d", esp_mesh_get_passive_scan_time());
    MDF_LOGI("monitor_duration_ms   : %d", switch_parent.duration_ms);
    MDF_LOGI("cnx_rssi              : %d", switch_parent.cnx_rssi);
    MDF_LOGI("select_rssi           : %d", switch_parent.select_rssi);
    MDF_LOGI("switch_rssi           : %d", switch_parent.switch_rssi);

    MDF_LOGI("**************** Transmission ****************");
    MDF_LOGI("xon_qsize             : %d", esp_mesh_get_xon_qsize());
    MDF_LOGI("authmode              : %d", esp_mesh_get_ap_authmode());

    MDF_LOGI("**************** Router info  ****************");
    MDF_LOGI("ssid                  : %s", cfg.router.ssid);
    MDF_LOGI("password              : %s", cfg.router.password);
    MDF_LOGI("bssid                 : " MACSTR, MAC2STR(cfg.router.bssid));
    MDF_LOGI("mesh_id               : " MACSTR, MAC2STR(cfg.mesh_id.addr));
    MDF_LOGI("mesh_password         : %s", cfg.mesh_ap.password);
    MDF_LOGI("channel               : %d", cfg.channel);
}

mdf_err_t mwifi_start()
{
    if (mwifi_is_started()) {
        return MDF_OK;
    }

    MDF_ERROR_CHECK(!g_mwifi_inited_flag, MDF_ERR_MWIFI_NOT_INIT, "Mwifi isn't initialized");
    MDF_ERROR_CHECK(!g_ap_config, MDF_ERR_MWIFI_NO_CONFIG, "Config information is not set");

    mesh_cfg_t mesh_config             = MESH_INIT_CONFIG_DEFAULT();
    mwifi_config_t *ap_config       = g_ap_config;
    mwifi_init_config_t *init_config   = g_init_config;
    mesh_attempts_t attempts           = {0};
    mesh_switch_parent_t switch_parent = {0};

    ESP_ERROR_CHECK(esp_mesh_init());

    switch (ap_config->mesh_type) {
        case MESH_ROOT:
            ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
            break;

        case MESH_NODE:
            ESP_ERROR_CHECK(esp_mesh_fix_root(true));
            break;

        case MESH_LEAF:
            ESP_ERROR_CHECK(esp_mesh_fix_root(MESH_LEAF));
            break;

        case MESH_IDLE:
            break;

        default:
            break;
    }

    mesh_config.event_cb = esp_mesh_event_cb;

    /**
     * @brief Mesh root configuration
     */
    attempts.vote       = init_config->vote_max_count;
    attempts.scan       = init_config->scan_min_count;
    attempts.fail       = init_config->attempt_count;
    attempts.monitor_ie = init_config->monitor_ie_count;
    ESP_ERROR_CHECK(esp_mesh_set_attempts(&attempts));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(init_config->vote_percentage / 100.0));
    ESP_ERROR_CHECK(esp_mesh_set_root_healing_delay(init_config->root_healing_ms));
    ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(init_config->root_conflicts_enable));

    /**
     * @brief Mesh network capacity configuration
     */
    mesh_config.mesh_ap.max_connection = init_config->max_connection;
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(init_config->max_layer));
    ESP_ERROR_CHECK(esp_mesh_set_capacity_num(init_config->capacity_num));

    /**
     * @brief Mesh network stability configuration
     */
    switch_parent.duration_ms  = init_config->monitor_duration_ms;
    switch_parent.backoff_rssi = init_config->backoff_rssi;
    switch_parent.cnx_rssi     = init_config->cnx_rssi;
    switch_parent.select_rssi  = init_config->select_rssi;
    switch_parent.switch_rssi  = init_config->switch_rssi;
    ESP_ERROR_CHECK(esp_mesh_set_switch_parent_paras(&switch_parent));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(init_config->assoc_expire_ms * 1000));
    ESP_ERROR_CHECK(esp_mesh_set_beacon_interval(init_config->beacon_interval_ms));
    ESP_ERROR_CHECK(esp_mesh_set_passive_scan_time(init_config->passive_scan_ms));

    /**
     * @brief Mesh network data transmission configuration
     */
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(init_config->xon_qsize));

    /**
     * @brief mwifi AP configuration
     */
    mesh_config.channel         = !ap_config->channel ? 1 : ap_config->channel;
    mesh_config.router.ssid_len = strlen(ap_config->router_ssid);
    memcpy(mesh_config.router.ssid, ap_config->router_ssid, mesh_config.router.ssid_len);
    memcpy(mesh_config.router.bssid, ap_config->router_bssid, MWIFI_ADDR_LEN);
    memcpy(mesh_config.router.password, ap_config->router_password,
           sizeof(mesh_config.router.password));
    memcpy(mesh_config.mesh_id.addr, ap_config->mesh_id, MWIFI_ADDR_LEN);

    if (*ap_config->mesh_password != '\0') {
        memcpy(mesh_config.mesh_ap.password, ap_config->mesh_password,
               sizeof(mesh_config.mesh_ap.password));
        ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA_WPA2_PSK));
    } else {
        ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN));
    }

    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_config));
    ESP_ERROR_CHECK(esp_mesh_start());

    if (!ap_config->channel) {
        mdf_event_loop_send(MDF_EVENT_MWIFI_CHANNEL_NO_FOUND, NULL);
    }

    return MDF_OK;
}

mdf_err_t mwifi_stop()
{
    mdf_err_t ret = MDF_OK;

    if (!mwifi_is_started()) {
        return MDF_OK;
    }

    g_mwifi_started_flag = false;

    ret = esp_mesh_stop();
    MDF_ERROR_CHECK(ret != MDF_OK, ret, "esp_mesh_stop");

    /**< To ensure that all tasks are properly exited */
    MDF_LOGD("vTaskDelay 50ms");
    vTaskDelay(50 / portTICK_RATE_MS);

    MDF_ERROR_ASSERT(esp_wifi_set_mode(WIFI_MODE_STA));

    return MDF_OK;
}

mdf_err_t mwifi_restart()
{
    mdf_err_t ret = MDF_OK;

    ret = mwifi_stop();
    MDF_ERROR_CHECK(ret != ESP_OK, ret, "Mwifi stop");

    ret = mwifi_start();
    MDF_ERROR_CHECK(ret != ESP_OK, ret, "Mwifi stop");

    return MDF_OK;
}

mdf_err_t mwifi_deinit()
{
    MDF_ERROR_CHECK(!g_mwifi_inited_flag, MDF_ERR_MWIFI_NOT_INIT, "Mwifi isn't initialized");

    MDF_FREE(g_init_config);
    MDF_FREE(g_ap_config);

    ESP_ERROR_CHECK(esp_mesh_deinit());

    return MDF_OK;
}

mdf_err_t mwifi_set_init_config(mwifi_init_config_t *init_config)
{
    MDF_PARAM_CHECK(init_config);
    MDF_ERROR_CHECK(!g_mwifi_inited_flag, MDF_ERR_MWIFI_NOT_INIT, "Mwifi isn't initialized");

    memcpy(g_init_config, init_config, sizeof(mwifi_init_config_t));

    return MDF_OK;
}

mdf_err_t mwifi_get_init_config(mwifi_init_config_t *init_config)
{
    MDF_PARAM_CHECK(init_config);
    MDF_ERROR_CHECK(!g_mwifi_inited_flag, MDF_ERR_MWIFI_NOT_INIT, "Mwifi isn't initialized");

    memcpy(init_config, g_init_config, sizeof(mwifi_init_config_t));

    return MDF_OK;
}

mdf_err_t mwifi_set_config(mwifi_config_t *config)
{
    MDF_PARAM_CHECK(config);
    MDF_PARAM_CHECK(!MWIFI_ADDR_IS_EMPTY(config->mesh_id));
    MDF_PARAM_CHECK(config->channel <= 14);
    MDF_PARAM_CHECK(!strlen(config->mesh_password)
                    || (strlen(config->mesh_password) >= 8 && strlen(config->mesh_password) <= 64));

    if (!g_ap_config) {
        g_ap_config = MDF_CALLOC(1, sizeof(mwifi_config_t));
    }

    memcpy(g_ap_config, config, sizeof(mwifi_config_t));

    return MDF_OK;
}

mdf_err_t mwifi_get_config(mwifi_config_t *config)
{
    MDF_PARAM_CHECK(config);
    MDF_ERROR_CHECK(!g_ap_config, ESP_ERR_NOT_SUPPORTED, "config information is not set");

    memcpy(config, g_ap_config, sizeof(mwifi_config_t));

    return MDF_OK;
}

static bool addrs_remove(mesh_addr_t *addrs_list, size_t *addrs_num, const mesh_addr_t *addr)
{
    for (int i = 0; i < *addrs_num; i++) {
        if (!memcmp(addrs_list + i, addr, sizeof(mesh_addr_t))) {
            if (--(*addrs_num)) {
                memcpy(addrs_list + i, addrs_list + i + 1, (*addrs_num - i) * MWIFI_ADDR_LEN);
            }

            return true;
        }
    }

    return false;
}

/**
 * @brief Fragmenting packets for transmission
 */
static mdf_err_t mwifi_subcontract_write(const mesh_addr_t *dest_addr, const mesh_data_t *data,
        int flag, const mesh_opt_t *opt)
{
    mdf_err_t ret = MDF_OK;
    static SemaphoreHandle_t *s_mwifi_send_lock = NULL;
    mwifi_data_head_t *data_head = (mwifi_data_head_t *)opt->val;
    mesh_data_t mesh_data = {0x0};
    data_head->total_size = data->size;
    data_head->packet_seq = 0;

    memcpy(&mesh_data, data, sizeof(mesh_data_t));

    if (!s_mwifi_send_lock) {
        s_mwifi_send_lock = xSemaphoreCreateMutex();
    }

    for (int unwritten_size = data_head->total_size; unwritten_size > 0;
            unwritten_size -= MWIFI_PAYLOAD_LEN) {
        data_head->magic = esp_random();
        mesh_data.size   = MIN(unwritten_size, MWIFI_PAYLOAD_LEN);

        MDF_LOGV("dest_addr: " MACSTR ", size: %d, data: %s",
                 MAC2STR(dest_addr->addr), mesh_data.size, mesh_data.data);
        xSemaphoreTake(s_mwifi_send_lock, portMAX_DELAY);
        ret = esp_mesh_send(dest_addr, &mesh_data, flag, opt, 1);
        xSemaphoreGive(s_mwifi_send_lock);
        MDF_ERROR_CHECK(ret != ESP_OK, ret, "Node failed to send packets, dest_mac: " MACSTR,
                        MAC2STR(dest_addr->addr));

        data_head->packet_seq++;
        mesh_data.data += MWIFI_PAYLOAD_LEN;
    }

    return MDF_OK;
}

/**
 * @brief Multicast forwarding
 */
static mdf_err_t mwifi_transmit_write(mesh_addr_t *addrs_list, size_t addrs_num, mesh_data_t *mesh_data,
                                      int data_flag, const mesh_opt_t *mesh_opt)
{
    mdf_err_t ret          = MDF_OK;
    wifi_sta_list_t sta    = {0};
    uint8_t *transmit_data = NULL;
    mwifi_data_head_t *data_head = (mwifi_data_head_t *)mesh_opt->val;

    if (MWIFI_ADDR_IS_ANY(addrs_list->addr) || MWIFI_ADDR_IS_BROADCAST(addrs_list->addr)) {
        data_head->transmit_all = true;
    }

    ESP_ERROR_CHECK(esp_wifi_ap_get_sta_list(&sta));

    for (int i = 0; i < sta.num && addrs_num > 0; ++i) {
        mesh_addr_t *child_addr = (mesh_addr_t *)&sta.sta[i].mac;
        MDF_LOGV("data_head->transmit_all: %d, child_addr: " MACSTR,
                 data_head->transmit_all, MAC2STR(child_addr->addr));

        if (!data_head->transmit_all) {
            int subnet_num           = 0;
            mesh_addr_t *subnet_addr = NULL;
            data_head->transmit_self = addrs_remove(addrs_list, &addrs_num, child_addr);
            data_head->transmit_num  = 0;

            ESP_ERROR_CHECK(esp_mesh_get_subnet_nodes_num(child_addr, &subnet_num));
            transmit_data = MDF_REALLOC(transmit_data, mesh_data->size + subnet_num * MWIFI_ADDR_LEN);
            mesh_addr_t *transmit_addr = (mesh_addr_t *)transmit_data;
            MDF_LOGV("subnet_num: %d", subnet_num);

            if (subnet_num) {
                subnet_addr = MDF_MALLOC(subnet_num * sizeof(mesh_addr_t));
                ESP_ERROR_CHECK(esp_mesh_get_subnet_nodes_list(child_addr, subnet_addr, subnet_num));
            }

            for (int j = 0; j < subnet_num && addrs_num > 0; ++j) {
                if (addrs_remove(addrs_list, &addrs_num, subnet_addr + j)) {
                    memcpy(transmit_addr + data_head->transmit_num, subnet_addr + j, sizeof(mesh_addr_t));
                    data_head->transmit_num++;
                    MDF_LOGV("count: %d, transmit_addr: " MACSTR,
                             data_head->transmit_num, MAC2STR(subnet_addr[j].addr));
                }
            }

            MDF_FREE(subnet_addr);
        }

        if (data_head->transmit_num || data_head->transmit_self || data_head->transmit_all) {
            mesh_data_t tmp_data = {
                .size = mesh_data->size + data_head->transmit_num * MWIFI_ADDR_LEN,
            };

            if (transmit_data) {
                memcpy(transmit_data + data_head->transmit_num * MWIFI_ADDR_LEN, mesh_data->data, mesh_data->size);
                tmp_data.data = transmit_data;
            } else {
                tmp_data.data = mesh_data->data;
            }

            MDF_LOGV("mesh_data->size: %d, transmit_num: %d, child_addr: " MACSTR,
                     mesh_data->size, data_head->transmit_num, MAC2STR(child_addr->addr));

            ret = mwifi_subcontract_write(child_addr, &tmp_data, data_flag, mesh_opt);
            MDF_ERROR_BREAK(ret != ESP_OK, "<%s> Root node failed to send packets, dest_mac: "MACSTR,
                            mdf_err_to_name(ret), MAC2STR(child_addr->addr));
        }
    }

    MDF_FREE(transmit_data);


    /**
     * @brief Prevent topology changes during the process of sending packets,
     *        such as: the child node becomes the parent node and cannot be found.
     */
    if (addrs_num > 0) {
        data_flag &= ~(1 << 2);
        data_flag |= MESH_DATA_P2P;
        data_head->transmit_num = 0;

        /**
         * @brief When sending to all devices, the device need to send it to the root node itself.
         */
        if (MWIFI_ADDR_IS_ANY(addrs_list->addr) && data_head->transmit_all && esp_mesh_is_root()) {
            ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_STA, (uint8_t *)addrs_list));
            data_head->transmit_all = false;
        }
    }

    for (int i = 0; i < addrs_num && !data_head->transmit_all; ++i) {
        ret = mwifi_subcontract_write(addrs_list + i, mesh_data, data_flag, mesh_opt);
        MDF_ERROR_CONTINUE(ret != ESP_OK, "<%s> Root node failed to send packets, dest_mac: "MACSTR,
                           mdf_err_to_name(ret), MAC2STR((addrs_list + i)->addr));
    }

    return MDF_OK;
}

mdf_err_t mwifi_write(const uint8_t *dest_addrs, const mwifi_data_type_t *data_type,
                      const void *data, size_t size, bool block)
{
    MDF_PARAM_CHECK(data_type);
    MDF_PARAM_CHECK(data);
    MDF_PARAM_CHECK(size > 0);
    MDF_PARAM_CHECK(!dest_addrs || !MWIFI_ADDR_IS_EMPTY(dest_addrs));
    MDF_ERROR_CHECK(!mwifi_is_started(), MDF_ERR_MWIFI_NOT_START, "Mwifi isn't started");

    mdf_err_t ret          = MDF_OK;
    int data_flag          = 0;
    mz_ulong compress_size = 0;
    uint8_t *compress_data = NULL;
    uint8_t root_addr[]    = MWIFI_ADDR_ROOT;
    bool to_root = !dest_addrs || !memcmp(dest_addrs, root_addr, MWIFI_ADDR_LEN) ? true : false;
    dest_addrs   = !dest_addrs ? root_addr : dest_addrs;

    mwifi_data_head_t data_head = {0x0};
    mesh_data_t mesh_data       = {
        .tos   = (data_type->communicate != MWIFI_COMMUNICATE_BROADCAST && g_init_config->retransmit_enable)
        ? MESH_TOS_P2P : MESH_TOS_DEF,
        .data  = (uint8_t *)data,
        .size  = size,
    };
    mesh_opt_t mesh_opt   = {
        .len  = sizeof(mwifi_data_head_t),
        .val  = (void *) &data_head,
        .type = (data_type->communicate == MWIFI_COMMUNICATE_BROADCAST)
        ? MESH_OPT_SEND_GROUP : MESH_OPT_RECV_DS_ADDR,
    };

    data_flag = (to_root) ? MESH_DATA_TODS : MESH_DATA_P2P;
    data_flag = (data_type->communicate == MWIFI_COMMUNICATE_BROADCAST) ? data_flag | MESH_DATA_GROUP : data_flag;
    data_flag = (g_init_config->data_drop_enable) ? data_flag | MESH_DATA_DROP : data_flag;
    data_flag = (!block) ? data_flag | MESH_DATA_NONBLOCK : data_flag;
    data_head.transmit_self = true;
    memcpy(&data_head.type, data_type, sizeof(mwifi_data_type_t));

    /**
     * @brief data compression
     */
    if (data_head.type.compression) {
        compress_size = compressBound(size);
        compress_data = MDF_MALLOC(compress_size);

        ret = compress(compress_data, &compress_size, mesh_data.data, mesh_data.size);
        MDF_ERROR_GOTO(ret != MZ_OK, EXIT, "Compressed whitelist failed, ret: 0x%x", -ret);
        MDF_LOGD("compress, size: %d, compress_size: %d, rate: %d%%",
                 size, (int)compress_size, (int)compress_size * 100 / size);

        if (compress_size > size) {
            data_head.type.compression = false;
        } else {
            mesh_data.data = compress_data;
            mesh_data.size = compress_size;
        }
    }

    data_head.total_size = mesh_data.size;
    ret = mwifi_subcontract_write((mesh_addr_t *)dest_addrs, &mesh_data, data_flag, &mesh_opt);
    MDF_ERROR_GOTO(ret != ESP_OK, EXIT, "<%s> Node failed to send packets, data_flag: 0x%x, dest_mac: " MACSTR,
                   mdf_err_to_name(ret), data_flag, MAC2STR(dest_addrs));

EXIT:
    MDF_FREE(compress_data);
    return ret;
}

mdf_err_t mwifi_read(uint8_t *src_addr, mwifi_data_type_t *data_type,
                     void *data, size_t *size, TickType_t wait_ticks)
{
    MDF_PARAM_CHECK(src_addr);
    MDF_PARAM_CHECK(data_type);
    MDF_PARAM_CHECK(data);
    MDF_PARAM_CHECK(size);
    MDF_PARAM_CHECK(*size > 0);
    MDF_ERROR_CHECK(!mwifi_is_started(), MDF_ERR_MWIFI_NOT_START, "Mwifi isn't started");

    mdf_err_t ret      = MDF_OK;
    uint8_t *recv_data = NULL;
    size_t recv_size   = 0;
    int data_flag      = 0;
    static uint32_t s_data_magic = 0;
    TickType_t start_ticks       = xTaskGetTickCount();
    mwifi_data_head_t data_head  = {0x0};
    mesh_data_t mesh_data        = {0x0};
    mesh_opt_t mesh_opt          = {
        .len  = sizeof(mwifi_data_head_t),
        .val  = (void *) &data_head,
        .type = MESH_OPT_RECV_DS_ADDR,
    };

    for (;;) {
        recv_size = 0;
        recv_data = MDF_REALLOC(recv_data, MWIFI_PAYLOAD_LEN);

        for (int expect_seq = 0; !recv_size || recv_size < data_head.total_size; expect_seq++) {
            mesh_data.size = recv_size ? data_head.total_size - recv_size : MWIFI_PAYLOAD_LEN;
            mesh_data.data = recv_data + recv_size;
            wait_ticks     = (wait_ticks == portMAX_DELAY) ? portMAX_DELAY :
                             xTaskGetTickCount() - start_ticks < wait_ticks ?
                             wait_ticks - (xTaskGetTickCount() - start_ticks) : 0;

            ret = esp_mesh_recv((mesh_addr_t *)src_addr, &mesh_data, wait_ticks * portTICK_RATE_MS,
                                &data_flag, &mesh_opt, 1);

            if (ret == ESP_ERR_MESH_NOT_START) {
                MDF_LOGW("<ESP_ERR_MESH_NOT_START> Node failed to receive packets");
                vTaskDelay(100 / portTICK_RATE_MS);
                continue;
            } else if (ret == ESP_ERR_MESH_TIMEOUT) {
                MDF_LOGW("<MDF_ERR_MWIFI_TIMEOUT> Node failed to receive packets");
                ret = MDF_ERR_MWIFI_TIMEOUT;
                goto EXIT;
            } else if (ret != ESP_OK) {
                MDF_LOGW("<%s> Node failed to receive packets", mdf_err_to_name(ret));
                goto EXIT;
            }

            /**
             * @brief Discard this packet if there is a packet loss in the middle
             */
            if (data_head.packet_seq != expect_seq) {
                MDF_LOGW("Part of the packet is lost, expect_seq: %d, recv_seq: %d",
                         expect_seq, data_head.packet_seq);

                recv_size  = 0;
                expect_seq = -1;

                if (data_head.packet_seq != 0) {
                    continue;
                }

                memcpy(recv_data, mesh_data.data, mesh_data.size);
            }

            /**
             * @brief Filter retransmitted packets
             */
            if (data_head.magic == s_data_magic) {
                expect_seq--;
                MDF_LOGD("Received duplicate packets, sequence: %d", s_data_magic);
                continue;
            }

            s_data_magic = data_head.magic;
            recv_size   += mesh_data.size;
            recv_data    = MDF_REALLOC(recv_data, data_head.total_size);
        }

        if (data_head.transmit_num || data_head.transmit_all) {
            mesh_addr_t *transmit_addr = NULL;
            size_t transmit_num        = data_head.transmit_num;
            uint8_t addr_any[] = MWIFI_ADDR_ANY;

            if (data_head.transmit_all) {
                transmit_num  = 1;
                transmit_addr = (mesh_addr_t *)addr_any;
            } else {
                transmit_addr = (mesh_addr_t *)recv_data;
                mesh_data.data = recv_data + data_head.transmit_num * MWIFI_ADDR_LEN;
                mesh_data.size = recv_size - data_head.transmit_num * MWIFI_ADDR_LEN;
            }

            MDF_LOGV("Data forwarding, size: %d, recv_size: %d, transmit_num: %d, data: %.*s",
                     mesh_data.size, recv_size, data_head.transmit_num, mesh_data.size, mesh_data.data);

            ret = mwifi_transmit_write(transmit_addr, transmit_num, &mesh_data,
                                       data_flag, &mesh_opt);
            MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> mwifi_root_write, size: %d",
                           mdf_err_to_name(ret), mesh_data.size);
        }

        if (data_head.transmit_self) {
            break;
        }
    }

    memcpy(data_type, &data_head.type, sizeof(mwifi_data_type_t));

    if (data_type->compression) {
        int mz_ret = uncompress((uint8_t *)data, (mz_ulong *)&mesh_data.size, mesh_data.data, recv_size);
        ret = (mz_ret == MZ_BUF_ERROR) ? MDF_ERR_BUF : MDF_FAIL;
        MDF_ERROR_GOTO(mz_ret != MZ_OK, EXIT, "Uncompress, ret: %0x", -mz_ret);
    } else {
        ret = (*size < recv_size) ? MDF_ERR_BUF : MDF_FAIL;
        MDF_ERROR_GOTO(*size < recv_size, EXIT,
                       "Buffer is too small, size: %d, the expected size is: %d", *size, recv_size);

        *size = mesh_data.size;
        memcpy(data, mesh_data.data, mesh_data.size);
    }

    ret = MDF_OK;

EXIT:
    MDF_FREE(recv_data);
    return ret;
}

mdf_err_t mwifi_root_write(const uint8_t *addrs_list, size_t addrs_num,
                           const mwifi_data_type_t *data_type, const void *data,
                           size_t size, bool block)
{
    MDF_PARAM_CHECK(addrs_list);
    MDF_PARAM_CHECK(addrs_num > 0);
    MDF_PARAM_CHECK(data_type);
    MDF_PARAM_CHECK(data);
    MDF_PARAM_CHECK(!MWIFI_ADDR_IS_EMPTY(addrs_list));
    MDF_PARAM_CHECK(size > 0 && size <= MWIFI_PAYLOAD_LEN);
    MDF_ERROR_CHECK(!mwifi_is_started(), MDF_ERR_MWIFI_NOT_START, "Mwifi isn't started");

    mdf_err_t ret = MDF_OK;
    int data_flag = MESH_DATA_FROMDS;
    uint8_t *compress_data = NULL;
    uint8_t *tmp_addrs = NULL;
    mwifi_data_head_t data_head = {
        .transmit_self = true,
    };
    mesh_data_t mesh_data = {
        .tos   = MESH_TOS_P2P,
        .data  = (uint8_t *)data,
        .size  = size,
    };
    mesh_opt_t mesh_opt   = {
        .len  = sizeof(mwifi_data_head_t),
        .val  = (void *) &data_head,
        .type = MESH_OPT_RECV_DS_ADDR,
    };

    data_flag = (!block) ? data_flag | MESH_DATA_NONBLOCK : data_flag;
    memcpy(&data_head.type, data_type, sizeof(mwifi_data_type_t));

    /**
     * @brief data compression
     */
    if (data_head.type.compression) {
        mz_ulong compress_size = compressBound(size);
        compress_data = MDF_MALLOC(compress_size);

        ret = compress(compress_data, &compress_size, (uint8_t *)data, size);
        MDF_ERROR_GOTO(ret != MZ_OK, EXIT, "Compressed whitelist failed, ret: 0x%x", -ret);

        MDF_LOGD("compress, size: %d, compress_size: %d, rate: %d%%",
                 size, (int)compress_size, (int)compress_size * 100 / size);

        if (compress_size > size) {
            data_head.type.compression = false;
        } else {
            mesh_data.data = compress_data;
            mesh_data.size = compress_size;
        }
    }

    if (data_type->communicate == MWIFI_COMMUNICATE_UNICAST) {
        if (MWIFI_ADDR_IS_ANY(addrs_list) || MWIFI_ADDR_IS_BROADCAST(addrs_list)) {
            if (data_type->communicate == MWIFI_COMMUNICATE_UNICAST) {
                addrs_num  = esp_mesh_get_routing_table_size();
                addrs_list = tmp_addrs = MDF_MALLOC(addrs_num * sizeof(mesh_addr_t));
                ESP_ERROR_CHECK(esp_mesh_get_routing_table((mesh_addr_t *)addrs_list,
                                addrs_num * sizeof(mesh_addr_t), (int *)&addrs_num));

                if (MWIFI_ADDR_IS_BROADCAST(addrs_list)) {
                    uint8_t root_mac[6] = {0x0};
                    ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_STA, root_mac));
                    addrs_remove((mesh_addr_t *)addrs_list, &addrs_num, (mesh_addr_t *)root_mac);
                    MDF_ERROR_GOTO(addrs_num > 2048 || addrs_num <= 0, EXIT, "dest_addrs_num: %d", addrs_num);
                }
            }
        }

        for (int i = 0; i < addrs_num; ++i) {
            MDF_LOGD("count: %d, dest_addr: " MACSTR" mesh_data.size: %d, data: %.*s",
                     i, MAC2STR(addrs_list + 6 * i), mesh_data.size, mesh_data.size, mesh_data.data);
            ret = mwifi_subcontract_write((mesh_addr_t *)addrs_list + i, &mesh_data, data_flag, &mesh_opt);
            MDF_ERROR_BREAK(ret != ESP_OK, "<%s> Root node failed to send packets, dest_mac: "MACSTR,
                            mdf_err_to_name(ret), MAC2STR(addrs_list));
        }
    } else if (data_type->communicate == MWIFI_COMMUNICATE_MULTICAST) {
        tmp_addrs = MDF_MALLOC(addrs_num * sizeof(mesh_addr_t));
        memcpy(tmp_addrs, addrs_list, addrs_num * sizeof(mesh_addr_t));
        MDF_LOGD("addrs_num: %d, addrs_list: " MACSTR ",mesh_data.size: %d, data: %.*s",
                 addrs_num, MAC2STR(tmp_addrs), mesh_data.size, mesh_data.size, mesh_data.data);
        ret = mwifi_transmit_write((mesh_addr_t *)tmp_addrs, addrs_num, &mesh_data,
                                   data_flag, &mesh_opt);
        MDF_ERROR_CHECK(ret != MDF_OK, ret, "Mwifi_transmit_write");
    } else {
        MDF_LOGE("<MDF_ERR_NOT_SUPPORTED> The current version does not support broadcasting.");
        return MDF_ERR_NOT_SUPPORTED;
    }

EXIT:
    MDF_FREE(tmp_addrs);
    MDF_FREE(compress_data);
    return ret;
}

mdf_err_t mwifi_root_read(uint8_t *src_addr, mwifi_data_type_t *data_type,
                          void *data, size_t *size, TickType_t wait_ticks)
{
    MDF_PARAM_CHECK(src_addr);
    MDF_PARAM_CHECK(data_type);
    MDF_PARAM_CHECK(data);
    MDF_PARAM_CHECK(size && *size > 0);
    MDF_ERROR_CHECK(!mwifi_is_started(), MDF_ERR_MWIFI_NOT_START, "Mwifi isn't started");

    mdf_err_t ret               = MDF_OK;
    int data_flag               = 0;
    mesh_addr_t dest_addr       = {0};
    mwifi_data_head_t data_head = {0x0};
    TickType_t start_ticks      = xTaskGetTickCount();
    ssize_t recv_size           = 0;
    uint8_t *recv_data          = MDF_MALLOC(MWIFI_PAYLOAD_LEN);

    mesh_data_t mesh_data = {0x0};
    mesh_opt_t mesh_opt   = {
        .len  = sizeof(mwifi_data_head_t),
        .val  = (void *) &data_head,
        .type = MESH_OPT_RECV_DS_ADDR,
    };

    for (int expect_seq = 0; !expect_seq || recv_size < data_head.total_size; expect_seq++) {
        mesh_data.size = !expect_seq ? MWIFI_PAYLOAD_LEN : data_head.total_size - recv_size;
        mesh_data.data = recv_data + recv_size;
        wait_ticks     = (wait_ticks == portMAX_DELAY) ? portMAX_DELAY :
                         xTaskGetTickCount() - start_ticks < wait_ticks ?
                         wait_ticks - (xTaskGetTickCount() - start_ticks) : 0;

        ret = esp_mesh_recv_toDS((mesh_addr_t *)src_addr, &dest_addr,
                                 &mesh_data, wait_ticks * portTICK_RATE_MS, &data_flag, &mesh_opt, 1);

        if (ret == ESP_ERR_MESH_NOT_START) {
            MDF_LOGW("<ESP_ERR_MESH_NOT_START> Node failed to receive packets");
            vTaskDelay(100 / portTICK_RATE_MS);
            continue;
        }

        MDF_ERROR_GOTO(ret != ESP_OK, EXIT, "<%s> Node failed to receive packets", mdf_err_to_name(ret));

        /**
         * @brief Discard this packet if there is a packet loss in the middle
         */
        if (data_head.packet_seq != expect_seq) {
            MDF_LOGW("Part of the packet is lost, expect_seq: %d, recv_seq: %d",
                     expect_seq, data_head.packet_seq);

            recv_size  = 0;
            expect_seq = -1;

            if (data_head.packet_seq != 0) {
                continue;
            }

            memcpy(recv_data, mesh_data.data, mesh_data.size);
        }

        recv_size += mesh_data.size;
        recv_data = MDF_REALLOC(recv_data, data_head.total_size);
    }

    memcpy(data_type, &data_head.type, sizeof(mwifi_data_type_t));

    if (data_type->compression) {
        int mz_ret = uncompress((uint8_t *)data, (mz_ulong *)size, recv_data, recv_size);
        ret = (mz_ret == MZ_BUF_ERROR) ? MDF_ERR_BUF : MDF_FAIL;
        MDF_ERROR_GOTO(mz_ret != MZ_OK, EXIT, "Uncompress, ret: %0x", -mz_ret);
    } else {
        ret = (*size < recv_size) ? MDF_ERR_BUF : MDF_FAIL;
        MDF_ERROR_GOTO(*size < recv_size, EXIT,
                       "Buffer is too small, size: %d, the expected size is: %d", *size, recv_size);

        *size = recv_size;
        memcpy(data, recv_data, recv_size);
    }

    ret = MDF_OK;

EXIT:
    MDF_FREE(recv_data);
    return ret;
}