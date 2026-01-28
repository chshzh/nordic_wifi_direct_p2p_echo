/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief Nordic Wi-Fi Direct P2P Simple Connection Demo
 *
 * This application demonstrates Wi-Fi Direct (P2P) connection between two
 * Nordic devices. Press Button 1 simultaneously on both devices to initiate
 * pairing. One device will become the Group Owner (GO), the other will be
 * the Client (CLI).
 *
 * After connection:
 * - GO runs UDP echo server on port 5001
 * - Client sends UDP packets to GO and receives echo responses
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <dk_buttons_and_leds.h>
#include <stdio.h>

#include <net/wifi_ready.h>

#include "wifi_p2p_utils.h"
#include "net_utils.h"
#include "udp_utils.h"

/* Helper function to format MAC address as string */
static inline char *format_mac_addr(const uint8_t *mac, char *buf, size_t buf_len)
{
	snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return buf;
}

/* LED definitions */
#define LED_P2P_FINDING    DK_LED1
#define LED_P2P_CONNECTED  DK_LED2
#define LED_GO_ROLE        DK_LED3
#define LED_CLI_ROLE       DK_LED4

/* Button definitions */
#define BUTTON_P2P_START   DK_BTN1_MSK
#define BUTTON_STOP_ECHO   DK_BTN2_MSK

/* Thread stack sizes */
#define UDP_ECHO_STACK_SIZE 4096

/* Work queue for P2P operations */
static struct k_work p2p_start_work;
static struct k_work p2p_connect_work;

/* Wi-Fi ready semaphore */
static K_SEM_DEFINE(wifi_ready_sem, 0, 1);
static bool wifi_ready_status;

/* P2P discovery result storage */
static struct wifi_p2p_device_info discovered_peers[CONFIG_WIFI_P2P_MAX_PEERS];
static uint16_t discovered_peer_count;

/* Connection state */
static bool p2p_pairing_in_progress;

/* DHCP bound handling (CLI) */
static struct k_work dhcp_bound_work;
static struct net_if *dhcp_bound_iface;
static bool dhcp_bound_handled;

/* LED blink work */
static struct k_work_delayable led_blink_work;
static bool led_blink_state;

/* UDP Echo state */
static int udp_socket = -1;
static struct sockaddr_in server_addr;
static volatile bool udp_echo_stop_flag;
static struct udp_echo_stats echo_stats;

/* UDP Echo threads */
static K_THREAD_STACK_DEFINE(udp_server_stack, UDP_ECHO_STACK_SIZE);
static struct k_thread udp_server_thread;
static k_tid_t udp_server_tid;

static K_THREAD_STACK_DEFINE(udp_client_stack, UDP_ECHO_STACK_SIZE);
static struct k_thread udp_client_thread;
static k_tid_t udp_client_tid;

/* Forward declarations */
static void udp_echo_server_thread_fn(void *p1, void *p2, void *p3);
static void udp_echo_client_thread_fn(void *p1, void *p2, void *p3);
static void start_udp_echo_client(const char *server_ip);

static void led_blink_handler(struct k_work *work)
{
	struct wifi_p2p_context *ctx = wifi_p2p_get_context();

	if (ctx->state == WIFI_P2P_STATE_FINDING ||
	    ctx->state == WIFI_P2P_STATE_CONNECTING) {
		led_blink_state = !led_blink_state;
		dk_set_led(LED_P2P_FINDING, led_blink_state);
		k_work_schedule(&led_blink_work, K_MSEC(250));
	} else {
		dk_set_led(LED_P2P_FINDING, 0);
	}
}

static void dhcp_bound_handler(struct k_work *work)
{
	struct net_if *iface = dhcp_bound_iface;

	if (!iface) {
		return;
	}

	LOG_INF("IP address obtained from DHCP");
	net_utils_print_status(iface);

	/* Give GO some time to start its echo server */
	k_sleep(K_MSEC(CONFIG_P2P_CLIENT_CONNECT_DELAY_MS));

	/* Start UDP echo client - connect to GO's IP */
	start_udp_echo_client(CONFIG_P2P_GO_IP_ADDRESS);
}

static void dhcp_bound_cb(struct net_if *iface)
{
	if (dhcp_bound_handled) {
		return;
	}

	dhcp_bound_handled = true;
	dhcp_bound_iface = iface;
	k_work_submit(&dhcp_bound_work);
}

