/* dhclient.c

   DHCP Client. */

/*
 * Copyright (c) 2004-2021 by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1995-2003 by Internet Software Consortium
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *   Internet Systems Consortium, Inc.
 *   950 Charter Street
 *   Redwood City, CA 94063
 *   <info@isc.org>
 *   https://www.isc.org/
 *
 * This code is based on the original client state machine that was
 * written by Elliot Poger.  The code has been extensively hacked on
 * by Ted Lemon since then, so any mistakes you find are probably his
 * fault and not Elliot's.
 */

#include "dhcpd.h"
#include <isc/util.h>
#include <isc/file.h>
#include <dns/result.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <limits.h>

TIME default_lease_time = 43200; /* 12 hours... */
TIME max_lease_time = 86400; /* 24 hours... */

const char *path_dhclient_conf = _PATH_DHCLIENT_CONF;
const char *path_dhclient_db = NULL;
const char *path_dhclient_pid = NULL;
static char path_dhclient_script_array[] = _PATH_DHCLIENT_SCRIPT;
char *path_dhclient_script = path_dhclient_script_array;
const char *path_dhclient_duid = NULL;

static void add_to_tail(struct client_lease** lease_list, struct client_lease* lease);

/* False (default) => we write and use a pid file */
isc_boolean_t no_pid_file = ISC_FALSE;

int dhcp_max_agent_option_packet_length = 0;

int interfaces_requested = 0;

struct iaddr iaddr_broadcast = { 4, { 255, 255, 255, 255 } };
struct iaddr iaddr_any = { 4, { 0, 0, 0, 0 } };
struct in_addr inaddr_any;
struct sockaddr_in sockaddr_broadcast;
struct in_addr giaddr;
struct data_string default_duid;
int duid_type = 0;
int duid_v4 = 0;
int std_dhcid = 0;

int decline_wait_time = 10; /* Default to 10 secs per, RFC 2131, 3.1.5 */

/* ASSERT_STATE() does nothing now; it used to be
   assert (state_is == state_shouldbe). */
#define ASSERT_STATE(state_is, state_shouldbe) {}

#ifndef UNIT_TEST
static const char copyright[] = "Copyright 2004-2021 Internet Systems Consortium.";
static const char arr [] = "All rights reserved.";
static const char message [] = "Internet Systems Consortium DHCP Client";
static const char url [] = "For info, please visit https://www.isc.org/software/dhcp/";
#endif /* UNIT_TEST */

u_int16_t local_port = 0;
u_int16_t remote_port = 0;
#if defined(DHCPv6) && defined(DHCP4o6)
int dhcp4o6_state = -1; /* -1 = stopped, 0 = polling, 1 = started */
#endif
int no_daemon = 0;
int dfd[2] = { -1, -1 };
struct string_list *client_env = NULL;
int client_env_count = 0;
int onetry = 0;
int quiet = 1;
int nowait = 0;
int stateless = 0;
int wanted_ia_na = -1;		/* the absolute value is the real one. */
int wanted_ia_ta = 0;
int wanted_ia_pd = 0;
int require_all_ias = 0;	/* If the user requires all of the IAs to
				   be available before accepting a lease
				   0 = no, 1 = requries */
#if defined(DHCPv6)
int dad_wait_time = 0;
int prefix_len_hint = 0;
#endif

int address_prefix_len = DHCLIENT_DEFAULT_PREFIX_LEN;
char *mockup_relay = NULL;

char *progname = NULL;

void run_stateless(int exit_mode, u_int16_t port);

static isc_result_t write_duid(struct data_string *duid);
static void add_reject(struct packet *packet);

static int check_domain_name(const char *ptr, size_t len, int dots);
static int check_domain_name_list(const char *ptr, size_t len, int dots);
static int check_option_values(struct universe *universe, unsigned int opt,
			       const char *ptr, size_t len);

#if defined(NSUPDATE)
static void dhclient_ddns_cb_free(dhcp_ddns_cb_t *ddns_cb,
                                   char* file, int line);
#endif /* defined NSUPDATE */


/*!
 *
 * \brief Print the generic usage message
 *
 * If the user has provided an incorrect command line print out
 * the description of the command line.  The arguments provide
 * a way for the caller to request more specific information about
 * the error be printed as well.  Mostly this will be that some
 * comamnd doesn't include its argument.
 *
 * \param sfmt - The basic string and format for the specific error
 * \param sarg - Generally the offending argument from the comamnd line.
 *
 * \return Nothing
 */

#if defined(DHCPv6) && defined(DHCP4o6)
static void dhcp4o6_poll(void *dummy);
static void dhcp4o6_resume(void);
static void recv_dhcpv4_response(struct data_string *raw);
static int send_dhcpv4_query(struct client_state *client, int broadcast);

static void dhcp4o6_stop(void);
static void forw_dhcpv4_response(struct packet *packet);
static void forw_dhcpv4_query(struct data_string *raw);
#endif

#ifndef UNIT_TEST
/* These are only used when we call usage() from the main routine
 * which isn't compiled when building for unit tests
 */
static const char use_noarg[] = "No argument for command: %s";
#ifdef DHCPv6
static const char use_v6command[] = "Command not used for DHCPv4: %s";
#endif

#ifdef DHCPv6
#ifdef DHCP4o6
#define DHCLIENT_USAGE0 \
"[-4|-6] [-SNTPRI1dvrxi] [-nw] -4o6 <port>] [-p <port>] [-D LL|LLT]\n" \
"                [--dad-wait-time <seconds>] [--prefix-len-hint <length>]\n" \
"                [--decline-wait-time <seconds>]\n" \
"                [--address-prefix-len <length>]\n"
#else /* DHCP4o6 */
#define DHCLIENT_USAGE0 \
"[-4|-6] [-SNTPRI1dvrxi] [-nw] [-p <port>] [-D LL|LLT]\n" \
"                [--dad-wait-time <seconds>] [--prefix-len-hint <length>]\n" \
"                [--decline-wait-time <seconds>]\n" \
"                [--address-prefix-len <length>]\n"
#endif
#else /* DHCPv6 */
#define DHCLIENT_USAGE0 \
"[-I1dvrxi] [-nw] [-p <port>] [-D LL|LLT] \n" \
"                [--decline-wait-time <seconds>]\n"
#endif

#define DHCLIENT_USAGEC \
"                [-s server-addr] [-cf config-file]\n" \
"                [-df duid-file] [-lf lease-file]\n" \
"                [-pf pid-file] [--no-pid] [-e VAR=val]\n" \
"                [-sf script-file] [interface]*"

#define DHCLIENT_USAGEH "{--version|--help|-h}"

static void
usage(const char *sfmt, const char *sarg)
{
	log_info("%s %s", message, PACKAGE_VERSION);
	log_info(copyright);
	log_info(arr);
	log_info(url);

	/* If desired print out the specific error message */
#ifdef PRINT_SPECIFIC_CL_ERRORS
	if (sfmt != NULL)
		log_error(sfmt, sarg);
#endif

	log_fatal("Usage: %s %s%s\n       %s %s",
		  isc_file_basename(progname),
		  DHCLIENT_USAGE0,
		  DHCLIENT_USAGEC,
		  isc_file_basename(progname),
		  DHCLIENT_USAGEH);
}

extern void initialize_client_option_spaces();

int
main(int argc, char **argv) {
	int fd;
	int i;
	struct interface_info *ip;
	struct client_state *client;
	unsigned seed;
	char *server = NULL;
	isc_result_t status;
	int exit_mode = 0;
	int release_mode = 0;
	struct timeval tv;
	omapi_object_t *listener;
	isc_result_t result;
	int persist = 0;
	int no_dhclient_conf = 0;
	int no_dhclient_db = 0;
	int no_dhclient_pid = 0;
	int no_dhclient_script = 0;
#ifdef DHCPv6
	int local_family_set = 0;
#ifdef DHCP4o6
	u_int16_t dhcp4o6_port = 0;
#endif /* DHCP4o6 */
#endif /* DHCPv6 */
	char *s;

#ifdef OLD_LOG_NAME
	progname = "dhclient";
#else
	progname = argv[0];
#endif
	/* Initialize client globals. */
	memset(&default_duid, 0, sizeof(default_duid));

	/* Make sure that file descriptors 0 (stdin), 1, (stdout), and
	   2 (stderr) are open. To do this, we assume that when we
	   open a file the lowest available file descriptor is used. */
	fd = open("/dev/null", O_RDWR);
	if (fd == 0)
		fd = open("/dev/null", O_RDWR);
	if (fd == 1)
		fd = open("/dev/null", O_RDWR);
	if (fd == 2)
		log_perror = 0; /* No sense logging to /dev/null. */
	else if (fd != -1)
		close(fd);

	openlog(isc_file_basename(progname), DHCP_LOG_OPTIONS, LOG_DAEMON);

#if !(defined(DEBUG) || defined(__CYGWIN32__))
	setlogmask(LOG_UPTO(LOG_INFO));
#endif

	/* Parse arguments changing no_daemon */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r")) {
			no_daemon = 1;
		} else if (!strcmp(argv[i], "-x")) {
			no_daemon = 0;
		} else if (!strcmp(argv[i], "-d")) {
			no_daemon = 1;
		} else if (!strcmp(argv[i], "--version")) {
			const char vstring[] = "isc-dhclient-";
			IGNORE_RET(write(STDERR_FILENO, vstring,
					 strlen(vstring)));
			IGNORE_RET(write(STDERR_FILENO,
					 PACKAGE_VERSION,
					 strlen(PACKAGE_VERSION)));
			IGNORE_RET(write(STDERR_FILENO, "\n", 1));
			exit(0);
		} else if (!strcmp(argv[i], "--help") ||
			   !strcmp(argv[i], "-h")) {
			const char *pname = isc_file_basename(progname);
			IGNORE_RET(write(STDERR_FILENO, "Usage: ", 7));
			IGNORE_RET(write(STDERR_FILENO, pname, strlen(pname)));
			IGNORE_RET(write(STDERR_FILENO, " ", 1));
			IGNORE_RET(write(STDERR_FILENO, DHCLIENT_USAGE0,
					 strlen(DHCLIENT_USAGE0)));
			IGNORE_RET(write(STDERR_FILENO, DHCLIENT_USAGEC,
					 strlen(DHCLIENT_USAGEC)));
			IGNORE_RET(write(STDERR_FILENO, "\n", 1));
			IGNORE_RET(write(STDERR_FILENO, "       ", 7));
			IGNORE_RET(write(STDERR_FILENO, pname, strlen(pname)));
			IGNORE_RET(write(STDERR_FILENO, " ", 1));
			IGNORE_RET(write(STDERR_FILENO, DHCLIENT_USAGEH,
					 strlen(DHCLIENT_USAGEH)));
			IGNORE_RET(write(STDERR_FILENO, "\n", 1));
			exit(0);
		}
	}
	/* When not forbidden prepare to become a daemon */
	if (!no_daemon) {
		int pid;

		if (pipe(dfd) == -1)
			log_fatal("Can't get pipe: %m");
		if ((pid = fork ()) < 0)
			log_fatal("Can't fork daemon: %m");
		if (pid != 0) {
			/* Parent: wait for the child to start */
			int n;

			(void) close(dfd[1]);
			do {
				char buf;

				n = read(dfd[0], &buf, 1);
				if (n == 1)
					_exit((int)buf);
			} while (n == -1 && errno == EINTR);
			_exit(1);
		}
		/* Child */
		(void) close(dfd[0]);
	}

	/* Set up the isc and dns library managers */
	status = dhcp_context_create(DHCP_CONTEXT_PRE_DB | DHCP_CONTEXT_POST_DB
				     | DHCP_DNS_CLIENT_LAZY_INIT, NULL, NULL);
	if (status != ISC_R_SUCCESS)
		log_fatal("Can't initialize context: %s",
			  isc_result_totext(status));

	/* Set up the OMAPI. */
	status = omapi_init();
	if (status != ISC_R_SUCCESS)
		log_fatal("Can't initialize OMAPI: %s",
			  isc_result_totext(status));

	/* Set up the OMAPI wrappers for various server database internal
	   objects. */
	dhcp_common_objects_setup();

	dhcp_interface_discovery_hook = dhclient_interface_discovery_hook;
	dhcp_interface_shutdown_hook = dhclient_interface_shutdown_hook;
	dhcp_interface_startup_hook = dhclient_interface_startup_hook;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r")) {
			release_mode = 1;
			/* no_daemon = 1; */
#ifdef DHCPv6
		} else if (!strcmp(argv[i], "-4")) {
			if (local_family_set && local_family != AF_INET)
				log_fatal("Client can only do v4 or v6, not "
					  "both.");
			local_family_set = 1;
			local_family = AF_INET;
		} else if (!strcmp(argv[i], "-6")) {
			if (local_family_set && local_family != AF_INET6)
				log_fatal("Client can only do v4 or v6, not "
					  "both.");
			local_family_set = 1;
			local_family = AF_INET6;
#ifdef DHCP4o6
		} else if (!strcmp(argv[i], "-4o6")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			dhcp4o6_port = validate_port_pair(argv[i]);

			log_debug("DHCPv4 over DHCPv6 over ::1 port %d and %d",
				  ntohs(dhcp4o6_port),
				  ntohs(dhcp4o6_port) + 1);
			dhcpv4_over_dhcpv6 = 1;
#endif /* DHCP4o6 */
#endif /* DHCPv6 */
		} else if (!strcmp(argv[i], "-x")) { /* eXit, no release */
			release_mode = 0;
			/* no_daemon = 0; */
			exit_mode = 1;
		} else if (!strcmp(argv[i], "-p")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			local_port = validate_port(argv[i]);
			log_debug("binding to user-specified port %d",
				  ntohs(local_port));
		} else if (!strcmp(argv[i], "-d")) {
			/* no_daemon = 1; */
			quiet = 0;
		} else if (!strcmp(argv[i], "-pf")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			path_dhclient_pid = argv[i];
			no_dhclient_pid = 1;
		} else if (!strcmp(argv[i], "--no-pid")) {
			no_pid_file = ISC_TRUE;
		} else if (!strcmp(argv[i], "-cf")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			path_dhclient_conf = argv[i];
			no_dhclient_conf = 1;
		} else if (!strcmp(argv[i], "-df")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			path_dhclient_duid = argv[i];
		} else if (!strcmp(argv[i], "-lf")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			path_dhclient_db = argv[i];
			no_dhclient_db = 1;
		} else if (!strcmp(argv[i], "-sf")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			path_dhclient_script = argv[i];
			no_dhclient_script = 1;
		} else if (!strcmp(argv[i], "-1")) {
			onetry = 1;
		} else if (!strcmp(argv[i], "-q")) {
			quiet = 1;
		} else if (!strcmp(argv[i], "-s")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			server = argv[i];
		} else if (!strcmp(argv[i], "-g")) {
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			mockup_relay = argv[i];
		} else if (!strcmp(argv[i], "-nw")) {
			nowait = 1;
		} else if (!strcmp(argv[i], "-n")) {
			/* do not start up any interfaces */
			interfaces_requested = -1;
		} else if (!strcmp(argv[i], "-w")) {
			/* do not exit if there are no broadcast interfaces. */
			persist = 1;
		} else if (!strcmp(argv[i], "-e")) {
			struct string_list *tmp;
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			tmp = dmalloc(strlen(argv[i]) + sizeof *tmp, MDL);
			if (!tmp)
				log_fatal("No memory for %s", argv[i]);
			strcpy(tmp->string, argv[i]);
			tmp->next = client_env;
			client_env = tmp;
			client_env_count++;
#ifdef DHCPv6
		} else if (!strcmp(argv[i], "-S")) {
			if (local_family_set && (local_family == AF_INET)) {
				usage(use_v6command, argv[i]);
			}
			local_family_set = 1;
			local_family = AF_INET6;
			wanted_ia_na = 0;
			stateless = 1;
		} else if (!strcmp(argv[i], "-N")) {
			if (local_family_set && (local_family == AF_INET)) {
				usage(use_v6command, argv[i]);
			}
			local_family_set = 1;
			local_family = AF_INET6;
			if (wanted_ia_na < 0) {
				wanted_ia_na = 0;
			}
			wanted_ia_na++;
		} else if (!strcmp(argv[i], "-T")) {
			if (local_family_set && (local_family == AF_INET)) {
				usage(use_v6command, argv[i]);
			}
			local_family_set = 1;
			local_family = AF_INET6;
			if (wanted_ia_na < 0) {
				wanted_ia_na = 0;
			}
			wanted_ia_ta++;
		} else if (!strcmp(argv[i], "-P")) {
			if (local_family_set && (local_family == AF_INET)) {
				usage(use_v6command, argv[i]);
			}
			local_family_set = 1;
			local_family = AF_INET6;
			if (wanted_ia_na < 0) {
				wanted_ia_na = 0;
			}
			wanted_ia_pd++;
		} else if (!strcmp(argv[i], "-R")) {
			if (local_family_set && (local_family == AF_INET)) {
				usage(use_v6command, argv[i]);
			}
			local_family_set = 1;
			local_family = AF_INET6;
			require_all_ias = 1;
		} else if (!strcmp(argv[i], "--dad-wait-time")) {
			if (++i == argc) {
				usage(use_noarg, argv[i-1]);
			}
			errno = 0;
			dad_wait_time = (int)strtol(argv[i], &s, 10);
			if (errno || (*s != '\0') || (dad_wait_time < 0)) {
				usage("Invalid value for --dad-wait-time: %s",
				      argv[i]);
			}
		} else if (!strcmp(argv[i], "--prefix-len-hint")) {
			if (++i == argc) {
				usage(use_noarg, argv[i-1]);
			}

			errno = 0;
			prefix_len_hint = (int)strtol(argv[i], &s, 10);
			if (errno || (*s != '\0') || (prefix_len_hint < 0)) {
				usage("Invalid value for --prefix-len-hint: %s",
				      argv[i]);
			}
		} else if (!strcmp(argv[i], "--address-prefix-len")) {
			if (++i == argc) {
				usage(use_noarg, argv[i-1]);
			}
			errno = 0;
			address_prefix_len = (int)strtol(argv[i], &s, 10);
			if (errno || (*s != '\0') ||
			    (address_prefix_len < 0)) {
				usage("Invalid value for"
				      " --address-prefix-len: %s", argv[i]);
			}
#endif /* DHCPv6 */
		} else if (!strcmp(argv[i], "--decline-wait-time")) {
			if (++i == argc) {
				usage(use_noarg, argv[i-1]);
			}

			errno = 0;
			decline_wait_time = (int)strtol(argv[i], &s, 10);
			if (errno || (*s != '\0') ||
			    (decline_wait_time < 0)) {
				usage("Invalid value for "
				      "--decline-wait-time: %s", argv[i]);
			}
		} else if (!strcmp(argv[i], "-D")) {
			duid_v4 = 1;
			if (++i == argc)
				usage(use_noarg, argv[i-1]);
			if (!strcasecmp(argv[i], "LL")) {
				duid_type = DUID_LL;
			} else if (!strcasecmp(argv[i], "LLT")) {
				duid_type = DUID_LLT;
			} else {
				usage("Unknown argument to -D: %s", argv[i]);
			}
		} else if (!strcmp(argv[i], "-i")) {
			/* enable DUID support for DHCPv4 clients */
			duid_v4 = 1;
		} else if (!strcmp(argv[i], "-I")) {
			/* enable standard DHCID support for DDNS updates */
			std_dhcid = 1;
		} else if (!strcmp(argv[i], "-v")) {
			quiet = 0;
		} else if (argv[i][0] == '-') {
			usage("Unknown command: %s", argv[i]);
		} else if (interfaces_requested < 0) {
			usage("No interfaces comamnd -n and "
			      " requested interface %s", argv[i]);
		} else {
		    struct interface_info *tmp = NULL;

		    status = interface_allocate(&tmp, MDL);
		    if (status != ISC_R_SUCCESS)
			log_fatal("Can't record interface %s:%s",
				  argv[i], isc_result_totext(status));
		    if (strlen(argv[i]) >= sizeof(tmp->name))
			    log_fatal("%s: interface name too long (is %ld)",
				      argv[i], (long)strlen(argv[i]));
		    strcpy(tmp->name, argv[i]);
		    if (interfaces) {
			    interface_reference(&tmp->next,
						interfaces, MDL);
			    interface_dereference(&interfaces, MDL);
		    }
		    interface_reference(&interfaces, tmp, MDL);
		    tmp->flags = INTERFACE_REQUESTED;
		    interfaces_requested++;
		}
	}

	if (wanted_ia_na < 0) {
		wanted_ia_na = 1;
	}

	/* Support only one (requested) interface for Prefix Delegation. */
	if (wanted_ia_pd && (interfaces_requested != 1)) {
		usage("PD %s only supports one requested interface", "-P");
	}

#if defined(DHCPv6) && defined(DHCP4o6)
	if ((local_family == AF_INET6) && dhcpv4_over_dhcpv6 &&
	    (exit_mode || release_mode))
		log_error("Can't relay DHCPv4-over-DHCPv6 "
			  "without a persistent DHCPv6 client");
	if ((local_family == AF_INET) && dhcpv4_over_dhcpv6 &&
	    (interfaces_requested != 1))
		log_fatal("DHCPv4-over-DHCPv6 requires an explicit "
			  "interface on which to be applied");
