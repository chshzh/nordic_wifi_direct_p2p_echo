/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wifi_p2p_utils, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <string.h>
#include <stdio.h>

#include "wifi_p2p_utils.h"

/* Helper function to format MAC address as string */
static inline char *format_mac_addr(const uint8_t *mac, char *buf, size_t buf_len)
{
	snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return buf;
}

/* P2P context */
static struct wifi_p2p_context p2p_ctx;

/* P2P event callbacks */
static struct net_mgmt_event_callback p2p_mgmt_cb;

/* Semaphores for P2P operations (event-driven) */
static K_SEM_DEFINE(p2p_find_sem, 0, 1);
static K_SEM_DEFINE(p2p_connect_sem, 0, 1);
static K_SEM_DEFINE(p2p_group_formed_sem, 0, 1);
static K_SEM_DEFINE(p2p_go_neg_request_sem, 0, 1);
static K_SEM_DEFINE(p2p_ap_sta_connected_sem, 0, 1);

/* User callback for P2P events */
static wifi_p2p_event_cb_t user_event_cb;

/* State strings */
static const char *const p2p_state_str[] = {
	[WIFI_P2P_STATE_IDLE] = "IDLE",
	[WIFI_P2P_STATE_FINDING] = "FINDING",
	[WIFI_P2P_STATE_FOUND] = "FOUND",
	[WIFI_P2P_STATE_CONNECTING] = "CONNECTING",
	[WIFI_P2P_STATE_CONNECTED] = "CONNECTED",
	[WIFI_P2P_STATE_GROUP_FORMED] = "GROUP_FORMED",
	[WIFI_P2P_STATE_ERROR] = "ERROR",
};

/* Role strings */
static const char *const p2p_role_str[] = {
	[WIFI_P2P_ROLE_UNDETERMINED] = "UNDETERMINED",
	[WIFI_P2P_ROLE_GO] = "GROUP_OWNER",
	[WIFI_P2P_ROLE_CLI] = "CLIENT",
};

const char *wifi_p2p_state_txt(enum wifi_p2p_state state)
{
	if (state < ARRAY_SIZE(p2p_state_str)) {
		return p2p_state_str[state];
	}
	return "UNKNOWN";
}

const char *wifi_p2p_role_txt(enum wifi_p2p_role role)
{
	if (role < ARRAY_SIZE(p2p_role_str)) {
		return p2p_role_str[role];
	}
	return "UNKNOWN";
}

static void notify_user_event(enum wifi_p2p_event event)
{
	if (user_event_cb) {
		user_event_cb(event, &p2p_ctx);
	}
}

static void handle_p2p_device_found(struct net_mgmt_event_callback *cb)
{
	const struct wifi_p2p_device_info *peer_info =
		(const struct wifi_p2p_device_info *)cb->info;

	if (peer_info == NULL) {
		LOG_WRN("P2P device found event with NULL info");
		return;
	}

	char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

	LOG_INF("P2P Device Found:");
	LOG_INF("  MAC: %s",
		format_mac_addr(peer_info->mac, mac_string_buf, sizeof(mac_string_buf)));
	LOG_INF("  Name: %s", peer_info->device_name);
	LOG_INF("  RSSI: %d dBm", peer_info->rssi);

	/* Store peer MAC for connection */
	memcpy(p2p_ctx.peer_mac, peer_info->mac, WIFI_MAC_ADDR_LEN);
	p2p_ctx.peer_count++;
	p2p_ctx.state = WIFI_P2P_STATE_FOUND;

	/* Signal that a peer was found */
	k_sem_give(&p2p_find_sem);

	/* Notify user callback */
	notify_user_event(WIFI_P2P_EVENT_DEVICE_FOUND);
}