static void update_leds(void)
{
	struct wifi_p2p_context *ctx = wifi_p2p_get_context();

	/* Update connection LED */
	dk_set_led(LED_P2P_CONNECTED, ctx->connected ? 1 : 0);

	/* Update role LEDs */
	if (ctx->role == WIFI_P2P_ROLE_GO) {
		dk_set_led(LED_GO_ROLE, 1);
		dk_set_led(LED_CLI_ROLE, 0);
	} else if (ctx->role == WIFI_P2P_ROLE_CLI) {
		dk_set_led(LED_GO_ROLE, 0);
		dk_set_led(LED_CLI_ROLE, 1);
	} else {
		dk_set_led(LED_GO_ROLE, 0);
		dk_set_led(LED_CLI_ROLE, 0);
	}
}

static void start_udp_echo_server(void)
{
	int ret;

	LOG_INF("Starting UDP Echo Server on port %d...", CONFIG_UDP_ECHO_PORT);

	/* Initialize UDP server */
	ret = udp_server_init(&udp_socket, CONFIG_UDP_ECHO_PORT);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UDP server: %d", ret);
		return;
	}

	/* Reset stats and stop flag */
	udp_echo_reset_stats(&echo_stats);
	udp_echo_stop_flag = false;

	/* Create and start UDP server thread */
	udp_server_tid = k_thread_create(&udp_server_thread,
					 udp_server_stack,
					 K_THREAD_STACK_SIZEOF(udp_server_stack),
					 udp_echo_server_thread_fn,
					 NULL, NULL, NULL,
					 K_PRIO_PREEMPT(8), 0, K_NO_WAIT);

	k_thread_name_set(udp_server_tid, "udp_echo_server");

	LOG_INF("UDP Echo Server started!");
	LOG_INF("Waiting for Client to send packets...");
}

static void start_udp_echo_client(const char *server_ip)
{
	int ret;

	LOG_INF("Starting UDP Echo Client...");
	LOG_INF("Target: %s:%d", server_ip, CONFIG_UDP_ECHO_PORT);

	/* Initialize UDP client */
	ret = udp_client_init(&udp_socket, &server_addr, server_ip,
			      CONFIG_UDP_ECHO_PORT);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UDP client: %d", ret);
		return;
	}

	/* Reset stats and stop flag */
	udp_echo_reset_stats(&echo_stats);
	udp_echo_stop_flag = false;

	/* Create and start UDP client thread */
	udp_client_tid = k_thread_create(&udp_client_thread,
					 udp_client_stack,
					 K_THREAD_STACK_SIZEOF(udp_client_stack),
					 udp_echo_client_thread_fn,
					 NULL, NULL, NULL,
					 K_PRIO_PREEMPT(8), 0, K_NO_WAIT);

	k_thread_name_set(udp_client_tid, "udp_echo_client");

	LOG_INF("UDP Echo Client started!");
}

static void stop_udp_echo(void)
{
	LOG_INF("Stopping UDP Echo...");

	/* Set stop flag */
	udp_echo_stop_flag = true;

	/* Wait a bit for threads to exit */
	k_sleep(K_MSEC(500));

	/* Close socket */
	if (udp_socket >= 0) {
		udp_client_cleanup(udp_socket);
		udp_socket = -1;
	}

	/* Print final statistics */
	udp_echo_print_stats(&echo_stats);

	LOG_INF("UDP Echo stopped");
}

static void udp_echo_server_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	udp_echo_server_run(udp_socket, &echo_stats, &udp_echo_stop_flag);
}

static void udp_echo_client_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	udp_echo_client_run(udp_socket, &server_addr,
			    CONFIG_UDP_ECHO_PACKET_SIZE,
			    CONFIG_UDP_ECHO_INTERVAL_MS,
			    CONFIG_UDP_ECHO_COUNT,
			    &echo_stats, &udp_echo_stop_flag);

	/* Print stats when done */
	udp_echo_print_stats(&echo_stats);
}