#endif

	if (!no_dhclient_conf && (s = getenv("PATH_DHCLIENT_CONF"))) {
		path_dhclient_conf = s;
	}
	if (!no_dhclient_db && (s = getenv("PATH_DHCLIENT_DB"))) {
		path_dhclient_db = s;
	}
	if (!no_dhclient_pid && (s = getenv("PATH_DHCLIENT_PID"))) {
		path_dhclient_pid = s;
	}
	if (!no_dhclient_script && (s = getenv("PATH_DHCLIENT_SCRIPT"))) {
		path_dhclient_script = s;
	}

	/* Set up the initial dhcp option universe. */
	initialize_common_option_spaces();

	/* Set up the initial client option universe. */
	initialize_client_option_spaces();

	/* Assign v4 or v6 specific running parameters. */
	if (local_family == AF_INET)
		dhcpv4_client_assignments();
#ifdef DHCPv6
	else if (local_family == AF_INET6)
		dhcpv6_client_assignments();
#endif /* DHCPv6 */
	else
		log_fatal("Impossible condition at %s:%d.", MDL);

	/*
	 * convert relative path names to absolute, for files that need
	 * to be reopened after chdir() has been called
	 */
	if (path_dhclient_db[0] != '/') {
		path_dhclient_db = absolute_path(path_dhclient_db);
	}

	if (path_dhclient_script[0] != '/') {
		path_dhclient_script = absolute_path(path_dhclient_script);
	}

	/*
	 * See if we should  kill off any currently running client
	 * we don't try to kill it off if the user told us not
	 * to write a pid file - we assume they are controlling
	 * the process in some other fashion.
	 */
	if ((release_mode || exit_mode) && (no_pid_file == ISC_FALSE)) {
		FILE *pidfd;
		pid_t oldpid;
		long temp;
		int e;

		if ((pidfd = fopen(path_dhclient_pid, "r")) != NULL) {
			e = fscanf(pidfd, "%ld\n", &temp);
			oldpid = (pid_t)temp;

			if (e != 0 && e != EOF && oldpid) {
				if (kill(oldpid, SIGTERM) == 0) {
					log_info("Killed old client process");
					(void) unlink(path_dhclient_pid);
					/*
					 * wait for the old process to
					 * cleanly terminate.
					 * Note kill() with sig=0 could
					 * detect termination but only
					 * the parent can be signaled...
					 */
					sleep(1);
				} else if (errno == ESRCH) {
					log_info("Removed stale PID file");
					(void) unlink(path_dhclient_pid);
				}
			}
			fclose(pidfd);
		}
	}

	if (!quiet) {
		log_info("%s %s", message, PACKAGE_VERSION);
		log_info(copyright);
		log_info(arr);
		log_info(url);
		log_info("%s", "");
	} else {
		log_perror = 0;
		quiet_interface_discovery = 1;
	}

	/* If we're given a relay agent address to insert, for testing
	   purposes, figure out what it is. */
	if (mockup_relay) {
		if (!inet_aton(mockup_relay, &giaddr)) {
			struct hostent *he;
			he = gethostbyname(mockup_relay);
			if (he) {
				memcpy(&giaddr, he->h_addr_list[0],
				       sizeof giaddr);
			} else {
				log_fatal("%s: no such host", mockup_relay);
			}
		}
	}

	/* Get the current time... */
	gettimeofday(&cur_tv, NULL);

	sockaddr_broadcast.sin_family = AF_INET;
	sockaddr_broadcast.sin_port = remote_port;
	if (server) {
		if (!inet_aton(server, &sockaddr_broadcast.sin_addr)) {
			struct hostent *he;
			he = gethostbyname(server);
			if (he) {
				memcpy(&sockaddr_broadcast.sin_addr,
				       he->h_addr_list[0],
				       sizeof sockaddr_broadcast.sin_addr);
			} else
				sockaddr_broadcast.sin_addr.s_addr =
					INADDR_BROADCAST;
		}
	} else {
		sockaddr_broadcast.sin_addr.s_addr = INADDR_BROADCAST;
	}

	inaddr_any.s_addr = INADDR_ANY;

	/* Stateless special case. */
	if (stateless) {
		if (release_mode || (wanted_ia_na > 0) ||
		    wanted_ia_ta || wanted_ia_pd ||
		    (interfaces_requested != 1)) {
			usage("Stateless command: %s incompatibile with "
			      "other commands", "-S");
		}
#if defined(DHCPv6) && defined(DHCP4o6)
		run_stateless(exit_mode, dhcp4o6_port);
#else
		run_stateless(exit_mode, 0);
#endif
		finish(0);
	}

	/* Discover all the network interfaces. */
	discover_interfaces(DISCOVER_UNCONFIGURED);

	/* Parse the dhclient.conf file. */
	read_client_conf();

	/* Parse the lease database. */
	read_client_leases();

	/* If desired parse the secondary lease database for a DUID */
	if ((default_duid.len == 0) && (path_dhclient_duid != NULL)) {
		read_client_duid();
	}

	/* Rewrite the lease database... */
	rewrite_client_leases();

	/* XXX */
/* 	config_counter(&snd_counter, &rcv_counter); */

	/*
	 * If no broadcast interfaces were discovered, call the script
	 * and tell it so.
	 */
	if (!interfaces) {
		/*
		 * Call dhclient-script with the NBI flag,
		 * in case somebody cares.
		 */
		script_init(NULL, "NBI", NULL);
		script_go(NULL);

		/*
		 * If we haven't been asked to persist, waiting for new
		 * interfaces, then just exit.
		 */
		if (!persist) {
			/* Nothing more to do. */
			log_info("No broadcast interfaces found - exiting.");
			finish(0);
		}
	} else if (!release_mode && !exit_mode) {
		/* Call the script with the list of interfaces. */
		for (ip = interfaces; ip; ip = ip->next) {
			/*
			 * If interfaces were specified, don't configure
			 * interfaces that weren't specified!
			 */
			if ((interfaces_requested > 0) &&
			    ((ip->flags & (INTERFACE_REQUESTED |
					   INTERFACE_AUTOMATIC)) !=
			     INTERFACE_REQUESTED))
				continue;

			if (local_family == AF_INET6) {
				script_init(ip->client, "PREINIT6", NULL);
			} else {
				script_init(ip->client, "PREINIT", NULL);
			    	if (ip->client->alias != NULL)
					script_write_params(ip->client,
							    "alias_",
							    ip->client->alias);
			}
			script_go(ip->client);
		}
	}

	/* At this point, all the interfaces that the script thinks
	   are relevant should be running, so now we once again call
	   discover_interfaces(), and this time ask it to actually set
	   up the interfaces. */
	discover_interfaces(interfaces_requested != 0
			    ? DISCOVER_REQUESTED
			    : DISCOVER_RUNNING);

	/* Make up a seed for the random number generator from current
	   time plus the sum of the last four bytes of each
	   interface's hardware address interpreted as an integer.
	   Not much entropy, but we're booting, so we're not likely to
	   find anything better. */
	seed = 0;
	for (ip = interfaces; ip; ip = ip->next) {
		int junk;
		memcpy(&junk,
		       &ip->hw_address.hbuf[ip->hw_address.hlen -
					    sizeof seed], sizeof seed);
		seed += junk;
	}
	srandom(seed + cur_time + (unsigned)getpid());


	/*
	 * Establish a default DUID.  We always do so for v6 and
	 * do so if desired for v4 via the -D or -i options
	 */
	if ((local_family == AF_INET6) ||
	    ((local_family == AF_INET) && (duid_v4 == 1))) {
		if (default_duid.len == 0) {
			if (default_duid.buffer != NULL)
				data_string_forget(&default_duid, MDL);

			form_duid(&default_duid, MDL);
			write_duid(&default_duid);
		}
	}

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6 && !exit_mode)
		dhcp4o6_setup(dhcp4o6_port);
#endif

	/* Start a configuration state machine for each interface. */
#ifdef DHCPv6
	if (local_family == AF_INET6) {
		for (ip = interfaces ; ip != NULL ; ip = ip->next) {
			for (client = ip->client ; client != NULL ;
			     client = client->next) {
				if (release_mode) {
					start_release6(client);
					continue;
				} else if (exit_mode) {
					unconfigure6(client, "STOP6");
					continue;
				}

				/* If we have a previous binding, Confirm
				 * that we can (or can't) still use it.
				 */
				if ((client->active_lease != NULL) &&
				    !client->active_lease->released)
					start_confirm6(client);
				else
					start_init6(client);
			}
		}
	} else
#endif /* DHCPv6 */
	{
		for (ip = interfaces ; ip ; ip = ip->next) {
			ip->flags |= INTERFACE_RUNNING;
			for (client = ip->client ; client ;
			     client = client->next) {
				if (exit_mode)
					state_stop(client);
				if (release_mode)
					do_release(client);
				else {
					client->state = S_INIT;

					if (top_level_config.initial_delay>0)
					{
						tv.tv_sec = 0;
						if (top_level_config.
						    initial_delay>1)
							tv.tv_sec = cur_time
							+ random()
							% (top_level_config.
							   initial_delay-1);
						tv.tv_usec = random()
							% 1000000;
						/*
						 * this gives better
						 * distribution than just
						 *whole seconds
						 */
						add_timeout(&tv, state_reboot,
						            client, 0, 0);
					} else {
						state_reboot(client);
					}
				}
			}
		}
	}

	if (exit_mode)
		finish(0);
	if (release_mode) {
#ifndef DHCPv6
		finish(0);
#else
		if ((local_family == AF_INET6) || dhcpv4_over_dhcpv6) {
			if (onetry)
				finish(0);
		} else
			finish(0);
#endif /* DHCPv6 */
	}

	/* Start up a listener for the object management API protocol. */
	if (top_level_config.omapi_port != -1) {
		listener = NULL;
		result = omapi_generic_new(&listener, MDL);
		if (result != ISC_R_SUCCESS)
			log_fatal("Can't allocate new generic object: %s\n",
				  isc_result_totext(result));
		result = omapi_protocol_listen(listener,
					       (unsigned)
					       top_level_config.omapi_port,
					       1);
		if (result != ISC_R_SUCCESS)
			log_fatal("Can't start OMAPI protocol: %s",
				  isc_result_totext (result));
	}

	/* Set up the bootp packet handler... */
	bootp_packet_handler = do_packet;
#ifdef DHCPv6
	dhcpv6_packet_handler = do_packet6;
#endif /* DHCPv6 */

#if defined(DEBUG_MEMORY_LEAKAGE) || defined(DEBUG_MALLOC_POOL) || \
		defined(DEBUG_MEMORY_LEAKAGE_ON_EXIT)
	dmalloc_cutoff_generation = dmalloc_generation;
	dmalloc_longterm = dmalloc_outstanding;
	dmalloc_outstanding = 0;
#endif

#if defined(ENABLE_GENTLE_SHUTDOWN)
	/* no signal handlers until we deal with the side effects */
        /* install signal handlers */
	signal(SIGINT, dhcp_signal_handler);   /* control-c */
	signal(SIGTERM, dhcp_signal_handler);  /* kill */
#endif

	/* If we're not supposed to wait before getting the address,
	   don't. */
	if (nowait)
		detach();

	/* If we're not going to daemonize, write the pid file
	   now. */
	if (no_daemon || nowait)
		write_client_pid_file();

	/* Start dispatching packets and timeouts... */
	dispatch();

	/* In fact dispatch() never returns. */
	return 0;
}

/*
 * \brief Run the DHCPv6 stateless client (dhclient -6 -S)
 *
 * \param exist_mode set to 1 when dhclient was called with -x
 * \param port DHCPv4-over-DHCPv6 client inter-process communication
 *  UDP port pair (port,port+1 with port in network byte order)
 */

void run_stateless(int exit_mode, u_int16_t port)
{
#ifdef DHCPv6
	struct client_state *client;
	omapi_object_t *listener;
	isc_result_t result;

#ifndef DHCP4o6
	IGNORE_UNUSED(port);
#endif

	/* Discover the network interface. */
	discover_interfaces(DISCOVER_REQUESTED);

	if (!interfaces)
		usage("No interfaces available for stateless command: %s", "-S");

	/* Parse the dhclient.conf file. */
#ifdef DHCP4o6
	if (dhcpv4_over_dhcpv6) {
		/* Mark we want to request IRT too! */
		dhcpv4_over_dhcpv6++;
	}
#endif
	read_client_conf();

	/* Parse the lease database. */
	read_client_leases();

	/* If desired parse the secondary lease database for a DUID */
	if ((default_duid.len == 0) && (path_dhclient_duid != NULL)) {
		read_client_duid();
	}

	/* Establish a default DUID. */
	if (default_duid.len == 0) {
		if (default_duid.buffer != NULL)
			data_string_forget(&default_duid, MDL);

		form_duid(&default_duid, MDL);
	}

#ifdef DHCP4o6
	if (dhcpv4_over_dhcpv6 && !exit_mode)
		dhcp4o6_setup(port);
#endif

	/* Start a configuration state machine. */
	for (client = interfaces->client ;
	     client != NULL ;
	     client = client->next) {
		if (exit_mode) {
			unconfigure6(client, "STOP6");
			continue;
		}
		start_info_request6(client);
	}
	if (exit_mode)
		return;

	/* Start up a listener for the object management API protocol. */
	if (top_level_config.omapi_port != -1) {
		listener = NULL;
		result = omapi_generic_new(&listener, MDL);
		if (result != ISC_R_SUCCESS)
			log_fatal("Can't allocate new generic object: %s\n",
				  isc_result_totext(result));
		result = omapi_protocol_listen(listener,
					       (unsigned)
					       top_level_config.omapi_port,
					       1);
		if (result != ISC_R_SUCCESS)
			log_fatal("Can't start OMAPI protocol: %s",
				  isc_result_totext(result));
	}

	/* Set up the packet handler... */
	dhcpv6_packet_handler = do_packet6;

#if defined(DEBUG_MEMORY_LEAKAGE) || defined(DEBUG_MALLOC_POOL) || \
		defined(DEBUG_MEMORY_LEAKAGE_ON_EXIT)
	dmalloc_cutoff_generation = dmalloc_generation;
	dmalloc_longterm = dmalloc_outstanding;
	dmalloc_outstanding = 0;
#endif

	/* If we're not supposed to wait before getting the address,
	   don't. */
	if (nowait)
		detach();

	/* If we're not going to daemonize, write the pid file
	   now. */
	if (no_daemon || nowait)
		write_client_pid_file();

	/* Start dispatching packets and timeouts... */
	dispatch();

#endif /* DHCPv6 */
	return;
}
#endif /* !UNIT_TEST */

isc_result_t find_class (struct class **c,
		const char *s, const char *file, int line)
{
	return 0;
}

int check_collection (packet, lease, collection)
	struct packet *packet;
	struct lease *lease;
	struct collection *collection;
{
	return 0;
}

void classify (packet, class)
	struct packet *packet;
	struct class *class;
{
}

void unbill_class (lease)
	struct lease *lease;
{
}

int find_subnet (struct subnet **sp,
		 struct iaddr addr, const char *file, int line)
{
	return 0;
}

/* Individual States:
 *
 * Each routine is called from the dhclient_state_machine() in one of
 * these conditions:
 * -> entering INIT state
 * -> recvpacket_flag == 0: timeout in this state
 * -> otherwise: received a packet in this state
 *
 * Return conditions as handled by dhclient_state_machine():
 * Returns 1, sendpacket_flag = 1: send packet, reset timer.
 * Returns 1, sendpacket_flag = 0: just reset the timer (wait for a milestone).
 * Returns 0: finish the nap which was interrupted for no good reason.
 *
 * Several per-interface variables are used to keep track of the process:
 *   active_lease: the lease that is being used on the interface
 *                 (null pointer if not configured yet).
 *   offered_leases: leases corresponding to DHCPOFFER messages that have
 *		     been sent to us by DHCP servers.
 *   acked_leases: leases corresponding to DHCPACK messages that have been
 *		   sent to us by DHCP servers.
 *   sendpacket: DHCP packet we're trying to send.
 *   destination: IP address to send sendpacket to
 * In addition, there are several relevant per-lease variables.
 *   T1_expiry, T2_expiry, lease_expiry: lease milestones
 * In the active lease, these control the process of renewing the lease;
 * In leases on the acked_leases list, this simply determines when we
 * can no longer legitimately use the lease.
 */

void state_reboot (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6 && (dhcp4o6_state <= 0)) {
		if (dhcp4o6_state < 0)
			dhcp4o6_poll(NULL);
		client->pending = P_REBOOT;
		return;
	}
#endif

	client->pending= P_NONE;

	/* If we don't remember an active lease, go straight to INIT. */
	if (!client -> active ||
	    client -> active -> is_bootp ||
	    client -> active -> expiry <= cur_time) {
		state_init (client);
		return;
	}

	/* We are in the rebooting state. */
	client -> state = S_REBOOTING;

	/*
	 * make_request doesn't initialize xid because it normally comes
	 * from the DHCPDISCOVER, but we haven't sent a DHCPDISCOVER,
	 * so pick an xid now.
	 */
	client -> xid = random ();

	/*
	 * Make a DHCPREQUEST packet, and set
	 * appropriate per-interface flags.
	 */
	make_request (client, client -> active);
	client -> destination = iaddr_broadcast;
	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;

	/* Zap the medium list... */
	client -> medium = NULL;

	/* Send out the first DHCPREQUEST packet. */
	send_request (client);
}

/* Called when a lease has completely expired and we've been unable to
   renew it. */

void state_init (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	ASSERT_STATE(state, S_INIT);

	/* Make a DHCPDISCOVER packet, and set appropriate per-interface
	   flags. */
	make_discover (client, client -> active);
	client -> xid = client -> packet.xid;
	client -> destination = iaddr_broadcast;
	client -> state = S_SELECTING;
	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;

	/* Add an immediate timeout to cause the first DHCPDISCOVER packet
	   to go out. */
	send_discover (client);
}

/*
 * state_selecting is called when one or more DHCPOFFER packets have been
 * received and a configurable period of time has passed.
 */

void state_selecting (cpp)
	void *cpp;
{
	struct client_state *client = cpp;
	struct client_lease *lp, *next, *picked;


	ASSERT_STATE(state, S_SELECTING);

	/*
	 * Cancel state_selecting and send_discover timeouts, since either
	 * one could have got us here.
	 */
	cancel_timeout (state_selecting, client);
	cancel_timeout (send_discover, client);

	/*
	 * We have received one or more DHCPOFFER packets.   Currently,
	 * the only criterion by which we judge leases is whether or
	 * not we get a response when we arp for them.
	 */
	picked = NULL;
	for (lp = client -> offered_leases; lp; lp = next) {
		next = lp -> next;

		/*
		 * Check to see if we got an ARPREPLY for the address
		 * in this particular lease.
		 */
		if (!picked) {
			picked = lp;
			picked -> next = NULL;
		} else {
			destroy_client_lease (lp);
		}
	}
	client -> offered_leases = NULL;

	/*
	 * If we just tossed all the leases we were offered, go back
	 * to square one.
	 */
	if (!picked) {
		client -> state = S_INIT;
		state_init (client);
		return;
	}

	/* If it was a BOOTREPLY, we can just take the address right now. */
	if (picked -> is_bootp) {
		client -> new = picked;

		/* Make up some lease expiry times
		   XXX these should be configurable. */
		client -> new -> expiry = cur_time + 12000;
		client -> new -> renewal += cur_time + 8000;
		client -> new -> rebind += cur_time + 10000;

		client -> state = S_REQUESTING;

		/* Bind to the address we received. */
		bind_lease (client);
		return;
	}

	/* Go to the REQUESTING state. */
	client -> destination = iaddr_broadcast;
	client -> state = S_REQUESTING;
	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;

	/* Make a DHCPREQUEST packet from the lease we picked. */
	make_request (client, picked);
	client -> xid = client -> packet.xid;

	/* Toss the lease we picked - we'll get it back in a DHCPACK. */
	destroy_client_lease (picked);

	/* Add an immediate timeout to send the first DHCPREQUEST packet. */
	send_request (client);
}

/* state_requesting is called when we receive a DHCPACK message after
   having sent out one or more DHCPREQUEST packets. */

