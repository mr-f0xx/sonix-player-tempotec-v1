#ifndef TRANSFERHTTP_H
#define TRANSFERHTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRANSFER_CODE_LEN 8

typedef struct transfer_http transfer_http_t;

// A single-session HTTP file manager. Binds only the supplied IPv4 address;
// port 0 asks the OS for a port (used by the host tests). The server duplicates
// the mounted card's directory descriptor and borrows page until stop().
transfer_http_t *transfer_http_start(int card_fd, const char *ip, uint16_t port, const char *hostname,
									 const char *page, size_t page_size);
uint16_t transfer_http_port(const transfer_http_t *server);
const char *transfer_http_code(const transfer_http_t *server);
bool transfer_http_running(const transfer_http_t *server);
// One event after a successful upload and a brief quiet period. A new upload
// defers it until the new file has been saved; failed/aborted uploads do not
// produce an event. The caller keeps the session alive during the read.
bool transfer_http_take_upload_complete(transfer_http_t *server);

// Interrupts any client and joins the worker. No card descriptor or unfinished
// upload is left behind when this returns. Accepts NULL.
void transfer_http_stop(transfer_http_t *server);

#endif /* TRANSFERHTTP_H */