static void setup_go_network(void)
{
	struct net_if *iface = net_utils_get_wifi_iface();
	int ret;

	/* Configure GO network AFTER P2P group is formed.
	 * This follows the same pattern as the wifi shell sample:
	 * 1. P2P connect completes
	 * 2. Manually configure IP with 'net ipv4 add'
	 * 3. Start DHCP server
	 */
	LOG_INF("Configuring GO network...");

	/* Configure IP address for GO */
	ret = net_utils_configure_go_ip(iface,
					CONFIG_P2P_GO_IP_ADDRESS,
					CONFIG_P2P_GO_IP_NETMASK);
	if (ret < 0) {
		LOG_ERR("Failed to configure GO IP: %d", ret);
	}

	/* Start DHCP server */
	ret = net_utils_start_dhcp_server(iface,
					  CONFIG_P2P_DHCP_SERVER_POOL_START);
	if (ret < 0) {
		LOG_ERR("Failed to start DHCP server: %d", ret);
	}

	LOG_INF("=================================");
	LOG_INF("Group Owner network ready!");
	LOG_INF("GO IP: %s", CONFIG_P2P_GO_IP_ADDRESS);
	LOG_INF("DHCP Pool: %s", CONFIG_P2P_DHCP_SERVER_POOL_START);
	LOG_INF("=================================");

	net_utils_print_status(iface);

	/* Start UDP echo server */
	start_udp_echo_server();
}