void dhcpack (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_state *client;
	struct client_lease *lease;
	struct option_cache *oc;
	struct data_string ds;

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	for (client = ip -> client; client; client = client -> next) {
		if (client -> xid == packet -> raw -> xid)
			break;
	}
	if (!client ||
	    (packet -> interface -> hw_address.hlen - 1 !=
	     packet -> raw -> hlen) ||
	    (memcmp (&packet -> interface -> hw_address.hbuf [1],
		     packet -> raw -> chaddr, packet -> raw -> hlen))) {
#if defined (DEBUG)
		log_debug ("DHCPACK in wrong transaction.");
#endif
		return;
	}

	if (client -> state != S_REBOOTING &&
	    client -> state != S_REQUESTING &&
	    client -> state != S_RENEWING &&
	    client -> state != S_REBINDING) {
#if defined (DEBUG)
		log_debug ("DHCPACK in wrong state.");
#endif
		return;
	}

	log_info ("DHCPACK of %s from %s",
		  inet_ntoa(packet->raw->yiaddr),
		  piaddr (packet->client_addr));

	lease = packet_to_lease (packet, client);
	if (!lease) {
		log_info ("packet_to_lease failed.");
		return;
	}

	client -> new = lease;

	/* Stop resending DHCPREQUEST. */
	cancel_timeout (send_request, client);

	/* Figure out the lease time. */
	oc = lookup_option (&dhcp_universe, client -> new -> options,
			    DHO_DHCP_LEASE_TIME);
	memset (&ds, 0, sizeof ds);
	if (oc &&
	    evaluate_option_cache (&ds, packet, (struct lease *)0, client,
				   packet -> options, client -> new -> options,
				   &global_scope, oc, MDL)) {
		if (ds.len > 3)
			client -> new -> expiry = getULong (ds.data);
		else
			client -> new -> expiry = 0;
		data_string_forget (&ds, MDL);
	} else
			client -> new -> expiry = 0;

	if (client->new->expiry == 0) {
		struct timeval tv;

		log_error ("no expiry time on offered lease.");

		/* Quench this (broken) server.  Return to INIT to reselect. */
		add_reject(packet);

		/* 1/2 second delay to restart at INIT. */
		tv.tv_sec = cur_tv.tv_sec;
		tv.tv_usec = cur_tv.tv_usec + 500000;

		if (tv.tv_usec >= 1000000) {
			tv.tv_sec++;
			tv.tv_usec -= 1000000;
		}

		add_timeout(&tv, state_init, client, 0, 0);
		return;
	}

	/*
	 * A number that looks negative here is really just very large,
	 * because the lease expiry offset is unsigned.
	 */
	if (client->new->expiry < 0)
		client->new->expiry = TIME_MAX;

	/* Take the server-provided renewal time if there is one. */
	oc = lookup_option (&dhcp_universe, client -> new -> options,
			    DHO_DHCP_RENEWAL_TIME);
	if (oc &&
	    evaluate_option_cache (&ds, packet, (struct lease *)0, client,
				   packet -> options, client -> new -> options,
				   &global_scope, oc, MDL)) {
		if (ds.len > 3)
			client -> new -> renewal = getULong (ds.data);
		else
			client -> new -> renewal = 0;
		data_string_forget (&ds, MDL);
	} else
			client -> new -> renewal = 0;

	/* If it wasn't specified by the server, calculate it. */
	if (!client -> new -> renewal)
		client -> new -> renewal = client -> new -> expiry / 2 + 1;

	if (client -> new -> renewal <= 0)
		client -> new -> renewal = TIME_MAX;

	/* Now introduce some randomness to the renewal time: */
	if (client->new->renewal <= ((TIME_MAX / 3) - 3))
		client->new->renewal = (((client->new->renewal * 3) + 3) / 4) +
				(((random() % client->new->renewal) + 3) / 4);

	/* Same deal with the rebind time. */
	oc = lookup_option (&dhcp_universe, client -> new -> options,
			    DHO_DHCP_REBINDING_TIME);
	if (oc &&
	    evaluate_option_cache (&ds, packet, (struct lease *)0, client,
				   packet -> options, client -> new -> options,
				   &global_scope, oc, MDL)) {
		if (ds.len > 3)
			client -> new -> rebind = getULong (ds.data);
		else
			client -> new -> rebind = 0;
		data_string_forget (&ds, MDL);
	} else
			client -> new -> rebind = 0;

	if (client -> new -> rebind <= 0) {
		if (client -> new -> expiry <= TIME_MAX / 7)
			client -> new -> rebind =
					client -> new -> expiry * 7 / 8;
		else
			client -> new -> rebind =
					client -> new -> expiry / 8 * 7;
	}

	/* Make sure our randomness didn't run the renewal time past the
	   rebind time. */
	if (client -> new -> renewal > client -> new -> rebind) {
		if (client -> new -> rebind <= TIME_MAX / 3)
			client -> new -> renewal =
					client -> new -> rebind * 3 / 4;
		else
			client -> new -> renewal =
					client -> new -> rebind / 4 * 3;
	}

	client -> new -> expiry += cur_time;
	/* Lease lengths can never be negative. */
	if (client -> new -> expiry < cur_time)
		client -> new -> expiry = TIME_MAX;
	client -> new -> renewal += cur_time;
	if (client -> new -> renewal < cur_time)
		client -> new -> renewal = TIME_MAX;
	client -> new -> rebind += cur_time;
	if (client -> new -> rebind < cur_time)
		client -> new -> rebind = TIME_MAX;

	bind_lease (client);
}

void bind_lease (client)
	struct client_state *client;
{
	struct timeval tv;

	/* Remember the medium. */
	client->new->medium = client->medium;

	/* Run the client script with the new parameters. */
	script_init(client, (client->state == S_REQUESTING ? "BOUND" :
			     (client->state == S_RENEWING ? "RENEW" :
			      (client->state == S_REBOOTING ? "REBOOT" :
			       "REBIND"))),
		    client->new->medium);
	if (client->active && client->state != S_REBOOTING)
		script_write_params(client, "old_", client->active);
	script_write_params(client, "new_", client->new);
	script_write_requested(client);
	if (client->alias)
		script_write_params(client, "alias_", client->alias);

	/* If the BOUND/RENEW code detects another machine using the
	   offered address, it exits nonzero.  We need to send a
	   DHCPDECLINE and toss the lease. */
	if (script_go(client)) {
		make_decline(client, client->new);
		send_decline(client);
		destroy_client_lease(client->new);
		client->new = NULL;
		if (onetry) {
			if (!quiet) {
				log_info("Unable to obtain a lease on first "
					 "try (declined).  Exiting.");
			}

#if defined (CALL_SCRIPT_ON_ONETRY_FAIL)
			/* Let's call a script and we're done */
			script_init(client, "FAIL", (struct string_list *)0);
			script_go(client);
#endif
			finish(2);
		} else {
			struct timeval tv;
			tv.tv_sec = cur_tv.tv_sec + decline_wait_time;
			tv.tv_usec = cur_tv.tv_usec;
			add_timeout(&tv, state_init, client, 0, 0);
			return;
		}
	}

	/* Write out the new lease if it has been long enough. */
	if (!client->last_write ||
	    (cur_time - client->last_write) >= MIN_LEASE_WRITE)
		write_client_lease(client, client->new, 0, 1);

	/* Replace the old active lease with the new one. */
	if (client->active) {
		if (client->active->is_static) {
			// We need to preserve the fallback lease in case
			// we lose DHCP service again.
			add_to_tail(&client->leases, client->active);
		} else {
			destroy_client_lease(client->active);
		}
	}

	client->active = client->new;
	client->new = NULL;

	/* Set up a timeout to start the renewal process. */
	tv.tv_sec = client->active->renewal;
	tv.tv_usec = ((client->active->renewal - cur_tv.tv_sec) > 1) ?
			random() % 1000000 : cur_tv.tv_usec;
	add_timeout(&tv, state_bound, client, 0, 0);

	log_info("bound to %s -- renewal in %ld seconds.",
	      piaddr(client->active->address),
	      (long)(client->active->renewal - cur_time));
	client->state = S_BOUND;
	reinitialize_interfaces();
	detach();
#if defined (NSUPDATE)
	if (client->config->do_forward_update)
		dhclient_schedule_updates(client, &client->active->address, 1);
#endif /* defined NSUPDATE */

}

/* state_bound is called when we've successfully bound to a particular
   lease, but the renewal time on that lease has expired.   We are
   expected to unicast a DHCPREQUEST to the server that gave us our
   original lease. */

void state_bound (cpp)
	void *cpp;
{
	struct client_state *client = cpp;
	struct option_cache *oc;
	struct data_string ds;

	ASSERT_STATE(state, S_BOUND);

	/* T1 has expired. */
	make_request (client, client -> active);
	client -> xid = client -> packet.xid;

	memset (&ds, 0, sizeof ds);
	oc = lookup_option (&dhcp_universe, client -> active -> options,
			    DHO_DHCP_SERVER_IDENTIFIER);
	if (oc &&
	    evaluate_option_cache (&ds, (struct packet *)0, (struct lease *)0,
				   client, (struct option_state *)0,
				   client -> active -> options,
				   &global_scope, oc, MDL)) {
		if (ds.len > 3) {
			memcpy (client -> destination.iabuf, ds.data, 4);
			client -> destination.len = 4;
		} else
			client -> destination = iaddr_broadcast;

		data_string_forget (&ds, MDL);
	} else
		client -> destination = iaddr_broadcast;

	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;
	client -> state = S_RENEWING;

	/* Send the first packet immediately. */
	send_request (client);
}

/* state_stop is called when we've been told to shut down.   We unconfigure
   the interfaces, and then stop operating until told otherwise. */

void state_stop (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	client->pending = P_NONE;

	/* Cancel all timeouts. */
	cancel_timeout(state_selecting, client);
	cancel_timeout(send_discover, client);
	cancel_timeout(send_request, client);
	cancel_timeout(state_bound, client);

	/* If we have an address, unconfigure it. */
	if (client->active) {
		script_init(client, "STOP", client->active->medium);
		script_write_params(client, "old_", client->active);
		script_write_requested(client);
		if (client->alias)
			script_write_params(client, "alias_", client->alias);
		script_go(client);
	}
}

int commit_leases ()
{
	return 0;
}

int write_lease (lease)
	struct lease *lease;
{
	return 0;
}

int write_host (host)
	struct host_decl *host;
{
	return 0;
}

void db_startup (testp)
	int testp;
{
}

void bootp (packet)
	struct packet *packet;
{
	struct iaddrmatchlist *ap;
	char addrbuf[4*16];
	char maskbuf[4*16];

	if (packet -> raw -> op != BOOTREPLY)
		return;

	/* If there's a reject list, make sure this packet's sender isn't
	   on it. */
	for (ap = packet -> interface -> client -> config -> reject_list;
	     ap; ap = ap -> next) {
		if (addr_match(&packet->client_addr, &ap->match)) {

		        /* piaddr() returns its result in a static
			   buffer sized 4*16 (see common/inet.c). */

		        strcpy(addrbuf, piaddr(ap->match.addr));
		        strcpy(maskbuf, piaddr(ap->match.mask));

			log_info("BOOTREPLY from %s rejected by rule %s "
				 "mask %s.", piaddr(packet->client_addr),
				 addrbuf, maskbuf);
			return;
		}
	}

	dhcpoffer (packet);

}

void dhcp (packet)
	struct packet *packet;
{
	struct iaddrmatchlist *ap;
	void (*handler) (struct packet *);
	const char *type;
	char addrbuf[4*16];
	char maskbuf[4*16];

	switch (packet -> packet_type) {
	      case DHCPOFFER:
		handler = dhcpoffer;
		type = "DHCPOFFER";
		break;

	      case DHCPNAK:
		handler = dhcpnak;
		type = "DHCPNACK";
		break;

	      case DHCPACK:
		handler = dhcpack;
		type = "DHCPACK";
		break;

	      default:
		return;
	}

	/* If there's a reject list, make sure this packet's sender isn't
	   on it. */
	for (ap = packet -> interface -> client -> config -> reject_list;
	     ap; ap = ap -> next) {
		if (addr_match(&packet->client_addr, &ap->match)) {

		        /* piaddr() returns its result in a static
			   buffer sized 4*16 (see common/inet.c). */

		        strcpy(addrbuf, piaddr(ap->match.addr));
		        strcpy(maskbuf, piaddr(ap->match.mask));

			log_info("%s from %s rejected by rule %s mask %s.",
				 type, piaddr(packet->client_addr),
				 addrbuf, maskbuf);
			return;
		}
	}
	(*handler) (packet);
}

#ifdef DHCPv6
void
dhcpv6(struct packet *packet) {
	struct iaddrmatchlist *ap;
	struct client_state *client;
	char addrbuf[sizeof("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")];

	/* Silently drop bogus messages. */
	if (packet->dhcpv6_msg_type >= dhcpv6_type_name_max)
		return;

	/* Discard, with log, packets from quenched sources. */
	for (ap = packet->interface->client->config->reject_list ;
	     ap ; ap = ap->next) {
		if (addr_match(&packet->client_addr, &ap->match)) {
			strcpy(addrbuf, piaddr(packet->client_addr));
			log_info("%s from %s rejected by rule %s",
				 dhcpv6_type_names[packet->dhcpv6_msg_type],
				 addrbuf,
				 piaddrmask(&ap->match.addr, &ap->match.mask));
			return;
		}
	}

	/* Screen out nonsensical messages. */
	switch(packet->dhcpv6_msg_type) {
#ifdef DHCP4o6
	      case DHCPV6_DHCPV4_RESPONSE:
		if (dhcpv4_over_dhcpv6) {
		  log_info("RCV: %s message on %s from %s.",
			   dhcpv6_type_names[packet->dhcpv6_msg_type],
			   packet->interface->name,
			   piaddr(packet->client_addr));
		  forw_dhcpv4_response(packet);
		}
		return;
#endif
	      case DHCPV6_ADVERTISE:
	      case DHCPV6_RECONFIGURE:
		if (stateless)
		  return;
	      /* Falls through */
	      case DHCPV6_REPLY:
		log_info("RCV: %s message on %s from %s.",
			 dhcpv6_type_names[packet->dhcpv6_msg_type],
			 packet->interface->name, piaddr(packet->client_addr));
		break;

	      default:
		return;
	}

	/* Find a client state that matches the incoming XID. */
	for (client = packet->interface->client ; client ;
	     client = client->next) {
		if (memcmp(&client->dhcpv6_transaction_id,
			   packet->dhcpv6_transaction_id, 3) == 0) {
			client->v6_handler(packet, client);
			return;
		}
	}

	/* XXX: temporary log for debugging */
	log_info("Packet received, but nothing done with it.");
}

#ifdef DHCP4o6
/*
 * \brief Forward a DHCPv4-response to the DHCPv4 client.
 *  (DHCPv6 client function)
 *
 * The DHCPv6 client receives a DHCPv4-response which is forwarded
 * to the DHCPv4 client.
 * Format: address:16 + DHCPv4 message content
 * (we have no state to keep the address so it is transported in
 *  DHCPv6 <-> DHCPv6 inter-process messages)
 *
 * \param packet the DHCPv4-response packet
 */
static void forw_dhcpv4_response(struct packet *packet)
{
	struct option_cache *oc;
	struct data_string enc_opt_data;
	struct data_string ds;
	int cc;

	/*
	 * Discard if relay is not ready.
	 */
	if (dhcp4o6_state == -1) {
		log_info("forw_dhcpv4_response: not ready.");
		return;
	}

	if (packet->client_addr.len != 16) {
		log_error("forw_dhcpv4_response: bad address");
		return;
	}

	/*
	 * Get our encapsulated DHCPv4 message.
	 */
	oc = lookup_option(&dhcpv6_universe, packet->options, D6O_DHCPV4_MSG);
	if (oc == NULL) {
		log_info("DHCPv4-response from %s missing "
			 "DHCPv4 Message option.",
			 piaddr(packet->client_addr));
		return;
	}

	memset(&enc_opt_data, 0, sizeof(enc_opt_data));
	if (!evaluate_option_cache(&enc_opt_data, NULL, NULL, NULL,
				   NULL, NULL, &global_scope, oc, MDL)) {
		log_error("forw_dhcpv4_response: error evaluating "
			  "DHCPv4 message.");
		data_string_forget(&enc_opt_data, MDL);
		return;
	}

	if (enc_opt_data.len < DHCP_FIXED_NON_UDP) {
		log_error("forw_dhcpv4_response: "
			  "no memory for encapsulated packet.");
		data_string_forget(&enc_opt_data, MDL);
		return;
	}

	/*
	 * Append address.
	 */
	memset(&ds, 0, sizeof(ds));
	if (!buffer_allocate(&ds.buffer, enc_opt_data.len + 16, MDL)) {
		log_error("forw_dhcpv4_response: no memory buffer.");
		data_string_forget(&enc_opt_data, MDL);
		return;
	}
	ds.data = ds.buffer->data;
	ds.len = enc_opt_data.len + 16;
	memcpy(ds.buffer->data, enc_opt_data.data, enc_opt_data.len);
	memcpy(ds.buffer->data + enc_opt_data.len,
	       packet->client_addr.iabuf, 16);
	data_string_forget(&enc_opt_data, MDL);

	/*
	 * Forward them.
	 */
	cc = send(dhcp4o6_fd, ds.data, ds.len, 0);
	if (cc < 0)
		log_error("forw_dhcpv4_response: send(): %m");

	data_string_forget(&ds, MDL);
}

/*
 * \brief Receive a DHCPv4-response from the DHCPv6 client.
 *  (DHCPv4 client function)
 *
 * The DHCPv4 client receives a DHCPv4-response forwarded
 * by the DHCPv6 client (using \ref forw_dhcpv4_response())
 *
 * \param raw the DHCPv4-response raw packet
 */
static void recv_dhcpv4_response(struct data_string *raw)
{
	struct packet *packet;
	struct iaddr from;

	if (interfaces == NULL) {
		log_error("recv_dhcpv4_response: no interfaces.");
		return;
	}

	from.len = 16;
	memcpy(from.iabuf, raw->data + (raw->len - 16), 16);

	/*
	 * Build a packet structure.
	 */
	packet = NULL;
	if (!packet_allocate(&packet, MDL)) {
		log_error("recv_dhcpv4_response: no memory for packet.");
		return;
	}

	packet->raw = (struct dhcp_packet *) raw->data;
	packet->packet_length = raw->len - 16;
	packet->client_port = remote_port;
	packet->client_addr = from;
	interface_reference(&packet->interface, interfaces, MDL);

	/* Allocate packet->options now so it is non-null for all packets */
	if (!option_state_allocate (&packet->options, MDL)) {
		log_error("recv_dhcpv4_response: no memory for options.");
		packet_dereference (&packet, MDL);
		return;
	}

	/* If there's an option buffer, try to parse it. */
	if (packet->packet_length >= DHCP_FIXED_NON_UDP + 4) {
		struct option_cache *op;
		if (!parse_options(packet)) {
			if (packet->options)
				option_state_dereference
					(&packet->options, MDL);
			packet_dereference (&packet, MDL);
			return;
		}

		if (packet->options_valid &&
		    (op = lookup_option(&dhcp_universe,
					packet->options,
					DHO_DHCP_MESSAGE_TYPE))) {
			struct data_string dp;
			memset(&dp, 0, sizeof dp);
			evaluate_option_cache(&dp, packet, NULL, NULL,
					      packet->options, NULL,
					      NULL, op, MDL);
			if (dp.len > 0)
				packet->packet_type = dp.data[0];
			else
				packet->packet_type = 0;
			data_string_forget(&dp, MDL);
		}
	}

	if (validate_packet(packet) != 0) {
		if (packet->packet_type)
			dhcp(packet);
		else
			bootp(packet);
	}

	/* If the caller kept the packet, they'll have upped the refcnt. */
	packet_dereference(&packet, MDL);
}
#endif /* DHCP4o6 */
#endif /* DHCPv6 */

void dhcpoffer (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_state *client;
	struct client_lease *lease, *lp;
	struct option **req;
	int i;
	int stop_selecting;
	const char *name = packet -> packet_type ? "DHCPOFFER" : "BOOTREPLY";
	char obuf [1024];
	struct timeval tv;

#ifdef DEBUG_PACKET
	dump_packet (packet);
#endif

	/* Find a client state that matches the xid... */
	for (client = ip -> client; client; client = client -> next)
		if (client -> xid == packet -> raw -> xid)
			break;

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	if (!client ||
	    client -> state != S_SELECTING ||
	    (packet -> interface -> hw_address.hlen - 1 !=
	     packet -> raw -> hlen) ||
	    (memcmp (&packet -> interface -> hw_address.hbuf [1],
		     packet -> raw -> chaddr, packet -> raw -> hlen))) {
#if defined (DEBUG)
		log_debug ("%s in wrong transaction.", name);
#endif
		return;
	}

	sprintf (obuf, "%s of %s from %s", name,
		 inet_ntoa(packet->raw->yiaddr),
		 piaddr(packet->client_addr));

	/* If this lease doesn't supply the minimum required DHCPv4 parameters,
	 * ignore it.
	 */
	req = client->config->required_options;
	if (req != NULL) {
		for (i = 0 ; req[i] != NULL ; i++) {
			if ((req[i]->universe == &dhcp_universe) &&
			    !lookup_option(&dhcp_universe, packet->options,
					   req[i]->code)) {
				struct option *option = NULL;
				unsigned code = req[i]->code;

				option_code_hash_lookup(&option,
							dhcp_universe.code_hash,
							&code, 0, MDL);

				if (option)
					log_info("%s: no %s option.", obuf,
						 option->name);
				else
					log_info("%s: no unknown-%u option.",
						 obuf, code);

				option_dereference(&option, MDL);

				return;
			}
		}
	}

	/* If we've already seen this lease, don't record it again. */
	for (lease = client -> offered_leases; lease; lease = lease -> next) {
		if (lease -> address.len == sizeof packet -> raw -> yiaddr &&
		    !memcmp (lease -> address.iabuf,
			     &packet -> raw -> yiaddr, lease -> address.len)) {
			log_debug ("%s: already seen.", obuf);
			return;
		}
	}

	lease = packet_to_lease (packet, client);
	if (!lease) {
		log_info ("%s: packet_to_lease failed.", obuf);
		return;
	}

	/* log it now, so it emits before the request goes out */
	log_info("%s", obuf);

	/* If this lease was acquired through a BOOTREPLY, record that
	   fact. */
	if (!packet -> options_valid || !packet -> packet_type)
		lease -> is_bootp = 1;

	/* Record the medium under which this lease was offered. */
	lease -> medium = client -> medium;

	/* Figure out when we're supposed to stop selecting. */
	stop_selecting = (client -> first_sending +
			  client -> config -> select_interval);

	/* If this is the lease we asked for, put it at the head of the
	   list, and don't mess with the arp request timeout. */
	if (lease -> address.len == client -> requested_address.len &&
	    !memcmp (lease -> address.iabuf,
		     client -> requested_address.iabuf,
		     client -> requested_address.len)) {
		lease -> next = client -> offered_leases;
		client -> offered_leases = lease;
	} else {
		/* Put the lease at the end of the list. */
		lease -> next = (struct client_lease *)0;
		if (!client -> offered_leases)
			client -> offered_leases = lease;
		else {
			for (lp = client -> offered_leases; lp -> next;
			     lp = lp -> next)
				;
			lp -> next = lease;
		}
	}

	/* If the selecting interval has expired, go immediately to
	   state_selecting().  Otherwise, time out into
	   state_selecting at the select interval. */
	if (stop_selecting <= cur_tv.tv_sec)
		state_selecting (client);
	else {
		tv.tv_sec = stop_selecting;
		tv.tv_usec = cur_tv.tv_usec;
		add_timeout(&tv, state_selecting, client, 0, 0);
		cancel_timeout(send_discover, client);
	}
}