static void handle_p2p_connect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (status->status == 0) {
		/* Only set role to CLI if not already determined as GO
		 * (AP_ENABLE_RESULT sets GO role before this event fires)
		 */
		if (p2p_ctx.role != WIFI_P2P_ROLE_GO) {
			LOG_INF("P2P connection successful (as Client)");
			p2p_ctx.role = WIFI_P2P_ROLE_CLI;
			/* Notify user callback - client connected */
			notify_user_event(WIFI_P2P_EVENT_CONNECTED);
		} else {
			LOG_INF("P2P connection successful (as GO - client connected)");
			/* Notify user callback - a client joined our group */
			notify_user_event(WIFI_P2P_EVENT_PEER_JOINED);
		}
		p2p_ctx.state = WIFI_P2P_STATE_CONNECTED;
		p2p_ctx.connected = true;
		p2p_ctx.group_formed = true;

		k_sem_give(&p2p_connect_sem);
		k_sem_give(&p2p_group_formed_sem);
	} else {
		/* During P2P WPS, there's a temporary disconnect after WPS completes
		 * but before the final connection with credentials. This triggers
		 * a CONNECT_RESULT with failure status. We should NOT treat this
		 * as a real failure - the client will automatically reconnect.
		 *
		 * Only report failure if we're not in the middle of P2P connection
		 * (state is CONNECTING) or if the group is already formed.
		 */
		if (p2p_ctx.state == WIFI_P2P_STATE_CONNECTING) {
			LOG_WRN("Ignoring intermediate disconnect during P2P WPS (status: %d)",
				status->status);
			/* Don't change state or signal semaphores - wait for actual result */
		} else {
			LOG_ERR("P2P connection failed: %d", status->status);
			p2p_ctx.state = WIFI_P2P_STATE_ERROR;

			/* Notify user callback */
			notify_user_event(WIFI_P2P_EVENT_CONNECT_FAILED);

			k_sem_give(&p2p_connect_sem);
			k_sem_give(&p2p_group_formed_sem);
		}
	}
}

static void handle_ap_enable_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (status->status == 0) {
		LOG_INF("P2P Group Owner mode enabled (AP mode)");
		p2p_ctx.role = WIFI_P2P_ROLE_GO;
		p2p_ctx.group_formed = true;
		p2p_ctx.state = WIFI_P2P_STATE_GROUP_FORMED;
		p2p_ctx.connected = true;

		/* CRITICAL: Bring interface up for L2 packet operations
		 * This is needed for WPA supplicant to send EAPOL packets during WPS.
		 * Without this, sendto() on AF_PACKET socket fails with ENETDOWN
		 * ("Network interface is not configured").
		 */
		struct net_if *iface = net_if_get_first_wifi();
		if (iface) {
			int ret;

			/* Set admin UP flag */
			ret = net_if_up(iface);
			if (ret && ret != -EALREADY) {
				LOG_WRN("Failed to bring interface up: %d", ret);
			}

			/* Set carrier/running flag - required for AP mode
			 * In AP mode, there's no external carrier detection,
			 * so we explicitly signal that the link is ready.
			 */
			net_if_carrier_on(iface);

			/* Clear dormant state - required for L2 packet operations */
			net_if_dormant_off(iface);

			LOG_DBG("Interface brought up for P2P GO L2 operations");
		}

		/* Signal that GO mode is ready */
		k_sem_give(&p2p_group_formed_sem);

		/* Notify user callback - GO is ready for clients */
		notify_user_event(WIFI_P2P_EVENT_GROUP_STARTED);
	} else {
		LOG_ERR("P2P Group Owner mode enable failed: %d", status->status);
		p2p_ctx.state = WIFI_P2P_STATE_ERROR;
		k_sem_give(&p2p_group_formed_sem);

		/* Notify user callback */
		notify_user_event(WIFI_P2P_EVENT_CONNECT_FAILED);
	}
}

static void handle_ap_sta_connected(struct net_mgmt_event_callback *cb)
{
	const struct wifi_ap_sta_info *sta_info =
		(const struct wifi_ap_sta_info *)cb->info;
	char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

	LOG_INF("P2P Client connected to GO:");
	LOG_INF("  MAC: %s",
		format_mac_addr(sta_info->mac, mac_string_buf, sizeof(mac_string_buf)));

	p2p_ctx.connected = true;
	memcpy(p2p_ctx.peer_mac, sta_info->mac, WIFI_MAC_ADDR_LEN);

	/* Notify user callback - a peer has joined our group */
	notify_user_event(WIFI_P2P_EVENT_PEER_JOINED);

	/* Signal AP-STA-CONNECTED for GO-side sequencing */
	k_sem_give(&p2p_ap_sta_connected_sem);
	notify_user_event(WIFI_P2P_EVENT_AP_STA_CONNECTED);
}

static void handle_ap_sta_disconnected(struct net_mgmt_event_callback *cb)
{
	const struct wifi_ap_sta_info *sta_info =
		(const struct wifi_ap_sta_info *)cb->info;
	char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

	LOG_INF("P2P Client disconnected from GO:");
	LOG_INF("  MAC: %s",
		format_mac_addr(sta_info->mac, mac_string_buf, sizeof(mac_string_buf)));

	if (memcmp(p2p_ctx.peer_mac, sta_info->mac, WIFI_MAC_ADDR_LEN) == 0) {
		p2p_ctx.connected = false;
		memset(p2p_ctx.peer_mac, 0, WIFI_MAC_ADDR_LEN);

		/* Notify user callback */
		notify_user_event(WIFI_P2P_EVENT_PEER_LEFT);
	}
}

