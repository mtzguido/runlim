#include <asm/param.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*------------------------------------------------------------------------*/

#define SAMPLE_RATE 10000	/* in milliseconds */
#define REPORT_RATE 100		/* in terms of sampling */

/*------------------------------------------------------------------------*/

#ifndef MAX_PROC
#define MAX_PROC 32768		/* default on most Linux boxes */
#endif

/*------------------------------------------------------------------------*/

typedef struct Process Process;
typedef enum Status Status;

/*------------------------------------------------------------------------*/

enum Status
{
  OK,
  OUT_OF_MEMORY,
  OUT_OF_TIME,
  SEGMENTATION_FAULT,
  BUS_ERROR,
  OTHER_SIGNAL,
  FORK_FAILED,
  INTERNAL_ERROR,
  EXEC_FAILED
};

/*------------------------------------------------------------------------*/

struct Process
{
  pid_t pid;
  pid_t ppid;
  double time;
  double mb;
  long samples;
  Process * next;
};

/*------------------------------------------------------------------------*/

#define USAGE \
"usage: runlim [option ...] program [arg ...]\n" \
"\n" \
"  where option is from the following list:\n" \
"\n" \
"    -h                         print this command line summary\n" \
"    --help\n" \
"\n" \
"    --version                  print version number\n" \
"\n" \
"    --space-limit=<number>     set space limit to <number> MB\n" \
"    -s <number>\n"\
"\n" \
"    --time-limit=<number>      set time limit to <number> seconds\n" \
"    -t <number>\n"\
"\n" \
"    --real-time-limit=<number> set real time limit to <number> seconds\n" \
"    -r <number>\n"\
"\n" \
"    -k|--kill                  propagate signals\n" \
"\n" \
"The program is the name of an executable followed by its arguments.\n"

/*------------------------------------------------------------------------*/

static void
usage (void)
{
  printf (USAGE);
}

/*------------------------------------------------------------------------*/

static int
isposnumber (const char *str)
{
  const char *p;
  int res;

  if (*str)
    {
      for (res = 1, p = str; res && *p; p++)
	res = isdigit ((int) *p);
    }
  else
    res = 0;

  return res;
}

/*------------------------------------------------------------------------*/

static unsigned
parse_number_argument (int *i, int argc, char **argv)
{
  unsigned res;

  if (argv[*i][2])
    {
      if (isposnumber (argv[*i] + 2))
	res = (unsigned) atoi (argv[*i] + 2);
      else
	goto ARGUMENT_IS_MISSING;
    }
  else if (*i + 1 < argc && isposnumber (argv[*i + 1]))
    {
      res = (unsigned) atoi (argv[*i + 1]);
      *i += 1;
    }
  else
    {

ARGUMENT_IS_MISSING:

      fprintf (stderr,
	       "*** runlim: number argument for '-%c' is missing\n",
	       argv[*i][1]);
      exit (1);

      res = 0;
    }

  return res;
}

/*------------------------------------------------------------------------*/

static void
print_long_command_line_option (FILE * file, char *str)
{
  const char *p;

  for (p = str; *p && *p != ' '; p++)
    fputc (*p, file);
}

/*------------------------------------------------------------------------*/

static unsigned
parse_number_rhs (char *str)
{
  unsigned res;
  char *p;

  p = strchr (str, '=');
  assert (p);

  if (!p[1])
    {
      fputs ("*** runlim: argument to ", stderr);
      print_long_command_line_option (stderr, str);
      fputs (" is missing\n", stderr);
      exit (1);

      res = 0;
    }

  if (!isposnumber (p + 1))
    {
      fputs ("*** runlim: argument to ", stderr);
      print_long_command_line_option (stderr, str);
      fputs (" is not a positive number\n", stderr);
      exit (1);

      res = 0;
    }

  res = (unsigned) atoi (p + 1);

  return res;
}

/*------------------------------------------------------------------------*/

static unsigned
get_physical_mb ()
{
  unsigned res;
  long tmp;

  tmp = sysconf (_SC_PAGE_SIZE) * sysconf (_SC_PHYS_PAGES);
  tmp >>= 20;
  res = (unsigned) tmp;

  return res;
}