/* Allocate a client_lease structure and initialize it from the parameters
   in the specified packet. */

struct client_lease *packet_to_lease (packet, client)
	struct packet *packet;
	struct client_state *client;
{
	struct client_lease *lease;
	unsigned i;
	struct option_cache *oc;
	struct option *option = NULL;
	struct data_string data;

	lease = (struct client_lease *)new_client_lease (MDL);

	if (!lease) {
		log_error("packet_to_lease: no memory to record lease.\n");
		return NULL;
	}

	memset(lease, 0, sizeof(*lease));

	/* Copy the lease options. */
	option_state_reference(&lease->options, packet->options, MDL);

	lease->address.len = sizeof(packet->raw->yiaddr);
	memcpy(lease->address.iabuf, &packet->raw->yiaddr,
	       lease->address.len);

	lease->next_srv_addr.len = sizeof(packet->raw->siaddr);
	memcpy(lease->next_srv_addr.iabuf, &packet->raw->siaddr,
	       lease->next_srv_addr.len);

	memset(&data, 0, sizeof(data));

	if (client -> config -> vendor_space_name) {
		i = DHO_VENDOR_ENCAPSULATED_OPTIONS;

		/* See if there was a vendor encapsulation option. */
		oc = lookup_option (&dhcp_universe, lease -> options, i);
		if (oc &&
		    client -> config -> vendor_space_name &&
		    evaluate_option_cache (&data, packet,
					   (struct lease *)0, client,
					   packet -> options, lease -> options,
					   &global_scope, oc, MDL)) {
			if (data.len) {
				if (!option_code_hash_lookup(&option,
						dhcp_universe.code_hash,
						&i, 0, MDL))
					log_fatal("Unable to find VENDOR "
						  "option (%s:%d).", MDL);
				parse_encapsulated_suboptions
					(packet -> options, option,
					 data.data, data.len, &dhcp_universe,
					 client -> config -> vendor_space_name
						);

				option_dereference(&option, MDL);
			}
			data_string_forget (&data, MDL);
		}
	} else
		i = 0;

	/* Figure out the overload flag. */
	oc = lookup_option (&dhcp_universe, lease -> options,
			    DHO_DHCP_OPTION_OVERLOAD);
	if (oc &&
	    evaluate_option_cache (&data, packet, (struct lease *)0, client,
				   packet -> options, lease -> options,
				   &global_scope, oc, MDL)) {
		if (data.len > 0)
			i = data.data [0];
		else
			i = 0;
		data_string_forget (&data, MDL);
	} else
		i = 0;

	/* If the server name was filled out, copy it. */
	if (!(i & 2) && packet -> raw -> sname [0]) {
		unsigned len;
		/* Don't count on the NUL terminator. */
		for (len = 0; len < DHCP_SNAME_LEN; len++)
			if (!packet -> raw -> sname [len])
				break;
		lease -> server_name = dmalloc (len + 1, MDL);
		if (!lease -> server_name) {
			log_error ("dhcpoffer: no memory for server name.\n");
			destroy_client_lease (lease);
			return (struct client_lease *)0;
		} else {
			memcpy (lease -> server_name,
				packet -> raw -> sname, len);
			lease -> server_name [len] = 0;
		}
	}

	/* Ditto for the filename. */
	if (!(i & 1) && packet -> raw -> file [0]) {
		unsigned len;
		/* Don't count on the NUL terminator. */
		for (len = 0; len < DHCP_FILE_LEN; len++)
			if (!packet -> raw -> file [len])
				break;
		lease -> filename = dmalloc (len + 1, MDL);
		if (!lease -> filename) {
			log_error ("dhcpoffer: no memory for filename.\n");
			destroy_client_lease (lease);
			return (struct client_lease *)0;
		} else {
			memcpy (lease -> filename,
				packet -> raw -> file, len);
			lease -> filename [len] = 0;
		}
	}

	execute_statements_in_scope(NULL, (struct packet *)packet, NULL,
				    client, lease->options, lease->options,
				    &global_scope, client->config->on_receipt,
				    NULL, NULL);

	return lease;
}

void dhcpnak (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_state *client;

	/* Find a client state that matches the xid... */
	for (client = ip -> client; client; client = client -> next)
		if (client -> xid == packet -> raw -> xid)
			break;

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	if (!client ||
	    (packet -> interface -> hw_address.hlen - 1 !=
	     packet -> raw -> hlen) ||
	    (memcmp (&packet -> interface -> hw_address.hbuf [1],
		     packet -> raw -> chaddr, packet -> raw -> hlen))) {
#if defined (DEBUG)
		log_debug ("DHCPNAK in wrong transaction.");
#endif
		return;
	}

	if (client -> state != S_REBOOTING &&
	    client -> state != S_REQUESTING &&
	    client -> state != S_RENEWING &&
	    client -> state != S_REBINDING) {
#if defined (DEBUG)
		log_debug ("DHCPNAK in wrong state.");
#endif
		return;
	}

	log_info ("DHCPNAK from %s", piaddr (packet -> client_addr));

	if (!client -> active) {
#if defined (DEBUG)
		log_info ("DHCPNAK with no active lease.\n");
#endif
		return;
	}

	/* If we get a DHCPNAK, we use the EXPIRE dhclient-script state
	 * to indicate that we want all old bindings to be removed.  (It
	 * is possible that we may get a NAK while in the RENEW state,
	 * so we might have bindings active at that time)
	 */
	script_init(client, "EXPIRE", NULL);
	script_write_params(client, "old_", client->active);
	script_write_requested(client);
	if (client->alias)
		script_write_params(client, "alias_", client->alias);
	script_go(client);

	destroy_client_lease (client -> active);
	client -> active = (struct client_lease *)0;

	/* Stop sending DHCPREQUEST packets... */
	cancel_timeout (send_request, client);

	/* On some scripts, 'EXPIRE' causes the interface to be ifconfig'd
	 * down (this expunges any routes and arp cache).  This makes the
	 * interface unusable by state_init(), which we call next.  So, we
	 * need to 'PREINIT' the interface to bring it back up.
	 */
	script_init(client, "PREINIT", NULL);
	if (client->alias)
		script_write_params(client, "alias_", client->alias);
	script_go(client);

	client -> state = S_INIT;
	state_init (client);
}

/* Send out a DHCPDISCOVER packet, and set a timeout to send out another
   one after the right interval has expired.  If we don't get an offer by
   the time we reach the panic interval, call the panic function. */

void send_discover (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;
	int interval;
	int increase = 1;
	struct timeval tv;

	/* Figure out how long it's been since we started transmitting. */
	interval = cur_time - client -> first_sending;

	/* If we're past the panic timeout, call the script and tell it
	   we haven't found anything for this interface yet. */
	if (interval > client -> config -> timeout) {
		state_panic (client);
		return;
	}

	/* If we're selecting media, try the whole list before doing
	   the exponential backoff, but if we've already received an
	   offer, stop looping, because we obviously have it right. */
	if (!client -> offered_leases &&
	    client -> config -> media) {
		int fail = 0;
	      again:
		if (client -> medium) {
			client -> medium = client -> medium -> next;
			increase = 0;
		}
		if (!client -> medium) {
			if (fail)
				log_fatal ("No valid media types for %s!",
				       client -> interface -> name);
			client -> medium =
				client -> config -> media;
			increase = 1;
		}

		log_info ("Trying medium \"%s\" %d",
			  client -> medium -> string, increase);
		script_init(client, "MEDIUM", client -> medium);
		if (script_go(client)) {
			fail = 1;
			goto again;
		}
	}

	/* If we're supposed to increase the interval, do so.  If it's
	   currently zero (i.e., we haven't sent any packets yet), set
	   it to initial_interval; otherwise, add to it a random number
	   between zero and two times itself.  On average, this means
	   that it will double with every transmission. */
	if (increase) {
		if (!client->interval)
			client->interval = client->config->initial_interval;
		else
			client->interval += random() % (2 * client->interval);

		/* Don't backoff past cutoff. */
		if (client->interval > client->config->backoff_cutoff)
			client->interval = (client->config->backoff_cutoff / 2)
				 + (random() % client->config->backoff_cutoff);
	} else if (!client->interval)
		client->interval = client->config->initial_interval;

	/* If the backoff would take us to the panic timeout, just use that
	   as the interval. */
	if (cur_time + client -> interval >
	    client -> first_sending + client -> config -> timeout)
		client -> interval =
			(client -> first_sending +
			 client -> config -> timeout) - cur_time + 1;

	/* Record the number of seconds since we started sending. */
	if (interval < 65536)
		client -> packet.secs = htons (interval);
	else
		client -> packet.secs = htons (65535);
	client -> secs = client -> packet.secs;

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		log_info ("DHCPDISCOVER interval %ld",
			  (long)(client -> interval));
	} else
#endif
	log_info ("DHCPDISCOVER on %s to %s port %d interval %ld",
	      client -> name ? client -> name : client -> interface -> name,
	      inet_ntoa (sockaddr_broadcast.sin_addr),
	      ntohs (sockaddr_broadcast.sin_port), (long)(client -> interval));

	/* Send out a packet. */
#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		result = send_dhcpv4_query(client, 1);
	} else
#endif
	result = send_packet(client->interface, NULL, &client->packet,
			     client->packet_length, inaddr_any,
                             &sockaddr_broadcast, NULL);
        if (result < 0) {
#if defined(DHCPv6) && defined(DHCP4o6)
		if (dhcpv4_over_dhcpv6) {
			log_error("%s:%d: Failed to send %d byte long packet.",
				  MDL, client->packet_length);
		} else
#endif
		log_error("%s:%d: Failed to send %d byte long packet over %s "
			  "interface.", MDL, client->packet_length,
			  client->interface->name);
	}

	/*
	 * If we used 0 microseconds here, and there were other clients on the
	 * same network with a synchronized local clock (ntp), and a similar
	 * zero-microsecond-scheduler behavior, then we could be participating
	 * in a sub-second DOS ttck.
	 */
	tv.tv_sec = cur_tv.tv_sec + client->interval;
	tv.tv_usec = client->interval > 1 ? random() % 1000000 : cur_tv.tv_usec;
	add_timeout(&tv, send_discover, client, 0, 0);
}


/*
 * \brief Remove leases from a list of leases which duplicate a given lease
 *
 * Searches through a linked-list of leases, remove the first one matches the
 * given lease's address and value of is_static.   The latter test is done
 * so we only remove leases that are from the same source (i.e server/lease file
 *  vs config file).  This ensures we do not discard "fallback" config file leases
 * that happen to match non-config file leases.
 *
 * \param lease_list list of leases to clean
 * \param lease lease for which duplicates should be removed
 */
void discard_duplicate (struct client_lease** lease_list, struct client_lease* lease) {
	struct client_lease *cur, *prev, *next;

	if (!lease_list || !lease) {
		return;
	}

	prev = (struct client_lease *)0;
	for (cur = *lease_list; cur; cur = next) {
		next = cur->next;
		if ((cur->is_static == lease->is_static) &&
		    (cur->address.len == lease->address.len &&
		     !memcmp (cur->address.iabuf, lease->address.iabuf,
			      lease->address.len))) {
			if (prev)
				prev->next = next;
			else
				*lease_list = next;

			destroy_client_lease (cur);
			break;
		} else {
			prev = cur;
		}
	}
}

/*
 * \brief Add a given lease to the end of list of leases
 *
 * Searches through a linked-list of leases, removing any that match the
 * given lease's address and value of is_static.  The latter test is done
 * so we only remove leases that are from the same source (i.e server/lease file
 *  vs config file).  This ensures we do not discard "fallback" config file leases
 * that happen to match non-config file leases.
 *
 * \param lease_list list of leases to clean
 * \param lease lease for which duplicates should be removed
 */
void add_to_tail(struct client_lease** lease_list,
		 struct client_lease* lease)
{
	if (!lease_list || !lease) {
		return;
	}

	/* If there is already a lease for this address and
	* is_static value, toss discard it.  This ensures
	* we only keep one dynamic and/or one static lease
	* for a given address. */
	discard_duplicate(lease_list, lease);

	/* Find the tail */
	struct client_lease* tail;
	for (tail = *lease_list; tail && tail->next; tail = tail->next){};

	/* Ensure the tail points nowhere. */
	lease->next = NULL;

	/* Add to the tail. */
	if (!tail) {
		*lease_list = lease;
	} else {
		tail->next = lease;
	}
}

#if 0
void dbg_print_lease(char *text, struct client_lease* lease) {
	if (!lease) {
		log_debug("%s, lease is null", text);
	} else {
		log_debug ("%s: %p addr:%s expires:%ld :is_static? %d",
			   text, lease, piaddr (lease->address),
                           (lease->expiry - cur_time),
			   lease->is_static);
	}
}
#endif

/* state_panic gets called if we haven't received any offers in a preset
   amount of time.   When this happens, we try to use existing leases that
   haven't yet expired, and failing that, we call the client script and
   hope it can do something. */

void state_panic (cpp)
	void *cpp;
{
	struct client_state *client = cpp;
	struct client_lease *loop;
	struct client_lease *lp;
	struct timeval tv;

	loop = lp = client -> active;

	log_info ("No DHCPOFFERS received.");

	/* We may not have an active lease, but we may have some
	   predefined leases that we can try. */
	if (!client -> active && client -> leases)
		goto activate_next;

	/* Run through the list of leases and see if one can be used. */
	while (client -> active) {
		if (client -> active -> expiry > cur_time) {
			log_info ("Trying %s lease %s",
				  (client -> active -> is_static
				   ? "fallback" : "recorded"),
				  piaddr (client -> active -> address));
			/* Run the client script with the existing
			   parameters. */
			script_init(client, "TIMEOUT",
				     client -> active -> medium);
			script_write_params(client, "new_", client -> active);
			script_write_requested(client);
			if (client -> alias)
				script_write_params(client, "alias_",
						    client -> alias);

			/* If the old lease is still good and doesn't
			   yet need renewal, go into BOUND state and
			   timeout at the renewal time. */
			if (!script_go(client)) {
			    if (cur_time < client -> active -> renewal) {
				client -> state = S_BOUND;
				log_info ("bound: renewal in %ld %s.",
					  (long)(client -> active -> renewal -
						 cur_time), "seconds");
				tv.tv_sec = client->active->renewal;
				tv.tv_usec = ((client->active->renewal -
						    cur_time) > 1) ?
						random() % 1000000 :
						cur_tv.tv_usec;
				add_timeout(&tv, state_bound, client, 0, 0);
			    } else {
				client -> state = S_BOUND;
				log_info ("bound: immediate renewal.");
				state_bound (client);
			    }
			    reinitialize_interfaces ();
			    detach ();
			    return;
			}
		}

		/* If there are no other leases, give up. */
		if (!client -> leases) {
			client -> leases = client -> active;
			client -> active = (struct client_lease *)0;
			break;
		}

	activate_next:
		/* Otherwise, put the active lease at the end of the
		   lease list, and try another lease.. */
		add_to_tail(&client->leases, client->active);

		client -> active = client -> leases;
		client -> leases = client -> leases -> next;

		/* If we already tried this lease, we've exhausted the
		   set of leases, so we might as well give up for
		   now. */
		if (client -> active == loop)
			break;
		else if (!loop)
			loop = client -> active;
	}

	/* No leases were available, or what was available didn't work, so
	   tell the shell script that we failed to allocate an address,
	   and try again later. */
	if (onetry) {
		if (!quiet) {
			log_info ("Unable to obtain a lease on first try.%s",
				  "  Exiting.");
		}

#if defined (CALL_SCRIPT_ON_ONETRY_FAIL)
		/* Let's call a script and we're done */
		script_init(client, "FAIL", (struct string_list *)0);
		script_go(client);
#endif
		finish(2);
	}

	log_info ("No working leases in persistent database - sleeping.");
	script_init(client, "FAIL", (struct string_list *)0);
	if (client -> alias)
		script_write_params(client, "alias_", client -> alias);
	script_go(client);
	client -> state = S_INIT;
	tv.tv_sec = cur_tv.tv_sec + ((client->config->retry_interval + 1) / 2 +
		    (random() % client->config->retry_interval));
	tv.tv_usec = ((tv.tv_sec - cur_tv.tv_sec) > 1) ?
			random() % 1000000 : cur_tv.tv_usec;
	add_timeout(&tv, state_init, client, 0, 0);
	detach ();
}

void send_request (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;
	int interval;
	struct sockaddr_in destination;
	struct in_addr from;
	struct timeval tv;
	char rip_buf[128];
	const char* rip_str = "";

	/* Figure out how long it's been since we started transmitting. */
	interval = cur_time - client -> first_sending;

	/* If we're in the INIT-REBOOT or REQUESTING state and we're
	   past the reboot timeout, go to INIT and see if we can
	   DISCOVER an address... */
	/* XXX In the INIT-REBOOT state, if we don't get an ACK, it
	   means either that we're on a network with no DHCP server,
	   or that our server is down.  In the latter case, assuming
	   that there is a backup DHCP server, DHCPDISCOVER will get
	   us a new address, but we could also have successfully
	   reused our old address.  In the former case, we're hosed
	   anyway.  This is not a win-prone situation. */
	if ((client -> state == S_REBOOTING ||
	     client -> state == S_REQUESTING) &&
	    interval > client -> config -> reboot_timeout) {
	cancel:
		client -> state = S_INIT;
		cancel_timeout (send_request, client);
		state_init (client);
		return;
	}

	/* If we're in the reboot state, make sure the media is set up
	   correctly. */
	if (client -> state == S_REBOOTING &&
	    !client -> medium &&
	    client -> active -> medium ) {
		script_init(client, "MEDIUM", client -> active -> medium);

		/* If the medium we chose won't fly, go to INIT state. */
		if (script_go(client))
			goto cancel;

		/* Record the medium. */
		client -> medium = client -> active -> medium;
	}

	/* If the lease has expired, relinquish the address and go back
	   to the INIT state. */
	if (client -> state != S_REQUESTING &&
	    cur_time > client -> active -> expiry) {
		/* Run the client script with the new parameters. */
		script_init(client, "EXPIRE", (struct string_list *)0);
		script_write_params(client, "old_", client -> active);
		script_write_requested(client);
		if (client -> alias)
			script_write_params(client, "alias_",
					    client -> alias);
		script_go(client);

		/* Now do a preinit on the interface so that we can
		   discover a new address. */
		script_init(client, "PREINIT", (struct string_list *)0);
		if (client -> alias)
			script_write_params(client, "alias_",
					    client -> alias);
		script_go(client);

		client -> state = S_INIT;
		state_init (client);
		return;
	}

	/* Do the exponential backoff... */
	if (!client -> interval)
		client -> interval = client -> config -> initial_interval;
	else {
		client -> interval += ((random () >> 2) %
				       (2 * client -> interval));
	}

	/* Don't backoff past cutoff. */
	if (client -> interval >
	    client -> config -> backoff_cutoff)
		client -> interval =
			((client -> config -> backoff_cutoff / 2)
			 + ((random () >> 2) %
					client -> config -> backoff_cutoff));

	/* If the backoff would take us to the expiry time, just set the
	   timeout to the expiry time. */
	if (client -> state != S_REQUESTING &&
	    cur_time + client -> interval > client -> active -> expiry)
		client -> interval =
			client -> active -> expiry - cur_time + 1;

	/* If the lease T2 time has elapsed, or if we're not yet bound,
	   broadcast the DHCPREQUEST rather than unicasting. */
	if (client -> state == S_REQUESTING ||
	    client -> state == S_REBOOTING ||
	    cur_time > client -> active -> rebind)
		destination.sin_addr = sockaddr_broadcast.sin_addr;
	else
		memcpy (&destination.sin_addr.s_addr,
			client -> destination.iabuf,
			sizeof destination.sin_addr.s_addr);
	destination.sin_port = remote_port;
	destination.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
	destination.sin_len = sizeof destination;
#endif

	if (client -> state == S_RENEWING ||
	    client -> state == S_REBINDING)
		memcpy (&from, client -> active -> address.iabuf,
			sizeof from);
	else
		from.s_addr = INADDR_ANY;

	/* Record the number of seconds since we started sending. */
	if (client -> state == S_REQUESTING)
		client -> packet.secs = client -> secs;
	else {
		if (interval < 65536)
			client -> packet.secs = htons (interval);
		else
			client -> packet.secs = htons (65535);
	}

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		log_info ("DHCPREQUEST");
	} else
#endif
	memset(rip_buf, 0x0, sizeof(rip_buf));
	if (client->state == S_BOUND || client->state == S_RENEWING ||
	    client->state == S_REBINDING) {
		rip_str = inet_ntoa(client->packet.ciaddr);
	} else {
		rip_str = piaddr(client->requested_address);
	}

	strncpy(rip_buf, rip_str, sizeof(rip_buf)-1);
	log_info ("DHCPREQUEST for %s on %s to %s port %d", rip_buf,
		  client->name ? client->name : client->interface->name,
		  inet_ntoa(destination.sin_addr),
		  ntohs (destination.sin_port));

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		int broadcast = 0;
		if (destination.sin_addr.s_addr == INADDR_BROADCAST)
			broadcast = 1;
		result = send_dhcpv4_query(client, broadcast);
		if (result < 0) {
			log_error("%s:%d: Failed to send %d byte long packet.",
				  MDL, client->packet_length);
		}
	} else
