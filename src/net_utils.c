/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(p2p_net_utils, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_event.h>
#include <stdio.h>

#include "net_utils.h"

/* Helper function to format MAC address as string */
static inline char *format_mac_addr(const uint8_t *mac, char *buf, size_t buf_len)
{
	snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return buf;
}

/* DHCP server status */
static bool dhcp_server_running;

/* DHCP bound semaphore */
static K_SEM_DEFINE(dhcp_bound_sem, 0, 1);

/* Network event callback */
static struct net_mgmt_event_callback net_mgmt_cb;
static net_utils_dhcp_bound_cb_t dhcp_bound_cb;

static void net_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				   uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_IPV4_DHCP_BOUND:
		LOG_INF("DHCP bound - IP address obtained");
		k_sem_give(&dhcp_bound_sem);
		if (dhcp_bound_cb) {
			dhcp_bound_cb(iface);
		}
		break;
	default:
		break;
	}
}

struct net_if *net_utils_get_wifi_iface(void)
{
	return net_if_get_first_wifi();
}

int net_utils_configure_go_ip(struct net_if *iface, const char *ip_addr,
			      const char *netmask)
{
	struct in_addr addr;
	struct in_addr mask;
	int ret;

	if (!iface) {
		iface = net_if_get_first_wifi();
	}

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	if (!ip_addr || !netmask) {
		LOG_ERR("Invalid IP address or netmask");
		return -EINVAL;
	}

	ret = net_addr_pton(AF_INET, ip_addr, &addr);
	if (ret < 0) {
		LOG_ERR("Invalid IP address format: %s", ip_addr);
		return ret;
	}

	ret = net_addr_pton(AF_INET, netmask, &mask);
	if (ret < 0) {
		LOG_ERR("Invalid netmask format: %s", netmask);
		return ret;
	}

	/* Add IP address to interface */
	struct net_if_addr *ifaddr;

	ifaddr = net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		LOG_ERR("Failed to add IP address to interface");
		return -ENOMEM;
	}

	/* Set netmask */
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);

	char ip_str[NET_IPV4_ADDR_LEN];

	net_addr_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
	LOG_INF("Configured GO IP address: %s", ip_str);

	return 0;
}

int net_utils_start_dhcp_server(struct net_if *iface, const char *pool_start)
{
	struct in_addr pool_addr;
	int ret;

	if (!iface) {
		iface = net_if_get_first_wifi();
	}

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	if (!pool_start) {
		LOG_ERR("Invalid DHCP pool start address");
		return -EINVAL;
	}

	ret = net_addr_pton(AF_INET, pool_start, &pool_addr);
	if (ret < 0) {
		LOG_ERR("Invalid DHCP pool address format: %s", pool_start);
		return ret;
	}

	ret = net_dhcpv4_server_start(iface, &pool_addr);
	if (ret == -EALREADY) {
		LOG_WRN("DHCP server already running");
		dhcp_server_running = true;
		return 0;
	} else if (ret < 0) {
		LOG_ERR("Failed to start DHCP server: %d", ret);
		return ret;
	}

	dhcp_server_running = true;
	LOG_INF("DHCP server started, pool starting at: %s", pool_start);

	return 0;
}

int net_utils_stop_dhcp_server(struct net_if *iface)
{
	int ret;

	if (!iface) {
		iface = net_if_get_first_wifi();
	}

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	if (!dhcp_server_running) {
		LOG_WRN("DHCP server not running");
		return 0;
	}

	ret = net_dhcpv4_server_stop(iface);
	if (ret < 0) {
		LOG_ERR("Failed to stop DHCP server: %d", ret);
		return ret;
	}

	dhcp_server_running = false;
	LOG_INF("DHCP server stopped");

	return 0;
}

/* Flag to track if DHCP callback is registered */
static bool dhcp_cb_registered;

void net_utils_register_dhcp_callback(void)
{
	if (!dhcp_cb_registered) {
		/* Reset semaphore before registering callback to ensure clean state */
		k_sem_reset(&dhcp_bound_sem);

		net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_event_handler,
					     NET_EVENT_IPV4_DHCP_BOUND);
		net_mgmt_add_event_callback(&net_mgmt_cb);
		dhcp_cb_registered = true;
		LOG_DBG("DHCP event callback registered");
	}
}

void net_utils_set_dhcp_bound_cb(net_utils_dhcp_bound_cb_t cb)
{
	dhcp_bound_cb = cb;
}

int net_utils_wait_for_dhcp(struct net_if *iface, uint32_t timeout_ms)
{
	int ret;

	if (!iface) {
		iface = net_if_get_first_wifi();
	}

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return -ENODEV;
	}

	/* Ensure callback is registered */
	net_utils_register_dhcp_callback();

	LOG_INF("Waiting for DHCP to assign IP address...");

	ret = k_sem_take(&dhcp_bound_sem, K_MSEC(timeout_ms));
	if (ret == -EAGAIN) {
		LOG_ERR("DHCP timeout after %d ms", timeout_ms);
		return -ETIMEDOUT;
	}

	return 0;
}

void net_utils_print_status(struct net_if *iface)
{
	if (!iface) {
		iface = net_if_get_first_wifi();
	}

	if (!iface) {
		LOG_ERR("No Wi-Fi interface found");
		return;
	}

	LOG_INF("=== Network Status ===");

	/* Get and print IPv4 addresses */
	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4) {
		for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			if (ipv4->unicast[i].ipv4.is_used) {
				char ip_str[NET_IPV4_ADDR_LEN];
				char netmask_str[NET_IPV4_ADDR_LEN];

				net_addr_ntop(AF_INET,
					      &ipv4->unicast[i].ipv4.address.in_addr,
					      ip_str, sizeof(ip_str));
				net_addr_ntop(AF_INET,
					      &ipv4->unicast[i].netmask,
					      netmask_str, sizeof(netmask_str));
				LOG_INF("IPv4 Address: %s", ip_str);
				LOG_INF("Netmask: %s", netmask_str);
			}
		}

		char gw_str[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET, &ipv4->gw, gw_str, sizeof(gw_str));
		LOG_INF("Gateway: %s", gw_str);
	} else {
		LOG_INF("No IPv4 configuration");
	}

	/* Print MAC address */
	struct net_linkaddr *linkaddr = net_if_get_link_addr(iface);

	if (linkaddr && linkaddr->len >= 6) {
		char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

		LOG_INF("MAC Address: %s",
			format_mac_addr(linkaddr->addr, mac_string_buf, sizeof(mac_string_buf)));
	}

	LOG_INF("DHCP Server: %s", dhcp_server_running ? "Running" : "Stopped");
	LOG_INF("======================");
}
