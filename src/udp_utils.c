/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <string.h>

#include "udp_utils.h"

LOG_MODULE_REGISTER(udp_utils, CONFIG_LOG_DEFAULT_LEVEL);

/* Timeout for receive operations (ms) */
#define UDP_RECV_TIMEOUT_MS 2000

int udp_client_init(int *socket, struct sockaddr_in *server_addr,
		    const char *target_ip, uint16_t port)
{
	int sock;
	int ret;
	struct timeval timeout = {
		.tv_sec = UDP_RECV_TIMEOUT_MS / 1000,
		.tv_usec = (UDP_RECV_TIMEOUT_MS % 1000) * 1000,
	};

	/* Create UDP socket */
	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Failed to create UDP socket: %d", errno);
		return -errno;
	}

	/* Set receive timeout */
	ret = zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
			       &timeout, sizeof(timeout));
	if (ret < 0) {
		/* Some socket backends do not support SO_RCVTIMEO for UDP */
		if (errno == ENOPROTOOPT || errno == ENOTSUP || errno == ENOTCONN) {
			LOG_DBG("SO_RCVTIMEO not supported (errno=%d)", errno);
		} else {
			LOG_WRN("Failed to set socket timeout: %d", errno);
		}
	}

	/* Configure server address */
	server_addr->sin_family = AF_INET;
	server_addr->sin_port = htons(port);

	ret = zsock_inet_pton(AF_INET, target_ip, &server_addr->sin_addr);
	if (ret <= 0) {
		LOG_ERR("Invalid target IP address: %s", target_ip);
		zsock_close(sock);
		return -EINVAL;
	}

	*socket = sock;
	LOG_INF("UDP client initialized, target: %s:%d", target_ip, port);
	return 0;
}

int udp_server_init(int *socket, uint16_t port)
{
	int sock;
	struct sockaddr_in server_addr;
	int ret;

	/* Create UDP socket */
	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Failed to create UDP socket: %d", errno);
		return -errno;
	}

	/* Configure server address */
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(port);

	/* Bind socket */
	ret = zsock_bind(sock, (struct sockaddr *)&server_addr,
			 sizeof(server_addr));
	if (ret < 0) {
		LOG_ERR("Failed to bind UDP socket: %d", errno);
		zsock_close(sock);
		return -errno;
	}

	*socket = sock;
	LOG_INF("UDP echo server initialized on port %d", port);
	return 0;
}

int udp_send(int socket, struct sockaddr_in *server_addr,
	     const char *data, size_t data_len)
{
	int ret;

	ret = zsock_sendto(socket, data, data_len, 0,
			   (struct sockaddr *)server_addr,
			   sizeof(*server_addr));
	if (ret < 0) {
		LOG_ERR("Failed to send UDP data: %d", errno);
		return -errno;
	}

	return ret;
}

int udp_receive(int socket, char *buffer, size_t buffer_size,
		struct sockaddr_in *client_addr)
{
	socklen_t addr_len = sizeof(struct sockaddr_in);
	struct sockaddr_in tmp_addr;
	struct sockaddr_in *addr_ptr;
	int ret;

	addr_ptr = client_addr ? client_addr : &tmp_addr;

	ret = zsock_recvfrom(socket, buffer, buffer_size, 0,
			     (struct sockaddr *)addr_ptr, &addr_len);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			/* Timeout - not an error */
			return 0;
		}
		LOG_ERR("Failed to receive UDP data: %d", errno);
		return -errno;
	}

	return ret;
}

int udp_echo_ping(int socket, struct sockaddr_in *server_addr,
		  const char *data, size_t data_len,
		  char *recv_buffer, size_t recv_buffer_size,
		  uint32_t *rtt_us)
{
	int64_t start_time, end_time;
	int ret;

	/* Record start time */
	start_time = k_uptime_get();

	/* Send packet */
	ret = udp_send(socket, server_addr, data, data_len);
	if (ret < 0) {
		return ret;
	}

	/* Receive echo response */
	ret = udp_receive(socket, recv_buffer, recv_buffer_size, NULL);
	if (ret <= 0) {
		if (ret == 0) {
			/* Timeout */
			return -ETIMEDOUT;
		}
		return ret;
	}

	/* Calculate RTT */
	end_time = k_uptime_get();
	if (rtt_us) {
		*rtt_us = (uint32_t)((end_time - start_time) * 1000);
	}

	return ret;
}

int udp_echo_server_run(int socket, struct udp_echo_stats *stats,
			volatile bool *stop_flag)
{
	char buffer[CONFIG_UDP_ECHO_PACKET_SIZE + 64];
	struct sockaddr_in client_addr;
	socklen_t client_addr_len;
	int recv_len, send_len;

	LOG_INF("UDP Echo Server started - waiting for packets...");

	while (!(*stop_flag)) {
		client_addr_len = sizeof(client_addr);

		/* Receive packet */
		recv_len = zsock_recvfrom(socket, buffer, sizeof(buffer), 0,
					  (struct sockaddr *)&client_addr,
					  &client_addr_len);

		if (recv_len < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* Timeout - check stop flag and continue */
				continue;
			}
			LOG_ERR("Echo server receive error: %d", errno);
			continue;
		}

		if (recv_len == 0) {
			continue;
		}

		/* Update stats */
		if (stats) {
			stats->packets_received++;
			stats->bytes_received += recv_len;
		}

		/* Log received packet */
		char ip_str[INET_ADDRSTRLEN];
		zsock_inet_ntop(AF_INET, &client_addr.sin_addr,
				ip_str, sizeof(ip_str));
		LOG_DBG("Received %d bytes from %s:%d",
			recv_len, ip_str, ntohs(client_addr.sin_port));

		/* Echo back the packet */
		send_len = zsock_sendto(socket, buffer, recv_len, 0,
					(struct sockaddr *)&client_addr,
					client_addr_len);

		if (send_len < 0) {
			LOG_ERR("Echo server send error: %d", errno);
			continue;
		}

		/* Update stats */
		if (stats) {
			stats->packets_sent++;
			stats->bytes_sent += send_len;
		}

		LOG_DBG("Echoed %d bytes back to %s:%d",
			send_len, ip_str, ntohs(client_addr.sin_port));
	}

	LOG_INF("UDP Echo Server stopped");
	return 0;
}

