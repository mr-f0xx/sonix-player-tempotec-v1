/* Intercept the HCI syscalls in the actual AFH writer. No Bluetooth hardware
 * or privileges: check the bytes, binding, and error paths the scheduler uses. */
#define socket test_socket
#define bind test_bind
#define fcntl test_fcntl
#define write test_write
#define close test_close
#include "../src/system/bluetooth/btafh.c"
#undef socket
#undef bind
#undef fcntl
#undef write
#undef close

#include <assert.h>

enum failure { NONE, SOCKET_FAIL, BIND_FAIL, WRITE_FAIL, SHORT_WRITE };
static enum failure failure;
static int closes;
static int writes;
static uint8_t sent[14];

int test_socket(int domain, int type, int protocol) {
	assert(domain == BT_AF_BLUETOOTH && type == SOCK_RAW && protocol == BT_PROTO_HCI);
	if (failure == SOCKET_FAIL) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	return 99;
}

int test_fcntl(int fd, int cmd, ...) {
	assert(fd == 99 && cmd == F_SETFD);
	return 0;
}

int test_bind(int fd, const struct sockaddr *addr, socklen_t size) {
	assert(fd == 99 && size == sizeof(struct bt_sockaddr_hci));
	const struct bt_sockaddr_hci *hci = (const struct bt_sockaddr_hci *)addr;
	assert(hci->hci_family == BT_AF_BLUETOOTH && hci->hci_dev == 0 && hci->hci_channel == BT_HCI_CHANNEL_RAW);
	if (failure == BIND_FAIL) {
		errno = ENODEV;
		return -1;
	}
	return 0;
}

ssize_t test_write(int fd, const void *data, size_t size) {
	assert(fd == 99 && size == sizeof(sent));
	writes++;
	memcpy(sent, data, size);
	if (failure == WRITE_FAIL) {
		errno = ENETDOWN;
		return -1;
	}
	return failure == SHORT_WRITE ? (ssize_t)size - 1 : (ssize_t)size;
}

int test_close(int fd) {
	assert(fd == 99);
	closes++;
	errno = EBADF; /* a close must not overwrite the error being reported */
	return 0;
}

int main(void) {
	failure = NONE;
	assert(btafh_apply_for_wifi(2437));
	const uint8_t expected[] = {0x01, 0x3f, 0x0c, 10, 0xff, 0xff, 0x7f, 0x00,
								0x00, 0x00, 0xff, 0xff, 0xff, 0x7f};
	assert(memcmp(sent, expected, sizeof(expected)) == 0);
	assert(writes == 1 && closes == 1);

	assert(btafh_apply_for_wifi(0));
	for (int i = 4; i < 13; i++) {
		assert(sent[i] == 0xff);
	}
	assert(sent[13] == 0x7f);

	failure = SOCKET_FAIL;
	int was_closed = closes;
	assert(!btafh_apply_for_wifi(2437) && closes == was_closed);
	failure = BIND_FAIL;
	int was_written = writes;
	assert(!btafh_apply_for_wifi(2437) && closes == ++was_closed && writes == was_written);
	failure = WRITE_FAIL;
	assert(!btafh_apply_for_wifi(2437) && closes == ++was_closed && writes == ++was_written);
	failure = SHORT_WRITE;
	assert(!btafh_apply_for_wifi(2437) && closes == ++was_closed && writes == ++was_written);
	failure = NONE;
	assert(btafh_apply_for_wifi(2437));
	assert(closes == ++was_closed && writes == ++was_written);

	printf("PASS btafh socket (HCI packet, unrestricted map and failure paths)\n");
	return 0;
}
