/*
 * tsh - A tiny shell program with job control
 *
 * Gani Raissov graissov
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include "stdbool.h"
#include "csapp.h"

/* Misc manifest constants */
#define MAXLINE_TSH 1024 /* max line size */
#define MAXARGS 128      /* max args on a command line */
#define MAXJOBS 16       /* max jobs at any point in time */
#define MAXJID 1 << 16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL 0x0  /* next token is an argument */
#define ST_INFILE 0x1  /* next token is the input file */
#define ST_OUTFILE 0x2 /* next token is the output file */

/* Global variables */
extern char **environ;   /* defined in libc */
char prompt[] = "tsh> "; /* command line prompt (DO NOT CHANGE) */
int verbose = 0;         /* if true, print additional output */
int nextjid = 1;         /* next job ID to allocate */
char sbuf[MAXLINE_TSH];  /* for composing sprintf messages */

struct job_t
{                              /* The job struct */
    pid_t pid;                 /* job PID */
    int jid;                   /* job ID [1, 2, ...] */
    int state;                 /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE_TSH]; /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens
{
    int argc;            /* Number of arguments */
    char *argv[MAXARGS]; /* The arguments list */
    char *infile;        /* The input file */
    char *outfile;       /* The output file */
    enum builtins_t
    { /* Indicates if argv[0] is a builtin command */
      BUILTIN_NONE,
      BUILTIN_QUIT,
      BUILTIN_JOBS,
      BUILTIN_BG,
      BUILTIN_FG
    } builtins;
};

/* End global variables */

