// SPDX-License-Identifier: Apache-2.0 OR MIT
/*
 * subucom-log: bring-up logger for the XDJ-RX3 panel links. Reads the TX
 * frames the player sends on each subucom channel from /dev/rx3_subucom
 * (guest/modules/rx3_board) and prints a frame to the console whenever it
 * differs from that channel's previous one.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct rec {
	uint8_t chan, pad;
	uint16_t len;
	uint32_t seq;
} __attribute__((packed));

static const char *names[] = { "spi1", "spi2", "rdy3", "rdy4" };

int main(void)
{
	static uint8_t last[4][256];
	static uint16_t last_len[4];
	uint8_t buf[sizeof(struct rec) + 256];
	int fd = open("/dev/rx3_subucom", O_RDONLY);

	if (fd < 0) {
		perror("subucom-log: /dev/rx3_subucom");
		return 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		struct rec r;

		if (n < (ssize_t)sizeof(r))
			continue;
		memcpy(&r, buf, sizeof(r));
		if (r.chan > 3 || r.len > 256)
			continue;
		if (r.len == last_len[r.chan] &&
		    !memcmp(last[r.chan], buf + sizeof(r), r.len))
			continue;
		memcpy(last[r.chan], buf + sizeof(r), r.len);
		last_len[r.chan] = r.len;
		printf("subucom %s tx #%u len %u:", names[r.chan], r.seq, r.len);
		for (unsigned i = 0; i < r.len; i++)
			printf("%s%02x", i % 32 ? " " : "\n  ", buf[sizeof(r) + i]);
		printf("\n");
		fflush(stdout);
	}
}