/*------------------------------------------------------------------------*/

static int child_pid = -1;
static int parent_pid = -1;
static int group_pid = -1;

static FILE *log = 0;
static int close_log = 0;

static long num_samples_since_last_report = 0;
static long num_samples = 0;

static double max_mb = 0;
static double max_seconds = 0;

static int propagate_signals = 0;
static int children = 0;

/*------------------------------------------------------------------------*/

static void
error (const char * fmt, ...)
{
  va_list ap;
  assert (log);
  fputs ("runlim error: ", log);
  va_start (ap, fmt);
  vfprintf (log, fmt, ap);
  fputc ('\n', log);
  va_end (ap);
  fflush (log);
  exit (1);
}

static void
warning (const char * fmt, ...)
{
  va_list ap;
  assert (log);
  fputs ("runlim warning: ", log);
  va_start (ap, fmt);
  vfprintf (log, fmt, ap);
  fputc ('\n', log);
  va_end (ap);
  fflush (log);
}

static void
message (const char * type, const char * fmt, ...)
{
  size_t len;
  va_list ap;
  assert (log);
  fputs ("[runlim] ", log);
  fputs (type, log);
  fputc (':', log);
  for (len = strlen (type); len < 22; len += 8)
    fputc ('\t', log);
  fputc ('\t', log);
  va_start (ap, fmt);
  vfprintf (log, fmt, ap);
  va_end (ap);
  fputc ('\n', log);
  fflush (log);
}

/*------------------------------------------------------------------------*/

#define PID_POS 0
#define PPID_POS 3
#define STIME_POS 13
#define UTIME_POS 14
#define RSIZE_POS 23
#define MAX_POS 23

/*------------------------------------------------------------------------*/

static unsigned start_time;
static unsigned time_limit;
static unsigned real_time_limit;
static unsigned space_limit;

/*------------------------------------------------------------------------*/

static char * buffer;
static size_t size_buffer;
static size_t pos_buffer;

static char * path;
static size_t size_path;

/*------------------------------------------------------------------------*/

static void
push_buffer (int ch)
{
  if (size_buffer == pos_buffer)
    {
      size_buffer = size_buffer ? 2*size_buffer : 128;
      buffer = realloc (buffer, size_buffer);
      if (!buffer)
	error ("out-of-memory reallocating buffer");
    }

  buffer[pos_buffer++] = ch;
}

static void
fit_path (size_t len)
{
  if (len > size_path)
    {
      size_path = 2*len;
      path = realloc (path, size_path);
      if (!path)
	error ("out-of-memory reallocating path");
    }
}

/*------------------------------------------------------------------------*/

#define PROC_PATH    "/proc"

static long
forall_child_processes (long (*f)(Process*))
{
  long ujiffies, sjiffies, rsize;
  struct dirent *de;
  int i, ch, empty;
  char *token;
  FILE *file;
  pid_t pid;
  DIR *dir;

  long res = 0;

  if (!(dir = opendir (PROC_PATH)))
    error ("can not open directory '%s'", PROC_PATH);

  empty = 1;

SKIP:

  while ((de = readdir (dir)) != NULL)
    {
      Process p;

      empty = 0;
      pid = (pid_t) atoi (de->d_name);
      if (pid == 0) continue;
      fit_path (strlen (PROC_PATH) + strlen (de->d_name) + 20);
      sprintf (path, "%s/%d/stat", PROC_PATH, pid);
      file = fopen (path, "r");
      if (!file) continue;

      pos_buffer = 0;
      
      while ((ch = getc (file)) != EOF)
	push_buffer (ch);
      
      fclose (file);

      push_buffer (0);
      
      p.pid = -1;
      p.ppid = -1;

      rsize = -1;
      ujiffies = -1;
      sjiffies = -1;
      
      token = strtok (buffer, " ");
      i = 0;
      
      while (token && i <= MAX_POS)
	{
	  switch (i++)
	    {
	    case PID_POS:
	      if (sscanf (token, "%d", &p.pid) != 1) goto SKIP;
	      break;
	    case PPID_POS:
	      if (sscanf (token, "%d", &p.ppid) != 1) goto SKIP;
	      break;
	    case RSIZE_POS:
	       if (sscanf (token, "%ld", &rsize) != 1) goto SKIP;
	       break;
	    case STIME_POS:
	      if (sscanf (token, "%ld", &sjiffies) != 1) goto SKIP;
	      break;
	    case UTIME_POS:
	      if (sscanf (token, "%ld", &ujiffies) != 1) goto SKIP;
	      break;
	    default:
	      break;
	    }
	  
	  token = strtok (0, " ");
	}

      if (p.pid < 0) goto SKIP;
      if (p.ppid < 0) goto SKIP;
      if (ujiffies < 0) goto SKIP;
      if (sjiffies < 0) goto SKIP;
      if (rsize < 0) goto SKIP;

      p.time += (ujiffies + sjiffies) / (double) HZ;
      p.mb += rsize / (double) (1 << 8);

      res += f (&p);
    }
  
  if (closedir (dir))
    warning ("failed to close directory '%s'", PROC_PATH);

  if (empty)
    error ("directory '%s' empty", PROC_PATH);

  return res;
}

