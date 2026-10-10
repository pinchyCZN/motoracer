/*
 * log.h - console and file logging.
 *
 * Everything the reconstruction prints goes through here. The file is
 * <exe_dir>\<exe_stem>.log - so mracer.exe writes mracer.log beside
 * itself, the same rule config.c uses for the ini.
 *
 *     [DEBUG]
 *     logfile=1                    nothing is written without this
 *     logpath=b:\somewhere\        optional; created if it can be
 *
 * A logpath that cannot be created falls back to the exe's folder
 * rather than losing the log. BESIDE THE EXE IS THE DEFAULT BECAUSE
 * TWO COPIES HAVE TO BE DEBUGGABLE - a second install writes its own
 * log, where one fixed directory would have them overwrite each
 * other. See config.c get_log_dir.
 *
 * KEEP IT SPARSE. This is not an event log and nothing is served by
 * making it one. What belongs here:
 *
 *   o SOMETHING WAS NOT FOUND. A missing asset, a bank that did not
 *     load, a face pointing at a page nobody opened. These are the
 *     lines that have actually saved time.
 *
 *   o SOMETHING BIG SUCCEEDED OR FAILED ONCE. The device coming up,
 *     the archive count, the font that loaded - a handful of startup
 *     lines that say the foundations are there.
 *
 * What does not:
 *
 *   o PER-FRAME ANYTHING. A line per frame is a line per sixteen
 *     milliseconds and the file is useless within a minute.
 *
 *   o TEMPORARY DIAGNOSTICS THAT OUTLIVED THEIR BUG. Adding a dump to
 *     chase a specific problem is right; leaving it in afterwards is
 *     how a log becomes noise nobody reads. Take it out with the fix.
 *
 * o WHY BOTHER AT ALL, given the above: a log file can be read after
 *   the fact, by someone who was not watching the console, without
 *   asking the user to copy and paste. That is its whole job.
 */

#ifndef MRACER_LOG_H
#define MRACER_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open the log file and write a header. Safe to call once, early -
 * before it is called, mrlog still writes to the console.
 *
 * IT OPENS NOTHING UNLESS [DEBUG] logfile=1 IS IN THE INI.
 * Returns 1 if the file opened, 0 if only the console is available
 * - and 0 is the ordinary case now, not a failure.  A log that
 * cannot be opened is not an error worth stopping for either.
 */
int mrlog_init(void);
void mrlog_shutdown(void);

/*
 * File output on or off. DEFAULT OFF, set at startup from
 * [DEBUG] logfile in the ini.  Turning it on here opens the file
 * if it is not already open, so the call works whatever the ini
 * said.
 *
 */
void mrlog_set_file(int on);
int  mrlog_file_enabled(void);

/* The path being written, or "" if the file is not open. */
const char *mrlog_path(void);

/*
 * mrlog     - an ordinary line. Console and file.
 * mrlog_err - the same, prefixed "ERROR: ". Use it when something was
 *             not found or did not work, so the failures can be found
 *             by searching the file for one word.
 *
 * Neither appends a newline; write it in the format string, as printf
 * would. That keeps the call sites identical to what they replaced.
 */
void mrlog(const char *fmt, ...);
void mrlog_err(const char *fmt, ...);

/*
 * MR_STUB_ONCE - a function that exists in the call graph but has no
 * body yet says so, ONCE, the first time it runs.
 *
 * THE POINT IS THAT A MISSING SUBSYSTEM MUST NOT LOOK LIKE A WORKING
 * ONE. A stub that returns the neutral value silently is
 * indistinguishable at run time from a correct implementation - which
 * is the failure mode CLAUDE.md rule 5a exists to prevent, and it is
 * how a gap survives three sessions unnoticed. assets.c already works
 * this way for extracted-tree fallbacks: a run that prints nothing is
 * a run with nothing missing.
 *
 *   tag    names the original and its address, so the line greps back
 *          to the binary.
 *   what   what the real thing would do, in a few words.
 *
 * ONCE PER PROCESS, not once per call - most of these sit in the
 * per-frame draw and would otherwise bury the log.
 */
#define MR_STUB_ONCE(tag, what)                                      \
    do {                                                                \
	    static int mr_stub_said;                                           \
	    if (!mr_stub_said) {                                               \
		    mr_stub_said = 1;                                                 \
		    mrlog("stub:  %s - %s\n", (tag), (what));                        \
	    }                                                                  \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* MRACER_LOG_H */
