/* $begin shellmain */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <spawn.h>
#include <sys/stat.h>

#define MAXARGS 128
#define MAXLINE 8192 /* Max text line length */

typedef enum { IS_SIMPLE, IS_PIPE, IS_INPUT_REDIR, IS_OUTPUT_REDIR, IS_INPUT_OUTPUT_REDIR, IS_SEQ, IS_ANDIF} Mode; /* simple command, |, >, <, ;, && */
typedef struct { 
    char *argv[MAXARGS]; /* Argument list */
    int argc; /* Number of args */
    int bg; /* Background job? */
    Mode mode; /* Handle special cases | > < ; */
} parsed_args; 

extern char **environ; /* Defined by libc */

/* Function prototypes */
void eval(char *cmdline);
parsed_args parseline(char *buf);
int builtin_command(char **argv, pid_t pid, int status);
void signal_handler(int sig);
int exec_cmd(char** argv, posix_spawn_file_actions_t *actions, pid_t *pid, int *status, int bg);
int find_index(char** argv, char* target); 

void unix_error(char *msg) /* Unix-style error */
{
  fprintf(stderr, "%s: %s\n", msg, strerror(errno));
  exit(EXIT_FAILURE);
}

int main() {
  char cmdline[MAXLINE]; /* Command line */
  /* TODO: register signal handlers */
  signal( SIGCHLD, signal_handler );
  signal(SIGINT, signal_handler);
  
  while (1) {
    char *result;
    /* Read */
    printf("CS361 >"); /* TODO: correct the prompt */
    result = fgets(cmdline, MAXLINE, stdin);
    if (result == NULL && ferror(stdin)) {
      fprintf(stderr, "fatal fgets error\n");
      exit(EXIT_FAILURE);
    }

    if (feof(stdin)) exit(EXIT_SUCCESS);

    /* Evaluate */
    eval(cmdline);
  }
}
/* $end shellmain */

