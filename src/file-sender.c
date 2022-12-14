#include "packet-format.h"
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

ssize_t send_data_packet(int sockfd, struct data_pkt_t *data_pkt, size_t len, struct sockaddr *src_addr) {
	size_t sent_len = sendto(sockfd, data_pkt, len, 0, src_addr, sizeof(*src_addr));
	printf("Sender: Sending segment %d, size %ld.\n", ntohl(data_pkt->seq_num), len);

	if (sent_len != len) {
		fprintf(stderr, "Sender: Truncated packet.\n");
		exit(-1);
	}

	return sent_len;
}

ssize_t receive_ack_packet(int sockfd, struct ack_pkt_t *ack_pkt, struct sockaddr *src_addr, socklen_t *src_addr_len) {
	ssize_t received_len = recvfrom(sockfd, ack_pkt, sizeof(*ack_pkt), 0, src_addr, src_addr_len);

	if (received_len == -1) {
		return -1;
	} else if (received_len == 0) {
		return 0;
	}

	if (received_len != sizeof(*ack_pkt)) {
		fprintf(stderr, "Sender: Truncated packet.\n");
		exit(-1);
	}

	printf("Sender: Received ACK segment %d, size %ld.\n", ntohl(ack_pkt->seq_num), received_len);

	return received_len;
}

int is_outstanding_packet(uint32_t sn, uint32_t sf, uint32_t seq_num) {
	if (sf < sn) {
		return sf <= seq_num && seq_num < sn;
	} else {
		return sf <= seq_num || seq_num < sn;
	}
}

int main(int argc, char *argv[]) {
	// Parse command line arguments.
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <port> <window-size>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	int port = atoi(argv[1]);

	int s_size = atoi(argv[2]);
	if (s_size <= 0 || s_size > MAX_WINDOW_SIZE) {
		fprintf(stderr, "Sender: Invalid window size.\n");
		exit(EXIT_FAILURE);
	}

	// Prepare server socket.
	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		fprintf(stderr, "Sender: Failed to prepare server socket.\n");
		exit(EXIT_FAILURE);
	}

	// Allow address reuse so we can rebind to the same port,
	// after restarting the server.
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
		fprintf(stderr, "Sender: Failed to allow address reuse.\n");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in srv_addr = {
	    .sin_family = AF_INET,
	    .sin_addr.s_addr = htonl(INADDR_ANY),
	    .sin_port = htons(port),
	};
	if (bind(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr))) {
		exit(EXIT_FAILURE);
	}
	fprintf(stderr, "Sender: Receiving on port: %d.\n", port);

	ssize_t len;
	struct sockaddr_in client_addr;
	req_file_pkt_t req_file_pkt;

	// Receive name of file from client.
	len = recvfrom(sockfd, &req_file_pkt, sizeof(req_file_pkt), 0, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(client_addr)});
	if (len < MAX_PATH_SIZE) {
		req_file_pkt.file_path[len] = '\0';
	}
	printf("Received request for file %s, size %ld.\n", req_file_pkt.file_path, len);

	FILE *file = fopen(req_file_pkt.file_path, "r");
	if (!file) {
		fprintf(stderr, "Sender: Failed to open file.\n");
		exit(EXIT_FAILURE);
	}

	// Set timeout.
	struct timeval tv;
	tv.tv_sec = TIMEOUT / 1000;
	tv.tv_usec = (TIMEOUT % 1000) * 1000;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
		fprintf(stderr, "Sender: Failed to set timeout.\n");
		exit(EXIT_FAILURE);
	}

	uint32_t sf = 0;
	uint32_t sn = 0;

	data_pkt_t data_pkts[SEQ_NUM_SIZE];
	ack_pkt_t ack_pkt;
	size_t data_len;
	size_t received_len;

	// Generate segments from file, until the end of the file.
	do {

		// Send segments until the window is full.
		while (sn != (sf + s_size) % SEQ_NUM_SIZE) {
			data_pkts[sn].seq_num = htonl(sn);
			data_len = fread(data_pkts[sn].data, 1, sizeof(data_pkts[sn].data), file);
			send_data_packet(sockfd, &data_pkts[sn], offsetof(data_pkt_t, data) + data_len, (struct sockaddr *)&client_addr);

			sn = (sn + 1) % SEQ_NUM_SIZE;
		}

		// Wait for ACK and resend all outstanding packets if timeout.
		do {
			received_len = receive_ack_packet(sockfd, &ack_pkt, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(client_addr)});

			// Timeout
			if (received_len == -1) {
				for (uint32_t i = sf; i < sn; i++) {
					fprintf(stderr, "Sender: Timeout - Resending segment %d.\n", ntohl(data_pkts[i].seq_num));
					send_data_packet(sockfd, &data_pkts[i], offsetof(data_pkt_t, data) + data_len, (struct sockaddr *)&client_addr);
				}
			}

		} while (received_len == -1 && !is_outstanding_packet(sf, sn, ntohl(ack_pkt.seq_num)));

		sf = ntohl(ack_pkt.seq_num);

	} while (!(feof(file) && data_len < sizeof(data_pkts[0].data)));

	// Clean up and exit.
	close(sockfd);
	fclose(file);
	fprintf(stderr, "Sender: Closed socket.\n");

	return 0;
}