static void p2p_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				   uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_P2P_DEVICE_FOUND:
		handle_p2p_device_found(cb);
		break;
	case NET_EVENT_WIFI_CONNECT_RESULT:
		handle_p2p_connect_result(cb);
		break;
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		handle_ap_enable_result(cb);
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		handle_ap_sta_connected(cb);
		break;
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
		handle_ap_sta_disconnected(cb);
		break;
	default:
		break;
	}
}

int wifi_p2p_init(void)
{
	int ret;

	/* Clear context */
	memset(&p2p_ctx, 0, sizeof(p2p_ctx));
	p2p_ctx.state = WIFI_P2P_STATE_IDLE;
	p2p_ctx.role = WIFI_P2P_ROLE_UNDETERMINED;

	/* Reset semaphores */
	k_sem_reset(&p2p_find_sem);
	k_sem_reset(&p2p_connect_sem);
	k_sem_reset(&p2p_group_formed_sem);
	k_sem_reset(&p2p_go_neg_request_sem);
	k_sem_reset(&p2p_ap_sta_connected_sem);

	/* CRITICAL: Bring interface up early for P2P operations
	 * This ensures the interface is ready for L2 packet operations
	 * before any P2P negotiation begins. Without this, WPA supplicant's
	 * sendto() on AF_PACKET socket fails with ENETDOWN.
	 *
	 * The nRF Wi-Fi driver initializes the interface as "dormant" which
	 * prevents L2 packet operations. We need to:
	 * 1. Call net_if_up() to set NET_IF_UP flag
	 * 2. Call net_if_carrier_on() to set NET_IF_RUNNING flag
	 * 3. Call net_if_dormant_off() to clear the dormant state
	 */
	struct net_if *iface = net_if_get_first_wifi();
	if (iface) {
		ret = net_if_up(iface);
		if (ret && ret != -EALREADY) {
			LOG_WRN("Failed to bring interface up: %d", ret);
		}

		/* Set carrier on - needed for net_if_is_up() to return true */
		net_if_carrier_on(iface);

		/* Clear dormant state - the driver sets this initially and
		 * only clears it when a connection is established. For P2P,
		 * we need the interface operational before the connection.
		 */
		net_if_dormant_off(iface);

		LOG_DBG("Wi-Fi interface brought up for P2P operations");
	}

	/* Register P2P event callbacks */
	net_mgmt_init_event_callback(&p2p_mgmt_cb, p2p_mgmt_event_handler,
				     NET_EVENT_WIFI_P2P_DEVICE_FOUND |
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_AP_ENABLE_RESULT |
				     NET_EVENT_WIFI_AP_STA_CONNECTED |
				     NET_EVENT_WIFI_AP_STA_DISCONNECTED);

	net_mgmt_add_event_callback(&p2p_mgmt_cb);

	LOG_INF("Wi-Fi P2P initialized");

	return 0;
}

void wifi_p2p_register_event_callback(wifi_p2p_event_cb_t cb)
{
	user_event_cb = cb;
}

int wifi_p2p_wait_for_peer(uint32_t timeout_ms)
{
	int ret;

	LOG_DBG("Waiting for P2P peer discovery...");

	ret = k_sem_take(&p2p_find_sem, K_MSEC(timeout_ms));
	if (ret == -EAGAIN) {
		LOG_WRN("Timeout waiting for P2P peer");
		return -ETIMEDOUT;
	}

	return 0;
}

int wifi_p2p_wait_for_group_formation(uint32_t timeout_ms)
{
	int ret;

	LOG_DBG("Waiting for P2P group formation...");

	ret = k_sem_take(&p2p_group_formed_sem, K_MSEC(timeout_ms));
	if (ret == -EAGAIN) {
		LOG_WRN("Timeout waiting for P2P group formation");
		return -ETIMEDOUT;
	}

	if (p2p_ctx.state == WIFI_P2P_STATE_ERROR) {
		LOG_ERR("P2P group formation failed");
		return -EIO;
	}

	return 0;
}