static void p2p_connect_handler(struct k_work *work)
{
	struct wifi_p2p_context *ctx = wifi_p2p_get_context();
	struct wifi_p2p_device_info *target_peer;
	int ret;

	if (discovered_peer_count == 0) {
		LOG_WRN("No peers discovered, cannot connect");
		p2p_pairing_in_progress = false;
		return;
	}

	/* Stop discovery before connecting */
	wifi_p2p_stop_find();

	/* Allow time for P2P-FIND-STOPPED before connecting */
	LOG_INF("Waiting for P2P-FIND-STOPPED...");
	k_sleep(K_MSEC(CONFIG_P2P_FIND_STOP_DELAY_MS));

	/* Find peer by MAC filter (if configured). Otherwise use highest RSSI. */
	if (CONFIG_P2P_TARGET_PEER_MAC[0] == '\0') {
		int best_idx = -1;
		int best_rssi = INT_MIN;

		if (discovered_peer_count > 1) {
			LOG_WRN("Multiple P2P peers found. Auto-selecting highest RSSI; set CONFIG_P2P_TARGET_PEER_MAC to force a specific peer.");
		}

		for (int i = 0; i < discovered_peer_count; i++) {
			if (discovered_peers[i].rssi > best_rssi) {
				best_rssi = discovered_peers[i].rssi;
				best_idx = i;
			}
		}

		target_peer = (best_idx >= 0) ? &discovered_peers[best_idx] : NULL;
	} else {
		target_peer = wifi_p2p_find_peer_by_mac(discovered_peers,
							discovered_peer_count,
							CONFIG_P2P_TARGET_PEER_MAC);
	}
	if (!target_peer) {
		char mac_str[sizeof("xx:xx:xx:xx:xx:xx")];

		LOG_ERR("No peer found matching MAC filter: '%s'", CONFIG_P2P_TARGET_PEER_MAC);
		LOG_INF("Available peers:");
		for (int i = 0; i < discovered_peer_count; i++) {
			LOG_INF("  [%d] %s", i,
				format_mac_addr(discovered_peers[i].mac, mac_str, sizeof(mac_str)));
		}
		p2p_pairing_in_progress = false;
		return;
	}

	uint8_t *peer_mac = target_peer->mac;
	uint8_t go_intent = CONFIG_P2P_GO_INTENT;
	uint32_t freq = CONFIG_P2P_OPERATING_FREQUENCY;

	LOG_INF("Attempting P2P connection with peer...");
	LOG_INF("Peer: %s", target_peer->device_name);
	LOG_INF("GO Intent: %d (15=GO, 0=Client)", go_intent);

	/* If we are CLI, wait for GO negotiation request before connecting.
	 * The wifi shell shows P2P-GO-NEG-REQUEST before the CLI initiates
	 * wifi p2p connect. This delay mirrors that sequence.
	 */
	if (go_intent == 0) {
		LOG_INF("Waiting for GO negotiation request...");
		k_sleep(K_MSEC(CONFIG_P2P_GO_NEG_REQUEST_WAIT_MS));
	}

	/* NOTE: Do NOT configure IP or call net_if_up() before P2P connect!
	 * The P2P subsystem manages the interface state internally.
	 * Interfering with interface state before P2P can cause WPS to fail.
	 * 
	 * The wifi shell sample works by NOT configuring anything before
	 * wifi p2p connect - IP is configured manually AFTER the P2P group forms.
	 */

	ret = wifi_p2p_connect(peer_mac, go_intent, freq);
	if (ret < 0) {
		LOG_ERR("P2P connect failed: %d", ret);
		p2p_pairing_in_progress = false;
		return;
	}

	/* Wait for P2P group formation (event-driven) */
	LOG_INF("Waiting for P2P group formation...");
	ret = wifi_p2p_wait_for_group_formation(CONFIG_P2P_GROUP_FORMATION_TIMEOUT_MS);
	if (ret < 0) {
		LOG_ERR("P2P group formation failed or timed out: %d", ret);
		p2p_pairing_in_progress = false;
		return;
	}

	/* Check the role we got from the negotiation */
	LOG_INF("P2P group formed!");
	LOG_INF("Role: %s", wifi_p2p_role_txt(ctx->role));

	/* Handle based on actual role (determined by negotiation) */
	if (ctx->role == WIFI_P2P_ROLE_GO) {
		/* We became Group Owner - wait for AP-STA-CONNECTED and 4-way handshake */
		LOG_INF("Waiting for AP-STA-CONNECTED...");
		ret = wifi_p2p_wait_for_ap_sta_connected(
			CONFIG_P2P_AP_STA_CONNECTED_TIMEOUT_MS);
		if (ret < 0) {
			LOG_WRN("AP-STA-CONNECTED not received, continuing anyway");
		}

		LOG_INF("Waiting for EAPOL 4-way handshake to complete...");
		k_sleep(K_MSEC(CONFIG_P2P_4WAY_HANDSHAKE_WAIT_MS));

		/* Now configure GO network and start DHCP server */
		setup_go_network();
	} else if (ctx->role == WIFI_P2P_ROLE_CLI) {
		/* We became Client - get IP from GO's DHCP server */
		struct net_if *iface = net_utils_get_wifi_iface();

		LOG_INF("P2P connection complete - starting DHCP client to get IP from GO...");

#if CONFIG_P2P_DHCP_START_DELAY_MS > 0
		/* Optional delay to wait for GO to start DHCP server */
		LOG_INF("Waiting %d ms for GO to start DHCP server...",
			CONFIG_P2P_DHCP_START_DELAY_MS);
		k_sleep(K_MSEC(CONFIG_P2P_DHCP_START_DELAY_MS));
#endif

		/* Register DHCP callback BEFORE starting DHCP client
		 * to ensure we don't miss the DHCP_BOUND event
		 */
		dhcp_bound_handled = false;
		dhcp_bound_iface = iface;
		net_utils_set_dhcp_bound_cb(dhcp_bound_cb);
		net_utils_register_dhcp_callback();

		/* Start DHCP client to get IP from GO */
		net_dhcpv4_start(iface);
		LOG_INF("DHCP client started - waiting for DHCP bound event...");
	} else {
		LOG_WRN("P2P role undetermined after connection");
	}

	wifi_p2p_print_status();
	update_leds();
	p2p_pairing_in_progress = false;
}

static void p2p_event_handler(enum wifi_p2p_event event, struct wifi_p2p_context *ctx)
{
	/* NOTE: Do NOT configure IP or DHCP in event handlers!
	 * The wifi shell works by configuring IP AFTER the P2P connection
	 * is fully established, not during the WPS handshake.
	 * Configuring during WPS can interfere with the handshake.
	 */
	switch (event) {
	case WIFI_P2P_EVENT_DEVICE_FOUND:
		LOG_INF("Event: P2P device found");
		break;
	case WIFI_P2P_EVENT_GROUP_STARTED:
		LOG_INF("Event: P2P group started (we are GO)");
		break;
	case WIFI_P2P_EVENT_CONNECTED:
		LOG_INF("Event: Connected to P2P group (we are Client)");
		break;
	case WIFI_P2P_EVENT_CONNECT_FAILED:
		LOG_ERR("Event: P2P connection failed");
		break;
	case WIFI_P2P_EVENT_PEER_JOINED:
		LOG_INF("Event: Peer joined our group");
		break;
	case WIFI_P2P_EVENT_AP_STA_CONNECTED:
		LOG_INF("Event: AP-STA-CONNECTED received");
		break;
	case WIFI_P2P_EVENT_PEER_LEFT:
		LOG_INF("Event: Peer left our group");
		stop_udp_echo();
		break;
	case WIFI_P2P_EVENT_DISCONNECTED:
		LOG_INF("Event: Disconnected from P2P group");
		stop_udp_echo();
		break;
	default:
		break;
	}

	/* Update LEDs based on current state */
	update_leds();
}

