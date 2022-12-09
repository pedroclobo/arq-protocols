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

int main(int argc, char *argv[]) {
	int port = atoi(argv[1]);

	// Prepare server socket.
	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		perror("socket");
		exit(-1);
	}

	// Allow address reuse so we can rebind to the same port,
	// after restarting the server.
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1},
	               sizeof(int)) < 0) {
		perror("setsockopt");
		exit(-1);
	}

	struct sockaddr_in srv_addr = {
	    .sin_family = AF_INET,
	    .sin_addr.s_addr = htonl(INADDR_ANY),
	    .sin_port = htons(port),
	};
	if (bind(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr))) {
		perror("bind");
		exit(-1);
	}
	fprintf(stderr, "Receiving on port: %d\n", port);

	ssize_t len;
	struct sockaddr_in src_addr;
	req_file_pkt_t req_file_pkt;

	len = recvfrom(sockfd, &req_file_pkt, sizeof(req_file_pkt), 0,
	               (struct sockaddr *)&src_addr,
	               &(socklen_t){sizeof(src_addr)});
	if (len < MAX_PATH_SIZE) {
		req_file_pkt.file_path[len] = '\0';
	}
	printf("Received request for file %s, size %ld.\n",
	       req_file_pkt.file_path, len);

	FILE *file = fopen(req_file_pkt.file_path, "r");
	if (!file) {
		perror("fopen");
		exit(-1);
	}

	uint32_t seq_num = 0;
	data_pkt_t data_pkt;
	size_t data_len;
	do { // Generate segments from file, until the the end of the file.
		// Prepare data segment.
		data_pkt.seq_num = htonl(seq_num++);

		// Load data from file.
		data_len = fread(data_pkt.data, 1, sizeof(data_pkt.data), file);
		printf("read %ld\n", data_len);

		// Send segment.
		ssize_t sent_len = sendto(
		    sockfd, &data_pkt, offsetof(data_pkt_t, data) + data_len, 0,
		    (struct sockaddr *)&src_addr, sizeof(src_addr));
		printf("Sending segment %d, size %ld.\n",
		       ntohl(data_pkt.seq_num),
		       offsetof(data_pkt_t, data) + data_len);
		if (sent_len != offsetof(data_pkt_t, data) + data_len) {
			fprintf(stderr, "Truncated packet.\n");
			exit(-1);
		}
	} while (!(feof(file) && data_len < sizeof(data_pkt.data)));

	// Clean up and exit.
	close(sockfd);
	fclose(file);

	return 0;
}