/*------------------------------------------------------------------------*/

static double accumulated_time;
static double sampled_time;
static double sampled_mb;

static Process * active;

static Process * find_process (pid_t pid)
{
  Process * res;
  for (res = active; res; res = res->next)
    if (res->pid == pid) break;
  return res;
}

static long
sample_process (Process * p)
{
  Process * q;

  assert (p->pid != parent_pid);

  sampled_time += p->time;
  sampled_mb += p->mb;

  q = find_process (p->pid);
  if (!q)
    {
      q = malloc (sizeof *q);
      if (!q)
	error ("out-of-memory allocating process");
      q->pid = p->pid;
      q->ppid = p->ppid;
      q->next = active;
      q->mb = 0;
      active = q;
    }

  q->time = p->time;
  if (q->mb < p->mb) q->mb = p->mb;
  q->samples = num_samples;

  return 1;
}

static long
sample_all_child_processes (void)
{
  Process * prev = 0, * p = active, * next;
  long sampled = forall_child_processes (sample_process);
  while (p)
    {
      next = p->next;
      if (p->samples != num_samples)
	{
	  accumulated_time += p->time;
	  if (prev) prev->next = next;
	  else active = next;
	  free (p);
	}
      else
	prev = p;
      p = next;
    }
  sampled_time += accumulated_time;
  return sampled;
}

/*------------------------------------------------------------------------*/

static struct itimerval timer;
static struct itimerval old_timer;

static int caught_out_of_memory;
static int caught_out_of_time;

static pthread_mutex_t caught_out_of_memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t caught_out_of_time_mutex = PTHREAD_MUTEX_INITIALIZER;

/*------------------------------------------------------------------------*/

static long
term_process (Process * p)
{
  assert (p->pid != parent_pid);
  printf ("terminate %d\n", p->pid), fflush (stdout);
  kill (p->pid, SIGTERM);
  return 1;
}

static long
kill_process (Process * p)
{
  assert (p->pid != parent_pid);
  printf ("killing %d\n", p->pid), fflush (stdout);
  kill (p->pid, SIGKILL);
  return 1;
}

static pthread_mutex_t kill_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
kill_all_child_processes (void)
{
  long ms = 16000;
  long rounds = 0;
  long killed;

  do 
    {
      usleep (ms);

      pthread_mutex_lock (&kill_mutex);

      if (ms > 2000) killed = forall_child_processes (term_process);
      else killed = forall_child_processes (kill_process);

      pthread_mutex_unlock (&kill_mutex);

      if (ms > 1000) ms /= 2;
    }
  while (killed > 0 && rounds++ < 10);
}

/*------------------------------------------------------------------------*/

static double
wall_clock_time (void)
{
  double res = -1;
  struct timeval tv;
  if (!gettimeofday (&tv, 0))
    {
      res = 1e-6 * tv.tv_usec;
      res += tv.tv_sec;
    }
  return res;
}

/*------------------------------------------------------------------------*/

static double
real_time (void) 
{
  double res;
  if (start_time < 0) return -1;
  res = wall_clock_time() - start_time;
  return res;
}

