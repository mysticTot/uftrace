#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>

#include "uftrace.h"
#include "utils/utils.h"
#include "utils/fstack.h"
#include "libmcount/mcount.h"


static char *tmp_dirname;
static void cleanup_tempdir(void)
{
	DIR *dp;
	struct dirent *ent;
	char path[PATH_MAX];

	if (!tmp_dirname)
		return;

	dp = opendir(tmp_dirname);
	if (dp == NULL) {
		if (errno == ENOENT)
			return;
		pr_err("cannot open temp dir");
	}

	while ((ent = readdir(dp)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "%s/%s", tmp_dirname, ent->d_name);
		if (unlink(path) < 0)
			pr_err("unlink failed: %s", path);
	}

	closedir(dp);

	if (rmdir(tmp_dirname) < 0)
		pr_err("rmdir failed: %s", tmp_dirname);
	tmp_dirname = NULL;
}

static void reset_live_opts(struct opts *opts)
{
	/* this is needed to set display_depth at replay */
	live_disabled = opts->disabled;

	/*
	 * These options are handled in record and no need to do it in
	 * replay again.
	 */
	opts->filter	= NULL;
	opts->depth	= MCOUNT_DEFAULT_DEPTH;
	opts->disabled	= false;
	opts->threshold = 0;
}

static void sigsegv_handler(int sig)
{
	pr_log("Segmentation fault\n");
	cleanup_tempdir();
	raise(sig);
}

static bool can_skip_replay(struct opts *opts, int record_result)
{
	if (opts->nop)
		return true;

	return false;
}

static void setup_child_environ(struct opts *opts)
{
	char buf[4096];
	char *old_preload, *old_libpath;

	if (opts->lib_path) {
		strcpy(buf, opts->lib_path);
		strcat(buf, "/libmcount:");
	} else {
		/* to make strcat() work */
		buf[0] = '\0';
	}

#ifdef INSTALL_LIB_PATH
	strcat(buf, INSTALL_LIB_PATH);
#endif

	old_libpath = getenv("LD_LIBRARY_PATH");
	if (old_libpath) {
		size_t len = strlen(buf) + strlen(old_libpath) + 2;
		char *libpath = xmalloc(len);

		snprintf(libpath, len, "%s:%s", buf, old_libpath);
		setenv("LD_LIBRARY_PATH", libpath, 1);
		free(libpath);
	}
	else
		setenv("LD_LIBRARY_PATH", buf, 1);

	if (opts->lib_path)
		snprintf(buf, sizeof(buf), "%s/libmcount/libmcount.so", opts->lib_path);
	else
		strcpy(buf, "libmcount.so");

	old_preload = getenv("LD_PRELOAD");
	if (old_preload) {
		size_t len = strlen(buf) + strlen(old_preload) + 2;
		char *preload = xmalloc(len);

		snprintf(preload, len, "%s:%s", buf, old_preload);
		setenv("LD_PRELOAD", preload, 1);
		free(preload);
	}
	else
		setenv("LD_PRELOAD", buf, 1);
}

int command_live(int argc, char *argv[], struct opts *opts)
{
	char template[32] = "/tmp/uftrace-live-XXXXXX";
	int fd = mkstemp(template);
	struct sigaction sa = {
		.sa_flags = SA_RESETHAND,
	};
	int ret;

	if (fd < 0)
		pr_err("cannot create temp name");

	close(fd);
	unlink(template);

	tmp_dirname = template;
	atexit(cleanup_tempdir);

	sa.sa_handler = sigsegv_handler;
	sigfillset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);

	opts->dirname = template;

	if (opts->list_event) {
		if (geteuid() == 0)
			list_kernel_events();

		if (fork() == 0) {
			setup_child_environ(opts);
			setenv("UFTRACE_LIST_EVENT", "1", 1);

			execv(opts->exename, &argv[opts->idx]);
			abort();
		}
		return 0;
	}

	ret = command_record(argc, argv, opts);
	if (!can_skip_replay(opts, ret)) {
		int ret2;

		reset_live_opts(opts);

		pr_dbg("live-record finished.. \n");
		if (opts->report) {
			pr_out("#\n# uftrace report\n#\n");
			ret2 = command_report(argc, argv, opts);
			if (ret == UFTRACE_EXIT_SUCCESS)
				ret = ret2;

			pr_out("\n#\n# uftrace replay\n#\n");
		}

		pr_dbg("start live-replaying...\n");
		ret2 = command_replay(argc, argv, opts);
		if (ret == UFTRACE_EXIT_SUCCESS)
			ret = ret2;
	}

	cleanup_tempdir();

	return ret;
}