static void p2p_start_handler(struct k_work *work)
{
	struct wifi_p2p_context *ctx = wifi_p2p_get_context();
	int ret;

	if (p2p_pairing_in_progress) {
		LOG_WRN("P2P pairing already in progress");
		return;
	}

	p2p_pairing_in_progress = true;
	discovered_peer_count = 0;

	LOG_INF("========================================");
	LOG_INF("Starting Wi-Fi Direct P2P Pairing...");
	LOG_INF("GO Intent: %d (15=GO, 0=Client)", CONFIG_P2P_GO_INTENT);
	LOG_INF("Target MAC: %s", CONFIG_P2P_TARGET_PEER_MAC[0] ?
		CONFIG_P2P_TARGET_PEER_MAC : "(any peer)");
	LOG_INF("========================================");

	/* Start LED blinking */
	k_work_schedule(&led_blink_work, K_NO_WAIT);

	/* Start P2P discovery */
	ret = wifi_p2p_find(CONFIG_P2P_DISCOVERY_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to start P2P discovery: %d", ret);
		p2p_pairing_in_progress = false;
		return;
	}

	/* Wait for peer discovery - use full timeout to find target peer
	 * In environments with multiple P2P devices, we need to wait long enough
	 * to discover the specific target peer, not just any peer.
	 */
	LOG_INF("Searching for P2P peers (waiting %d ms)...",
		CONFIG_P2P_DISCOVERY_WAIT_MS);

	/* Wait for the full discovery period */
	k_sleep(K_MSEC(CONFIG_P2P_DISCOVERY_WAIT_MS));

	/* Get discovered peers */
	ret = wifi_p2p_get_peers(discovered_peers, CONFIG_WIFI_P2P_MAX_PEERS,
				 &discovered_peer_count);
	if (ret < 0) {
		LOG_WRN("Failed to get peer list: %d", ret);
	}

	LOG_INF("Found %d P2P peer(s)", discovered_peer_count);

	/* Print discovered peers */
	for (int i = 0; i < discovered_peer_count; i++) {
		char mac_str[sizeof("xx:xx:xx:xx:xx:xx")];

		LOG_INF("Peer %d:", i + 1);
		LOG_INF("  Name: %s", discovered_peers[i].device_name);
		LOG_INF("  MAC: %s",
			format_mac_addr(discovered_peers[i].mac, mac_str, sizeof(mac_str)));
		LOG_INF("  RSSI: %d dBm", discovered_peers[i].rssi);
	}

	/* If peers found, initiate connection */
	if (discovered_peer_count > 0 || ctx->state == WIFI_P2P_STATE_FOUND) {
		LOG_INF("Peer found! Initiating connection...");
		k_work_submit(&p2p_connect_work);
	} else {
		LOG_INF("No peers found. Press BUTTON 0 on both devices simultaneously.");
		p2p_pairing_in_progress = false;
	}
}

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	if ((has_changed & BUTTON_P2P_START) && (button_state & BUTTON_P2P_START)) {
		struct wifi_p2p_context *ctx = wifi_p2p_get_context();

		if (!ctx->connected) {
			LOG_INF("BUTTON 0 pressed - Starting P2P pairing");
			k_work_submit(&p2p_start_work);
		} else {
			LOG_INF("BUTTON 0 pressed - Print UDP Echo stats");
			udp_echo_print_stats(&echo_stats);
		}
	}

	if ((has_changed & BUTTON_STOP_ECHO) && (button_state & BUTTON_STOP_ECHO)) {
		LOG_INF("BUTTON 1 pressed - Stop UDP Echo");
		stop_udp_echo();
	}
}

static void wifi_ready_cb(bool ready)
{
	LOG_INF("Wi-Fi ready: %s", ready ? "yes" : "no");
	wifi_ready_status = ready;
	k_sem_give(&wifi_ready_sem);
}

