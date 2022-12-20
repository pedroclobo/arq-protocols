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

bool is_inside_window(uint32_t rn, uint32_t r_size, uint32_t seq_num) {
	uint32_t limit = (rn + r_size) % SEQ_NUM_SIZE;
	if (limit < rn) {
		return seq_num >= rn || seq_num < limit;
	} else {
		return seq_num >= rn && seq_num < limit;
	}
}

bool has_been_received(uint32_t seq_num, uint32_t rn, uint32_t selective_acks) {
	int mask = 1 << (seq_num - rn - 1);
	return (selective_acks & mask) == mask;
}

int main(int argc, char *argv[]) {
	// Parse command line arguments.
	if (argc != 5) {
		fprintf(stderr, RECEIVER "Usage: %s <file> <host> <port> <window-size>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	char *file_path = argv[1];
	char *host = argv[2];
	int port = atoi(argv[3]);

	int r_size = atoi(argv[4]);
	if (r_size < 1 || r_size > MAX_WINDOW_SIZE) {
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
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
		fprintf(stderr, RECEIVER "Failed to set timeout.\n");
		exit(EXIT_FAILURE);
	}

	uint32_t rn = 0;
	uint32_t selective_acks = 0;

	ssize_t received_len;
	data_pkt_t data_pkt;
	data_pkt_t data_pkts[SEQ_NUM_SIZE];
	size_t data_pkts_len[SEQ_NUM_SIZE];
	ack_pkt_t ack_pkt;
	struct sockaddr_in src_addr;

	// Iterate over segments, until last the segment is detected.
	do {
		// Receive data packet.
		received_len = receive_data_packet(sockfd, &data_pkt, (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
		if (received_len == -1) {
			received_len = receive_data_packet(sockfd, &data_pkt, (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
		}
		if (received_len == -1) {
			fprintf(stderr, RECEIVER "Timeout has been reached.\n");
			fclose(file);
			exit(EXIT_FAILURE);
		}

		// Packet outside window arrived.
		// Discard it and resend ACK with seq_num = rn.
		if (!is_inside_window(rn, r_size, ntohl(data_pkt.seq_num))) {
			ack_pkt.seq_num = htonl(rn);
			ack_pkt.selective_acks = htonl(selective_acks);
			sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

			// Packet inside window arrived.
		} else {
			if (ntohl(data_pkt.seq_num) == rn) {

				fwrite(data_pkt.data, 1, received_len - sizeof(data_pkt.seq_num), file);
				fprintf(stderr, RECEIVER "Wrote %ld bytes to file, from packet %d.\n", received_len - sizeof(data_pkt.seq_num), rn);
				rn = (rn + 1) % SEQ_NUM_SIZE;

				// Update window and send ACK.
				while ((selective_acks & 1) == 1) {
					fwrite(data_pkts[rn].data, 1, data_pkts_len[rn] - sizeof(data_pkt.seq_num), file);
					fprintf(stderr, RECEIVER "Wrote %ld bytes to file, from packet %d.\n", data_pkts_len[rn] - sizeof(data_pkt.seq_num), rn);
					selective_acks >>= 1;
					rn = (rn + 1) % SEQ_NUM_SIZE;
				}

				selective_acks >>= 1;

				ack_pkt.seq_num = htonl(rn);
				ack_pkt.selective_acks = htonl(selective_acks);
				sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

			} else {
				if (!has_been_received(ntohl(data_pkt.seq_num), rn, selective_acks)) {
					// Mark as received and send ACK.
					selective_acks |= 1 << (ntohl(data_pkt.seq_num) - rn - 1);

					ack_pkt.seq_num = htonl(rn);
					ack_pkt.selective_acks = htonl(selective_acks);
					sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

					// Store packet and its length.
					data_pkts[ntohl(data_pkt.seq_num)] = data_pkt;
					data_pkts_len[ntohl(data_pkt.seq_num)] = received_len;

				} else {
					// Send ACK.
					ack_pkt.seq_num = htonl(rn);
					ack_pkt.selective_acks = htonl(selective_acks);
					sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);
				}
			}
		}

	} while (received_len == sizeof(data_pkt_t) || selective_acks != 0);

	// Clean up and exit.
	while (true) {
		received_len = receive_data_packet(sockfd, &data_pkt, (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
		if (received_len == -1) {
			break;
		}
		sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);
	}

	// Clean up and exit.
	close(sockfd);
	fprintf(stderr, RECEIVER "Closing socket.\n");
	fclose(file);
	fprintf(stderr, RECEIVER "Closing file.\n");

	return 0;
}
