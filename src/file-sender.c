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

#define RED "\033[31m"
#define RESET "\033[0m"
#define SENDER RED " [Sender]:  " RESET

ssize_t send_data_packet(int sockfd, struct data_pkt_t *data_pkt, size_t len, struct sockaddr *src_addr) {
	size_t sent_len = sendto(sockfd, data_pkt, len, 0, src_addr, sizeof(*src_addr));
	printf(SENDER "Sending segment %d, size %ld.\n", ntohl(data_pkt->seq_num), len);

	if (sent_len != len) {
		fprintf(stderr, SENDER "Truncated packet.\n");
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
		fprintf(stderr, SENDER "Truncated packet.\n");
		exit(-1);
	}

	printf(SENDER "Received ACK with seq_num %d and selective_acks %b.\n", ntohl(ack_pkt->seq_num), ntohl(ack_pkt->selective_acks));

	return received_len;
}

int is_outstanding_packet(uint32_t sn, uint32_t sf, uint32_t seq_num) {
	if (sf < sn) {
		return sf <= seq_num && seq_num < sn;
	} else {
		return sf <= seq_num || seq_num < sn;
	}
}

bool has_been_received(uint32_t seq_num, uint32_t rn, uint32_t selective_acks) {
	if (seq_num == rn) {
		return false;
	}

	int mask = 1 << (seq_num - rn - 1);
	return (selective_acks & mask) == mask;
}

int main(int argc, char *argv[]) {
	// Parse command line arguments.
	if (argc != 3) {
		fprintf(stderr, SENDER "Usage: %s <port> <window-size>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	int port = atoi(argv[1]);

	int s_size = atoi(argv[2]);
	if (s_size <= 0 || s_size > MAX_WINDOW_SIZE) {
		fprintf(stderr, SENDER "Invalid window size.\n");
		exit(EXIT_FAILURE);
	}

	// Prepare server socket.
	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		fprintf(stderr, SENDER "Failed to prepare server socket.\n");
		exit(EXIT_FAILURE);
	}

	// Allow address reuse so we can rebind to the same port,
	// after restarting the server.
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
		fprintf(stderr, SENDER "Failed to allow address reuse.\n");
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
	fprintf(stderr, SENDER "Receiving on port: %d.\n", port);

	ssize_t len;
	struct sockaddr_in client_addr;
	req_file_pkt_t req_file_pkt;

	// Receive name of file from client.
	len = recvfrom(sockfd, &req_file_pkt, sizeof(req_file_pkt), 0, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(client_addr)});
	if (len < MAX_PATH_SIZE) {
		req_file_pkt.file_path[len] = '\0';
	}
	printf(SENDER "Received request for file %s, size %ld.\n", req_file_pkt.file_path, len);

	FILE *file = fopen(req_file_pkt.file_path, "r");
	if (!file) {
		fprintf(stderr, SENDER "Failed to open file.\n");
		exit(EXIT_FAILURE);
	}

	// Set timeout.
	struct timeval tv;
	tv.tv_sec = TIMEOUT / 1000;
	tv.tv_usec = (TIMEOUT % 1000) * 1000;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
		fprintf(stderr, SENDER "Failed to set timeout.\n");
		exit(EXIT_FAILURE);
	}

	uint32_t sf = 0;
	uint32_t sn = 0;

	data_pkt_t data_pkts[SEQ_NUM_SIZE];
	size_t data_pkts_len[SEQ_NUM_SIZE];

	ack_pkt_t ack_pkt;
	uint32_t rn = 0;
	uint32_t selective_acks = 0;

	size_t data_len;
	size_t received_len;

	do {
		// Send segments while window is not full and end of file has
		// not been reached.
		while (sn != (sf + s_size) % SEQ_NUM_SIZE && !feof(file)) {
			data_pkts[sn].seq_num = htonl(sn);
			data_len = fread(data_pkts[sn].data, 1, sizeof(data_pkts[sn].data), file);
			data_pkts_len[sn] = data_len;
			send_data_packet(sockfd, &data_pkts[sn], offsetof(data_pkt_t, data) + data_len, (struct sockaddr *)&client_addr);
			sn = (sn + 1) % SEQ_NUM_SIZE;
		}

		// Receive ACK's until the window base in updated.
		int timeouts = 0;
		while (true) {
			received_len = receive_ack_packet(sockfd, &ack_pkt, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(client_addr)});

			rn = ntohl(ack_pkt.seq_num);
			selective_acks = ntohl(ack_pkt.selective_acks);

			if (received_len == -1) {
				// Exit on 3 consecutive timouts
				if (++timeouts == MAX_RETRIES) {
					fprintf(stderr, SENDER "Exiting: Consecutive timeouts.\n");
					close(sockfd);
					fclose(file);
					exit(EXIT_FAILURE);
				}

				// Resend all not yet acknowledged outstanding packets.
				for (uint32_t seq_num = sf; !has_been_received(seq_num, rn, selective_acks) && is_outstanding_packet(sn, sf, seq_num); seq_num = (seq_num + 1) % SEQ_NUM_SIZE) {
					fprintf(stderr, SENDER "Timeout:\n");
					send_data_packet(sockfd, &data_pkts[seq_num], offsetof(data_pkt_t, data) + data_pkts_len[seq_num], (struct sockaddr *)&client_addr);
				}

			} else {
				// Receiver has new base, update our window base.
				if (sf != ntohl(ack_pkt.seq_num)) {
					sf = ntohl(ack_pkt.seq_num);
					break;
				}
			}
		}

	} while (!(feof(file) && data_len < sizeof(data_pkts[0].data)) || selective_acks != 0 || sf != sn);

	// Clean up and exit.
	close(sockfd);
	fprintf(stderr, SENDER "Closed socket.\n");
	fclose(file);
	fprintf(stderr, SENDER "Closed file.\n");

	exit(EXIT_SUCCESS);
}