/* $begin eval */
/* eval - Evaluate a command line */
void eval(char *cmdline) {
  char buf[MAXLINE];   /* Holds modified command line */
  pid_t pid, pid1;           /* Process id */
  int status;          /* Process status */
  int pipe_fds[2];
  posix_spawn_file_actions_t actions; /* used in performing spawn operations */
  posix_spawn_file_actions_t actions1;
  posix_spawn_file_actions_init(&actions); 

  strcpy(buf, cmdline);
  parsed_args parsed_line = parseline(buf);
  if (parsed_line.argv[0] == NULL) return; /* Ignore empty lines */

  /* Not a bultin command */
  if (!builtin_command(parsed_line.argv, pid, status)) {
    switch (parsed_line.mode) {
      case IS_SIMPLE: /* cmd argv1 argv2 ... */
        if (!exec_cmd(parsed_line.argv, &actions, &pid, &status, parsed_line.bg)) return;
        break;
      case IS_PIPE: /* command1 args | command2 args */
        posix_spawn_file_actions_init(&actions1);
        pipe(pipe_fds);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        posix_spawn_file_actions_adddup2(&actions1, pipe_fds[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions1, pipe_fds[1]);
        int pipe_index = find_index(parsed_line.argv, "|");
        char *pipe_command_one[] = {parsed_line.argv[0], parsed_line.argv[1], NULL};
        for(int i = 0; i < pipe_index; i++){
          pipe_command_one[i] = parsed_line.argv[i];
        }
        pipe_command_one[pipe_index] = NULL;
    
        char *pipe_command_two[] = {parsed_line.argv[pipe_index+1], parsed_line.argv[pipe_index+2], NULL};
        int i = pipe_index + 1;
        int j = 0;
        while(parsed_line.argv[i] != NULL){
          pipe_command_two[j] = parsed_line.argv[i];
          i++;
          j++;
        }
        pipe_command_two[j+1] = NULL;
        if (0 != posix_spawnp(&pid, pipe_command_one[0], &actions, NULL, pipe_command_one, environ)) {
          perror("spawn failed");
          exit(1);
        }
        if (0 != posix_spawnp(&pid1, pipe_command_two[0], &actions1, NULL, pipe_command_two, environ)) {
          perror("spawn failed"); 
          exit(1);
        }
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        waitpid(pid, &status, 0);
        waitpid(pid1, &status, 0);
        break;
      case IS_OUTPUT_REDIR: /* command args > output_redirection */
        puts("ok");
        int output_redirection_index = find_index(parsed_line.argv, ">");
        int output_redirection_file = open(parsed_line.argv[output_redirection_index+1], O_RDWR|O_CREAT|O_APPEND, 0600);
        int save_output_redirection = dup(fileno(stdout));
        int save_err_redirection = dup(fileno(stderr));
        if (-1 == dup2(1, output_redirection_file)) { perror("cannot redirect stdout"); }
        char *output_redirection_command[] = {">", NULL};
        for(int i = 0; i<output_redirection_index; i++){
          output_redirection_command[i] = parsed_line.argv[i];
        }
        output_redirection_command[output_redirection_index] = NULL;
        freopen(parsed_line.argv[output_redirection_index+1], "a+", stdout);
        if (0 != posix_spawnp(&pid, output_redirection_command[0], &actions, NULL, output_redirection_command, environ)) {
          perror("spawn failed");
          exit(1);
        }
        fflush(stdout); 
        close(output_redirection_file);
        fflush(stderr); 
        dup2(save_output_redirection, fileno(stdout));
        dup2(save_err_redirection, fileno(stderr));
        waitpid(pid, &status, 0);
        close(save_output_redirection);
        close(save_err_redirection);
        break;
      case IS_INPUT_REDIR: /* command args < input_redirection */
        puts("ok");
        int input_command_index = find_index(parsed_line.argv, "<");
        int input_redirection_file = open(parsed_line.argv[input_command_index+1], O_RDWR|O_CREAT|O_APPEND, 0600);
        int input_stdin = dup(fileno(stdin));
        int stderr_stdin = dup(fileno(stderr));
        char *input_redirection_cmd[] = {"1", NULL};
        for(int i = 0; i<input_command_index; i++){
          input_redirection_cmd[i] = parsed_line.argv[i];
        }
        input_redirection_cmd[input_command_index] = NULL;
        freopen(parsed_line.argv[input_command_index+1], "a+", stdin);
        if (0 != posix_spawnp(&pid, input_redirection_cmd[0], &actions, NULL, input_redirection_cmd, environ)) {
          perror("spawn failed");
          exit(1);
        }
        close(input_redirection_file);
        dup2(input_stdin, fileno(stdin));
        dup2(stderr_stdin, fileno(stderr));
        waitpid(pid, &status, 0);
        close(input_stdin);
        close(stderr_stdin);
        break;
      case IS_INPUT_OUTPUT_REDIR: /* command args < input_redirection > output_redirection */
        // TODO: handle input output redirection 
        puts("ok");
        int input_output_redirection_index = find_index(parsed_line.argv, "<");
        int input_output_redirection_index_two = find_index(parsed_line.argv, ">");
        int input_output_redirection_file = open(parsed_line.argv[input_output_redirection_index+1], O_RDWR|O_CREAT|O_APPEND, 0600);
        int input_output_redirection_stdin = dup(fileno(stdin));
        int input_output_redirection_stderr = dup(fileno(stderr));
        char *input_output_redirection_command_one[] = {"1", NULL};
        for(int i = 0; i<input_output_redirection_index; i++){
          input_output_redirection_command_one[i] = parsed_line.argv[i];
        }
        input_output_redirection_command_one[input_output_redirection_index] = NULL;
        freopen(parsed_line.argv[input_output_redirection_index+1], "a+", stdin);
        if (0 != posix_spawnp(&pid, input_output_redirection_command_one[0], &actions, NULL, input_output_redirection_command_one, environ)) {
          perror("spawn failed");
          exit(1);
        }
        int input_output_second_file = open(parsed_line.argv[input_output_redirection_index_two+1], O_RDWR|O_CREAT|O_APPEND, 0600);
        int input_output_redirection_stdout = dup(fileno(stdout));
        int input_output_redirection_sderr = dup(fileno(stderr));
        if (-1 == dup2(1, input_output_second_file)) { perror("cannot redirect stdout"); }
        char *input_output_redirection_command_two[] = {"1", NULL};
        for(int i = 0; i<input_output_redirection_index_two; i++){
          input_output_redirection_command_two[i] = parsed_line.argv[i];
        }
        input_output_redirection_command_two[input_output_redirection_index_two] = NULL;
        freopen(parsed_line.argv[input_output_redirection_index_two+1], "a+", stdout);
        if (0 != posix_spawnp(&pid, input_output_redirection_command_two[0], &actions, NULL, input_output_redirection_command_two, environ)) {
          perror("spawn failed");
          exit(1);
        }
        fflush(stdout); 
        close(input_output_second_file);
        fflush(stderr); 
        dup2(input_output_redirection_stdout, fileno(stdout));
        dup2(input_output_redirection_sderr, fileno(stderr));
        waitpid(pid, &status, 0);
        close(input_output_redirection_stdout);
        close(input_output_redirection_sderr);
        close(input_output_redirection_file);
        dup2(input_output_redirection_stdin, fileno(stdin));
        dup2(input_output_redirection_stderr, fileno(stderr));
        waitpid(pid, &status, 0);
        close(input_output_redirection_stdin);
        close(input_output_redirection_stderr);
        break;
      case IS_SEQ: /* command1 args ; command2 args */
        // TODO: handle sequential 
        puts("ok");
        int is_sequence_first_command_index = find_index(parsed_line.argv, ";");
        char *is_sequence_first_command[] = {parsed_line.argv[is_sequence_first_command_index], NULL};
        for(int i = 0; i<is_sequence_first_command_index; i++){
          is_sequence_first_command[i] = parsed_line.argv[i];
        }
        is_sequence_first_command[is_sequence_first_command_index] = NULL;
        int end_index = is_sequence_first_command_index + 1;
        char *is_sequence_second_command[] = {parsed_line.argv[is_sequence_first_command_index], NULL};
        int start = 0;
        while(parsed_line.argv[end_index] != NULL){
          is_sequence_second_command[start] = parsed_line.argv[end_index];
          end_index++;
          start++;
        }
        is_sequence_second_command[end_index] = NULL;
        if (0 != posix_spawnp(&pid, is_sequence_first_command[0], &actions, NULL, is_sequence_first_command, environ)) {
          perror("spawn failed");
          exit(1);
        }
        if (0 != posix_spawnp(&pid, is_sequence_second_command[0], &actions, NULL, is_sequence_second_command, environ)) {
          printf("this aint working cheif \n");
          perror("spawn failed");
          exit(1);
        }
        waitpid(pid, &status, 0);
        waitpid(pid1, &status, 0);
        break;
      case IS_ANDIF: /* command1 args && command2 args */
        // TODO: handle "and if"
        printf("composed && commands not yet implemented :(\n");
        break;
    }
    if (parsed_line.bg)
      printf("%d %s", pid, cmdline);
    
  }
  posix_spawn_file_actions_destroy(&actions);
  return;
}

/* If first arg is a builtin command, run it and return true */
int builtin_command(char **argv, pid_t pid, int status) {
  if (!strcmp(argv[0], "exit")) /* exit command */
    exit(EXIT_SUCCESS);
  if (!strcmp(argv[0], "&")) /* Ignore singleton & */
    return 1; 
  if (!strcmp(argv[0], "?")){
    printf("\npid:%d status:%d\n", pid, status);
    return 5;
  }
  return 0; /* Not a builtin command */
}
/* $end eval */

/* Run commands using posix_spawnp */
int exec_cmd(char** argv, posix_spawn_file_actions_t *actions, pid_t *pid, int *status, int bg) {
  if (0 != posix_spawnp(pid, argv[0], actions, NULL, argv, environ)) {
    perror("spawn failed");
    exit(1);
  }
  if (!bg)
    if (waitpid(*pid, status, 0) < 0) unix_error("waitfg: waitpid error");
  return 1;
}
/* $end exec_cmd */

/* signal handler */
void signal_handler(int sig) {
  if(sig == SIGINT){
    printf("Caught SIGINT!\n");
    waitpid(-1, NULL, 0);
  }
  if(sig == SIGTSTP){
  printf("Caught SIGTSTP!\n");
   waitpid(-1, NULL, 0);
  }
 if(sig == SIGCHLD){
   waitpid(-1, NULL, WNOHANG);
 }
  // TODO: handle SIGINT and SIGTSTP and SIGCHLD signals here
}

/* finds index of the matching target in the argumets */
int find_index(char** argv, char* target) {
  for (int i=0; argv[i] != NULL; i++)
    if (!strcmp(argv[i], target))
      return i;
  return 0;
}

/* $begin parseline */
/* parseline - Parse the command line and build the argv array */
parsed_args parseline(char *buf) {
  char *delim; /* Points to first space delimiter */
  parsed_args pa;

  buf[strlen(buf) - 1] = ' ';   /* Replace trailing '\n' with space */
  while (*buf && (*buf == ' ')) /* Ignore leading spaces */
    buf++;

  /* Build the argv list */
  pa.argc = 0;
  while ((delim = strchr(buf, ' '))) {
    pa.argv[pa.argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')){ /* Ignore spaces */
      buf++;
    }
  }
  pa.argv[pa.argc] = NULL;

  if (pa.argc == 0){ /* Ignore blank line */
    return pa;
  }

  /* Should the job run in the background? */
  if ((pa.bg = (*pa.argv[pa.argc - 1] == '&')) != 0) pa.argv[--pa.argc] = NULL;

  /* Detect various command modes */
  pa.mode = IS_SIMPLE;
  if (find_index(pa.argv, "|"))
    pa.mode = IS_PIPE;
  else if(find_index(pa.argv, ";")) 
    pa.mode = IS_SEQ; 
  else if(find_index(pa.argv, "&&"))
    pa.mode = IS_ANDIF;
  else {
    if(find_index(pa.argv, "<")) 
      pa.mode = IS_INPUT_REDIR;
    if(find_index(pa.argv, ">")){
      if (pa.mode == IS_INPUT_REDIR)
        pa.mode = IS_INPUT_OUTPUT_REDIR;
      else
        pa.mode = IS_OUTPUT_REDIR; 
    }
  }

  return pa;
}
/* $end parseline */