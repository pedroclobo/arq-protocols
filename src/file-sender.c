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

struct window_t {
	uint32_t sf;
	uint32_t sn;
	short size;
};

ssize_t send_data_pkt(int sockfd, FILE *file, uint32_t seq_num, struct sockaddr *dest_addr) {
	struct data_pkt_t pkt;

	// Set cursor to the right position.
	fseek(file, seq_num * sizeof(pkt.data), SEEK_SET);

	// Set pkt sequence number and copy data to pkt.
	pkt.seq_num = htonl(seq_num);
	size_t read_len = fread(pkt.data, 1, sizeof(pkt.data), file);

	// Send data_pkt
	size_t sent_len = sendto(sockfd, &pkt, read_len + offsetof(data_pkt_t, data), 0, dest_addr, sizeof(*dest_addr));
	printf(SENDER "Sending segment %d, size %ld.\n", ntohl(pkt.seq_num), read_len + offsetof(data_pkt_t, data));

	if (sent_len != read_len + offsetof(data_pkt_t, data)) {
		fprintf(stderr, SENDER "Truncated packet.\n");
		exit(EXIT_FAILURE);
	}

	return sent_len;
}

ssize_t recv_ack_pkt(int sockfd, struct ack_pkt_t *ack_pkt) {
	struct sockaddr recv_addr;

	ssize_t received_len = recvfrom(sockfd, ack_pkt, sizeof(*ack_pkt), 0, &recv_addr, &(socklen_t){sizeof(recv_addr)});

	if (received_len == -1) {
		return -1;
	} else if (received_len != sizeof(*ack_pkt)) {
		fprintf(stderr, SENDER "Truncated packet.\n");
		exit(EXIT_FAILURE);
	}

	printf(SENDER "Received ACK with seq_num %d and selective_acks %b.\n", ntohl(ack_pkt->seq_num), ntohl(ack_pkt->selective_acks));

	return received_len;
}

bool has_been_received(uint32_t seq_num, uint32_t rn, uint32_t selective_acks) {
	// The window base cannot have been received as that would advance the window.
	if (seq_num == rn) {
		return false;
	}

	int mask = 1 << (seq_num - rn - 1);
	return (selective_acks & mask) == mask;
}

int main(int argc, char *argv[]) {
	// Create sender window.
	struct window_t window = {
	    .sf = 0,
	    .sn = 0,
	    .size = 0,
	};

	// Parse command line arguments.
	if (argc != 3) {
		fprintf(stderr, SENDER "Usage: %s <port> <window-size>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	int port = atoi(argv[1]);

	window.size = atoi(argv[2]);
	if (window.size <= 0 || window.size > MAX_WINDOW_SIZE) {
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

	ack_pkt_t ack_pkt;
	size_t recv_len;
	bool feof_reached = false;

	do {
		// Send segments while window is not full and end of file has
		// not been reached.
		while (window.sn < window.sf + window.size && !feof_reached) {
			send_data_pkt(sockfd, file, window.sn, (struct sockaddr *)&client_addr);
			window.sn++;

			// A flag has to be used because fseek can reset the end-of-file
			// indicator.
			if (feof(file)) {
				feof_reached = true;
			}
		}

		// Receive ACK's until the window base in updated.
		int timeouts = 0;
		bool updated_sf = false;

		do {
			recv_len = recv_ack_pkt(sockfd, &ack_pkt);

			if (recv_len == -1) {
				// Exit on MAX_RETRIES consecutive timeouts.
				if (++timeouts == MAX_RETRIES) {
					fprintf(stderr, SENDER "Exiting: Consecutive timeouts.\n");
					close(sockfd);
					fclose(file);
					exit(EXIT_FAILURE);
				}

				// Resend all not yet acknowledged outstanding packets.
				for (uint32_t seq_num = window.sf; window.sf <= seq_num && seq_num < window.sn; seq_num++) {
					if (!has_been_received(seq_num, ntohl(ack_pkt.seq_num), ntohl(ack_pkt.selective_acks))) {
						fprintf(stderr, SENDER "Timeout:\n");
						send_data_pkt(sockfd, file, seq_num, (struct sockaddr *)&client_addr);
					}
				}

			} else {
				// Receiver has new base, update window base.
				if (window.sf != ntohl(ack_pkt.seq_num)) {
					window.sf = ntohl(ack_pkt.seq_num);
					updated_sf = true;
				}
			}

			// Check for end-of-file indicator.
			if (feof(file)) {
				feof_reached = true;
			}

		} while (!updated_sf);

	} while (!(feof_reached && window.sn == window.sf && ntohl(ack_pkt.seq_num) == window.sn && htonl(ack_pkt.selective_acks) == 0));
	// The end-of-file indicator needs to be set, sf and sn must point to the same
	// sequence number. The receiver must acknowledge the last segment and have no
	// missing segments.

	// Clean up and exit.
	close(sockfd);
	fprintf(stderr, SENDER "Closed socket.\n");

	fclose(file);
	fprintf(stderr, SENDER "Closed file.\n");

	exit(EXIT_SUCCESS);
}