/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list);
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid);
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE_TSH]; /* cmdline for fgets */
    int emit_prompt = 1;       /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF)
    {
        switch (c)
        {
        case 'h': /* print help message */
            usage();
            break;
        case 'v': /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':            /* don't print a prompt */
            emit_prompt = 0; /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT, sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler); /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler); /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(job_list);

    /* Execute the shell's read/eval loop */
    while (1)
    {

        if (emit_prompt)
        {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin))
        {
            /* End of file (ctrl-d) */
            printf("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }

        /* Remove the trailing newline */
        cmdline[strlen(cmdline) - 1] = '\0';

        /* Evaluate the command line */
        eval(cmdline);

        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

// bg_handler changing a stopped background job into a running background job.
void bg_handler(struct cmdline_tokens *tok)
{
    // define a job pointer
    struct job_t *job;

    // check if the job is specified by its job ID
    if (tok->argv[1][0] == '%')
    {
        int jid = atoi(&tok->argv[1][1]);
        job = getjobjid(job_list, jid);
    }
    else
    {
        // if not specified by job ID, get it by process ID
        pid_t pid = atoi(tok->argv[1]);
        job = getjobpid(job_list, pid);
    }

    // if the job is not found, print an error message and return
    if (!job)
    {
        unix_error("Job not found\n");
        return;
    }

    // set the job's state to background
    job->state = BG;
    // send a continue signal to the job
    kill(-(job->pid), SIGCONT);
    // print the job's details
    printf("[%d] (%d) %s\n", job->jid, job->pid, job->cmdline);
}

// fg_handler - changing a stopped background job into a running  foreground job
void fg_handler(struct cmdline_tokens *tok)
{
    // define a job pointer and a status variable
    struct job_t *job;
    int status;

    // check if the job is specified by its job ID (starts with '%')
    if (tok->argv[1][0] == '%')
    {
        int jid = atoi(&tok->argv[1][1]);
        job = getjobjid(job_list, jid);
    }
    else
    {
        // if not specified by job ID, get it by process ID
        pid_t pid = atoi(tok->argv[1]);
        job = getjobpid(job_list, pid);
    }

    // if the job is not found, print an error message and return
    if (!job)
    {
        unix_error("Job not found\n");
        return;
    }

    // send a continue signal to the job
    kill(-(job->pid), SIGCONT);

    // set the job's state to foreground
    job->state = FG;

    // wait for the foreground job to terminate or be stopped
    waitpid(job->pid, &status, WUNTRACED);

    // check if the job was stopped
    if (WIFSTOPPED(status))
    {
        job->state = ST;
    }
    else
    {
        // if the job terminated, delete it from the job list
        deletejob(job_list, job->pid);
    }
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */

void eval(char *cmdline)
{
    // define necessary variables and data structures
    sigset_t set;
    int bg;
    pid_t pid;
    struct cmdline_tokens tok;

    // parse the command line input
    bg = parseline(cmdline, &tok);

    // if parsing returns -1, return from the function
    if (bg == -1)
        return;

    // if no command is provided, return from the function
    if (tok.argv[0] == NULL)
        return;

    // handle built-in commands
    switch (tok.builtins)
    {
    case BUILTIN_QUIT:
        // exit the shell
        exit(0);
        break;
    case BUILTIN_JOBS:
        // list the jobs
        // if output file is specified, redirect output to the file
        if (tok.outfile != NULL)
        {
            int out_fd = open(tok.outfile, O_WRONLY);
            if (out_fd < 0)
            {
                unix_error("error opening file");
                return;
            }
            listjobs(job_list, out_fd);
            close(out_fd);
        }
        else
            listjobs(job_list, STDOUT_FILENO);
        return;
    case BUILTIN_BG:
        // handle background jobs
        bg_handler(&tok);
        return;
    case BUILTIN_FG:
        // handle foreground jobs
        fg_handler(&tok);
        return;
    default:
        break;
    }

    // if no built-in command is matched, proceed with external commands
    if (tok.builtins == BUILTIN_NONE)
    {
        // block certain signals to handle race conditions
        sigemptyset(&set);
        sigaddset(&set, SIGCHLD);
        sigaddset(&set, SIGTSTP);
        sigaddset(&set, SIGINT);
        sigprocmask(SIG_BLOCK, &set, NULL);

        // create a child process
        pid = fork();

        // handle fork error
        if (pid < 0)
            unix_error("error with fork");

        // child process code
        if (pid == 0)
        {
            // set process group ID for the child
            setpgid(0, 0);
            // unblock signals in child
            sigprocmask(SIG_UNBLOCK, &set, NULL);

            // handle input redirection
            if (tok.infile != NULL)
            {
                int in_fd = open(tok.infile, O_RDONLY);
                if (in_fd < 0)
                {
                    unix_error("error opening file");
                }
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }

            // handle output redirection
            if (tok.outfile != NULL)
            {
                int out_fd = open(tok.outfile, O_WRONLY);
                if (out_fd < 0)
                {
                    unix_error("error opening file");
                }
                dup2(out_fd, STDOUT_FILENO);
                close(out_fd);
            }

            // execute the command
            if (execve(tok.argv[0], tok.argv, environ) < 0)
            {
                printf("%s: Command not found.\n", tok.argv[0]);
                exit(0);
            }
        }

        // parent process code
        // add the child process to the job list
        addjob(job_list, pid, bg + 1, cmdline);
        // unblock signals in parent
        sigprocmask(SIG_UNBLOCK, &set, NULL);

        // if it's a background process, print its details
        if (bg)
        {
            printf("[%d] (%d) %s\n", pid2jid(pid), pid, cmdline);
        }
        // if it's a foreground process, wait for it to complete
        else
        {
            // block all signals
            sigset_t mask, prev_mask;
            sigfillset(&mask);
            sigprocmask(SIG_SETMASK, &mask, &prev_mask);

            // wait for SIGCHLD signal
            while (pid == fgpid(job_list))
                sigsuspend(&prev_mask);

            // restore the signal mask
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
        }
    }
    return;
}
/*
 * parseline - Parse the command line and build the argv array.
 *
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters
 *             enclosed in single or double quotes are treated as a single
 *             argument.
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job
 *  -1:        if cmdline is incorrectly formatted
 *
 * Note:       The string elements of tok (e.g., argv[], infile, outfile)
 *             are statically allocated inside parseline() and will be
 *             overwritten the next time this function is invoked.
 */
int parseline(const char *cmdline, struct cmdline_tokens *tok)
{

    static char array[MAXLINE_TSH];    /* holds local copy of command line */
    const char delims[10] = " \t\r\n"; /* argument delimiters (white-space) */
    char *buf = array;                 /* ptr that traverses command line */
    char *next;                        /* ptr to the end of the current arg */
    char *endbuf;                      /* ptr to end of cmdline string */
    int is_bg;                         /* background job? */

    int parsing_state; /* indicates if the next token is the
                          input or output file */

    if (cmdline == NULL)
    {
        (void)fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void)strncpy(buf, cmdline, MAXLINE_TSH);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf)
    {
        /* Skip the white-spaces */
        buf += strspn(buf, delims);
        if (buf >= endbuf)
            break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<')
        {
            if (tok->infile)
            {
                (void)fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>')
        {
            if (tok->outfile)
            {
                (void)fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"')
        {
            /* Detect quoted tokens */
            buf++;
            next = strchr(buf, *(buf - 1));
        }
        else
        {
            /* Find next delimiter */
            next = buf + strcspn(buf, delims);
        }

        if (next == NULL)
        {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void)fprintf(stderr, "Error: unmatched %c.\n", *(buf - 1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the i/o file */
        switch (parsing_state)
        {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void)fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS - 1)
            break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL)
    {
        (void)fprintf(stderr,
                      "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0) /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit"))
    { /* quit command */
        tok->builtins = BUILTIN_QUIT;
    }
    else if (!strcmp(tok->argv[0], "jobs"))
    { /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    }
    else if (!strcmp(tok->argv[0], "bg"))
    { /* bg command */
        tok->builtins = BUILTIN_BG;
    }
    else if (!strcmp(tok->argv[0], "fg"))
    { /* fg command */
        tok->builtins = BUILTIN_FG;
    }
    else
    {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc - 1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The
 *     handler reaps all available zombie children, but doesn't wait
 *     for any other currently running children to terminate.
 */
void sigchld_handler(int sig)
{
    pid_t pid; // process ID for child
    int stat;  // status for waitpid

    // loop to reap all terminated child processes
    while ((pid = waitpid(-1, &stat, WNOHANG | WUNTRACED)) > 0)
    {
        // check if child process was stopped
        if (WIFSTOPPED(stat))
        {
            struct job_t *cur_job = getjobpid(job_list, pid); // get the job from job list
            cur_job->state = ST;                              // set the job state to stopped
            sio_puts("Job [");
            sio_putl(pid2jid(pid));
            sio_puts("] (");
            sio_putl(pid);
            sio_puts(") stopped by signal ");
            sio_putl(WSTOPSIG(stat));
            sio_puts("\n");
        }
        // check if child process was terminated by a signal
        if (WIFSIGNALED(stat))
        {
            sio_puts("Job [");
            sio_putl(pid2jid(pid));
            sio_puts("] (");
            sio_putl(pid);
            sio_puts(") terminated by signal ");
            sio_putl(WTERMSIG(stat));
            sio_puts("\n");
            deletejob(job_list, pid); // remove the job from job list
        }
        // check if child process exited normally
        if (WIFEXITED(stat))
            deletejob(job_list, pid); // remove the job from job list
    }
    return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig)
{
    // Get the process ID of the current foreground job
    pid_t fg_pid = fgpid(job_list);
    // If there's a foreground job (fg_pid is not 0)
    // Try to send a SIGINT signal to the foreground job
    if (fg_pid && kill(-fg_pid, SIGINT) < 0)
    {
        // If there's an error sending the signal
        unix_error("error with signal");
    }
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
    // Get the process ID of the current foreground job
    pid_t fg_pid = fgpid(job_list);
    // If there's a foreground job and an error occurs while sending the SIGTSTP signal
    if (fg_pid && kill(-fg_pid, SIGTSTP) < 0)
    {
        unix_error("error with signal");
    }
}
/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    unix_error("Terminating after receipt of SIGQUIT signal\n");
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job)
{
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *job_list)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *job_list)
{
    int i, max = 0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (job_list[i].pid == 0)
        {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if (verbose)
            {
                printf("Added job [%d] %d %s\n",
                       job_list[i].jid,
                       job_list[i].pid,
                       job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *job_list, pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (job_list[i].pid == pid)
        {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list) + 1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *job_list)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *job_list, pid_t pid)
{
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid)
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
        {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *job_list, int output_fd)
{
    int i;
    char buf[MAXLINE_TSH];

    for (i = 0; i < MAXJOBS; i++)
    {
        memset(buf, '\0', MAXLINE_TSH);
        if (job_list[i].pid != 0)
        {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if (write(output_fd, buf, strlen(buf)) < 0)
            {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE_TSH);
            switch (job_list[i].state)
            {
            case BG:
                sprintf(buf, "Running    ");
                break;

            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if (write(output_fd, buf, strlen(buf)) < 0)
            {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE_TSH);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if (write(output_fd, buf, strlen(buf)) < 0)
            {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}
