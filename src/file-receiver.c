#include "packet-format.h"
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

	printf("Receiver: Sent ACK segment %d, size %ld.\n", ntohl(ack_pkt->seq_num), sent_len);

	return sent_len;
}

ssize_t receive_data_packet(int sockfd, struct data_pkt_t *data_pkt, struct sockaddr *src_addr, socklen_t *src_addr_len) {
	ssize_t received_len = recvfrom(sockfd, data_pkt, sizeof(*data_pkt), 0, src_addr, src_addr_len);

	if (received_len == -1) {
		return -1;
	} else if (received_len == 0) {
		return 0;
	}

	printf("Receiver: Received segment %d, size %ld.\n", ntohl(data_pkt->seq_num), received_len);

	return received_len;
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

	int window_size = atoi(argv[4]);
	if (window_size < 1 || window_size > MAX_WINDOW_SIZE) {
		fprintf(stderr, "Invalid window size\n");
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
		fprintf(stderr, "Truncated packet.\n");
		exit(EXIT_FAILURE);
	}
	printf("Sending request for file %s, size %ld.\n", file_path, file_path_len);

	// Set timeout.
	struct timeval tv;
	tv.tv_sec = 4;
	tv.tv_usec = 0;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
		fprintf(stderr, "Failed to set timeout\n");
		exit(EXIT_FAILURE);
	}

	uint32_t seq_num = 0;

	ssize_t received_len;
	data_pkt_t data_pkt;
	ack_pkt_t ack_pkt;
	struct sockaddr_in src_addr;

	// Iterate over segments, until last the segment is detected.
	do {
		// Receive data packet.
		received_len = receive_data_packet(sockfd, &data_pkt, (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
		if (received_len == -1) {
			fprintf(stderr, "Receiver timeout has been reached\n");
			fclose(file);
			exit(EXIT_FAILURE);
		}

		// Corrupted packet
		/* if (received_len != sizeof(data_pkt_t)) {
		        continue;
		} */

		// Write new packet data to file.
		// If seq_num != data_pkt.seq_num the packet is duplicated and we need to
		// send an ACK
		if (seq_num == ntohl(data_pkt.seq_num)) {
			fwrite(data_pkt.data, 1, received_len - offsetof(data_pkt_t, data), file);
			seq_num = (seq_num + 1) % MAX_WINDOW_SIZE;
		}

		// Send ACK
		ack_pkt.seq_num = htonl(seq_num);
		ack_pkt.selective_acks = 0; // TODO
		sent_len = send_ack_packet(sockfd, &ack_pkt, sizeof(ack_pkt), (struct sockaddr *)&src_addr);

	} while (received_len == sizeof(data_pkt_t));

	// Clean up and exit.
	close(sockfd);
	fclose(file);

	return 0;
}
