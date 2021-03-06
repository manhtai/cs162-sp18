#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_wait(unused struct tokens *tokens);
int fork_then_exec(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_cd, "cd", "change directory"},
  {cmd_pwd, "pwd", "output current working directory"},
  {cmd_wait, "wait", "wait for background processes to stop"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* cd & pwd from libc */
int cmd_cd(struct tokens *tokens) {
  char* cd = tokens_get_token(tokens, 1);
  if (cd == NULL)
    cd = getenv("HOME");

  if (chdir(cd) == 0)
    printf("%s\n", cd);
  else
    perror("cd error");
  return 1;;
}

int cmd_pwd(unused struct tokens *tokens) {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != NULL)
    printf("%s\n", cwd);
  else
    perror("getcwd() error");
  return 1;
}

int cmd_wait(unused struct tokens *tokens) {
  int status;
  pid_t pid;
  while ((pid = wait(&status)) > 0) {
    printf("process [%d] terminated.\n", pid);
  }
  return 1;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Check whether a program p is excutable, if yes, return, if not find in PATH */
char* get_executable(char *p) {
  struct stat sb;

  /* check if p is absolute path */
  if (strstr(p, "/")) {
    return p;
  }

  /* check if p executable */
  if (stat(p, &sb) == 0 && sb.st_mode & S_IXUSR) {
    return p;
  }

  /* find p in PATH */
  char* path = strdup(getenv("PATH"));
  size_t p_len = strlen(p);
  char *token = strtok(path, ":");

  while(token) {
    char* fullpath = malloc(p_len + strlen(token) + 2);
    sprintf(fullpath, "%s/%s", token, p);

    /* check if fullpath executable */
    if (stat(fullpath, &sb) == 0 && sb.st_mode & S_IXUSR) {
      return fullpath;
    }

    token = strtok(NULL, ":");
    free(fullpath);
  }
  free(path);
  return p;
}


  /* Ignore & unignore signals */
void ignore_signal(void) {
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);
}

void unignore_signal(void) {
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);
}


/* execute command from file */
int fork_then_exec(struct tokens *tokens) {
  size_t n = tokens_get_length(tokens);

  if (n == 0) return 1;

  /* set argv to run */
  char** argv = malloc((n+1)*sizeof(char *));
  for(int j=0;j<n;j++) {
    argv[j] = tokens_get_token(tokens, j);
  }
  argv[0] = get_executable(argv[0]);
  argv[n] = NULL;

  /* check for & */
  int bg = 0;
  if (strcmp(argv[n-1], "&") == 0) {
    bg = 1;
    argv[n-1] = NULL;
    n--;
  }

  int child_pid = fork();
  if (child_pid == 0) { /* I'm child */
    /* check for < and > */
    int in_out = -1;
    if (n > 2 && (strcmp(argv[n-2], "<") == 0 || strcmp(argv[n-2], ">") == 0)) {
      if (strcmp(argv[n-2], "<") == 0) {
          freopen(argv[n-1], "r", stdin);
          in_out = 0;
      } else {
          freopen(argv[n-1], "w", stdout);
          in_out = 1;
      }
      argv[n-2] = NULL;
      argv[n-1] = NULL;
    }

    /* unignore signal */
    unignore_signal();

    /* exec */
    int result = execv(argv[0], argv);
    if (result == -1) perror("execv() error");

    /* close our open fd */
    if (in_out == 0) {
      fclose(stdin);
    } else if (in_out == 1) {
      fclose(stdout);
    }
  } else { /* I'm parent */
    if (!bg) {
      int status;
      wait(&status);
    } else {
      printf("[%d]: %s\n", child_pid, argv[0]);
    }
  }

  free(argv);
  return 1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Put shell in its own process group */
    setpgid(shell_pgid, shell_pgid);

    /* ignore signal */
    ignore_signal();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      fork_then_exec(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
