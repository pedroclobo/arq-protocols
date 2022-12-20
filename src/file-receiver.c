#include "packet-format.h"
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RESET "\033[0m"
#define GREEN "\033[32m"
#define RECEIVER GREEN "[Receiver]: " RESET

struct window_t {
	uint32_t base;
	short size;
};

int find_last_path_separator(char *path) {
	int last_found_pos = -1;
	int curr_pos = 0;

	while (*path != '\0') {
		if (*path == '/') {
			last_found_pos = curr_pos;
		}

		path += 1;
		curr_pos += 1;
	}

	return last_found_pos;
}

ssize_t send_ack_packet(int sockfd, struct ack_pkt_t *ack_pkt, size_t len, struct sockaddr *src_addr) {
	size_t sent_len = sendto(sockfd, ack_pkt, len, 0, src_addr, sizeof(*src_addr));

	if (sent_len != len) {
		fprintf(stderr, "Truncated packet.\n");
		exit(-1);
	}

	printf(RECEIVER "Sent ACK with seq_num %d and selective_acks %b.\n", ntohl(ack_pkt->seq_num), ntohl(ack_pkt->selective_acks));

	return sent_len;
}

ssize_t receive_data_packet(int sockfd, struct data_pkt_t *data_pkt, struct sockaddr *src_addr, socklen_t *src_addr_len) {
	ssize_t received_len = recvfrom(sockfd, data_pkt, sizeof(*data_pkt), 0, src_addr, src_addr_len);

	if (received_len == -1) {
		return -1;
	} else if (received_len == 0) {
		return 0;
	}

	printf(RECEIVER "Received segment %d, size %ld.\n", ntohl(data_pkt->seq_num), received_len);

	return received_len;
}

bool is_inside_window(struct window_t *window, uint32_t seq_num) {
	uint32_t limit = (window->base + window->size) % SEQ_NUM_SIZE;
	if (limit < window->base) {
		return window->base <= seq_num || seq_num < limit;
	} else {
		return window->base <= seq_num && seq_num < limit;
	}
}

bool has_been_received(uint32_t seq_num, uint32_t rn, uint32_t selective_acks) {
	int mask = 1 << (seq_num - rn - 1);
	return (selective_acks & mask) == mask;
}

int main(int argc, char *argv[]) {
	// Create window.
	struct window_t window = {
		.base = 0,
		.size = 0
	};

	// Parse command line arguments.
	if (argc != 5) {
		fprintf(stderr, RECEIVER "Usage: %s <file> <host> <port> <window-size>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	char *file_path = argv[1];
	char *host = argv[2];
	int port = atoi(argv[3]);

	window.size = atoi(argv[4]);
	if (window.size <= 0 || window.size > MAX_WINDOW_SIZE) {
		fprintf(stderr, RECEIVER "Invalid window size.\n");
		exit(EXIT_FAILURE);
	}

	int last_path_sep_index = find_last_path_separator(file_path);
	char *file_name = file_path;

	if (last_path_sep_index != -1 && last_path_sep_index < MAX_PATH_SIZE - 1) {
		file_name = file_path + last_path_sep_index + 1;
	}

	FILE *file = fopen(file_name, "w");
	if (!file) {
		fprintf(stderr, RECEIVER "Failed to open file.\n");
		exit(EXIT_FAILURE);
	}

	// Prepare server host address.
	struct hostent *he;
	if (!(he = gethostbyname(host))) {
		fprintf(stderr, RECEIVER "Failed to prepare host address.\n");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in srv_addr = {
	    .sin_family = AF_INET,
	    .sin_port = htons(port),
	    .sin_addr = *((struct in_addr *)he->h_addr),
	};

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		fprintf(stderr, RECEIVER "Failed to initialize socket.\n");
		exit(EXIT_FAILURE);
	}

	// Request file.
	req_file_pkt_t req_file_pkt;
	size_t file_path_len = strlen(file_path);
	strncpy(req_file_pkt.file_path, file_path, file_path_len);

	ssize_t sent_len = sendto(sockfd, &req_file_pkt, file_path_len, 0, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
	if (sent_len != file_path_len) {
		fprintf(stderr, RECEIVER "Truncated packet.\n");
		exit(EXIT_FAILURE);
	}
	printf(RECEIVER "Sending request for file %s, size %ld.\n", file_path, file_path_len);

	// Set timeout.
	struct timeval tv;
	tv.tv_sec = 4;
	tv.tv_usec = 0;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
		fprintf(stderr, RECEIVER "Failed to set timeout.\n");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in src_addr;

	uint32_t selective_acks = 0;

	ssize_t received_len;
	data_pkt_t data_pkt;
	struct data_pkt data_pkts[SEQ_NUM_SIZE];

	ack_pkt_t ack_pkt;

	uint32_t last_packet = 0;
	bool received_eof = false;

	// Iterate over segments, until last the segment is detected.
	do {
		// Receive data packet.
		received_len = receive_data_packet(sockfd, &data_pkt, (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
		if (received_len == -1) {
			fprintf(stderr, RECEIVER "Timeout has been reached.\n");
			close(sockfd);
			fclose(file);
			remove(file_name);
			exit(EXIT_FAILURE);
		} else if (received_len != sizeof(data_pkt)) {
			last_packet = ntohl(data_pkt.seq_num);
			received_eof = true;
		}

		// Packet is inside window.
		if (is_inside_window(&window, ntohl(data_pkt.seq_num))) {
			if (ntohl(data_pkt.seq_num) == window.base) {

				// Write packet with seq_num = rn to file.
				fwrite(data_pkt.data, 1, received_len - sizeof(data_pkt.seq_num), file);
				fprintf(stderr, RECEIVER "Wrote %ld bytes to file, from packet %d.\n", received_len - sizeof(data_pkt.seq_num), window.base);
				window.base = (window.base + 1) % SEQ_NUM_SIZE;

				// Write the following received packets to the file, updating the
				// window.
				while ((selective_acks & 1) == 1) {
					fwrite(data_pkts[window.base].pkt.data, 1, data_pkts[window.base].len - sizeof(data_pkt.seq_num), file);
					fprintf(stderr, RECEIVER "Wrote %ld bytes to file, from packet %d.\n", data_pkts[window.base].len - sizeof(data_pkt.seq_num), window.base);
					selective_acks >>= 1;
					window.base = (window.base + 1) % SEQ_NUM_SIZE;
				}

				selective_acks >>= 1;

			} else {
				// Mark packet as received and store it.
				if (!has_been_received(ntohl(data_pkt.seq_num), window.base, selective_acks)) {
					selective_acks |= 1 << (ntohl(data_pkt.seq_num) - window.base - 1);

					data_pkts[ntohl(data_pkt.seq_num)].pkt = data_pkt;
					data_pkts[ntohl(data_pkt.seq_num)].len = received_len;
				}
			}
		}

		// Send ACK.
		ack_pkt.seq_num = htonl(window.base);
		ack_pkt.selective_acks = htonl(selective_acks);
		sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

	} while (!(received_eof && window.base == last_packet + 1 && selective_acks == 0));

	// Reset timeout to 2 seconds.
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
		fprintf(stderr, RECEIVER "Failed to set timeout.\n");
		exit(EXIT_FAILURE);
	}

	// Reply to the server while it replies back.
	while ((received_len = receive_data_packet(sockfd, &data_pkt, (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)}) != -1)) {
		sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);
	}

	// Clean up and exit.
	close(sockfd);
	fprintf(stderr, RECEIVER "Closed socket.\n");

	fclose(file);
	fprintf(stderr, RECEIVER "Closed file.\n");

	exit(EXIT_SUCCESS);
}