#endif
	if (destination.sin_addr.s_addr != INADDR_BROADCAST &&
	    fallback_interface) {
		result = send_packet(fallback_interface, NULL, &client->packet,
				     client->packet_length, from, &destination,
				     NULL);
		if (result < 0) {
			log_error("%s:%d: Failed to send %d byte long packet "
				  "over %s interface.", MDL,
				  client->packet_length,
				  fallback_interface->name);
		}
        }
	else {
		/* Send out a packet. */
		result = send_packet(client->interface, NULL, &client->packet,
				     client->packet_length, from, &destination,
				     NULL);
		if (result < 0) {
			log_error("%s:%d: Failed to send %d byte long packet"
				  " over %s interface.", MDL,
				  client->packet_length,
				  client->interface->name);
		}
        }

	tv.tv_sec = cur_tv.tv_sec + client->interval;
	tv.tv_usec = ((tv.tv_sec - cur_tv.tv_sec) > 1) ?
			random() % 1000000 : cur_tv.tv_usec;
	add_timeout(&tv, send_request, client, 0, 0);
}

void send_decline (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		log_info ("DHCPDECLINE");
	} else
#endif
	log_info ("DHCPDECLINE of %s on %s to %s port %d",
		  piaddr(client->requested_address),
		  (client->name ? client->name : client->interface->name),
		  inet_ntoa(sockaddr_broadcast.sin_addr),
		  ntohs(sockaddr_broadcast.sin_port));

	/* Send out a packet. */
#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		result = send_dhcpv4_query(client, 1);
	} else
#endif
	result = send_packet(client->interface, NULL, &client->packet,
			     client->packet_length, inaddr_any,
			     &sockaddr_broadcast, NULL);
	if (result < 0) {
#if defined(DHCPv6) && defined(DHCP4o6)
		if (dhcpv4_over_dhcpv6) {
			log_error("%s:%d: Failed to send %d byte long packet.",
				  MDL, client->packet_length);
		} else
#endif
		log_error("%s:%d: Failed to send %d byte long packet over %s"
			  " interface.", MDL, client->packet_length,
			  client->interface->name);
	}
}

void send_release (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;
	struct sockaddr_in destination;
	struct in_addr from;

	memcpy (&from, client -> active -> address.iabuf,
		sizeof from);
	memcpy (&destination.sin_addr.s_addr,
		client -> destination.iabuf,
		sizeof destination.sin_addr.s_addr);
	destination.sin_port = remote_port;
	destination.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
	destination.sin_len = sizeof destination;
#endif

	/* Set the lease to end now, so that we don't accidentally
	   reuse it if we restart before the old expiry time. */
	client -> active -> expiry =
		client -> active -> renewal =
		client -> active -> rebind = cur_time;
	if (!write_client_lease (client, client -> active, 1, 1)) {
		log_error ("Can't release lease: lease write failed.");
		return;
	}

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		log_info ("DHCPRELEASE");
	} else
#endif
	log_info ("DHCPRELEASE of %s on %s to %s port %d",
		  piaddr(client->active->address),
		  client->name ? client->name : client->interface->name,
		  inet_ntoa (destination.sin_addr),
		  ntohs (destination.sin_port));

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6) {
		int broadcast = 0;
		if (destination.sin_addr.s_addr == INADDR_BROADCAST)
			broadcast = 1;
		result = send_dhcpv4_query(client, broadcast);
		if (result < 0) {
			log_error("%s:%d: Failed to send %d byte long packet.",
				  MDL, client->packet_length);
		}
	} else
#endif
	if (fallback_interface) {
		result = send_packet(fallback_interface, NULL, &client->packet,
				      client->packet_length, from, &destination,
				      NULL);
		if (result < 0) {
			log_error("%s:%d: Failed to send %d byte long packet"
				  " over %s interface.", MDL,
				  client->packet_length,
				  fallback_interface->name);
		}
        } else {
		/* Send out a packet. */
		result = send_packet(client->interface, NULL, &client->packet,
				      client->packet_length, from, &destination,
				      NULL);
		if (result < 0) {
			log_error ("%s:%d: Failed to send %d byte long packet"
				   " over %s interface.", MDL,
				   client->packet_length,
				   client->interface->name);
		}

        }
}

#if defined(DHCPv6) && defined(DHCP4o6)
/*
 * \brief Send a DHCPv4-query to the DHCPv6 client
 *  (DHCPv4 client function)
 *
 * The DHCPv4 client sends a DHCPv4-query to the DHCPv6 client over
 * the inter-process communication socket.
 *
 * \param client the DHCPv4 client state
 * \param broadcast the broadcast flag
 * \return the sent byte count (-1 on error)
 */
static int send_dhcpv4_query(struct client_state *client, int broadcast) {
	struct data_string ds;
	struct dhcpv4_over_dhcpv6_packet *query;
	int ofs, len, cc;

	if (dhcp4o6_state <= 0) {
		log_info("send_dhcpv4_query: not ready.");
		return -1;
	}

	/*
	 * Compute buffer length and allocate it.
	 */
	len = ofs = (int)(offsetof(struct dhcpv4_over_dhcpv6_packet, options));
	len += dhcpv6_universe.tag_size + dhcpv6_universe.length_size;
	len += client->packet_length;
	memset(&ds, 0, sizeof(ds));
	if (!buffer_allocate(&ds.buffer, len, MDL)) {
		log_error("Unable to allocate memory for DHCPv4-query.");
		return -1;
	}
	ds.data = ds.buffer->data;
	ds.len = len;

	/*
	 * Fill header.
	 */
	query = (struct dhcpv4_over_dhcpv6_packet *)ds.data;
	query->msg_type = DHCPV6_DHCPV4_QUERY;
	query->flags[0] = query->flags[1] = query->flags[2] = 0;
	if (!broadcast)
		query->flags[0] |= DHCP4O6_QUERY_UNICAST;

	/*
	 * Append DHCPv4 message.
	 */
	dhcpv6_universe.store_tag(ds.buffer->data + ofs, D6O_DHCPV4_MSG);
	ofs += dhcpv6_universe.tag_size;
	dhcpv6_universe.store_length(ds.buffer->data + ofs,
				     client->packet_length);
	ofs += dhcpv6_universe.length_size;
	memcpy(ds.buffer->data + ofs, &client->packet, client->packet_length);

	/*
	 * Send DHCPv6 message.
	 */
	cc = send(dhcp4o6_fd, ds.data, ds.len, 0);
	if (cc < 0)
		log_error("send_dhcpv4_query: send(): %m");

	data_string_forget(&ds, MDL);

	return cc;
}

/*
 * \brief Forward a DHCPv4-query to all DHCPv4 over DHCPv6 server addresses.
 *  (DHCPv6 client function)
 *
 * \param raw the DHCPv6 DHCPv4-query message raw content
 */
static void forw_dhcpv4_query(struct data_string *raw) {
	struct interface_info *ip;
	struct client_state *client;
	struct dhc6_lease *lease;
	struct option_cache *oc;
	struct data_string addrs;
	struct sockaddr_in6 sin6;
	int i, send_ret, attempt, success;

	attempt = success = 0;
	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = remote_port;
#ifdef HAVE_SA_LEN
	sin6.sin6_len = sizeof(sin6);
#endif
	memset(&addrs, 0, sizeof(addrs));
	for (ip = interfaces; ip != NULL; ip = ip->next) {
		for (client = ip->client; client != NULL;
		     client = client->next) {
			if ((client->state != S_BOUND) &&
			    (client->state != S_RENEWING) &&
			    (client->state != S_REBINDING))
				continue;
			lease = client->active_lease;
			if ((lease == NULL) || lease->released)
				continue;
			oc = lookup_option(&dhcpv6_universe,
					   lease->options,
					   D6O_DHCP4_O_DHCP6_SERVER);
			if ((oc == NULL) ||
			    !evaluate_option_cache(&addrs, NULL, NULL, NULL,
						   lease->options, NULL,
						   &global_scope, oc, MDL) ||
			    ((addrs.len % sizeof(sin6.sin6_addr)) != 0)) {
				data_string_forget(&addrs, MDL);
				continue;
			}
			if (addrs.len == 0) {
				/* note there is nothing to forget */
				inet_pton(AF_INET6,
					  All_DHCP_Relay_Agents_and_Servers,
					  &sin6.sin6_addr);
				attempt++;
				send_ret = send_packet6(ip, raw->data,
							raw->len, &sin6);
				if (send_ret == raw->len)
					success++;
				continue;
			}
			for (i = 0; i < addrs.len;
			     i += sizeof(sin6.sin6_addr)) {
				memcpy(&sin6.sin6_addr, addrs.data + i,
				       sizeof(sin6.sin6_addr));
				attempt++;
				send_ret = send_packet6(ip, raw->data,
							raw->len, &sin6);
				if (send_ret == raw->len)
					success++;
			}
			data_string_forget(&addrs, MDL);
		}
	}

	log_info("forw_dhcpv4_query: sent(%d): %d/%d",
		 raw->len, success, attempt);

	if (attempt == 0)
		dhcp4o6_stop();
}
#endif

void
make_client_options(struct client_state *client, struct client_lease *lease,
		    u_int8_t *type, struct option_cache *sid,
		    struct iaddr *rip, struct option **prl,
		    struct option_state **op)
{
	unsigned i;
	struct option_cache *oc;
	struct option *option = NULL;
	struct buffer *bp = NULL;

	/* If there are any leftover options, get rid of them. */
	if (*op)
		option_state_dereference(op, MDL);

	/* Allocate space for options. */
	option_state_allocate(op, MDL);

	/* Send the server identifier if provided. */
	if (sid)
		save_option(&dhcp_universe, *op, sid);

	oc = NULL;

	/* Send the requested address if provided. */
	if (rip) {
		client->requested_address = *rip;
		i = DHO_DHCP_REQUESTED_ADDRESS;
		if (!(option_code_hash_lookup(&option, dhcp_universe.code_hash,
					      &i, 0, MDL) &&
		      make_const_option_cache(&oc, NULL, rip->iabuf, rip->len,
					      option, MDL)))
			log_error ("can't make requested address cache.");
		else {
			save_option(&dhcp_universe, *op, oc);
			option_cache_dereference(&oc, MDL);
		}
		option_dereference(&option, MDL);
	} else {
		client->requested_address.len = 0;
	}

	i = DHO_DHCP_MESSAGE_TYPE;
	if (!(option_code_hash_lookup(&option, dhcp_universe.code_hash, &i, 0,
				      MDL) &&
	      make_const_option_cache(&oc, NULL, type, 1, option, MDL)))
		log_error("can't make message type.");
	else {
		save_option(&dhcp_universe, *op, oc);
		option_cache_dereference(&oc, MDL);
	}
	option_dereference(&option, MDL);

	if (prl) {
		int len;

		/* Probe the length of the list. */
		len = 0;
		for (i = 0 ; prl[i] != NULL ; i++)
			if (prl[i]->universe == &dhcp_universe)
				len++;

		if (!buffer_allocate(&bp, len, MDL))
			log_error("can't make parameter list buffer.");
		else {
			unsigned code = DHO_DHCP_PARAMETER_REQUEST_LIST;

			len = 0;
			for (i = 0 ; prl[i] != NULL ; i++)
				if (prl[i]->universe == &dhcp_universe)
					bp->data[len++] = prl[i]->code;

			if (!(option_code_hash_lookup(&option,
						      dhcp_universe.code_hash,
						      &code, 0, MDL) &&
			      make_const_option_cache(&oc, &bp, NULL, len,
						      option, MDL))) {
				if (bp != NULL)
					buffer_dereference(&bp, MDL);
				log_error ("can't make option cache");
			} else {
				save_option(&dhcp_universe, *op, oc);
				option_cache_dereference(&oc, MDL);
			}
			option_dereference(&option, MDL);
		}
	}

	/*
	 * If requested (duid_v4 == 1) add an RFC4361 compliant client-identifier
	 * This can be overridden by including a client id in the configuration
	 * file.
	 */
 	if (duid_v4 == 1) {
		struct data_string client_identifier;
		int hw_idx, hw_len;

		memset(&client_identifier, 0, sizeof(client_identifier));
		client_identifier.len = 1 + 4 + default_duid.len;
		if (!buffer_allocate(&client_identifier.buffer,
				     client_identifier.len, MDL))
			log_fatal("no memory for default DUID!");
		client_identifier.data = client_identifier.buffer->data;

		i = DHO_DHCP_CLIENT_IDENTIFIER;

		/* Client-identifier type : 1 byte */
		*client_identifier.buffer->data = 255;

		/* IAID : 4 bytes
		 * we use the low 4 bytes from the interface address
		 */
		if (client->interface->hw_address.hlen > 4) {
			hw_idx = client->interface->hw_address.hlen - 4;
			hw_len = 4;
		} else {
			hw_idx = 0;
			hw_len = client->interface->hw_address.hlen;
		}
		memcpy(&client_identifier.buffer->data + 5 - hw_len,
		       client->interface->hw_address.hbuf + hw_idx,
		       hw_len);

		/* Add the default duid */
		memcpy(&client_identifier.buffer->data+(1+4),
		       default_duid.data, default_duid.len);

		/* And save the option */
		if (!(option_code_hash_lookup(&option, dhcp_universe.code_hash,
					      &i, 0, MDL) &&
		      make_const_option_cache(&oc, NULL,
					      (u_int8_t *)client_identifier.data,
					      client_identifier.len,
					      option, MDL)))
			log_error ("can't make requested client id cache..");
		else {
			save_option (&dhcp_universe, *op, oc);
			option_cache_dereference (&oc, MDL);
		}
		option_dereference(&option, MDL);
	}

	/* Run statements that need to be run on transmission. */
	if (client->config->on_transmission)
		execute_statements_in_scope(NULL, NULL, NULL, client,
					    (lease ? lease->options : NULL),
					    *op, &global_scope,
					    client->config->on_transmission,
					    NULL, NULL);
}

void make_discover (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	unsigned char discover = DHCPDISCOVER;
	struct option_state *options = (struct option_state *)0;

	memset (&client -> packet, 0, sizeof (client -> packet));

	make_client_options (client,
			     lease, &discover, (struct option_cache *)0,
			     lease ? &lease -> address : (struct iaddr *)0,
			     client -> config -> requested_options,
			     &options);

	/* Set up the option buffer... */
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet,
			      (struct lease *)0, client,
			      /* maximum packet size */1500,
			      (struct option_state *)0,
			      options,
			      /* scope */ &global_scope,
			      /* overload */ 0,
			      /* terminate */0,
			      /* bootpp    */0,
			      (struct data_string *)0,
			      client -> config -> vendor_space_name);

	option_state_dereference (&options, MDL);
	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.hbuf [0];
	/* Assumes hw_address is known, otherwise a random value may result */
	client -> packet.hlen = client -> interface -> hw_address.hlen - 1;
	client -> packet.hops = 0;
	client -> packet.xid = random ();
	client -> packet.secs = 0; /* filled in by send_discover. */

	if (can_receive_unicast_unconfigured (client -> interface))
		client -> packet.flags = 0;
	else
		client -> packet.flags = htons (BOOTP_BROADCAST);

	memset (&(client -> packet.ciaddr),
		0, sizeof client -> packet.ciaddr);
	memset (&(client -> packet.yiaddr),
		0, sizeof client -> packet.yiaddr);
	memset (&(client -> packet.siaddr),
		0, sizeof client -> packet.siaddr);
	client -> packet.giaddr = giaddr;
	if (client -> interface -> hw_address.hlen > 0)
	    memcpy (client -> packet.chaddr,
		    &client -> interface -> hw_address.hbuf [1],
		    (unsigned)(client -> interface -> hw_address.hlen - 1));

#ifdef DEBUG_PACKET
	dump_raw ((unsigned char *)&client -> packet, client -> packet_length);
#endif
}


void make_request (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	unsigned char request = DHCPREQUEST;
	struct option_cache *oc;

	memset (&client -> packet, 0, sizeof (client -> packet));

	if (client -> state == S_REQUESTING)
		oc = lookup_option (&dhcp_universe, lease -> options,
				    DHO_DHCP_SERVER_IDENTIFIER);
	else
		oc = (struct option_cache *)0;

	if (client -> sent_options)
		option_state_dereference (&client -> sent_options, MDL);

	make_client_options (client, lease, &request, oc,
			     ((client -> state == S_REQUESTING ||
			       client -> state == S_REBOOTING)
			      ? &lease -> address
			      : (struct iaddr *)0),
			     client -> config -> requested_options,
			     &client -> sent_options);

	/* Set up the option buffer... */
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet,
			      (struct lease *)0, client,
			      /* maximum packet size */1500,
			      (struct option_state *)0,
			      client -> sent_options,
			      /* scope */ &global_scope,
			      /* overload */ 0,
			      /* terminate */0,
			      /* bootpp    */0,
			      (struct data_string *)0,
			      client -> config -> vendor_space_name);

	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.hbuf [0];
	/* Assumes hw_address is known, otherwise a random value may result */
	client -> packet.hlen = client -> interface -> hw_address.hlen - 1;
	client -> packet.hops = 0;
	client -> packet.xid = client -> xid;
	client -> packet.secs = 0; /* Filled in by send_request. */

	/* If we own the address we're requesting, put it in ciaddr;
	   otherwise set ciaddr to zero. */
	if (client -> state == S_BOUND ||
	    client -> state == S_RENEWING ||
	    client -> state == S_REBINDING) {
		memcpy (&client -> packet.ciaddr,
			lease -> address.iabuf, lease -> address.len);
		client -> packet.flags = 0;
	} else {
		memset (&client -> packet.ciaddr, 0,
			sizeof client -> packet.ciaddr);
		if (can_receive_unicast_unconfigured (client -> interface))
			client -> packet.flags = 0;
		else
			client -> packet.flags = htons (BOOTP_BROADCAST);
	}

	memset (&client -> packet.yiaddr, 0,
		sizeof client -> packet.yiaddr);
	memset (&client -> packet.siaddr, 0,
		sizeof client -> packet.siaddr);
	if (client -> state != S_BOUND &&
	    client -> state != S_RENEWING)
		client -> packet.giaddr = giaddr;
	else
		memset (&client -> packet.giaddr, 0,
			sizeof client -> packet.giaddr);
	if (client -> interface -> hw_address.hlen > 0)
	    memcpy (client -> packet.chaddr,
		    &client -> interface -> hw_address.hbuf [1],
		    (unsigned)(client -> interface -> hw_address.hlen - 1));

#ifdef DEBUG_PACKET
	dump_raw ((unsigned char *)&client -> packet, client -> packet_length);
#endif
}

void make_decline (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	unsigned char decline = DHCPDECLINE;
	struct option_cache *oc;

	struct option_state *options = (struct option_state *)0;

	/* Create the options cache. */
	oc = lookup_option (&dhcp_universe, lease -> options,
			    DHO_DHCP_SERVER_IDENTIFIER);
	make_client_options(client, lease, &decline, oc, &lease->address,
			    NULL, &options);

	/* Consume the options cache into the option buffer. */
	memset (&client -> packet, 0, sizeof (client -> packet));
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet,
			      (struct lease *)0, client, 0,
			      (struct option_state *)0, options,
			      &global_scope, 0, 0, 0, (struct data_string *)0,
			      client -> config -> vendor_space_name);

	/* Destroy the options cache. */
	option_state_dereference (&options, MDL);

	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.hbuf [0];
	/* Assumes hw_address is known, otherwise a random value may result */
	client -> packet.hlen = client -> interface -> hw_address.hlen - 1;
	client -> packet.hops = 0;
	client -> packet.xid = client -> xid;
	client -> packet.secs = 0; /* Filled in by send_request. */
	if (can_receive_unicast_unconfigured (client -> interface))
		client -> packet.flags = 0;
	else
		client -> packet.flags = htons (BOOTP_BROADCAST);

	/* ciaddr must always be zero. */
	memset (&client -> packet.ciaddr, 0,
		sizeof client -> packet.ciaddr);
	memset (&client -> packet.yiaddr, 0,
		sizeof client -> packet.yiaddr);
	memset (&client -> packet.siaddr, 0,
		sizeof client -> packet.siaddr);
	client -> packet.giaddr = giaddr;
	memcpy (client -> packet.chaddr,
		&client -> interface -> hw_address.hbuf [1],
		client -> interface -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_raw ((unsigned char *)&client -> packet, client -> packet_length);
#endif
}

void make_release (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	unsigned char request = DHCPRELEASE;
	struct option_cache *oc;

	struct option_state *options = (struct option_state *)0;

	memset (&client -> packet, 0, sizeof (client -> packet));

	oc = lookup_option (&dhcp_universe, lease -> options,
			    DHO_DHCP_SERVER_IDENTIFIER);
	make_client_options(client, lease, &request, oc, NULL, NULL, &options);

	/* Set up the option buffer... */
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet,
			      (struct lease *)0, client,
			      /* maximum packet size */1500,
			      (struct option_state *)0,
			      options,
			      /* scope */ &global_scope,
			      /* overload */ 0,
			      /* terminate */0,
			      /* bootpp    */0,
			      (struct data_string *)0,
			      client -> config -> vendor_space_name);

	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;
	option_state_dereference (&options, MDL);

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.hbuf [0];
	/* Assumes hw_address is known, otherwise a random value may result */
	client -> packet.hlen = client -> interface -> hw_address.hlen - 1;
	client -> packet.hops = 0;
	client -> packet.xid = random ();
	client -> packet.secs = 0;
	client -> packet.flags = 0;
	memcpy (&client -> packet.ciaddr,
		lease -> address.iabuf, lease -> address.len);
	memset (&client -> packet.yiaddr, 0,
		sizeof client -> packet.yiaddr);
	memset (&client -> packet.siaddr, 0,
		sizeof client -> packet.siaddr);
	client -> packet.giaddr = giaddr;
	memcpy (client -> packet.chaddr,
		&client -> interface -> hw_address.hbuf [1],
		client -> interface -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_raw ((unsigned char *)&client -> packet, client -> packet_length);
#endif
}