int wifi_p2p_wait_for_connection(uint32_t timeout_ms)
{
	int ret;

	LOG_DBG("Waiting for P2P connection...");

	ret = k_sem_take(&p2p_connect_sem, K_MSEC(timeout_ms));
	if (ret == -EAGAIN) {
		LOG_WRN("Timeout waiting for P2P connection");
		return -ETIMEDOUT;
	}

	if (p2p_ctx.state == WIFI_P2P_STATE_ERROR) {
		LOG_ERR("P2P connection failed");
		return -EIO;
	}

	return 0;
}

int wifi_p2p_wait_for_ap_sta_connected(uint32_t timeout_ms)
{
	int ret;

	LOG_DBG("Waiting for AP-STA-CONNECTED...");

	ret = k_sem_take(&p2p_ap_sta_connected_sem, K_MSEC(timeout_ms));
	if (ret == -EAGAIN) {
		LOG_WRN("Timeout waiting for AP-STA-CONNECTED");
		return -ETIMEDOUT;
	}

	return 0;
}

int wifi_p2p_find(uint16_t timeout_sec)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_p2p_params params = {0};
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	params.oper = WIFI_P2P_FIND;
	params.discovery_type = WIFI_P2P_FIND_START_WITH_FULL;
	params.timeout = timeout_sec;

	LOG_INF("Starting P2P device discovery (timeout: %d sec)...", timeout_sec);

	p2p_ctx.state = WIFI_P2P_STATE_FINDING;
	p2p_ctx.peer_count = 0;

	ret = net_mgmt(NET_REQUEST_WIFI_P2P_OPER, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("P2P find failed: %d", ret);
		p2p_ctx.state = WIFI_P2P_STATE_ERROR;
		return ret;
	}

	return 0;
}

int wifi_p2p_stop_find(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_p2p_params params = {0};
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	params.oper = WIFI_P2P_STOP_FIND;

	LOG_INF("Stopping P2P device discovery...");

	ret = net_mgmt(NET_REQUEST_WIFI_P2P_OPER, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("P2P stop find failed: %d", ret);
		return ret;
	}

	p2p_ctx.state = WIFI_P2P_STATE_IDLE;

	return 0;
}

int wifi_p2p_connect(const uint8_t *peer_mac, uint8_t go_intent, uint32_t freq)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_p2p_params params = {0};
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	if (!peer_mac) {
		LOG_ERR("Invalid peer MAC address");
		return -EINVAL;
	}

	params.oper = WIFI_P2P_CONNECT;
	memcpy(params.peer_addr, peer_mac, WIFI_MAC_ADDR_LEN);

#if defined(CONFIG_P2P_METHOD_PBC)
	params.connect.method = WIFI_P2P_METHOD_PBC;
#elif defined(CONFIG_P2P_METHOD_DISPLAY)
	params.connect.method = WIFI_P2P_METHOD_DISPLAY;
#elif defined(CONFIG_P2P_METHOD_KEYPAD)
	params.connect.method = WIFI_P2P_METHOD_KEYPAD;
#else
	params.connect.method = WIFI_P2P_METHOD_PBC;
#endif

	params.connect.go_intent = go_intent;
	params.connect.freq = freq;

	p2p_ctx.go_intent = go_intent;
	p2p_ctx.frequency = freq;

	char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

	LOG_INF("Connecting to P2P peer:");
	LOG_INF("  MAC: %s",
		format_mac_addr(peer_mac, mac_string_buf, sizeof(mac_string_buf)));
	LOG_INF("  GO Intent: %d", go_intent);
	LOG_INF("  Frequency: %d MHz", freq);
	LOG_INF("  Method: PBC");

	p2p_ctx.state = WIFI_P2P_STATE_CONNECTING;

	ret = net_mgmt(NET_REQUEST_WIFI_P2P_OPER, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("P2P connect failed: %d", ret);
		p2p_ctx.state = WIFI_P2P_STATE_ERROR;
		return ret;
	}

	/* Determine role based on GO intent */
	if (go_intent == 15) {
		p2p_ctx.role = WIFI_P2P_ROLE_GO;
		LOG_INF("Device will act as Group Owner (GO)");
	} else if (go_intent == 0) {
		p2p_ctx.role = WIFI_P2P_ROLE_CLI;
		LOG_INF("Device will act as Client");
	} else {
		LOG_INF("Role will be negotiated (GO intent: %d)", go_intent);
	}

	return 0;
}

int wifi_p2p_group_add(uint32_t freq)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_p2p_params params = {0};
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	params.oper = WIFI_P2P_GROUP_ADD;
	params.group_add.freq = freq;
	params.group_add.persistent = -1; /* Not persistent */

	LOG_INF("Creating P2P group as GO (freq: %d MHz)...", freq);

	ret = net_mgmt(NET_REQUEST_WIFI_P2P_OPER, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("P2P group add failed: %d", ret);
		return ret;
	}

	p2p_ctx.role = WIFI_P2P_ROLE_GO;
	p2p_ctx.frequency = freq;

	return 0;
}