/*------------------------------------------------------------------------*/

static void
report (double time, double mb)
{
  message ("sample", "%.1f time, %1.f real, %.1f MB");
}

/*------------------------------------------------------------------------*/

static void
sampler (int s)
{
  long sampled;

  assert (s == SIGALRM);
  num_samples++;

  sampled_time = sampled_mb = 0;
  sampled = sample_all_child_processes ();

  if (sampled > 0)
    { 
      if (sampled_mb > max_mb)
	max_mb = sampled_mb;

      if (sampled_time > max_seconds)
	max_seconds = sampled_time;
    }

  if (++num_samples_since_last_report >= REPORT_RATE)
    {
      num_samples_since_last_report = 0;
      if (sampled > 0)
	report (sampled_time, sampled_mb);
    }

  if (sampled > 0)
    {
      if (sampled_time > time_limit || real_time () > real_time_limit)
	{
	  int already_caught;
	  pthread_mutex_lock (&caught_out_of_time_mutex);
	  already_caught = caught_out_of_time;
	  caught_out_of_time = 1;
	  pthread_mutex_unlock (&caught_out_of_time_mutex);
	  if (!already_caught) kill_all_child_processes ();
	}
      else if (sampled_mb > space_limit)
	{
	  int already_caught;
	  pthread_mutex_lock (&caught_out_of_memory_mutex);
	  already_caught = caught_out_of_memory;
	  caught_out_of_memory = 1;
	  pthread_mutex_unlock (&caught_out_of_memory_mutex);
	  if (!already_caught) kill_all_child_processes ();
	  kill_all_child_processes ();
	}
    }
}

/*------------------------------------------------------------------------*/

static volatile int caught_usr1_signal = 0;
static volatile int caught_other_signal = 0;

static pthread_mutex_t caught_other_signal_mutex = PTHREAD_MUTEX_INITIALIZER;

/*------------------------------------------------------------------------*/

static void
sig_usr1_handler (int s)
{
  assert (s == SIGUSR1);
  caught_usr1_signal = 1;
}

static void (*old_sig_int_handler);
static void (*old_sig_segv_handler);
static void (*old_sig_kill_handler);
static void (*old_sig_term_handler);

static void restore_signal_handlers () {
  (void) signal (SIGINT, old_sig_int_handler);
  (void) signal (SIGSEGV, old_sig_segv_handler);
  (void) signal (SIGKILL, old_sig_kill_handler);
  (void) signal (SIGTERM, old_sig_term_handler);
}

static void
sig_other_handler (int s)
{
  int already_caught;
  pthread_mutex_lock (&caught_other_signal_mutex);
  already_caught = caught_other_signal;
  caught_other_signal = 1;
  pthread_mutex_unlock (&caught_other_signal_mutex);
  if (already_caught) return;
  restore_signal_handlers ();
  kill_all_child_processes ();
  usleep (10000);
  raise (s);
}

/*------------------------------------------------------------------------*/

static const char * get_host_name ()
{
  const char * path = "/proc/sys/kernel/hostname";
  FILE * file;
  int ch;
  file = fopen (path, "r");
  if (!file)
    error ("can not read host name from '%s'", path);

  pos_buffer = 0;
  while ((ch = getc (file)) != EOF)
    push_buffer (ch);

  fclose (file);
  push_buffer (0);

  return buffer;
}

/*------------------------------------------------------------------------*/

