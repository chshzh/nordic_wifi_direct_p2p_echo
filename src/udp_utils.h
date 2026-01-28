/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef UDP_UTILS_H
#define UDP_UTILS_H

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/** UDP Echo statistics */
struct udp_echo_stats {
	/** Total packets sent */
	uint32_t packets_sent;
	/** Total packets received */
	uint32_t packets_received;
	/** Total bytes sent */
	uint32_t bytes_sent;
	/** Total bytes received */
	uint32_t bytes_received;
	/** Packet loss count */
	uint32_t packets_lost;
	/** Minimum RTT in microseconds */
	uint32_t rtt_min_us;
	/** Maximum RTT in microseconds */
	uint32_t rtt_max_us;
	/** Average RTT in microseconds */
	uint32_t rtt_avg_us;
	/** Total RTT for averaging */
	uint64_t rtt_total_us;
};

/**
 * @brief Initialize UDP client
 *
 * @param socket Pointer to store the socket descriptor
 * @param server_addr Pointer to store the server address structure
 * @param target_ip Target IP address as string
 * @param port Target port number
 * @return 0 on success, negative error code on failure
 */
int udp_client_init(int *socket, struct sockaddr_in *server_addr,
		    const char *target_ip, uint16_t port);

/**
 * @brief Initialize UDP server (echo server)
 *
 * @param socket Pointer to store the socket descriptor
 * @param port Port number to listen on
 * @return 0 on success, negative error code on failure
 */
int udp_server_init(int *socket, uint16_t port);

/**
 * @brief Send UDP packet
 *
 * @param socket Socket descriptor
 * @param server_addr Server address structure
 * @param data Data to send
 * @param data_len Length of data to send
 * @return Number of bytes sent on success, negative error code on failure
 */
int udp_send(int socket, struct sockaddr_in *server_addr,
	     const char *data, size_t data_len);

/**
 * @brief Receive UDP packet
 *
 * @param socket Socket descriptor
 * @param buffer Buffer to store received data
 * @param buffer_size Size of the buffer
 * @param client_addr Pointer to store client address (can be NULL)
 * @return Number of bytes received on success, negative error code on failure
 */
int udp_receive(int socket, char *buffer, size_t buffer_size,
		struct sockaddr_in *client_addr);

/**
 * @brief Send UDP packet and receive echo response (with RTT measurement)
 *
 * @param socket Socket descriptor
 * @param server_addr Server address structure
 * @param data Data to send
 * @param data_len Length of data to send
 * @param recv_buffer Buffer for received echo data
 * @param recv_buffer_size Size of receive buffer
 * @param rtt_us Output: Round-trip time in microseconds
 * @return Number of bytes received on success, negative error code on failure
 */
int udp_echo_ping(int socket, struct sockaddr_in *server_addr,
		  const char *data, size_t data_len,
		  char *recv_buffer, size_t recv_buffer_size,
		  uint32_t *rtt_us);

/**
 * @brief Run UDP echo server (blocks and loops back packets)
 *
 * @param socket Server socket descriptor
 * @param stats Pointer to statistics structure (can be NULL)
 * @param stop_flag Pointer to stop flag (set to true to stop server)
 * @return 0 on success, negative error code on failure
 */
int udp_echo_server_run(int socket, struct udp_echo_stats *stats,
			volatile bool *stop_flag);

/**
 * @brief Run UDP echo client (sends packets and measures RTT)
 *
 * @param socket Client socket descriptor
 * @param server_addr Server address structure
 * @param packet_size Size of packets to send
 * @param interval_ms Interval between packets in milliseconds
 * @param count Number of packets to send (0 = infinite)
 * @param stats Pointer to statistics structure (can be NULL)
 * @param stop_flag Pointer to stop flag (set to true to stop client)
 * @return 0 on success, negative error code on failure
 */
int udp_echo_client_run(int socket, struct sockaddr_in *server_addr,
			size_t packet_size, uint32_t interval_ms,
			uint32_t count, struct udp_echo_stats *stats,
			volatile bool *stop_flag);

/**
 * @brief Cleanup UDP client
 *
 * @param socket Socket descriptor to close
 */
void udp_client_cleanup(int socket);

/**
 * @brief Cleanup UDP server
 *
 * @param socket Socket descriptor to close
 */
void udp_server_cleanup(int socket);

/**
 * @brief Print UDP echo statistics
 *
 * @param stats Pointer to statistics structure
 */
void udp_echo_print_stats(const struct udp_echo_stats *stats);

/**
 * @brief Reset UDP echo statistics
 *
 * @param stats Pointer to statistics structure
 */
void udp_echo_reset_stats(struct udp_echo_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* UDP_UTILS_H */