int wifi_p2p_group_remove(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_p2p_params params = {0};
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	params.oper = WIFI_P2P_GROUP_REMOVE;
	/* Interface name will be determined automatically */

	LOG_INF("Removing P2P group...");

	ret = net_mgmt(NET_REQUEST_WIFI_P2P_OPER, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("P2P group remove failed: %d", ret);
		return ret;
	}

	p2p_ctx.group_formed = false;
	p2p_ctx.connected = false;
	p2p_ctx.state = WIFI_P2P_STATE_IDLE;

	return 0;
}

int wifi_p2p_get_peers(struct wifi_p2p_device_info *peers, uint16_t max_peers,
		       uint16_t *peer_count)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_p2p_params params = {0};
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	if (!peers || !peer_count) {
		return -EINVAL;
	}

	params.oper = WIFI_P2P_PEER;
	/* Broadcast MAC to list all peers */
	memset(params.peer_addr, 0xFF, WIFI_MAC_ADDR_LEN);
	params.discovered_only = true;
	params.peers = peers;
	params.peer_count = max_peers;

	ret = net_mgmt(NET_REQUEST_WIFI_P2P_OPER, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("P2P get peers failed: %d", ret);
		return ret;
	}

	*peer_count = params.peer_count;

	return 0;
}

struct wifi_p2p_context *wifi_p2p_get_context(void)
{
	return &p2p_ctx;
}

void wifi_p2p_print_status(void)
{
	char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

	LOG_INF("=== P2P Status ===");
	LOG_INF("State: %s", wifi_p2p_state_txt(p2p_ctx.state));
	LOG_INF("Role: %s", wifi_p2p_role_txt(p2p_ctx.role));
	LOG_INF("GO Intent: %d", p2p_ctx.go_intent);
	LOG_INF("Frequency: %d MHz", p2p_ctx.frequency);
	LOG_INF("Group Formed: %s", p2p_ctx.group_formed ? "Yes" : "No");
	LOG_INF("Connected: %s", p2p_ctx.connected ? "Yes" : "No");

	if (p2p_ctx.connected || p2p_ctx.peer_count > 0) {
		LOG_INF("Peer MAC: %s",
			format_mac_addr(p2p_ctx.peer_mac, mac_string_buf, sizeof(mac_string_buf)));
	}

	LOG_INF("Discovered Peers: %d", p2p_ctx.peer_count);
	LOG_INF("==================");
}

/**
 * @brief Parse MAC address string to bytes
 *
 * @param mac_str MAC address string (format: "xx:xx:xx:xx:xx:xx")
 * @param mac_bytes Output buffer for 6 MAC bytes
 * @return 0 on success, -1 on failure
 */
static int parse_mac_address(const char *mac_str, uint8_t *mac_bytes)
{
	unsigned int bytes[6];
	int ret;

	ret = sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
		     &bytes[0], &bytes[1], &bytes[2],
		     &bytes[3], &bytes[4], &bytes[5]);

	if (ret != 6) {
		return -1;
	}

	for (int i = 0; i < 6; i++) {
		mac_bytes[i] = (uint8_t)bytes[i];
	}

	return 0;
}

struct wifi_p2p_device_info *wifi_p2p_find_peer_by_mac(
	struct wifi_p2p_device_info *peers,
	uint16_t peer_count,
	const char *mac_filter)
{
	char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];
	uint8_t filter_mac[6];

	if (!peers || peer_count == 0 || !mac_filter) {
		return NULL;
	}

	/* If filter is empty, return first peer */
	if (mac_filter[0] == '\0') {
		return &peers[0];
	}

	/* Parse the MAC filter string */
	if (parse_mac_address(mac_filter, filter_mac) != 0) {
		LOG_ERR("Invalid MAC address format: %s", mac_filter);
		return NULL;
	}

	for (int i = 0; i < peer_count; i++) {
		/* Check if peer MAC matches the filter */
		if (memcmp(peers[i].mac, filter_mac, 6) == 0) {
			LOG_INF("Found matching peer: %s",
				format_mac_addr(peers[i].mac, mac_string_buf,
						sizeof(mac_string_buf)));
			return &peers[i];
		}
	}

	LOG_WRN("No peer found matching MAC filter: %s", mac_filter);
	return NULL;
}
