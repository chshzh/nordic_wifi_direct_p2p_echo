/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef WIFI_P2P_UTILS_H_
#define WIFI_P2P_UTILS_H_

#include <zephyr/kernel.h>
#include <zephyr/net/wifi_mgmt.h>

#ifdef __cplusplus
extern "C" {
#endif

/** P2P device role */
enum wifi_p2p_role {
	WIFI_P2P_ROLE_UNDETERMINED = 0,
	WIFI_P2P_ROLE_GO,   /* Group Owner */
	WIFI_P2P_ROLE_CLI,  /* Client */
};

/** P2P connection state */
enum wifi_p2p_state {
	WIFI_P2P_STATE_IDLE = 0,
	WIFI_P2P_STATE_FINDING,
	WIFI_P2P_STATE_FOUND,
	WIFI_P2P_STATE_CONNECTING,
	WIFI_P2P_STATE_CONNECTED,
	WIFI_P2P_STATE_GROUP_FORMED,
	WIFI_P2P_STATE_ERROR,
};

/** P2P events for user callback */
enum wifi_p2p_event {
	WIFI_P2P_EVENT_DEVICE_FOUND = 0,  /* P2P device discovered */
	WIFI_P2P_EVENT_GROUP_STARTED,     /* P2P group formed (as GO) */
	WIFI_P2P_EVENT_CONNECTED,         /* Connected to P2P group (as Client) */
	WIFI_P2P_EVENT_CONNECT_FAILED,    /* Connection/group formation failed */
	WIFI_P2P_EVENT_PEER_JOINED,       /* Peer joined our group (GO only) */
	WIFI_P2P_EVENT_AP_STA_CONNECTED,  /* AP-STA-CONNECTED received (GO only) */
	WIFI_P2P_EVENT_PEER_LEFT,         /* Peer left our group (GO only) */
	WIFI_P2P_EVENT_DISCONNECTED,      /* Disconnected from group */
};

/** P2P context structure */
struct wifi_p2p_context {
	/** Current P2P state */
	enum wifi_p2p_state state;
	/** Device role (GO or Client) */
	enum wifi_p2p_role role;
	/** Peer device MAC address */
	uint8_t peer_mac[WIFI_MAC_ADDR_LEN];
	/** Own device MAC address */
	uint8_t own_mac[WIFI_MAC_ADDR_LEN];
	/** Number of discovered peers */
	uint16_t peer_count;
	/** GO intent value (0-15) */
	uint8_t go_intent;
	/** Operating frequency in MHz */
	uint32_t frequency;
	/** Group formed flag */
	bool group_formed;
	/** Connection established flag */
	bool connected;
};

/**
 * @brief P2P event callback type
 *
 * @param event The P2P event that occurred
 * @param ctx Current P2P context
 */
typedef void (*wifi_p2p_event_cb_t)(enum wifi_p2p_event event,
				    struct wifi_p2p_context *ctx);

/**
 * @brief Initialize Wi-Fi P2P subsystem
 *
 * @return 0 on success, negative error code on failure
 */
int wifi_p2p_init(void);

/**
 * @brief Register callback for P2P events
 *
 * @param cb Callback function to register
 */
void wifi_p2p_register_event_callback(wifi_p2p_event_cb_t cb);

/**
 * @brief Wait for P2P peer discovery (event-driven)
 *
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int wifi_p2p_wait_for_peer(uint32_t timeout_ms);

/**
 * @brief Wait for P2P group formation (event-driven)
 *
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -ETIMEDOUT on timeout, -EIO on failure
 */
int wifi_p2p_wait_for_group_formation(uint32_t timeout_ms);

/**
 * @brief Wait for P2P connection (event-driven)
 *
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -ETIMEDOUT on timeout, -EIO on failure
 */
int wifi_p2p_wait_for_connection(uint32_t timeout_ms);

/**
 * @brief Wait for AP-STA-CONNECTED event (GO only)
 *
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int wifi_p2p_wait_for_ap_sta_connected(uint32_t timeout_ms);

/**
 * @brief Start P2P device discovery
 *
 * @param timeout_sec Discovery timeout in seconds (0 = no timeout)
 * @return 0 on success, negative error code on failure
 */
int wifi_p2p_find(uint16_t timeout_sec);

/**
 * @brief Stop P2P device discovery
 *
 * @return 0 on success, negative error code on failure
 */
int wifi_p2p_stop_find(void);

/**
 * @brief Connect to a P2P peer device
 *
 * @param peer_mac MAC address of the peer device
 * @param go_intent GO intent value (0-15, higher = more likely to be GO)
 * @param freq Operating frequency in MHz (0 = auto)
 * @return 0 on success, negative error code on failure
 */
int wifi_p2p_connect(const uint8_t *peer_mac, uint8_t go_intent, uint32_t freq);

/**
 * @brief Create a P2P group as Group Owner
 *
 * @param freq Operating frequency in MHz (0 = auto)
 * @return 0 on success, negative error code on failure
 */
int wifi_p2p_group_add(uint32_t freq);

/**
 * @brief Remove P2P group
 *
 * @return 0 on success, negative error code on failure
 */
int wifi_p2p_group_remove(void);

/**
 * @brief Get list of discovered P2P peers
 *
 * @param peers Array to store peer information
 * @param max_peers Maximum number of peers to retrieve
 * @param peer_count Output: actual number of peers found
 * @return 0 on success, negative error code on failure
 */
int wifi_p2p_get_peers(struct wifi_p2p_device_info *peers, uint16_t max_peers,
		       uint16_t *peer_count);

/**
 * @brief Get current P2P context
 *
 * @return Pointer to the P2P context structure
 */
struct wifi_p2p_context *wifi_p2p_get_context(void);

/**
 * @brief Get P2P state as string
 *
 * @param state P2P state enum value
 * @return String representation of the state
 */
const char *wifi_p2p_state_txt(enum wifi_p2p_state state);

/**
 * @brief Get P2P role as string
 *
 * @param role P2P role enum value
 * @return String representation of the role
 */
const char *wifi_p2p_role_txt(enum wifi_p2p_role role);

/**
 * @brief Print P2P status information
 */
void wifi_p2p_print_status(void);

/**
 * @brief Find a peer by MAC address filter
 *
 * @param peers Array of discovered peers
 * @param peer_count Number of peers in array
 * @param mac_filter MAC address filter string (format: "xx:xx:xx:xx:xx:xx")
 *                   Empty string means return first peer
 * @return Pointer to matching peer, or NULL if not found
 */
struct wifi_p2p_device_info *wifi_p2p_find_peer_by_mac(
	struct wifi_p2p_device_info *peers,
	uint16_t peer_count,
	const char *mac_filter);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_P2P_UTILS_H_ */