void destroy_client_lease (lease)
	struct client_lease *lease;
{
	if (lease -> server_name)
		dfree (lease -> server_name, MDL);
	if (lease -> filename)
		dfree (lease -> filename, MDL);
	option_state_dereference (&lease -> options, MDL);
	free_client_lease (lease, MDL);
}

FILE *leaseFile = NULL;
int leases_written = 0;

void rewrite_client_leases ()
{
	struct interface_info *ip;
	struct client_state *client;
	struct client_lease *lp;

	if (leaseFile != NULL)
		fclose (leaseFile);
	leaseFile = fopen (path_dhclient_db, "w");
	if (leaseFile == NULL) {
		log_error ("can't create %s: %m", path_dhclient_db);
		return;
	}

	/* If there is a default duid, write it out. */
	if (default_duid.len != 0)
		write_duid(&default_duid);

	/* Write out all the leases attached to configured interfaces that
	   we know about. */
	for (ip = interfaces; ip; ip = ip -> next) {
		for (client = ip -> client; client; client = client -> next) {
			for (lp = client -> leases; lp; lp = lp -> next) {
				write_client_lease (client, lp, 1, 0);
			}
			if (client -> active)
				write_client_lease (client,
						    client -> active, 1, 0);

			if (client->active_lease != NULL)
				write_client6_lease(client,
						    client->active_lease,
						    1, 0);

			/* Reset last_write after rewrites. */
			client->last_write = 0;
		}
	}

	/* Write out any leases that are attached to interfaces that aren't
	   currently configured. */
	for (ip = dummy_interfaces; ip; ip = ip -> next) {
		for (client = ip -> client; client; client = client -> next) {
			for (lp = client -> leases; lp; lp = lp -> next) {
				write_client_lease (client, lp, 1, 0);
			}
			if (client -> active)
				write_client_lease (client,
						    client -> active, 1, 0);

			if (client->active_lease != NULL)
				write_client6_lease(client,
						    client->active_lease,
						    1, 0);

			/* Reset last_write after rewrites. */
			client->last_write = 0;
		}
	}
	fflush (leaseFile);
}

void write_lease_option (struct option_cache *oc,
			 struct packet *packet, struct lease *lease,
			 struct client_state *client_state,
			 struct option_state *in_options,
			 struct option_state *cfg_options,
			 struct binding_scope **scope,
			 struct universe *u, void *stuff)
{
	const char *name, *dot;
	struct data_string ds;
	char *preamble = stuff;

	memset (&ds, 0, sizeof ds);

	if (u != &dhcp_universe) {
		name = u -> name;
		dot = ".";
	} else {
		name = "";
		dot = "";
	}
	if (evaluate_option_cache (&ds, packet, lease, client_state,
				   in_options, cfg_options, scope, oc, MDL)) {
		/* The option name */
		fprintf(leaseFile, "%soption %s%s%s", preamble,
			name, dot, oc->option->name);

		/* The option value if there is one */
		if ((oc->option->format == NULL) ||
		    (oc->option->format[0] != 'Z')) {
			fprintf(leaseFile, " %s",
				pretty_print_option(oc->option, ds.data,
						    ds.len, 1, 1));
		}

		/* The closing semi-colon and newline */
		fprintf(leaseFile, ";\n");

		data_string_forget (&ds, MDL);
	}
}

/* Write an option cache to the lease store. */
static void
write_options(struct client_state *client, struct option_state *options,
	      const char *preamble)
{
	int i;

	for (i = 0; i < options->universe_count; i++) {
		option_space_foreach(NULL, NULL, client, NULL, options,
				     &global_scope, universes[i],
				     (char *)preamble, write_lease_option);
	}
}

/*
 * The "best" default DUID, since we cannot predict any information
 * about the system (such as whether or not the hardware addresses are
 * integrated into the motherboard or similar), is the "LLT", link local
 * plus time, DUID. For real stateless "LL" is better.
 *
 * Once generated, this duid is stored into the state database, and
 * retained across restarts.
 *
 * For the time being, there is probably a different state database for
 * every daemon, so this winds up being a per-interface identifier...which
 * is not how it is intended.  Upcoming rearchitecting the client should
 * address this "one daemon model."
 */
void
form_duid(struct data_string *duid, const char *file, int line)
{
	struct interface_info *ip;
	int len;
	char *str;

	/* For now, just use the first interface on the list. */
	ip = interfaces;

	if (ip == NULL)
		log_fatal("Impossible condition at %s:%d.", MDL);

	if ((ip->hw_address.hlen == 0) ||
	    (ip->hw_address.hlen > sizeof(ip->hw_address.hbuf)))
		log_fatal("Impossible hardware address length at %s:%d.", MDL);

	if (duid_type == 0)
		duid_type = stateless ? DUID_LL : DUID_LLT;

	/*
	 * 2 bytes for the 'duid type' field.
	 * 2 bytes for the 'htype' field.
	 * (DUID_LLT) 4 bytes for the 'current time'.
	 * enough bytes for the hardware address (note that hw_address has
	 * the 'htype' on byte zero).
	 */
	len = 4 + (ip->hw_address.hlen - 1);
	if (duid_type == DUID_LLT)
		len += 4;
	if (!buffer_allocate(&duid->buffer, len, MDL))
		log_fatal("no memory for default DUID!");
	duid->data = duid->buffer->data;
	duid->len = len;

	/* Basic Link Local Address type of DUID. */
	if (duid_type == DUID_LLT) {
		putUShort(duid->buffer->data, DUID_LLT);
		putUShort(duid->buffer->data + 2, ip->hw_address.hbuf[0]);
		putULong(duid->buffer->data + 4, cur_time - DUID_TIME_EPOCH);
		memcpy(duid->buffer->data + 8, ip->hw_address.hbuf + 1,
		       ip->hw_address.hlen - 1);
	} else {
		putUShort(duid->buffer->data, DUID_LL);
		putUShort(duid->buffer->data + 2, ip->hw_address.hbuf[0]);
		memcpy(duid->buffer->data + 4, ip->hw_address.hbuf + 1,
		       ip->hw_address.hlen - 1);
	}

	/* Now format the output based on lease-id-format */
	str = format_lease_id(duid->data, duid->len,
			      top_level_config.lease_id_format, MDL);
	if (str == NULL) {
		log_info("form_duid: Couldn't allocate memory to log duid!");
	} else {
		log_info("Created duid %s.", str);
		dfree(str, MDL);
	}
}

/* Write the default DUID to the lease store. */
static isc_result_t
write_duid(struct data_string *duid)
{
	char *str;
	int stat;

	if ((duid == NULL) || (duid->len <= 2))
		return DHCP_R_INVALIDARG;

	if (leaseFile == NULL) {	/* XXX? */
		leaseFile = fopen(path_dhclient_db, "w");
		if (leaseFile == NULL) {
			log_error("can't create %s: %m", path_dhclient_db);
			return ISC_R_IOERROR;
		}
	}

	/* Generate a formatted duid string per lease-id-format */
	str = format_lease_id(duid->data, duid->len,
			      top_level_config.lease_id_format, MDL);
	if (str == NULL)
		return ISC_R_NOMEMORY;

	stat = fprintf(leaseFile, "default-duid %s;\n", str);
	dfree(str, MDL);
	if (stat <= 0)
		return ISC_R_IOERROR;

	if (fflush(leaseFile) != 0)
		return ISC_R_IOERROR;

	return ISC_R_SUCCESS;
}

/* Write a DHCPv6 lease to the store. */
isc_result_t
write_client6_lease(struct client_state *client, struct dhc6_lease *lease,
		    int rewrite, int sync)
{
	struct dhc6_ia *ia;
	struct dhc6_addr *addr;
	int stat;
	const char *ianame;

	/* This should include the current lease. */
	if (!rewrite && (leases_written++ > 20)) {
		rewrite_client_leases();
		leases_written = 0;
		return ISC_R_SUCCESS;
	}

	if (client == NULL || lease == NULL)
		return DHCP_R_INVALIDARG;

	if (leaseFile == NULL) {	/* XXX? */
		leaseFile = fopen(path_dhclient_db, "w");
		if (leaseFile == NULL) {
			log_error("can't create %s: %m", path_dhclient_db);
			return ISC_R_IOERROR;
		}
	}

	stat = fprintf(leaseFile, "lease6 {\n");
	if (stat <= 0)
		return ISC_R_IOERROR;

	stat = fprintf(leaseFile, "  interface \"%s\";\n",
		       client->interface->name);
	if (stat <= 0)
		return ISC_R_IOERROR;

	for (ia = lease->bindings ; ia != NULL ; ia = ia->next) {
		switch (ia->ia_type) {
			case D6O_IA_NA:
			default:
				ianame = "ia-na";
				break;
			case D6O_IA_TA:
				ianame = "ia-ta";
				break;
			case D6O_IA_PD:
				ianame = "ia-pd";
				break;
		}

		/* For some reason IAID was never octal or hex, but string or
		 * hex. Go figure.  So for compatibilty's sake we will either
		 * do hex or "legacy" i.e string rather than octal. What a
		 * cluster. */
		switch(top_level_config.lease_id_format) {
			case TOKEN_HEX: {
				char* iaid_str = format_lease_id(
					(const unsigned char *) &ia->iaid, 4,
					top_level_config.lease_id_format, MDL);

				if (!iaid_str) {
					log_error("Can't format iaid");
					return ISC_R_IOERROR;
				}

				stat = fprintf(leaseFile, "  %s %s {\n",
					       ianame, iaid_str);
				dfree(iaid_str, MDL);
				break;
			}

			case TOKEN_OCTAL:
			default:
				stat = fprintf(leaseFile, "  %s %s {\n", ianame,
					       print_hex_1(4, ia->iaid, 12));
				break;
		}

		if (stat <= 0)
			return ISC_R_IOERROR;

		if (ia->ia_type != D6O_IA_TA)
			stat = fprintf(leaseFile, "    starts %d;\n"
						  "    renew %u;\n"
						  "    rebind %u;\n",
				       (int)ia->starts, ia->renew, ia->rebind);
		else
			stat = fprintf(leaseFile, "    starts %d;\n",
				       (int)ia->starts);
		if (stat <= 0)
			return ISC_R_IOERROR;

		for (addr = ia->addrs ; addr != NULL ; addr = addr->next) {
			if (ia->ia_type != D6O_IA_PD)
				stat = fprintf(leaseFile,
					       "    iaaddr %s {\n",
					       piaddr(addr->address));
			else
				stat = fprintf(leaseFile,
					       "    iaprefix %s/%d {\n",
					       piaddr(addr->address),
					       (int)addr->plen);
			if (stat <= 0)
				return ISC_R_IOERROR;

			stat = fprintf(leaseFile, "      starts %d;\n"
						  "      preferred-life %u;\n"
						  "      max-life %u;\n",
				       (int)addr->starts, addr->preferred_life,
				       addr->max_life);
			if (stat <= 0)
				return ISC_R_IOERROR;

			if (addr->options != NULL)
				write_options(client, addr->options, "      ");

			stat = fprintf(leaseFile, "    }\n");
			if (stat <= 0)
				return ISC_R_IOERROR;
		}

		if (ia->options != NULL)
			write_options(client, ia->options, "    ");

		stat = fprintf(leaseFile, "  }\n");
		if (stat <= 0)
			return ISC_R_IOERROR;
	}

	if (lease->released) {
		stat = fprintf(leaseFile, "  released;\n");
		if (stat <= 0)
			return ISC_R_IOERROR;
	}

	if (lease->options != NULL)
		write_options(client, lease->options, "  ");

	stat = fprintf(leaseFile, "}\n");
	if (stat <= 0)
		return ISC_R_IOERROR;

	if (fflush(leaseFile) != 0)
		return ISC_R_IOERROR;

	if (sync) {
		if (fsync(fileno(leaseFile)) < 0) {
			log_error("write_client_lease: fsync(): %m");
			return ISC_R_IOERROR;
		}
	}

	return ISC_R_SUCCESS;
}

int write_client_lease (client, lease, rewrite, makesure)
	struct client_state *client;
	struct client_lease *lease;
	int rewrite;
	int makesure;
{
	struct data_string ds;
	int errors = 0;
	char *s;
	const char *tval;

	if (!rewrite) {
		if (leases_written++ > 20) {
			rewrite_client_leases ();
			leases_written = 0;
		}
	}

	/* If the lease came from the config file, we don't need to stash
	   a copy in the lease database. */
	if (lease -> is_static)
		return 1;

	if (leaseFile == NULL) {	/* XXX */
		leaseFile = fopen (path_dhclient_db, "w");
		if (leaseFile == NULL) {
			log_error ("can't create %s: %m", path_dhclient_db);
			return 0;
		}
	}

	errno = 0;
	fprintf (leaseFile, "lease {\n");
	if (lease -> is_bootp) {
		fprintf (leaseFile, "  bootp;\n");
		if (errno) {
			++errors;
			errno = 0;
		}
	}
	fprintf (leaseFile, "  interface \"%s\";\n",
		 client -> interface -> name);
	if (errno) {
		++errors;
		errno = 0;
	}
	if (client -> name) {
		fprintf (leaseFile, "  name \"%s\";\n", client -> name);
		if (errno) {
			++errors;
			errno = 0;
		}
	}
	fprintf (leaseFile, "  fixed-address %s;\n",
		 piaddr (lease -> address));
	if (errno) {
		++errors;
		errno = 0;
	}
	if (lease -> filename) {
		s = quotify_string (lease -> filename, MDL);
		if (s) {
			fprintf (leaseFile, "  filename \"%s\";\n", s);
			if (errno) {
				++errors;
				errno = 0;
			}
			dfree (s, MDL);
		} else
			errors++;

	}
	if (lease->server_name != NULL) {
		s = quotify_string(lease->server_name, MDL);
		if (s != NULL) {
			fprintf(leaseFile, "  server-name \"%s\";\n", s);
			if (errno) {
				++errors;
				errno = 0;
			}
			dfree(s, MDL);
		} else
			++errors;
	}
	if (lease -> medium) {
		s = quotify_string (lease -> medium -> string, MDL);
		if (s) {
			fprintf (leaseFile, "  medium \"%s\";\n", s);
			if (errno) {
				++errors;
				errno = 0;
			}
			dfree (s, MDL);
		} else
			errors++;
	}
	if (errno != 0) {
		errors++;
		errno = 0;
	}

	memset (&ds, 0, sizeof ds);

	write_options(client, lease->options, "  ");

	tval = print_time(lease->renewal);
	if (tval == NULL ||
	    fprintf(leaseFile, "  renew %s\n", tval) < 0)
		errors++;

	tval = print_time(lease->rebind);
	if (tval == NULL ||
	    fprintf(leaseFile, "  rebind %s\n", tval) < 0)
		errors++;

	tval = print_time(lease->expiry);
	if (tval == NULL ||
	    fprintf(leaseFile, "  expire %s\n", tval) < 0)
		errors++;

	if (fprintf(leaseFile, "}\n") < 0)
		errors++;

	if (fflush(leaseFile) != 0)
		errors++;

	client->last_write = cur_time;

	if (!errors && makesure) {
		if (fsync (fileno (leaseFile)) < 0) {
			log_info ("write_client_lease: %m");
			return 0;
		}
	}

	return errors ? 0 : 1;
}

/* Variables holding name of script and file pointer for writing to
   script.   Needless to say, this is not reentrant - only one script
   can be invoked at a time. */
char scriptName [256];
FILE *scriptFile;

/**
 * @brief Initializes basic variables for a script
 *
 * This function is called as an initial preparation for calling a script.
 * It sets up a number of common env. variables that will be passed to
 * the script. For actual script calling, see @ref script_go .
 *
 * @param client variables will be stored here (if null, the whole function
 *               is no-op)
 * @param reason specified the reason for calling a script (must be non-null)
 * @param medium if specified, defines medium type (may be null)
 */
void script_init(struct client_state *client, const char *reason,
                 struct string_list *medium)
{
	struct string_list *sl, *next;

	if (client) {
		for (sl = client -> env; sl; sl = next) {
			next = sl -> next;
			dfree (sl, MDL);
		}
		client -> env = (struct string_list *)0;
		client -> envc = 0;

		if (client -> interface) {
			client_envadd (client, "", "interface", "%s",
				       client -> interface -> name);
		}
		if (client -> name)
			client_envadd (client,
				       "", "client", "%s", client -> name);
		if (medium)
			client_envadd (client,
				       "", "medium", "%s", medium -> string);

		client_envadd (client, "", "reason", "%s", reason);
		client_envadd (client, "", "pid", "%ld", (long int)getpid ());
#if defined(DHCPv6)
		client_envadd (client, "", "dad_wait_time", "%ld",
					   (long int)dad_wait_time);
#endif
	}
}

void client_option_envadd (struct option_cache *oc,
			   struct packet *packet, struct lease *lease,
			   struct client_state *client_state,
			   struct option_state *in_options,
			   struct option_state *cfg_options,
			   struct binding_scope **scope,
			   struct universe *u, void *stuff)
{
	struct envadd_state *es = stuff;
	struct data_string data;
	memset (&data, 0, sizeof data);

	if (evaluate_option_cache (&data, packet, lease, client_state,
				   in_options, cfg_options, scope, oc, MDL)) {
		if (data.len) {
			char name [256];
			if (dhcp_option_ev_name (name, sizeof name,
						 oc->option)) {
				const char *value;
				size_t length;
				value = pretty_print_option(oc->option,
							    data.data,
							    data.len, 0, 0);
				length = strlen(value);

				if (check_option_values(oc->option->universe,
							oc->option->code,
							value, length) == 0) {
					client_envadd(es->client, es->prefix,
						      name, "%s", value);
				} else {
					log_error("suspect value in %s "
						  "option - discarded",
						  name);
				}
			}
		}

		data_string_forget (&data, MDL);
	}
}

/**
 * @brief Adds parameters to environment variables for a script
 *
 * This function add details of specified lease to a list of env. variables
 * to be passed to a script. The lease details will be prepended with
 * specified prefix (e.g. "old_") and added to the list stored in client.
 * Following variables may be set:
 * - ip_address
 * - next_server
 * - network_number
 * - broadcast_address
 * - filename
 * - server_name
 * - expiry
 *
 * @param client env. variables will be stored here
 * @param prefix textual prefix to be added to each variable (e.g. "old_")
 * @param lease lease details will be extracted from here
 */
void script_write_params(struct client_state *client, const char *prefix,
			 struct client_lease *lease)
{
	int i;
	struct data_string data;
	struct option_cache *oc;
	struct envadd_state es;

	es.client = client;
	es.prefix = prefix;

	client_envadd (client,
		       prefix, "ip_address", "%s", piaddr (lease -> address));

	/* If we've set the next server address in the lease structure
	   put it into an environment variable for the script */
	if (lease->next_srv_addr.len != 0) {
		client_envadd(client, prefix, "next_server", "%s",
			      piaddr(lease->next_srv_addr));
	}

	/* For the benefit of Linux (and operating systems which may
	   have similar needs), compute the network address based on
	   the supplied ip address and netmask, if provided.  Also
	   compute the broadcast address (the host address all ones
	   broadcast address, not the host address all zeroes
	   broadcast address). */

	memset (&data, 0, sizeof data);
	oc = lookup_option (&dhcp_universe, lease -> options, DHO_SUBNET_MASK);
	if (oc && evaluate_option_cache (&data, (struct packet *)0,
					 (struct lease *)0, client,
					 (struct option_state *)0,
					 lease -> options,
					 &global_scope, oc, MDL)) {
		if (data.len > 3) {
			struct iaddr netmask, subnet, broadcast;

			/*
			 * No matter the length of the subnet-mask option,
			 * use only the first four octets.  Note that
			 * subnet-mask options longer than 4 octets are not
			 * in conformance with RFC 2132, but servers with this
			 * flaw do exist.
			 */
			memcpy(netmask.iabuf, data.data, 4);
			netmask.len = 4;
			data_string_forget (&data, MDL);

			subnet = subnet_number (lease -> address, netmask);
			if (subnet.len) {
			    client_envadd (client, prefix, "network_number",
					   "%s", piaddr (subnet));

			    oc = lookup_option (&dhcp_universe,
						lease -> options,
						DHO_BROADCAST_ADDRESS);
			    if (!oc ||
				!(evaluate_option_cache
				  (&data, (struct packet *)0,
				   (struct lease *)0, client,
				   (struct option_state *)0,
				   lease -> options,
				   &global_scope, oc, MDL))) {
				broadcast = broadcast_addr (subnet, netmask);
				if (broadcast.len) {
				    client_envadd (client,
						   prefix, "broadcast_address",
						   "%s", piaddr (broadcast));
				}
			    }
			}
		}
		data_string_forget (&data, MDL);
	}

	if (lease->filename) {
		if (check_option_values(NULL, DHO_ROOT_PATH,
					lease->filename,
					strlen(lease->filename)) == 0) {
			client_envadd(client, prefix, "filename",
				      "%s", lease->filename);
		} else {
			log_error("suspect value in %s "
				  "option - discarded",
				  lease->filename);
		}
	}

	if (lease->server_name) {
		if (check_option_values(NULL, DHO_HOST_NAME,
					lease->server_name,
					strlen(lease->server_name)) == 0 ) {
			client_envadd (client, prefix, "server_name",
				       "%s", lease->server_name);
		} else {
			log_error("suspect value in %s "
				  "option - discarded",
				  lease->server_name);
		}
	}

