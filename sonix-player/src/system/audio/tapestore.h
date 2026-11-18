#ifndef TAPESTORE_H
#define TAPESTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAPE_SAVE_NAME_SIZE 40

typedef enum { TAPE_STORE_SEQUENCE, TAPE_STORE_TAKE } tape_store_kind_t;

typedef struct {
	int dir, fd;
	char name[TAPE_SAVE_NAME_SIZE];
	char part[TAPE_SAVE_NAME_SIZE + sizeof("..part") - 1];
} tape_savefile_t;

// Open a fresh hidden part on a reserved card, then publish it only after
// the payload is complete and flushed. end() always closes it. No overwrite.
bool tape_store_begin(int card, tape_store_kind_t kind, tape_savefile_t *out);
bool tape_store_end(tape_savefile_t *file, bool publish);
bool tape_store_write(int fd, const void *data, size_t size);

// Caller owns the fd. Checks that the name is one of the deck's own names.
int tape_store_open(int card, tape_store_kind_t kind, const char *name);
// Newest first, after an optional filename cursor. Only finalized files.
int tape_store_list(tape_store_kind_t kind, const char *after, char names[][TAPE_SAVE_NAME_SIZE], int cap);
bool tape_store_take_path(const char *name, char *out, size_t size);

// The recorded stream is stereo signed 16-bit little endian PCM.
bool tape_store_wav_header(int fd, uint32_t rate, uint32_t frames);

#endif
