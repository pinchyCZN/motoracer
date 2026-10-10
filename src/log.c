/*
 * log.c - see log.h.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "config.h"

static FILE *s_file;
static int   s_fileEnabled;         /* default OFF, see log.h */
static char  s_path[MAX_PATH];

static int open_log_file(void)
{
	const char *p;
	time_t      now;
	struct tm  *lt;

	if (s_file)
		return 1;

	p = get_log_file();
	if (!p || !p[0])
		return 0;
	strncpy(s_path, p, MAX_PATH - 1);
	s_path[MAX_PATH - 1] = 0;

	/*
	 * TRUNCATED, NOT APPENDED. The question this file answers is
	 * "what happened on the run I just did", and an appended log
	 * makes you find the last session before you can start. If a
	 * history is ever wanted, it wants rotation, not append.
	 */
	s_file = fopen(s_path, "w");
	if (!s_file) {
		s_path[0] = 0;
		return 0;
	}

	now = time(NULL);
	lt  = localtime(&now);
	if (lt)
		fprintf(s_file, "mracer log  %04d-%02d-%02d %02d:%02d:%02d\n\n",
	            lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
	            lt->tm_hour, lt->tm_min, lt->tm_sec);
	fflush(s_file);
	return 1;
}

/*
 * [DEBUG] logfile.  ABSENT MEANS NO FILE.
 */
int mrlog_init(void)
{
	int on = 0;

	if (s_file)
		return 1;

	get_ini_value("DEBUG", "logfile", &on);
	s_fileEnabled = on ? 1 : 0;
	if (!s_fileEnabled)
		return 0;

	return open_log_file();
}

void mrlog_shutdown(void)
{
	if (s_file) {
		fclose(s_file);
		s_file = NULL;
	}
	s_path[0] = 0;
}

void mrlog_set_file(int on)
{
	s_fileEnabled = on ? 1 : 0;
	if (s_fileEnabled)
		open_log_file();
}

int mrlog_file_enabled(void)
{
	return s_fileEnabled && s_file != NULL;
}

const char *mrlog_path(void)
{
	return s_path;
}

/*
 * One formatting pass, two destinations. Formatting twice would let
 * the console and the file disagree if a %n$ or a side effect ever
 * crept into a format string, and it costs a buffer to avoid.
 *
 * FLUSHED EVERY LINE. A log that is buffered when the process dies is
 * missing exactly the part that matters.
 */
static void emit(const char *prefix, const char *fmt, va_list ap)
{
	char line[1024];
	int  n;

	n = _vsnprintf(line, sizeof(line) - 1, fmt, ap);
	if (n < 0)
		n = (int)sizeof(line) - 1;
	line[sizeof(line) - 1] = 0;

	if (prefix)
		fputs(prefix, stdout);
	fputs(line, stdout);

	if (s_file && s_fileEnabled) {
		if (prefix)
			fputs(prefix, s_file);
		fputs(line, s_file);
		fflush(s_file);
	}
}

void mrlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	emit(NULL, fmt, ap);
	va_end(ap);
}

void mrlog_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	emit("ERROR: ", fmt, ap);
	va_end(ap);
}