	for (i = 0; i < lease -> options -> universe_count; i++) {
		option_space_foreach ((struct packet *)0, (struct lease *)0,
				      client, (struct option_state *)0,
				      lease -> options, &global_scope,
				      universes [i],
				      &es, client_option_envadd);
	}

	client_envadd (client, prefix, "expiry", "%lu",
		       (unsigned long)(lease -> expiry));
}

/**
 * @brief Write out the environent variable the client requested.
 * Write out the environment variables for the objects that the
 * client requested.  If the object was requested the variable will be:
 * requested_<option_name>=1
 * If it wasn't requested there won't be a variable.
 *
 * @param client client structure
 */
void script_write_requested(struct client_state *client)
{
	int i;
	struct option **req;
	char name[256];
	req = client->config->requested_options;

	if (req == NULL)
		return;

	for (i = 0 ; req[i] != NULL ; i++) {
		if ((req[i]->universe == &dhcp_universe) &&
		    dhcp_option_ev_name(name, sizeof(name), req[i])) {
			client_envadd(client, "requested_", name, "%d", 1);
		}
	}
}

/**
 * @brief Calls external script.
 *
 * External script is specified either using -sf command line or
 * script parameter in the configuration file.
 *
 * @param client specifies client information (environment variables,
 *        and other parameters will be extracted and passed to the script.
 * @return If positive, it contains exit code of the process running script.
 *         If negative, returns the signal number that cause the script process
 *         to terminate.
 */
int script_go(struct client_state *client)
{
	char *scriptName;
	char *argv [2];
	char **envp;
	char reason [] = "REASON=NBI";
	static char client_path [] = CLIENT_PATH;
	int i;
	struct string_list *sp, *next;
	int pid, wpid, wstatus;

	if (client)
		scriptName = client -> config -> script_name;
	else
		scriptName = top_level_config.script_name;

	envp = dmalloc (((client ? client -> envc : 2) +
			 client_env_count + 2) * sizeof (char *), MDL);
	if (!envp) {
		log_error ("No memory for client script environment.");
		return 0;
	}
	i = 0;
	/* Copy out the environment specified on the command line,
	   if any. */
	for (sp = client_env; sp; sp = sp -> next) {
		envp [i++] = sp -> string;
	}
	/* Copy out the environment specified by dhclient. */
	if (client) {
		for (sp = client -> env; sp; sp = sp -> next) {
			envp [i++] = sp -> string;
		}
	} else {
		envp [i++] = reason;
	}
	/* Set $PATH. */
	envp [i++] = client_path;
	envp [i] = (char *)0;

	argv [0] = scriptName;
	argv [1] = (char *)0;

	pid = fork ();
	if (pid < 0) {
		log_error ("fork: %m");
		wstatus = 0;
	} else if (pid) {
		do {
			wpid = wait (&wstatus);
		} while (wpid != pid && wpid > 0);
		if (wpid < 0) {
			log_error ("wait: %m");
			wstatus = 0;
		}
	} else {
		/* We don't want to pass an open file descriptor for
		 * dhclient.leases when executing dhclient-script.
		 */
		if (leaseFile != NULL)
			fclose(leaseFile);
		execve (scriptName, argv, envp);
		log_error ("execve (%s, ...): %m", scriptName);
		exit (0);
	}

	if (client) {
		for (sp = client -> env; sp; sp = next) {
			next = sp -> next;
			dfree (sp, MDL);
		}
		client -> env = (struct string_list *)0;
		client -> envc = 0;
	}
	dfree (envp, MDL);
	gettimeofday(&cur_tv, NULL);
	return (WIFEXITED (wstatus) ?
		WEXITSTATUS (wstatus) : -WTERMSIG (wstatus));
}

void client_envadd (struct client_state *client,
		    const char *prefix, const char *name, const char *fmt, ...)
{
	char spbuf [1024];
	char *s;
	unsigned len;
	struct string_list *val;
	va_list list;

	va_start (list, fmt);
	len = vsnprintf (spbuf, sizeof spbuf, fmt, list);
	va_end (list);

	val = dmalloc (strlen (prefix) + strlen (name) + 1 /* = */ +
		       len + sizeof *val, MDL);
	if (!val) {
		log_error ("client_envadd: cannot allocate space for variable");
		return;
	}

	s = val -> string;
	strcpy (s, prefix);
	strcat (s, name);
	s += strlen (s);
	*s++ = '=';
	if (len >= sizeof spbuf) {
		va_start (list, fmt);
		vsnprintf (s, len + 1, fmt, list);
		va_end (list);
	} else {
		strcpy (s, spbuf);
	}

	val -> next = client -> env;
	client -> env = val;
	client -> envc++;
}

int dhcp_option_ev_name (buf, buflen, option)
	char *buf;
	size_t buflen;
	struct option *option;
{
	int i, j;
	const char *s;

	j = 0;
	if (option -> universe != &dhcp_universe) {
		s = option -> universe -> name;
		i = 0;
	} else {
		s = option -> name;
		i = 1;
	}

	do {
		while (*s) {
			if (j + 1 == buflen)
				return 0;
			if (*s == '-')
				buf [j++] = '_';
			else
				buf [j++] = *s;
			++s;
		}
		if (!i) {
			s = option -> name;
			if (j + 1 == buflen)
				return 0;
			buf [j++] = '_';
		}
		++i;
	} while (i != 2);

	buf [j] = 0;
	return 1;
}

void finish (char ret)
{
	if (no_daemon || dfd[0] == -1 || dfd[1] == -1)
		exit((int)ret);
	if (write(dfd[1], &ret, 1) != 1)
		log_fatal("write to parent: %m");
	(void) close(dfd[1]);
	dfd[0] = dfd[1] = -1;
	exit((int)ret);
}

void detach ()
{
	char buf = 0;

	/* Don't become a daemon if the user requested otherwise. */
	if (no_daemon) {
		write_client_pid_file ();
		return;
	}

	/* Only do it once. */
	if (dfd[0] == -1 || dfd[1] == -1)
		return;

	/* Signal parent we started successfully. */
	if (write(dfd[1], &buf, 1) != 1)
		log_fatal("write to parent: %m");
	(void) close(dfd[1]);
	dfd[0] = dfd[1] = -1;

	/* Stop logging to stderr... */
	log_perror = 0;

	/* Become session leader and get pid... */
	(void) setsid ();

	/* Close standard I/O descriptors. */
	(void) close(0);
	(void) close(1);
	(void) close(2);

	/* Reopen them on /dev/null. */
	(void) open("/dev/null", O_RDWR);
	(void) open("/dev/null", O_RDWR);
	(void) open("/dev/null", O_RDWR);

	write_client_pid_file ();

	IGNORE_RET (chdir("/"));

}

void write_client_pid_file ()
{
	FILE *pf;
	int pfdesc;

	/* nothing to do if the user doesn't want a pid file */
	if (no_pid_file == ISC_TRUE) {
		return;
	}

	pfdesc = open (path_dhclient_pid, O_CREAT | O_TRUNC | O_WRONLY, 0644);

	if (pfdesc < 0) {
		log_error ("Can't create %s: %m", path_dhclient_pid);
		return;
	}

	pf = fdopen (pfdesc, "w");
	if (!pf) {
		close(pfdesc);
		log_error ("Can't fdopen %s: %m", path_dhclient_pid);
	} else {
		fprintf (pf, "%ld\n", (long)getpid ());
		fclose (pf);
	}
}

void client_location_changed ()
{
	struct interface_info *ip;
	struct client_state *client;

	for (ip = interfaces; ip; ip = ip -> next) {
		for (client = ip -> client; client; client = client -> next) {
			switch (client -> state) {
			      case S_SELECTING:
				cancel_timeout (send_discover, client);
				break;

			      case S_BOUND:
				cancel_timeout (state_bound, client);
				break;

			      case S_REBOOTING:
			      case S_REQUESTING:
			      case S_RENEWING:
				cancel_timeout (send_request, client);
				break;

			      case S_INIT:
			      case S_REBINDING:
			      case S_STOPPED:
			      case S_DECLINING:
				break;
			}
			client -> state = S_INIT;
			state_reboot (client);
		}
	}
}

void do_release(client)
	struct client_state *client;
{
	struct data_string ds;
	struct option_cache *oc;

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6 && (dhcp4o6_state <= 0)) {
		if (dhcp4o6_state < 0)
			dhcp4o6_poll(NULL);
		client->pending = P_RELEASE;
		return;
	}
#endif

	/* Pick a random xid. */
	client -> xid = random ();

	/* is there even a lease to release? */
	if (client -> active) {
		/* Make a DHCPRELEASE packet, and set appropriate per-interface
		   flags. */
		make_release (client, client -> active);

		memset (&ds, 0, sizeof ds);
		oc = lookup_option (&dhcp_universe,
				    client -> active -> options,
				    DHO_DHCP_SERVER_IDENTIFIER);
		if (oc &&
		    evaluate_option_cache (&ds, (struct packet *)0,
					   (struct lease *)0, client,
					   (struct option_state *)0,
					   client -> active -> options,
					   &global_scope, oc, MDL)) {
			if (ds.len > 3) {
				memcpy (client -> destination.iabuf,
					ds.data, 4);
				client -> destination.len = 4;
			} else
				client -> destination = iaddr_broadcast;

			data_string_forget (&ds, MDL);
		} else
			client -> destination = iaddr_broadcast;
		client -> first_sending = cur_time;
		client -> interval = client -> config -> initial_interval;

		/* Zap the medium list... */
		client -> medium = (struct string_list *)0;

		/* Send out the first and only DHCPRELEASE packet. */
		send_release (client);

		/* Do the client script RELEASE operation. */
		script_init (client,
			     "RELEASE", (struct string_list *)0);
		if (client -> alias)
			script_write_params(client, "alias_",
					    client -> alias);
		script_write_params(client, "old_", client -> active);
		script_write_requested(client);
		script_go(client);
	}

	/* Cancel any timeouts. */
	cancel_timeout (state_bound, client);
	cancel_timeout (send_discover, client);
	cancel_timeout (state_init, client);
	cancel_timeout (send_request, client);
	cancel_timeout (state_reboot, client);
	client -> state = S_STOPPED;

#if defined(DHCPv6) && defined(DHCP4o6)
	if (dhcpv4_over_dhcpv6)
		finish(0);
#endif
}

int dhclient_interface_shutdown_hook (struct interface_info *interface)
{
	do_release (interface -> client);

	return 1;
}

int dhclient_interface_discovery_hook (struct interface_info *tmp)
{
	struct interface_info *last, *ip;
	/* See if we can find the client from dummy_interfaces */
	last = 0;
	for (ip = dummy_interfaces; ip; ip = ip -> next) {
		if (!strcmp (ip -> name, tmp -> name)) {
			/* Remove from dummy_interfaces */
			if (last) {
				ip = (struct interface_info *)0;
				interface_reference (&ip, last -> next, MDL);
				interface_dereference (&last -> next, MDL);
				if (ip -> next) {
					interface_reference (&last -> next,
							     ip -> next, MDL);
					interface_dereference (&ip -> next,
							       MDL);
				}
			} else {
				ip = (struct interface_info *)0;
				interface_reference (&ip,
						     dummy_interfaces, MDL);
				interface_dereference (&dummy_interfaces, MDL);
				if (ip -> next) {
					interface_reference (&dummy_interfaces,
							     ip -> next, MDL);
					interface_dereference (&ip -> next,
							       MDL);
				}
			}
			/* Copy "client" to tmp */
			if (ip -> client) {
				tmp -> client = ip -> client;
				tmp -> client -> interface = tmp;
			}
			interface_dereference (&ip, MDL);
			break;
		}
		last = ip;
	}
	return 1;
}

isc_result_t dhclient_interface_startup_hook (struct interface_info *interface)
{
	struct interface_info *ip;
	struct client_state *client;

	/* This code needs some rethinking.   It doesn't test against
	   a signal name, and it just kind of bulls into doing something
	   that may or may not be appropriate. */

	if (interfaces) {
		interface_reference (&interface -> next, interfaces, MDL);
		interface_dereference (&interfaces, MDL);
	}
	interface_reference (&interfaces, interface, MDL);

	discover_interfaces (DISCOVER_UNCONFIGURED);

	for (ip = interfaces; ip; ip = ip -> next) {
		/* If interfaces were specified, don't configure
		   interfaces that weren't specified! */
		if (ip -> flags & INTERFACE_RUNNING ||
		   (ip -> flags & (INTERFACE_REQUESTED |
				     INTERFACE_AUTOMATIC)) !=
		     INTERFACE_REQUESTED)
			continue;
		script_init (ip -> client,
			     "PREINIT", (struct string_list *)0);
		if (ip -> client -> alias)
			script_write_params(ip -> client, "alias_",
					    ip -> client -> alias);
		script_go(ip -> client);
	}

	discover_interfaces (interfaces_requested != 0
			     ? DISCOVER_REQUESTED
			     : DISCOVER_RUNNING);

	for (ip = interfaces; ip; ip = ip -> next) {
		if (ip -> flags & INTERFACE_RUNNING)
			continue;
		ip -> flags |= INTERFACE_RUNNING;
		for (client = ip->client ; client ; client = client->next) {
			client->state = S_INIT;
			state_reboot(client);
		}
	}
	return ISC_R_SUCCESS;
}

/* The client should never receive a relay agent information option,
   so if it does, log it and discard it. */

int parse_agent_information_option (packet, len, data)
	struct packet *packet;
	int len;
	u_int8_t *data;
{
	return 1;
}

/* The client never sends relay agent information options. */

unsigned cons_agent_information_options (cfg_options, outpacket,
					 agentix, length)
	struct option_state *cfg_options;
	struct dhcp_packet *outpacket;
	unsigned agentix;
	unsigned length;
{
	return length;
}

static void shutdown_exit (void *foo)
{
	/* get rid of the pid if we can */
	if (no_pid_file == ISC_FALSE)
		(void) unlink(path_dhclient_pid);
	finish(0);
}

#if defined (NSUPDATE)
/*
 * If the first query fails, the updater MUST NOT delete the DNS name.  It
 * may be that the host whose lease on the server has expired has moved
 * to another network and obtained a lease from a different server,
 * which has caused the client's A RR to be replaced. It may also be
 * that some other client has been configured with a name that matches
 * the name of the DHCP client, and the policy was that the last client
 * to specify the name would get the name.  In this case, the DHCID RR
 * will no longer match the updater's notion of the client-identity of
 * the host pointed to by the DNS name.
 *   -- "Interaction between DHCP and DNS"
 */

/* The first and second stages are pretty similar so we combine them */
void
client_dns_remove_action(dhcp_ddns_cb_t *ddns_cb,
			 isc_result_t    eresult)
{

	isc_result_t result;

	if ((eresult == ISC_R_SUCCESS) &&
	    (ddns_cb->state == DDNS_STATE_REM_FW_YXDHCID)) {
		/* Do the second stage of the FWD removal */
		ddns_cb->state = DDNS_STATE_REM_FW_NXRR;

		result = ddns_modify_fwd(ddns_cb, MDL);
		if (result == ISC_R_SUCCESS) {
			return;
		}
	}

	/* If we are done or have an error clean up */
	dhclient_ddns_cb_free(ddns_cb, MDL);
	return;
}

void
client_dns_remove(struct client_state *client,
		  struct iaddr        *addr)
{
	dhcp_ddns_cb_t *ddns_cb;
	isc_result_t result;

	/* if we have an old ddns request for this client, cancel it */
	if (client->ddns_cb != NULL) {
		ddns_cancel(client->ddns_cb, MDL);
		client->ddns_cb = NULL;
	}

	ddns_cb = ddns_cb_alloc(MDL);
	if (ddns_cb != NULL) {
		ddns_cb->address = *addr;
		ddns_cb->timeout = 0;

		ddns_cb->state = DDNS_STATE_REM_FW_YXDHCID;
		ddns_cb->flags = DDNS_UPDATE_ADDR;
		ddns_cb->cur_func = client_dns_remove_action;

		result = client_dns_update(client, ddns_cb);

		if (result != ISC_R_TIMEDOUT) {
			dhclient_ddns_cb_free(ddns_cb, MDL);
		}
	}
}
#endif /* defined NSUPDATE */


isc_result_t dhcp_set_control_state (control_object_state_t oldstate,
				     control_object_state_t newstate)
{
	struct interface_info *ip;
	struct client_state *client;
	struct timeval tv;

	if (newstate == server_shutdown) {
		/* Re-entry */
		if (shutdown_signal == SIGUSR1)
			return ISC_R_SUCCESS;
		/* Log shutdown on signal. */
		if ((shutdown_signal == SIGINT) ||
		    (shutdown_signal == SIGTERM)) {
			log_info("Received signal %d, initiating shutdown.",
				 shutdown_signal);
		}
		/* Mark it was called. */
		shutdown_signal = SIGUSR1;
	}

	/* Do the right thing for each interface. */
	for (ip = interfaces; ip; ip = ip -> next) {
	    for (client = ip -> client; client; client = client -> next) {
		switch (newstate) {
		  case server_startup:
		    return ISC_R_SUCCESS;

		  case server_running:
		    return ISC_R_SUCCESS;

		  case server_shutdown:
		    if (client -> active &&
			client -> active -> expiry > cur_time) {
#if defined (NSUPDATE)
			    if (client->config->do_forward_update) {
				    client_dns_remove(client,
						      &client->active->address);
			    }
#endif /* defined NSUPDATE */

			    do_release (client);
		    }
		    break;

		  case server_hibernate:
		    state_stop (client);
		    break;

		  case server_awaken:
		    state_reboot (client);
		    break;
		}
	    }
	}

	if (newstate == server_shutdown) {
		tv.tv_sec = cur_tv.tv_sec;
		tv.tv_usec = cur_tv.tv_usec + 1;
		add_timeout(&tv, shutdown_exit, 0, 0, 0);
	}
	return ISC_R_SUCCESS;
}

#if defined (NSUPDATE)
/*
 * Called after a timeout if the DNS update failed on the previous try.
 * Starts the retry process.  If the retry times out it will schedule
 * this routine to run again after a 10x wait.
 */
void
client_dns_update_timeout (void *cp)
{
	dhcp_ddns_cb_t *ddns_cb = (dhcp_ddns_cb_t *)cp;
	struct client_state *client = (struct client_state *)ddns_cb->lease;
	isc_result_t status = ISC_R_FAILURE;

	if ((client != NULL) &&
	    ((client->active != NULL) ||
	     (client->active_lease != NULL)))
		status = client_dns_update(client, ddns_cb);

	/*
	 * A status of timedout indicates that we started the update and
	 * have released control of the control block.  Any other status
	 * indicates that we should clean up the control block.  We either
	 * got a success which indicates that we didn't really need to
	 * send an update or some other error in which case we weren't able
	 * to start the update process.  In both cases we still own
	 * the control block and should free it.
	 */
	if (status != ISC_R_TIMEDOUT) {
		dhclient_ddns_cb_free(ddns_cb, MDL);
	}
}

/*
 * If the first query succeeds, the updater can conclude that it
 * has added a new name whose only RRs are the A and DHCID RR records.
 * The A RR update is now complete (and a client updater is finished,
 * while a server might proceed to perform a PTR RR update).
 *   -- "Interaction between DHCP and DNS"
 *
 * If the second query succeeds, the updater can conclude that the current
 * client was the last client associated with the domain name, and that
 * the name now contains the updated A RR. The A RR update is now
 * complete (and a client updater is finished, while a server would
 * then proceed to perform a PTR RR update).
 *   -- "Interaction between DHCP and DNS"
 *
 * If the second query fails with NXRRSET, the updater must conclude
 * that the client's desired name is in use by another host.  At this
 * juncture, the updater can decide (based on some administrative
 * configuration outside of the scope of this document) whether to let
 * the existing owner of the name keep that name, and to (possibly)
 * perform some name disambiguation operation on behalf of the current
 * client, or to replace the RRs on the name with RRs that represent
 * the current client. If the configured policy allows replacement of
 * existing records, the updater submits a query that deletes the
 * existing A RR and the existing DHCID RR, adding A and DHCID RRs that
 * represent the IP address and client-identity of the new client.
 *   -- "Interaction between DHCP and DNS"
 */

/* The first and second stages are pretty similar so we combine them */
void
client_dns_update_action(dhcp_ddns_cb_t *ddns_cb,
			 isc_result_t    eresult)
{
	isc_result_t result;
	struct timeval tv;

	switch(eresult) {
	case ISC_R_SUCCESS:
	default:
		/* Either we succeeded or broke in a bad way, clean up */
		break;

	case DNS_R_YXRRSET:
		/*
		 * This is the only difference between the two stages,
		 * check to see if it is the first stage, in which case
		 * start the second stage
		 */
		if (ddns_cb->state == DDNS_STATE_ADD_FW_NXDOMAIN) {
			ddns_cb->state = DDNS_STATE_ADD_FW_YXDHCID;
			ddns_cb->cur_func = client_dns_update_action;

			result = ddns_modify_fwd(ddns_cb, MDL);
			if (result == ISC_R_SUCCESS) {
				return;
			}
		}
		break;

	case ISC_R_TIMEDOUT:
		/*
		 * We got a timeout response from the DNS module.  Schedule
		 * another attempt for later.  We forget the name, dhcid and
		 * zone so if it gets changed we will get the new information.
		 */
		data_string_forget(&ddns_cb->fwd_name, MDL);
		data_string_forget(&ddns_cb->dhcid, MDL);
		if (ddns_cb->zone != NULL) {
			forget_zone((struct dns_zone **)&ddns_cb->zone);
		}

		/* Reset to doing the first stage */
		ddns_cb->state    = DDNS_STATE_ADD_FW_NXDOMAIN;
		ddns_cb->cur_func = client_dns_update_action;

		/* and update our timer */
		if (ddns_cb->timeout < 3600)
			ddns_cb->timeout *= 10;
		tv.tv_sec = cur_tv.tv_sec + ddns_cb->timeout;
		tv.tv_usec = cur_tv.tv_usec;
		add_timeout(&tv, client_dns_update_timeout,
			    ddns_cb, NULL, NULL);
		return;
	}

	dhclient_ddns_cb_free(ddns_cb, MDL);
	return;
}

