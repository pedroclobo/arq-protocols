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

int main(int argc, char *argv[]) {
	char *file_path = argv[1];
	char *host = argv[2];
	int port = atoi(argv[3]);

	int last_path_sep_index = find_last_path_separator(file_path);
	char *file_name = file_path;

	if (last_path_sep_index != -1 &&
	    last_path_sep_index < MAX_PATH_SIZE - 1) {
		file_name = file_path + last_path_sep_index + 1;
	}

	FILE *file = fopen(file_name, "w");
	if (!file) {
		perror("fopen");
		exit(-1);
	}

	// Prepare server host address.
	struct hostent *he;
	if (!(he = gethostbyname(host))) {
		perror("gethostbyname");
		exit(-1);
	}

	struct sockaddr_in srv_addr = {
	    .sin_family = AF_INET,
	    .sin_port = htons(port),
	    .sin_addr = *((struct in_addr *)he->h_addr),
	};

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		perror("socket");
		exit(-1);
	}

	req_file_pkt_t req_file_pkt;
	size_t file_path_len = strlen(file_path);
	strncpy(req_file_pkt.file_path, file_path, file_path_len);

	ssize_t sent_len =
	    sendto(sockfd, &req_file_pkt, file_path_len, 0,
	           (struct sockaddr *)&srv_addr, sizeof(srv_addr));
	if (sent_len != file_path_len) {
		fprintf(stderr, "Truncated packet.\n");
		exit(-1);
	}
	printf("Sending request for file %s, size %ld.\n", file_path,
	       file_path_len);

	ssize_t len;
	do { // Iterate over segments, until last the segment is detected.
		// Receive segment.
		data_pkt_t data_pkt;
		struct sockaddr_in src_addr;

		len = recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0,
		               (struct sockaddr *)&src_addr,
		               &(socklen_t){sizeof(src_addr)});
		printf("Received segment %d, size %ld.\n",
		       ntohl(data_pkt.seq_num), len);

		// Write data to file.
		fwrite(data_pkt.data, 1, len - offsetof(data_pkt_t, data),
		       file);
	} while (len == sizeof(data_pkt_t));

	// Clean up and exit.
	close(sockfd);
	fclose(file);

	return 0;
}