int
main (int argc, char **argv)
{
  int i, j, res, status, s, ok;
  struct rlimit l;
  double real;
  time_t t;

  log = stderr;
  assert (!close_log);

#if 0
  i = 0;
  message ("1", "%d", i++);
  message ("12", "%d", i++);
  message ("123", "%d", i++);
  message ("1234", "%d", i++);
  message ("12345", "%d", i++);
  message ("123456", "%d", i++);
  message ("1234567", "%d", i++);
  message ("12345678", "%d", i++);
  message ("123456789", "%d", i++);
  message ("1234567890", "%d", i++);
  message ("12345678901", "%d", i++);
  message ("123456789012", "%d", i++);
  message ("1234567890123", "%d", i++);
  message ("12345678901234", "%d", i++);
  message ("123456789012345", "%d", i++);
  message ("1234567890123456", "%d", i++);
  message ("12345678901234567", "%d", i++);
  message ("123456789012345678", "%d", i++);
  message ("1234567890123456789", "%d", i++);
  message ("12345678901234567890", "%d", i++);
  exit (1);
#endif

  ok = OK;				/* status of the runlim */
  s = 0;				/* signal caught */

  time_limit = 60 * 60 * 24 * 3600;	/* one year */
  real_time_limit = time_limit;		/* same as time limit by default */
  space_limit = get_physical_mb ();	/* physical memory size */

  for (i = 1; i < argc; i++)
    {
      if (argv[i][0] == '-')
	{
	  if (argv[i][1] == 't')
	    {
	      time_limit = parse_number_argument (&i, argc, argv);
	    }
	  else if (strstr (argv[i], "--time-limit=") == argv[i])
	    {
	      time_limit = parse_number_rhs (argv[i]);
	    }
	  else if (argv[i][1] == 'r')
	    {
	      real_time_limit = parse_number_argument (&i, argc, argv);
	    }
	  else if (strstr (argv[i], "--real-time-limit=") == argv[i])
	    {
	      real_time_limit = parse_number_rhs (argv[i]);
	    }
	  else if (argv[i][1] == 's')
	    {
	      space_limit = parse_number_argument (&i, argc, argv);
	    }
	  else if (strstr (argv[i], "--space-limit=") == argv[i])
	    {
	      space_limit = parse_number_rhs (argv[i]);
	    }
	  else if (strcmp (argv[i], "-v") == 0 ||
	           strcmp (argv[i], "--version") == 0)
	    {
	      printf ("%g\n", VERSION);
	      fflush (stdout);
	      exit (0);
	    }
	  else if (strcmp (argv[i], "-k") == 0 ||
	           strcmp (argv[i], "--kill") == 0)
	    {
	      propagate_signals = 1;
	    }
	  else if (strcmp (argv[i], "-h") == 0 ||
	           strcmp (argv[i], "--help") == 0)
	    {
	      usage ();
	      exit (0);
	    }
	  else
	    error ("invalid option '%s' (try '-h')", argv[1]);
	}
      else
	break;
    }

  if (i >= argc)
    error ("no program specified (try '-h')");

  fprintf (log, "[runlim] version:\t\t%g\n", VERSION);
  fprintf (log, "[runlim] host:\t\t\t%s", get_host_name ());
  fprintf (log, "[runlim] time limit:\t\t%u seconds\n", time_limit);
  fprintf (log, "[runlim] real time limit:\t%u seconds\n", real_time_limit);
  fprintf (log, "[runlim] space limit:\t\t%u MB\n", space_limit);

  for (j = i; j < argc; j++)
    fprintf (log, "[runlim] argv[%d]:\t\t%s\n", j - i, argv[j]);

  t = time (0);
  fprintf (log, "[runlim] start:\t\t\t%s", ctime (&t));
  fflush (log);

  (void) signal (SIGUSR1, sig_usr1_handler);

  start_time = wall_clock_time();

  parent_pid = getpid ();
  child_pid = fork ();

  if (child_pid != 0)
    {
      if (child_pid < 0)
	{
	  ok = FORK_FAILED;
	  res = 1;
	}
      else
	{
	  status = 0;

	  old_sig_int_handler = signal (SIGINT, sig_other_handler);
	  old_sig_segv_handler = signal (SIGSEGV, sig_other_handler);
	  old_sig_kill_handler = signal (SIGKILL, sig_other_handler);
	  old_sig_term_handler = signal (SIGTERM, sig_other_handler);

	  fprintf (log, "[runlim] parent pid:\t\t%d\n", (int) child_pid);
	  fprintf (log, "[runlim] child pid:\t\t%d\n", (int) parent_pid);
	  fprintf (log, "[runlim] group pid:\t\t%d\n", (int) group_pid);
	  fflush (log);

	  assert (SAMPLE_RATE < 1000000);
	  timer.it_interval.tv_sec = 0;
	  timer.it_interval.tv_usec = SAMPLE_RATE;
	  timer.it_value = timer.it_interval;
	  signal (SIGALRM, sampler);
	  setitimer (ITIMER_REAL, &timer, &old_timer);

	  (void) wait (&status);

	  setitimer (ITIMER_REAL, &old_timer, &timer);

	  if (WIFEXITED (status))
	    res = WEXITSTATUS (status);
	  else if (WIFSIGNALED (status))
	    {
	      s = WTERMSIG (status);
	      res = 128 + s;
	      switch (s)
		{
		case SIGXFSZ:
		  ok = OUT_OF_MEMORY;
		  break;
		case SIGXCPU:
		  ok = OUT_OF_TIME;
		  break;
		case SIGSEGV:
		  ok = SEGMENTATION_FAULT;
		  break;
		case SIGBUS:
		  ok = BUS_ERROR;
		  break;
		default:
		  ok = OTHER_SIGNAL;
		  break;
		}
	    }
	  else
	    {
	      ok = INTERNAL_ERROR;
	      res = 1;
	    }
	}
    }
  else
    {
      unsigned hard_time_limit;

      if (time_limit < real_time_limit)
	{
	  hard_time_limit = time_limit;
	  hard_time_limit = (hard_time_limit * 101 + 99) / 100;	// + 1%
	  l.rlim_cur = l.rlim_max = hard_time_limit;
	  setrlimit (RLIMIT_CPU, &l);
	}

      execvp (argv[i], argv + i);
      kill (getppid (), SIGUSR1);		// TODO DOES THIS WORK?
      exit (1);
    }

  real = real_time ();

  if (caught_usr1_signal)
    ok = EXEC_FAILED;
  else if (caught_out_of_memory)
    ok = OUT_OF_MEMORY;
  else if (caught_out_of_time)
    ok = OUT_OF_TIME;

  kill_all_child_processes ();

  t = time (0);
  fprintf (log, "[runlim] end:\t\t\t%s", ctime (&t));
  fprintf (log, "[runlim] status:\t\t");

  if (max_seconds >= time_limit || real_time () >= real_time_limit)
    goto FORCE_OUT_OF_TIME_ENTRY;

  switch (ok)
    {
    case OK:
      fputs ("ok", log);
      res = 0;
      break;
    case OUT_OF_TIME:
FORCE_OUT_OF_TIME_ENTRY:
      fputs ("out of time", log);
      res = 2;
      break;
    case OUT_OF_MEMORY:
      fputs ("out of memory", log);
      res = 3;
      break;
    case SEGMENTATION_FAULT:
      fputs ("segmentation fault", log);
      res = 4;
      break;
    case BUS_ERROR:
      fputs ("bus error", log);
      res = 5;
      break;
    case FORK_FAILED:
      fputs ("fork failed", log);
      res = 6;
      break;
    case INTERNAL_ERROR:
      fputs ("internal error", log);
      res = 7;
      break;
    case EXEC_FAILED:
      fputs ("execvp failed", log);
      res = 1;
      break;
    default:
      fprintf (log, "signal(%d)", s);
      res = 11;
      break;
    }
  fputc ('\n', log);
  fprintf (log, "[runlim] result:\t\t%d\n", res);
  fflush (log);

  fprintf (log, "[runlim] children:\t\t%d\n", children);
  fprintf (log, "[runlim] real:\t\t\t%.2f seconds\n", real);
  fprintf (log, "[runlim] time:\t\t\t%.2f seconds\n", max_seconds);
  fprintf (log, "[runlim] space:\t\t\t%.1f MB\n", max_mb);
  fprintf (log, "[runlim] samples:\t\t%ld\n", num_samples);

  fflush (log);
  if (close_log) fclose (log);

  if (buffer) free (buffer);
  if (path) free (path);

  restore_signal_handlers ();

  if (propagate_signals)
    {
      switch (ok)
	{
	case OK:
	case OUT_OF_TIME:
	case OUT_OF_MEMORY:
	case FORK_FAILED:
	case INTERNAL_ERROR:
	case EXEC_FAILED:
	  break;
	default:
	  kill (parent_pid, s);
	  break;
	}
    }

  return res;
}