int udp_echo_client_run(int socket, struct sockaddr_in *server_addr,
			size_t packet_size, uint32_t interval_ms,
			uint32_t count, struct udp_echo_stats *stats,
			volatile bool *stop_flag)
{
	char send_buffer[CONFIG_UDP_ECHO_PACKET_SIZE + 64];
	char recv_buffer[CONFIG_UDP_ECHO_PACKET_SIZE + 64];
	uint32_t seq_num = 0;
	uint32_t rtt_us;
	int ret;

	/* Ensure packet size is within bounds */
	if (packet_size > sizeof(send_buffer)) {
		packet_size = sizeof(send_buffer);
	}

	LOG_INF("UDP Echo Client started");
	LOG_INF("  Packet size: %d bytes", packet_size);
	LOG_INF("  Interval: %d ms", interval_ms);
	LOG_INF("  Count: %s", count == 0 ? "infinite" : "");

	while (!(*stop_flag)) {
		/* Check count limit */
		if (count > 0 && seq_num >= count) {
			LOG_INF("Completed %d echo requests", count);
			break;
		}

		/* Prepare packet with sequence number and timestamp */
		memset(send_buffer, 'A' + (seq_num % 26), packet_size);
		snprintf(send_buffer, packet_size, "SEQ=%08u,T=%lld",
			 seq_num, k_uptime_get());

		/* Send and receive echo */
		ret = udp_echo_ping(socket, server_addr,
				    send_buffer, packet_size,
				    recv_buffer, sizeof(recv_buffer),
				    &rtt_us);

		if (ret > 0) {
			/* Success */
			if (stats) {
				stats->packets_sent++;
				stats->packets_received++;
				stats->bytes_sent += packet_size;
				stats->bytes_received += ret;

				/* Update RTT statistics */
				if (stats->packets_received == 1 ||
				    rtt_us < stats->rtt_min_us) {
					stats->rtt_min_us = rtt_us;
				}
				if (rtt_us > stats->rtt_max_us) {
					stats->rtt_max_us = rtt_us;
				}
				stats->rtt_total_us += rtt_us;
				stats->rtt_avg_us = (uint32_t)(stats->rtt_total_us /
							       stats->packets_received);
			}

			LOG_INF("Echo reply: seq=%u, bytes=%d, RTT=%u.%03u ms",
				seq_num, ret, rtt_us / 1000, rtt_us % 1000);
		} else if (ret == -ETIMEDOUT) {
			/* Timeout */
			if (stats) {
				stats->packets_sent++;
				stats->packets_lost++;
				stats->bytes_sent += packet_size;
			}
			LOG_WRN("Echo timeout: seq=%u", seq_num);
		} else {
			/* Error */
			LOG_ERR("Echo error: seq=%u, ret=%d", seq_num, ret);
		}

		seq_num++;

		/* Wait for next interval */
		k_msleep(interval_ms);
	}

	LOG_INF("UDP Echo Client stopped");
	return 0;
}

void udp_client_cleanup(int socket)
{
	if (socket >= 0) {
		zsock_close(socket);
		LOG_INF("UDP client socket closed");
	}
}

void udp_server_cleanup(int socket)
{
	if (socket >= 0) {
		zsock_close(socket);
		LOG_INF("UDP server socket closed");
	}
}

void udp_echo_print_stats(const struct udp_echo_stats *stats)
{
	if (!stats) {
		return;
	}

	LOG_INF("=== UDP Echo Statistics ===");
	LOG_INF("Packets sent:     %u", stats->packets_sent);
	LOG_INF("Packets received: %u", stats->packets_received);
	LOG_INF("Packets lost:     %u", stats->packets_lost);
	LOG_INF("Bytes sent:       %u", stats->bytes_sent);
	LOG_INF("Bytes received:   %u", stats->bytes_received);

	if (stats->packets_received > 0) {
		LOG_INF("RTT min:          %u.%03u ms", stats->rtt_min_us / 1000,
			stats->rtt_min_us % 1000);
		LOG_INF("RTT max:          %u.%03u ms", stats->rtt_max_us / 1000,
			stats->rtt_max_us % 1000);
		LOG_INF("RTT avg:          %u.%03u ms", stats->rtt_avg_us / 1000,
			stats->rtt_avg_us % 1000);
	}

	if (stats->packets_sent > 0) {
		uint32_t loss_pct = (stats->packets_lost * 100) / stats->packets_sent;
		LOG_INF("Packet loss:      %u%%", loss_pct);
	}

	LOG_INF("===========================");
}

void udp_echo_reset_stats(struct udp_echo_stats *stats)
{
	if (stats) {
		memset(stats, 0, sizeof(*stats));
		stats->rtt_min_us = UINT32_MAX;
	}
}
