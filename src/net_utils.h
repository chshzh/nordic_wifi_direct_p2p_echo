/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef NET_UTILS_H_
#define NET_UTILS_H_

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure static IP address for GO role
 *
 * @param iface Network interface to configure
 * @param ip_addr IP address string (e.g., "192.168.88.1")
 * @param netmask Netmask string (e.g., "255.255.255.0")
 * @return 0 on success, negative error code on failure
 */
int net_utils_configure_go_ip(struct net_if *iface, const char *ip_addr,
			      const char *netmask);

/**
 * @brief Start DHCP server for P2P GO role
 *
 * @param iface Network interface to start DHCP server on
 * @param pool_start Start address of DHCP pool (e.g., "192.168.88.10")
 * @return 0 on success, negative error code on failure
 */
int net_utils_start_dhcp_server(struct net_if *iface, const char *pool_start);

/**
 * @brief Stop DHCP server
 *
 * @param iface Network interface
 * @return 0 on success, negative error code on failure
 */
int net_utils_stop_dhcp_server(struct net_if *iface);

/**
 * @brief Register DHCP event callback
 *
 * Should be called before starting DHCP client to ensure
 * the DHCP_BOUND event is not missed.
 */
void net_utils_register_dhcp_callback(void);

/**
 * @brief DHCP bound notification callback type
 *
 * @param iface Network interface where DHCP bound occurred
 */
typedef void (*net_utils_dhcp_bound_cb_t)(struct net_if *iface);

/**
 * @brief Set user callback for DHCP bound event
 *
 * @param cb Callback function (NULL to clear)
 */
void net_utils_set_dhcp_bound_cb(net_utils_dhcp_bound_cb_t cb);

/**
 * @brief Wait for DHCP client to obtain IP address
 *
 * @param iface Network interface
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, negative error code on failure/timeout
 */
int net_utils_wait_for_dhcp(struct net_if *iface, uint32_t timeout_ms);

/**
 * @brief Print current network interface status
 *
 * @param iface Network interface (NULL for default Wi-Fi interface)
 */
void net_utils_print_status(struct net_if *iface);

/**
 * @brief Get the first Wi-Fi network interface
 *
 * @return Pointer to Wi-Fi network interface, or NULL if not found
 */
struct net_if *net_utils_get_wifi_iface(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_UTILS_H_ */
