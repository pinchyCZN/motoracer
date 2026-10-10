/*
 * bkf.c - see bkf.h. A transcription of tools/bkf.py, which is the
 * oracle.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bkf.h"
#include "log.h"

typedef struct {
	char         name[BKF_NAME_LEN + 1];
	unsigned int offset;
	unsigned int size;
} BKF_ENTRY;

struct BKF {
	FILE      *f;
	long       fileSize;
	int        count;
	BKF_ENTRY *entries;
};

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* '/' and '\\' are the same character for our purposes, and case is
 * not significant. */
static int name_char_eq(char a, char b)
{
	if (a == '/') a = '\\';
	if (b == '/') b = '\\';
	if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
	if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
	return a == b;
}

static int name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (!name_char_eq(*a, *b))
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

BKF *bkf_open(const char *path)
{
	BKF           *a;
	unsigned char  head[4];
	unsigned char *dir = NULL;
	unsigned int   count;
	unsigned int   dirSize;
	unsigned int   i;

	if (!path)
		return NULL;

	a = (BKF *)calloc(1, sizeof(BKF));
	if (!a)
		return NULL;

	a->f = fopen(path, "rb");
	if (!a->f) {
		free(a);
		return NULL;
	}
	if (fseek(a->f, 0, SEEK_END) != 0 || (a->fileSize = ftell(a->f)) <= 0 ||
	    fseek(a->f, 0, SEEK_SET) != 0) {
		bkf_close(a);
		return NULL;
	}
	if (fread(head, 1, 4, a->f) != 4) {
		bkf_close(a);
		return NULL;
	}

	/*
	 * There is NO MAGIC NUMBER. The file opens with the entry count, so
	 * the only structural check available is that the count is
	 * plausible and the directory fits - which is exactly what
	 * tools/bkf.py does. 100000 is its bound and it is kept.
	 */
	count = rd32(head);
	if (count == 0 || count > 100000) {
		mrlog_err("bkf: %s has an implausible entry count %u\n", path, count);
		bkf_close(a);
		return NULL;
	}
	dirSize = count * BKF_ENTRY_SIZE;
	if ((unsigned int)a->fileSize < 4 + dirSize) {
		mrlog("bkf: %s directory of %u entries overruns the file\n",
		       path, count);
		bkf_close(a);
		return NULL;
	}

	dir = (unsigned char *)malloc(dirSize);
	a->entries = (BKF_ENTRY *)calloc(count, sizeof(BKF_ENTRY));
	if (!dir || !a->entries) {
		free(dir);
		bkf_close(a);
		return NULL;
	}
	if (fread(dir, 1, dirSize, a->f) != dirSize) {
		mrlog("bkf: %s short read on the directory\n", path);
		free(dir);
		bkf_close(a);
		return NULL;
	}

	for (i = 0; i < count; i++) {
		const unsigned char *e = dir + i * BKF_ENTRY_SIZE;

		/*
		 * The name field MAY HOLD GARBAGE AFTER ITS TERMINATOR - the
		 * writer clearly reused a buffer - so it has to be copied up to
		 * the NUL rather than taken whole. It is also not guaranteed to
		 * be terminated at all inside 36 bytes, hence the explicit
		 * terminator on our copy.
		 */
		memcpy(a->entries[i].name, e, BKF_NAME_LEN);
		a->entries[i].name[BKF_NAME_LEN] = 0;

		a->entries[i].offset = rd32(e + BKF_NAME_LEN);
		a->entries[i].size   = rd32(e + BKF_NAME_LEN + 4);

		if (a->entries[i].offset > (unsigned int)a->fileSize ||
		    a->entries[i].size > (unsigned int)a->fileSize -
		                         a->entries[i].offset) {
			mrlog("bkf: %s member %u (%s) lies outside the file\n",
			       path, i, a->entries[i].name);
			free(dir);
			bkf_close(a);
			return NULL;
		}
	}
	free(dir);

	a->count = (int)count;
	return a;
}

void bkf_close(BKF *a)
{
	if (!a)
		return;
	if (a->f)
		fclose(a->f);
	free(a->entries);
	free(a);
}

int bkf_count(const BKF *a)
{
	return a ? a->count : 0;
}

const char *bkf_name(const BKF *a, int index)
{
	if (!a || index < 0 || index >= a->count)
		return NULL;
	return a->entries[index].name;
}

unsigned int bkf_size(const BKF *a, int index)
{
	if (!a || index < 0 || index >= a->count)
		return 0;
	return a->entries[index].size;
}

int bkf_find(const BKF *a, const char *member)
{
	int i;

	if (!a || !member)
		return -1;
	for (i = 0; i < a->count; i++) {
		if (name_eq(a->entries[i].name, member))
			return i;
	}
	return -1;
}

void *bkf_read(BKF *a, int index, unsigned int *sizeOut)
{
	void        *buf;
	unsigned int size;

	if (sizeOut)
		*sizeOut = 0;
	if (!a || index < 0 || index >= a->count)
		return NULL;

	size = a->entries[index].size;
	if (size == 0)
		return NULL;

	if (fseek(a->f, (long)a->entries[index].offset, SEEK_SET) != 0)
		return NULL;

	buf = malloc(size);
	if (!buf)
		return NULL;
	if (fread(buf, 1, size, a->f) != size) {
		mrlog("bkf: short read on member %s\n", a->entries[index].name);
		free(buf);
		return NULL;
	}

	if (sizeOut)
		*sizeOut = size;
	return buf;
}
