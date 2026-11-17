#ifndef WEBEMU_WASM_COMPAT_LINUX_NETLINK_H
#define WEBEMU_WASM_COMPAT_LINUX_NETLINK_H

#include <stdint.h>
#include <sys/socket.h>

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#ifndef PF_NETLINK
#define PF_NETLINK AF_NETLINK
#endif
#define NETLINK_ROUTE 0
#define NETLINK_KOBJECT_UEVENT 15

struct sockaddr_nl {
	sa_family_t nl_family;
	unsigned short nl_pad;
	uint32_t nl_pid;
	uint32_t nl_groups;
};

#endif /* WEBEMU_WASM_COMPAT_LINUX_NETLINK_H */