/* See if we should do a DNS update, and if so, do it. */

isc_result_t
client_dns_update(struct client_state *client, dhcp_ddns_cb_t *ddns_cb)
{
	struct data_string client_identifier;
	struct option_cache *oc;
	int ignorep;
	int result;
	int ddns_v4_type;
	isc_result_t rcode;

	/* If we didn't send an FQDN option, we certainly aren't going to
	   be doing an update. */
	if (!client -> sent_options)
		return ISC_R_SUCCESS;

	/* If we don't have a lease, we can't do an update. */
	if ((client->active == NULL) && (client->active_lease == NULL))
		return ISC_R_SUCCESS;

	/* If we set the no client update flag, don't do the update. */
	if ((oc = lookup_option (&fqdn_universe, client -> sent_options,
				  FQDN_NO_CLIENT_UPDATE)) &&
	    evaluate_boolean_option_cache (&ignorep, (struct packet *)0,
					   (struct lease *)0, client,
					   client -> sent_options,
					   (struct option_state *)0,
					   &global_scope, oc, MDL))
		return ISC_R_SUCCESS;

	/* If we set the "server, please update" flag, or didn't set it
	   to false, don't do the update. */
	if (!(oc = lookup_option (&fqdn_universe, client -> sent_options,
				  FQDN_SERVER_UPDATE)) ||
	    evaluate_boolean_option_cache (&ignorep, (struct packet *)0,
					   (struct lease *)0, client,
					   client -> sent_options,
					   (struct option_state *)0,
					   &global_scope, oc, MDL))
		return ISC_R_SUCCESS;

	/* If no FQDN option was supplied, don't do the update. */
	if (!(oc = lookup_option (&fqdn_universe, client -> sent_options,
				  FQDN_FQDN)) ||
	    !evaluate_option_cache (&ddns_cb->fwd_name, (struct packet *)0,
				    (struct lease *)0, client,
				    client -> sent_options,
				    (struct option_state *)0,
				    &global_scope, oc, MDL))
		return ISC_R_SUCCESS;

	/*
	 * Construct the DHCID value for use in the DDNS update process
	 * We have the newer standard version and the older interim version
	 * chosen by the '-I' option.  The interim version is left as is
	 * for backwards compatibility.  The standard version is based on
	 * RFC 4701 section 3.3
	 */

	result = 0;
	POST(result);
	memset(&client_identifier, 0, sizeof(client_identifier));

	if (std_dhcid == 1) {
		/* standard style */
		ddns_cb->dhcid_class = dns_rdatatype_dhcid;
		ddns_v4_type = 1;
	} else {
		/* interim style */
		ddns_cb->dhcid_class = dns_rdatatype_txt;
		/* for backwards compatibility */
		ddns_v4_type = DHO_DHCP_CLIENT_IDENTIFIER;
	}
	if (client->active_lease != NULL) {
		/* V6 request, get the client identifier, then
		 * construct the dhcid for either standard
		 * or interim */
		if (((oc = lookup_option(&dhcpv6_universe,
					 client->sent_options,
					 D6O_CLIENTID)) != NULL) &&
		    evaluate_option_cache(&client_identifier, NULL,
					  NULL, client,
					  client->sent_options, NULL,
					  &global_scope, oc, MDL)) {
			result = get_dhcid(ddns_cb, 2,
					   client_identifier.data,
					   client_identifier.len);
			data_string_forget(&client_identifier, MDL);
		} else
			log_fatal("Impossible condition at %s:%d.", MDL);
	} else {
		/*
		 * V4 request, use the client id if there is one or the
		 * mac address if there isn't.  If we have a client id
		 * we check to see if it is an embedded DUID.
		 */
		if (((oc = lookup_option(&dhcp_universe,
					 client->sent_options,
					 DHO_DHCP_CLIENT_IDENTIFIER)) != NULL) &&
		    evaluate_option_cache(&client_identifier, NULL,
					  NULL, client,
					  client->sent_options, NULL,
					  &global_scope, oc, MDL)) {
			if ((std_dhcid == 1) && (duid_v4 == 1) &&
			    (client_identifier.data[0] == 255)) {
				/*
				 * This appears to be an embedded DUID,
				 * extract it and treat it as such
				 */
				if (client_identifier.len <= 5)
					log_fatal("Impossible condition at %s:%d.",
						  MDL);
				result = get_dhcid(ddns_cb, 2,
						   client_identifier.data + 5,
						   client_identifier.len - 5);
			} else {
				result = get_dhcid(ddns_cb, ddns_v4_type,
						   client_identifier.data,
						   client_identifier.len);
			}
			data_string_forget(&client_identifier, MDL);
		} else
			result = get_dhcid(ddns_cb, 0,
					   client->interface->hw_address.hbuf,
					   client->interface->hw_address.hlen);
	}

	if (!result) {
		return ISC_R_SUCCESS;
	}

	/*
	 * Perform updates.
	 */
	if (ddns_cb->fwd_name.len && ddns_cb->dhcid.len) {
		rcode = ddns_modify_fwd(ddns_cb, MDL);
	} else
		rcode = ISC_R_FAILURE;

	/*
	 * A success from the modify routine means we are performing
	 * async processing, for which we use the timedout error message.
	 */
	if (rcode == ISC_R_SUCCESS) {
		rcode = ISC_R_TIMEDOUT;
	}

	return rcode;
}


/*
 * Schedule the first update.  They will continue to retry occasionally
 * until they no longer time out (or fail).
 */
void
dhclient_schedule_updates(struct client_state *client,
			  struct iaddr        *addr,
			  int                  offset)
{
	dhcp_ddns_cb_t *ddns_cb;
	struct timeval tv;

	if (!client->config->do_forward_update)
		return;

	/* cancel any outstanding ddns requests */
	if (client->ddns_cb != NULL) {
		ddns_cancel(client->ddns_cb, MDL);
		client->ddns_cb = NULL;
	}

	ddns_cb = ddns_cb_alloc(MDL);

	if (ddns_cb != NULL) {
		ddns_cb->lease = (void *)client;
		ddns_cb->address = *addr;
		ddns_cb->timeout = 1;

		/*
		 * XXX: DNS TTL is a problem we need to solve properly.
		 * Until that time, 300 is a placeholder default for
		 * something that is less insane than a value scaled
		 * by lease timeout.
		 */
		ddns_cb->ttl = 300;

		ddns_cb->state = DDNS_STATE_ADD_FW_NXDOMAIN;
		ddns_cb->cur_func = client_dns_update_action;
		ddns_cb->flags = DDNS_UPDATE_ADDR | DDNS_INCLUDE_RRSET;

		client->ddns_cb = ddns_cb;
		tv.tv_sec = cur_tv.tv_sec + offset;
		tv.tv_usec = cur_tv.tv_usec;
		add_timeout(&tv, client_dns_update_timeout,
			    ddns_cb, NULL, NULL);
	} else {
		log_error("Unable to allocate dns update state for %s",
			  piaddr(*addr));
	}
}
#endif /* defined NSUPDATE */

void
dhcpv4_client_assignments(void)
{
	struct servent *ent;

	if (path_dhclient_pid == NULL)
		path_dhclient_pid = _PATH_DHCLIENT_PID;
	if (path_dhclient_db == NULL)
		path_dhclient_db = _PATH_DHCLIENT_DB;

	/* Default to the DHCP/BOOTP port. */
	if (!local_port) {
		/* If we're faking a relay agent, and we're not using loopback,
		   use the server port, not the client port. */
		if (mockup_relay && giaddr.s_addr != htonl(INADDR_LOOPBACK)) {
			local_port = htons(67);
		} else {
			ent = getservbyname("dhcpc", "udp");
			if (ent == NULL)
				ent = getservbyname("bootpc", "udp");
			if (ent == NULL)
				local_port = htons(68);
			else
				local_port = ent->s_port;
#ifndef __CYGWIN32__
			endservent ();
#endif
		}
	}

	/* If we're faking a relay agent, and we're not using loopback,
	   we're using the server port, not the client port. */
	if (mockup_relay && giaddr.s_addr != htonl(INADDR_LOOPBACK)) {
		remote_port = local_port;
	} else
		remote_port = htons(ntohs(local_port) - 1);   /* XXX */
}

/*
 * The following routines are used to check that certain
 * strings are reasonable before we pass them to the scripts.
 * This avoids some problems with scripts treating the strings
 * as commands - see ticket 23722
 * The domain checking code should be done as part of assembling
 * the string but we are doing it here for now due to time
 * constraints.
 */

static int check_domain_name(const char *ptr, size_t len, int dots)
{
	const char *p;

	/* not empty or complete length not over 255 characters   */
	if ((len == 0) || (len > 256))
		return(-1);

	/* consists of [[:alnum:]-]+ labels separated by [.]      */
	/* a [_] is against RFC but seems to be "widely used"...  */
	for (p=ptr; (*p != 0) && (len-- > 0); p++) {
		if ((*p == '-') || (*p == '_')) {
			/* not allowed at begin or end of a label */
			if (((p - ptr) == 0) || (len == 0) || (p[1] == '.'))
				return(-1);
		} else if (*p == '.') {
			/* each label has to be 1-63 characters;
			   we allow [.] at the end ('foo.bar.')   */
			size_t d = p - ptr;
			if ((d <= 0) || (d >= 64))
				return(-1);
			ptr = p + 1; /* jump to the next label    */
			if ((dots > 0) && (len > 0))
				dots--;
		} else if (isalnum((unsigned char)*p) == 0) {
			/* also numbers at the begin are fine     */
			return(-1);
		}
	}
	return(dots ? -1 : 0);
}

static int check_domain_name_list(const char *ptr, size_t len, int dots)
{
	const char *p;
	int ret = -1; /* at least one needed */

	if ((ptr == NULL) || (len == 0))
		return(-1);

	for (p=ptr; (*p != 0) && (len > 0); p++, len--) {
		if (*p != ' ')
			continue;
		if (p > ptr) {
			if (check_domain_name(ptr, p - ptr, dots) != 0)
				return(-1);
			ret = 0;
		}
		ptr = p + 1;
	}
	if (p > ptr)
		return(check_domain_name(ptr, p - ptr, dots));
	else
		return(ret);
}

static int check_option_values(struct universe *universe,
			       unsigned int opt,
			       const char *ptr,
			       size_t len)
{
	if (ptr == NULL)
		return(-1);

	/* just reject options we want to protect, will be escaped anyway */
	if ((universe == NULL) || (universe == &dhcp_universe)) {
		switch(opt) {
		      case DHO_DOMAIN_NAME:
#ifdef ACCEPT_LIST_IN_DOMAIN_NAME
			      return check_domain_name_list(ptr, len, 0);
#else
			      return check_domain_name(ptr, len, 0);
#endif
		      case DHO_HOST_NAME:
		      case DHO_NIS_DOMAIN:
		      case DHO_NETBIOS_SCOPE:
			return check_domain_name(ptr, len, 0);
			break;
		      case DHO_DOMAIN_SEARCH:
			return check_domain_name_list(ptr, len, 0);
			break;
		      case DHO_ROOT_PATH:
			if (len == 0)
				return(-1);
			for (; (*ptr != 0) && (len-- > 0); ptr++) {
				if(!(isalnum((unsigned char)*ptr) ||
				     *ptr == '#'  || *ptr == '%' ||
				     *ptr == '+'  || *ptr == '-' ||
				     *ptr == '_'  || *ptr == ':' ||
				     *ptr == '.'  || *ptr == ',' ||
				     *ptr == '@'  || *ptr == '~' ||
				     *ptr == '\\' || *ptr == '/' ||
				     *ptr == '['  || *ptr == ']' ||
				     *ptr == '='  || *ptr == ' '))
					return(-1);
			}
			return(0);
			break;
		}
	}

#ifdef DHCPv6
	if (universe == &dhcpv6_universe) {
		switch(opt) {
		      case D6O_SIP_SERVERS_DNS:
		      case D6O_DOMAIN_SEARCH:
		      case D6O_NIS_DOMAIN_NAME:
		      case D6O_NISP_DOMAIN_NAME:
			return check_domain_name_list(ptr, len, 0);
			break;
		}
	}
#endif

	return(0);
}

static void
add_reject(struct packet *packet) {
	struct iaddrmatchlist *list;

	list = dmalloc(sizeof(struct iaddrmatchlist), MDL);
	if (!list)
		log_fatal ("no memory for reject list!");

	/*
	 * client_addr is misleading - it is set to source address in common
	 * code.
	 */
	list->match.addr = packet->client_addr;
	/* Set mask to indicate host address. */
	list->match.mask.len = list->match.addr.len;
	memset(list->match.mask.iabuf, 0xff, sizeof(list->match.mask.iabuf));

	/* Append to reject list for the source interface. */
	list->next = packet->interface->client->config->reject_list;
	packet->interface->client->config->reject_list = list;

	/*
	 * We should inform user that we won't be accepting this server
	 * anymore.
	 */
	log_info("Server added to list of rejected servers.");
}

#if defined(NSUPDATE)
/* Wrapper function around common ddns_cb_free function that ensures
 * we set the client_state pointer to the control block to NULL. */
static void
dhclient_ddns_cb_free(dhcp_ddns_cb_t *ddns_cb, char* file, int line) {
    if (ddns_cb) {
        struct client_state *client = (struct client_state *)ddns_cb->lease;
        if (client != NULL) {
            client->ddns_cb = NULL;
        }

        ddns_cb_free(ddns_cb, file, line);
    }
}
#endif /* defined NSUPDATE */

#if defined(DHCPv6) && defined(DHCP4o6)
/*
 * \brief Omapi I/O handler
 *
 * The inter-process communication receive handler.
 *
 * On the DHCPv6 side, the message is either a POLL (which is answered
 *  by a START or a STOP) or a DHCPv4-QUERY (which is forwarded to
 *  DHCPv4 over DHCPv6 servers by forw_dhcpv4_query()).
 *
 * On the DHCPv4 side, the message is either a START, a STOP
 *  (both for the DHCP4 over DHCPv6 state machine) or a DHCPv4-RESPONSE
 *  (which is processed by recv_dhcpv4_response()).
 *
 * \param h the OMAPI object
 * \return a result for I/O success or error (used by the I/O subsystem)
 */
isc_result_t dhcpv4o6_handler(omapi_object_t *h) {
	char buf[65536];
	char start_msg[5] = { 'S', 'T', 'A', 'R', 'T' };
	char stop_msg[4] = { 'S', 'T', 'O', 'P' };
	char poll_msg[4] = { 'P', 'O', 'L', 'L' };
	struct data_string raw;
	int cc;

	if (h->type != dhcp4o6_type)
		return DHCP_R_INVALIDARG;

	cc = recv(dhcp4o6_fd, buf, sizeof(buf), 0);
	if (cc <= 0)
		return ISC_R_UNEXPECTED;

	if (local_family == AF_INET6) {
		if ((cc == 4) &&
		    (memcmp(buf, poll_msg, sizeof(poll_msg)) == 0)) {
			log_info("RCV: POLL");
			if (dhcp4o6_state < 0)
				cc = send(dhcp4o6_fd, stop_msg,
					  sizeof(stop_msg), 0);
			else
				cc = send(dhcp4o6_fd, start_msg,
					  sizeof(start_msg), 0);
			if (cc < 0) {
				log_error("dhcpv4o6_handler: send(): %m");
				return ISC_R_IOERROR;
			}
		} else {
			if (cc < DHCP_FIXED_NON_UDP + 8)
				return ISC_R_UNEXPECTED;
			memset(&raw, 0, sizeof(raw));
			if (!buffer_allocate(&raw.buffer, cc, MDL)) {
				log_error("dhcpv4o6_handler: "
					  "no memory buffer.");
				return ISC_R_NOMEMORY;
			}
			raw.data = raw.buffer->data;
			raw.len = cc;
			memcpy(raw.buffer->data, buf, cc);

			forw_dhcpv4_query(&raw);

			data_string_forget(&raw, MDL);
		}
	} else {
		if ((cc == 4) &&
		    (memcmp(buf, stop_msg, sizeof(stop_msg)) == 0)) {
			log_info("RCV: STOP");
			if (dhcp4o6_state > 0) {
				dhcp4o6_state = 0;
				dhcp4o6_poll(NULL);
			}
		} else if ((cc == 5) &&
			   (memcmp(buf, start_msg, sizeof(start_msg)) == 0)) {
			log_info("RCV: START");
			if (dhcp4o6_state == 0)
				cancel_timeout(dhcp4o6_poll, NULL);
			dhcp4o6_state = 1;
			dhcp4o6_resume();
		} else {
			if (cc < DHCP_FIXED_NON_UDP + 16)
				return ISC_R_UNEXPECTED;
			memset(&raw, 0, sizeof(raw));
			if (!buffer_allocate(&raw.buffer, cc, MDL)) {
				log_error("dhcpv4o6_handler: "
					  "no memory buffer.");
				return ISC_R_NOMEMORY;
			}
			raw.data = raw.buffer->data;
			raw.len = cc;
			memcpy(raw.buffer->data, buf, cc);

			recv_dhcpv4_response(&raw);

			data_string_forget(&raw, MDL);
		}
	}

	return ISC_R_SUCCESS;
}

/*
 * \brief Poll the DHCPv6 client
 *  (DHCPv4 client function)
 *
 * A POLL message is sent to the DHCPv6 client periodically to check
 * if the DHCPv6 is ready (i.e., has a valid DHCPv4-over-DHCPv6 server
 * address option).
 */
static void dhcp4o6_poll(void *dummy) {
	char msg[4] = { 'P', 'O', 'L', 'L' };
	struct timeval tv;
	int cc;

	IGNORE_UNUSED(dummy);

	if (dhcp4o6_state < 0)
		dhcp4o6_state = 0;

	log_info("POLL");

	cc = send(dhcp4o6_fd, msg, sizeof(msg), 0);
	if (cc < 0)
		log_error("dhcp4o6_poll: send(): %m");

	tv.tv_sec = cur_time + 60;
	tv.tv_usec = random() % 1000000;

	add_timeout(&tv, dhcp4o6_poll, NULL, 0, 0);
}

/*
 * \brief Resume pending operations
 *  (DHCPv4 client function)
 *
 * A START message was received from the DHCPv6 client so pending
 * operations (RELEASE or REBOOT) must be resumed.
 */
static void dhcp4o6_resume() {
	struct interface_info *ip;
	struct client_state *client;

	for (ip = interfaces; ip != NULL; ip = ip->next) {
		for (client = ip->client; client != NULL;
		     client = client->next) {
			if (client->pending == P_RELEASE)
				do_release(client);
			else if (client->pending == P_REBOOT)
				state_reboot(client);
		}
	}
}

/*
 * \brief Send a START to the DHCPv4 client
 *  (DHCPv6 client function)
 *
 * First check if there is a valid DHCPv4-over-DHCPv6 server address option,
 * and when found go UP and on a transition from another state send
 * a START message to the DHCPv4 client.
 */
void dhcp4o6_start() {
	struct interface_info *ip;
	struct client_state *client;
	struct dhc6_lease *lease;
	struct option_cache *oc;
	struct data_string addrs;
	char msg[5] = { 'S', 'T', 'A', 'R', 'T' };
	int cc;

	memset(&addrs, 0, sizeof(addrs));
	for (ip = interfaces; ip != NULL; ip = ip->next) {
		for (client = ip->client; client != NULL;
		     client = client->next) {
			if ((client->state != S_BOUND) &&
			    (client->state != S_RENEWING) &&
			    (client->state != S_REBINDING))
				continue;
			lease = client->active_lease;
			if ((lease == NULL) || lease->released)
				continue;
			oc = lookup_option(&dhcpv6_universe,
					   lease->options,
					   D6O_DHCP4_O_DHCP6_SERVER);
			if ((oc == NULL) ||
			    !evaluate_option_cache(&addrs, NULL, NULL, NULL,
						   lease->options, NULL,
						   &global_scope, oc, MDL))
				continue;
			if ((addrs.len % 16) != 0) {
				data_string_forget(&addrs, MDL);
				continue;
			}
			data_string_forget(&addrs, MDL);
			goto found;
		}
	}
	log_info("dhcp4o6_start: failed");
	dhcp4o6_stop();
	return;

found:
	if (dhcp4o6_state == 1)
		return;
	log_info("dhcp4o6_start: go to UP");
	dhcp4o6_state = 1;

	cc = send(dhcp4o6_fd, msg, sizeof(msg), 0);
	if (cc < 0)
		log_info("dhcp4o6_start: send(): %m");
}

/*
 * Send a STOP to the DHCPv4 client
 *  (DHCPv6 client function)
 *
 * Go DOWN and on a transition from another state send a STOP message
 * to the DHCPv4 client.
 */
static void dhcp4o6_stop() {
	char msg[4] = { 'S', 'T', 'O', 'P' };
	int cc;

	if (dhcp4o6_state == -1)
		return;

	log_info("dhcp4o6_stop: go to DOWN");
	dhcp4o6_state = -1;

	cc = send(dhcp4o6_fd, msg, sizeof(msg), 0);
	if (cc < 0)
		log_error("dhcp4o6_stop: send(): %m");
}
#endif /* DHCPv6 && DHCP4o6 */
