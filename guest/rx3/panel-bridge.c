// SPDX-License-Identifier: Apache-2.0 OR MIT
/* Host records on hvc1 -> RX3 panel bus. Stream framing survives partial
 * virtio-console reads. The host supplies complete control states with CRC.
 * Start with valid zero diagnostic frames so rbp can finish key registration.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

static uint16_t crc(const uint8_t *p, unsigned n)
{
    uint16_t c = 0xffff;
    for (unsigned i = 0; i < n; i += 4)
        for (int j = 3; j >= 0; --j) {
            c ^= p[i + j];
            for (unsigned k = 0; k < 8; ++k)
                c = (c >> 1) ^ ((c & 1) ? 0x8408 : 0);
        }
    return c ^ 0xffff;
}
static int exact(int fd, uint8_t *p, unsigned n)
{
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}
int main(int argc, char **argv)
{
    int bus = open("/dev/rx3_subucom", O_WRONLY);
    int input = open(argc > 1 ? argv[1] : "/dev/hvc1", O_RDWR | O_NOCTTY);
    if (bus < 0 || input < 0) { perror("rx3-panel"); return 1; }
    struct termios t;
    if (!tcgetattr(input, &t)) { cfmakeraw(&t); tcsetattr(input, TCSANOW, &t); }
    for (unsigned chan = 0; chan < 2; ++chan) {
        uint8_t rec[112] = {0};
        unsigned covered = chan ? 24 : 96, len = chan ? 104 : 100;
        uint16_t c = crc(rec + 8, covered);
        rec[0] = chan; rec[2] = len;
        rec[8 + covered + 2] = c; rec[8 + covered + 3] = c >> 8;
        if (write(bus, rec, 8 + len) != (ssize_t)(8 + len)) { perror("panel idle"); return 1; }
    }
    for (;;) {
        uint8_t rec[264];
        if (exact(input, rec, 8)) { usleep(20000); continue; }
        unsigned len = rec[2] | ((unsigned)rec[3] << 8);
        if (rec[0] > 3 || len > 256 || (rec[0] < 2 && len > 128)) {
            fprintf(stderr, "rx3-panel: invalid host record\n");
            return 1;
        }
        if (exact(input, rec + 8, len)) return 1;
        if (write(bus, rec, len + 8) != (ssize_t)(len + 8)) perror("panel write");
    }
}
