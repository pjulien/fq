/*
 * Copyright (c) 2013 OmniTI Computer Consulting, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include "getopt.h"
#include "fqd.h"
#include "fqd_private.h"
#ifndef NO_BCD
#include <bcd.h>

static void bcd_signal_handler(int s, siginfo_t *si, void *unused) {
  (void)si;
  (void)unused;
  bcd_fatal("This is a fatal crash");
  signal(s, SIG_DFL);
  return;
}

static void
bcd_setup_sigaction(void)
{
  struct sigaction sa;
  int signals[] = {
    SIGSEGV,
    SIGFPE,
    SIGABRT,
    SIGBUS,
    SIGILL,
    SIGFPE
  };

  sa.sa_sigaction = bcd_signal_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

  for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); i++) {
    if (sigaction(signals[i], &sa, NULL) == -1) {
      fprintf(stderr, "warning: failed to set signal "
          "handler %d\n", signals[i]);
    }
  }

  return;
}
#else
typedef void *bcd_t;
#endif

static uint32_t nodeid = 0;
static unsigned short port = 8765;
static int foreground = 0;
static bool usebcd = false;
static int worker_threads = 1;
static char *config_path = NULL;
static char *queue_path = NULL;
static char *libexecdir = NULL;
static bcd_t global_bcd = { 0 };

#define die(str) do { \
  fprintf(stderr, "%s: %s\n", str, strerror(errno)); \
  exit(-1); \
} while(0)

void fqd_bcd_attach(void) {
  if(!usebcd) return;
#ifndef NO_BCD
  bcd_error_t error;
  if (bcd_attach(&global_bcd, &error) == -1) {
    fprintf(stderr, "error: %s\n",
            bcd_error_message(&error));
     exit(-1);
  }
#endif
}

static void *listener_thread(void *unused) {
  (void)unused;
  fqd_start_worker_threads(worker_threads);
  fprintf(stderr, "Listening on port: %d\n", port);
  fqd_listener(NULL, port);
  fqd_stop_worker_threads();
  return NULL;
}
static void usage(const char *prog) {
  printf("%s:\n", prog);
  printf("\t-h\t\tthis help message\n");
  printf("\t-D\t\trun in the foreground\n");
  printf("\t-B\t\tenable BCD backtrace reporting\n");
  printf("\t-t <count>\tnumber of worker threads to use (default 1)\n");
  printf("\t-n <ip>\t\tnode self identifier (IPv4)\n");
  printf("\t-p <port>\tspecify listening port (default: 8765)\n");
  printf("\t-c <file>\tlocation of the configdb\n");
  printf("\t-q <dir>\twhere persistent queues are stored\n");
  printf("\t-w <dir>\twhere files for web services are available\n");
  printf("\t-v <flags>\tprint additional debugging information, by overriding FQ_DEBUG (cf. fq.h)\n");
  printf("\t-l <dir>\tuse this dir for relative module loads\n");
  printf("\t-m <module>\tmodule to load\n");
}
static void parse_cli(int argc, char **argv) {
  int c;
  char *debug = NULL;
  if(getenv("FQ_DEBUG")) {
    debug = strdup(getenv("FQ_DEBUG"));
  }
  libexecdir = strdup(LIBEXECDIR);
  while((c = getopt(argc, argv, "Bbl:m:hDt:n:p:q:c:w:v:")) != EOF) {
    switch(c) {
      case 'B':
        usebcd = true;
        break;
      case 'b':
        usebcd = false;
        break;
      case 'l':
        free(libexecdir);
        libexecdir = strdup(optarg);
        break;
      case 'm':
        fqd_route_load_module(libexecdir, optarg, ".so");
        break;
      case 'q':
        free(queue_path);
        queue_path = strdup(optarg);
        break;
      case 'w':
        fqd_http_set_root(optarg);
        break;
      case 'c':
        free(config_path);
        config_path = strdup(optarg);
        break;
      case 'D':
        foreground = 1;
        break;
      case 't':
        worker_threads = atoi(optarg);
        break;
      case 'h':
        usage(argv[0]);
        exit(0);
      case 'n':
        if(inet_pton(AF_INET, optarg, &nodeid) != 1) {
          fprintf(stderr, "Bad argument to -n, must be an IPv4 address.\n");
          exit(-1);
        }
        if(nodeid == 0 || nodeid == htonl(0x7f000001)) {
          fprintf(stderr, "nodeid cannot be INADDR_ANY or loopback\n");
          exit(-1);
        }
        break;
      case 'p':
        port = atoi(optarg);
        break;
      case 'v':
        free(debug);
        debug = strdup(optarg);
        break;
      default:
        usage(argv[0]);
        exit(-1);
    }
  }
  if(debug) fq_debug_set_string(debug);
  free(debug);
}
static uint32_t get_my_ip(void) {
  uint32_t ip;
  struct hostent *h;
  char buff[128];
  gethostname(buff, sizeof(buff));
  h = gethostbyname(buff);
  if(h && h->h_addrtype == AF_INET && h->h_length == 4) {
    memcpy(&ip, h->h_addr_list[0], h->h_length);
    if(ip == htonl(0x7f000001)) return 0;
    return ip;
  }
  return 0;
}
int main(int argc, char **argv) {
  nodeid = get_my_ip();
  parse_cli(argc,argv);
  global_functions_init(libexecdir);
  if(nodeid == 0) {
    fprintf(stderr, "Could not determine host address, use -n <ip>\n");
    exit(-1);
  }
  signal(SIGPIPE, SIG_IGN);
  if(!foreground) {
    int pid, fd;

    /* Handle stdin/stdout/stderr */
    fd = open("/dev/null", O_RDONLY);
    if(fd < 0 || dup2(fd, STDIN_FILENO) < 0) die("Failed to setup stdin");
    close(fd);
    fd = open("/dev/null", O_WRONLY);
    if(fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
      die("Failed to setup std{out,err}");
    close(fd);

    /* daemonize */
    pid = fork();
    if(pid < 0) die("Failed to fork");
    if(pid > 0) exit(0);
    setsid();
    pid = fork();
    if(pid < 0) die("Failed to fork");
    if(pid > 0) exit(0);

    /* run */
  }
#ifndef NO_BCD
  bcd_error_t error;
  if(usebcd) {
    struct bcd_config config;

    /* Initialize BCD configuration. See bcd.h for options */
    if (bcd_config_init(&config, &error) == -1)
      goto fatal;

    /* Initialize the library. */
    if (bcd_init(&config, &error) == -1)
      goto fatal;

    /* Initialize a handle to BCD. This should be called by every thread interacting with BCD. */
    if (bcd_attach(&global_bcd, &error) == -1)
      goto fatal;

    if (bcd_kv(&global_bcd, "application", "fqd", &error) == -1)
      goto fatal;

    if (bcd_kv(&global_bcd, "version", FQ_VERSION, &error) == -1)
      goto fatal;

    bcd_setup_sigaction();
  }
#endif
  fqd_config_init(nodeid, config_path, queue_path);
  listener_thread(NULL);
  fprintf(stderr, "Listener thread could not start. Exiting.\n");
  exit(0);
  return 0;

#ifndef NO_BCD
fatal:
  fprintf(stderr, "error: %s\n",
          bcd_error_message(&error));
  exit(-1);
#endif
}
