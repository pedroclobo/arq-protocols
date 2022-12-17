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

	printf(GREEN "Receiver: " RESET "Sent ACK with seq_num %d and selective_acks %b.\n", ntohl(ack_pkt->seq_num), ntohl(ack_pkt->selective_acks));

	return sent_len;
}

ssize_t receive_data_packet(int sockfd, struct data_pkt_t *data_pkt, struct sockaddr *src_addr, socklen_t *src_addr_len) {
	ssize_t received_len = recvfrom(sockfd, data_pkt, sizeof(*data_pkt), 0, src_addr, src_addr_len);

	if (received_len == -1) {
		return -1;
	} else if (received_len == 0) {
		return 0;
	}

	printf(GREEN "Receiver: " RESET "Received segment %d, size %ld.\n", ntohl(data_pkt->seq_num), received_len);

	return received_len;
}

int is_inside_window(uint32_t rn, uint32_t r_size, uint32_t seq_num) { return rn <= seq_num && seq_num < rn + r_size; }

int has_been_received(uint32_t seq_num, uint32_t rn, uint32_t selective_acks) {
	int mask = 1 << (seq_num - rn - 1);
	return (selective_acks & mask) == mask;
}

int main(int argc, char *argv[]) {
	// Parse command line arguments.
	if (argc != 5) {
		fprintf(stderr, "Usage: %s <file> <host> <port> <window-size>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	char *file_path = argv[1];
	char *host = argv[2];
	int port = atoi(argv[3]);

	int r_size = atoi(argv[4]);
	if (r_size < 1 || r_size > MAX_WINDOW_SIZE) {
		fprintf(stderr, "Receiver: Invalid window size.\n");
		exit(EXIT_FAILURE);
	}

	int last_path_sep_index = find_last_path_separator(file_path);
	char *file_name = file_path;

	if (last_path_sep_index != -1 && last_path_sep_index < MAX_PATH_SIZE - 1) {
		file_name = file_path + last_path_sep_index + 1;
	}

	FILE *file = fopen(file_name, "w");
	if (!file) {
		exit(EXIT_FAILURE);
	}

	// Prepare server host address.
	struct hostent *he;
	if (!(he = gethostbyname(host))) {
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in srv_addr = {
	    .sin_family = AF_INET,
	    .sin_port = htons(port),
	    .sin_addr = *((struct in_addr *)he->h_addr),
	};

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		exit(EXIT_FAILURE);
	}

	// Request file.
	req_file_pkt_t req_file_pkt;
	size_t file_path_len = strlen(file_path);
	strncpy(req_file_pkt.file_path, file_path, file_path_len);

	ssize_t sent_len = sendto(sockfd, &req_file_pkt, file_path_len, 0, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
	if (sent_len != file_path_len) {
		fprintf(stderr, "Receiver: Truncated packet.\n");
		exit(EXIT_FAILURE);
	}
	printf("Sending request for file %s, size %ld.\n", file_path, file_path_len);

	// Set timeout.
	struct timeval tv;
	tv.tv_sec = 4;
	tv.tv_usec = 0;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
		fprintf(stderr, "Receiver: Failed to set timeout.\n");
		exit(EXIT_FAILURE);
	}

	uint32_t rn = 0;
	uint32_t selective_acks = 0;

	ssize_t received_len;
	data_pkt_t data_pkt;
	data_pkt_t data_pkts[SEQ_NUM_SIZE];
	ack_pkt_t ack_pkt;
	struct sockaddr_in src_addr;

	// Iterate over segments, until last the segment is detected.
	do {
		// Receive data packet.
		received_len = receive_data_packet(sockfd, &data_pkt, (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
		// if (received_len == -1) {
		// 	fprintf(stderr, "Receiver: Timeout has been reached.\n");
		// 	fclose(file);
		// 	exit(EXIT_FAILURE);
		// }

		// packet outside window arrived
		// discard it and resend ack with seq_num = rn
		if (!is_inside_window(rn, r_size, ntohl(data_pkt.seq_num))) {
			ack_pkt.seq_num = htonl(rn);
			ack_pkt.selective_acks = htonl(selective_acks);
			sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

		// packet inside window arrived
		} else {
			if (ntohl(data_pkt.seq_num) == rn) {
				// update window
				ack_pkt.seq_num = htonl((rn + 1) % SEQ_NUM_SIZE);
				ack_pkt.selective_acks = htonl(selective_acks >> 1);
				sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

				data_pkts[rn] = data_pkt;

				// write packets to file
				do {
					fwrite(data_pkts[rn].data, 1, received_len - sizeof(data_pkt.seq_num), file);
					selective_acks >>= 1;
					rn = (rn + 1) % SEQ_NUM_SIZE;
				} while (has_been_received(rn, rn, selective_acks));

			} else {
				if (!has_been_received(ntohl(data_pkt.seq_num), rn, selective_acks)) {
					// Mark as received.
					// Send ACK.
					ack_pkt.seq_num = htonl(rn);
					selective_acks |= 1 << (ntohl(data_pkt.seq_num) - rn - 1);
					ack_pkt.selective_acks = htonl(selective_acks);
					sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

					// Store packet.
					data_pkts[ntohl(data_pkt.seq_num)] = data_pkt;
				} else {
					// Send ACK.
					ack_pkt.seq_num = htonl(rn);
					ack_pkt.selective_acks = htonl(selective_acks);
					sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);
				}
			}
		}

	} while (received_len == sizeof(data_pkt_t));

	// Clean up and exit.
	close(sockfd);
	fclose(file);
	fprintf(stderr, "Receiver: Closing socket.\n");

	return 0;
}