static int register_wifi_ready(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	wifi_ready_callback_t cb;
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	cb.wifi_ready_cb = wifi_ready_cb;

	ret = register_wifi_ready_callback(cb, iface);
	if (ret) {
		LOG_ERR("Failed to register Wi-Fi ready callback: %d", ret);
		return ret;
	}

	return 0;
}

static int init_leds_and_buttons(void)
{
	int ret;

	ret = dk_leds_init();
	if (ret) {
		LOG_ERR("Failed to initialize LEDs: %d", ret);
		return ret;
	}

	ret = dk_buttons_init(button_handler);
	if (ret) {
		LOG_ERR("Failed to initialize buttons: %d", ret);
		return ret;
	}

	/* Turn off all LEDs initially */
	dk_set_leds(0);

	return 0;
}

void start_wifi_thread(void);

#define THREAD_PRIORITY K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
K_THREAD_DEFINE(start_wifi_thread_id, CONFIG_P2P_SAMPLE_START_WIFI_THREAD_STACK_SIZE,
		start_wifi_thread, NULL, NULL, NULL,
		THREAD_PRIORITY, 0, -1);

void start_wifi_thread(void)
{
	int ret;

	while (1) {
		LOG_INF("Waiting for Wi-Fi to be ready...");

		ret = k_sem_take(&wifi_ready_sem, K_FOREVER);
		if (ret) {
			LOG_ERR("Failed to wait for Wi-Fi ready: %d", ret);
			return;
		}

		if (!wifi_ready_status) {
			LOG_WRN("Wi-Fi not ready");
			continue;
		}

		LOG_INF("Wi-Fi is ready!");

		/* Initialize P2P subsystem */
		ret = wifi_p2p_init();
		if (ret) {
			LOG_ERR("Failed to initialize P2P: %d", ret);
			return;
		}

		/* Register P2P event callback */
		wifi_p2p_register_event_callback(p2p_event_handler);

		LOG_INF("============================================");
		LOG_INF("Nordic Wi-Fi Direct P2P Echo Demo Ready");
		LOG_INF("============================================");
		LOG_INF("");
		LOG_INF("Press BUTTON 0 to start P2P pairing");
		LOG_INF("Press on both devices simultaneously!");
		LOG_INF("");
		LOG_INF("After connection:");
		LOG_INF("  GO  -> UDP Echo Server on port %d", CONFIG_UDP_ECHO_PORT);
		LOG_INF("  CLI -> Sends packets, measures RTT");
		LOG_INF("");
		LOG_INF("BUTTON 0: Start pairing / Print stats");
		LOG_INF("BUTTON 1: Stop UDP Echo");
		LOG_INF("");
		LOG_INF("LED1: P2P Discovery (blink)");
		LOG_INF("LED2: P2P Connected");
		LOG_INF("LED3: Group Owner (GO)");
		LOG_INF("LED4: Client (CLI)");
		LOG_INF("============================================");

		/* Keep running and wait for button press or Wi-Fi state change */
		ret = k_sem_take(&wifi_ready_sem, K_FOREVER);
		if (ret) {
			LOG_ERR("Failed to wait for Wi-Fi state: %d", ret);
			return;
		}
	}
}

int main(void)
{
	int ret;

	LOG_INF("Starting Nordic Wi-Fi Direct P2P Echo Demo");
	LOG_INF("Board: %s", CONFIG_BOARD);

	/* Initialize work items */
	k_work_init(&p2p_start_work, p2p_start_handler);
	k_work_init(&p2p_connect_work, p2p_connect_handler);
	k_work_init(&dhcp_bound_work, dhcp_bound_handler);
	k_work_init_delayable(&led_blink_work, led_blink_handler);

	/* Initialize LEDs and buttons */
	ret = init_leds_and_buttons();
	if (ret) {
		LOG_ERR("Failed to initialize LEDs and buttons: %d", ret);
		return ret;
	}

	/* Register Wi-Fi ready callback */
	ret = register_wifi_ready();
	if (ret) {
		LOG_ERR("Failed to register Wi-Fi ready callback: %d", ret);
		return ret;
	}

	/* Start Wi-Fi thread */
	k_thread_start(start_wifi_thread_id);

	return 0;
}
