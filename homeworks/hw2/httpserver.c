#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"
#include "threadpool.h"

#define MAX_BUFF 8192

/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
wq_t work_queue;
int num_threads;
int server_port;
char *server_files_directory;
char *server_proxy_hostname;
int server_proxy_port;

char buffer[MAX_BUFF];

int cat(char* filename, char** result);
int ls(char* dir, char** result);
char* serve_directory(char* dir, char** result, int* size);

void http200(int fd, char* message, char* mime_type, int size);
void http404(int fd);
void http500(int fd);

static void* handle_proxy_routine(int is_client, struct sock_fd* sock_fd);
static void* handle_proxy_routine_c(void* sock_fd);
static void* handle_proxy_routine_p(void* sock_fd);

/*
 * Reads an HTTP request from stream (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 */
void handle_files_request(int fd) {
  struct http_request *request = http_request_parse(fd);

  char* full_path = malloc(strlen(server_files_directory) + strlen(request->path) + 1);
  sprintf(full_path, "%s%s", server_files_directory, request->path);

  char* content = malloc(1);
  int size = 0;
  char* mime_type = serve_directory(full_path, &content, &size);
  size = size ? size : strlen(content);
  free(full_path);

  if (content == NULL || strlen(content) == 0) {
    http404(fd);
    return;
  }
  http200(fd, content, mime_type, size);
}


/* Display list files in directory, or server index.html, return mime type */
char* serve_directory(char* dir, char** result, int* size) {
  char* mime_type = http_get_mime_type("index.html");

  /* If it's a file */
  int is_dir = ls(dir, result);
  if (is_dir == 0) {
    *size = cat(dir, result);
    if (*size) {
      mime_type = http_get_mime_type(dir);
    }
    return mime_type;
  }

  /* If directory contains no index.html */
  if (strstr(*result, "index.html") == NULL) {
    return mime_type;
  }

  /* If directory contains a index.html */
  char* index = 0;
  char* filename = malloc(strlen(server_files_directory) + strlen("index.html") + 2);
  sprintf(filename, "%s/%s", server_files_directory, "index.html");

  *size = cat(filename, &index);
  if (*size) {
    free(*result);
    *result = index;
  } else {
    free(index);
  }
  free(filename);
  return mime_type;
}


/* Get content of filename then put to result, return file size if it's a file, 0 otherwise */
int cat(char* filename, char** result) {
  int length = 1 << 17; /* Max file size allowed */
  int size = 0;
  FILE* f = fopen(filename, "rb");

  if (f == NULL) {
    return 0;
  }

  *result = malloc(length);
  if (*result) {
    size = fread(*result, 1, length, f);
  }
  fclose(f);
  return size;
}

/* List files & sub directories in a directory, return 1 if it's a directory, 0 otherwise */
int ls(char* dir, char** result) {
    struct dirent *de;
    size_t size = 0;

    DIR *dr = opendir(dir);

    if (dr == NULL) {
      return 0;
    }

    /* Header part */
    char* header = "<h1>Index</h1>";
    size = strlen(header)+1;
    *result = realloc(*result, size);
    strcat(*result, header);

    char* line_template = "<a href='%s'>%s</a>";
    char* name = malloc(1);
    while ((de = readdir(dr)) != NULL) {
      size += 2*(de->d_namlen)+strlen(line_template)+strlen("<br />")+1;
      *result = realloc(*result, size);
      if (*result == NULL) {
        break;
      }
      name = realloc(name, 2*(de->d_namlen)+strlen(line_template));
      sprintf(name, line_template, de->d_name, de->d_name);
      strcat(*result, name);
      strcat(*result, "<br />");
    }
    closedir(dr);

    free(name);
    return 1;
}

void http200(int fd, char* message, char* mime_type, int size) {
  char content_length[64];
  sprintf(content_length, "%d", size);
  http_start_response(fd, 200);
  http_send_header(fd, "Content-type", mime_type);
  http_send_header(fd, "Content-Length", content_length);
  http_send_header(fd, "Server", "httpserver/1.0");
  http_end_headers(fd);
  http_send_data(fd, message, size);
}

void http500(int fd) {
  http_start_response(fd, 500);
  http_send_header(fd, "Content-Type", "text/html");
  http_send_header(fd, "Server", "httpserver/1.0");
  http_end_headers(fd);
  http_send_string(fd, "Internal server error.");
}

void http404(int fd) {
  http_start_response(fd, 404);
  http_send_header(fd, "Content-Type", "text/html");
  http_send_header(fd, "Server", "httpserver/1.0");
  http_end_headers(fd);
  http_send_string(fd,
      "<center>"
      "<h1>404 - Not found!</h1>"
      "</center>");
}


/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream fd and the
 * proxy target. HTTP requests from the client (fd) should be sent to the
 * proxy target, and HTTP responses from the proxy target should be sent to
 * the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 */
void handle_proxy_request(int fd) {

  /*
  * The code below does a DNS lookup of server_proxy_hostname and 
  * opens a connection to it. Please do not modify.
  */

  struct sockaddr_in target_address;
  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  struct hostent *target_dns_entry = gethostbyname2(server_proxy_hostname, AF_INET);

  int client_socket_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (client_socket_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    exit(ENXIO);
  }

  char *dns_address = target_dns_entry->h_addr_list[0];

  memcpy(&target_address.sin_addr, dns_address, sizeof(target_address.sin_addr));
  int connection_status = connect(client_socket_fd, (struct sockaddr*) &target_address,
      sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    http_request_parse(fd);

    http_start_response(fd, 502);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    http_send_string(fd, "<center><h1>502 Bad Gateway</h1><hr></center>");
    return;
  }

  /* Our code here */
  pthread_t client_to_proxy_t;
  pthread_t proxy_to_client_t;

  struct sock_fd* s;

  s = (struct sock_fd*) malloc(sizeof(struct sock_fd*));

  s->p_fd = fd;
  s->c_fd = client_socket_fd;

  if (pthread_create(&client_to_proxy_t, NULL, handle_proxy_routine_c, (void*) s) != 0) {
    perror("Error create client_to_proxy thread");
  }

  if (pthread_create(&proxy_to_client_t, NULL, handle_proxy_routine_p, (void*) s) != 0) {
    perror("Error create proxy_to_client thread");
  }

  pthread_join(proxy_to_client_t, NULL);
  pthread_join(client_to_proxy_t, NULL);
  free(s);
}

static void* handle_proxy_routine_c(void* sock) {
  struct sock_fd *s = (struct sock_fd *) sock;
  return handle_proxy_routine(1, s);
}

static void* handle_proxy_routine_p(void* sock) {
  struct sock_fd *s = (struct sock_fd *) sock;
  return handle_proxy_routine(0, s);
}


static void* handle_proxy_routine(int is_client, struct sock_fd* s) {
  int nread;
  int c_fd;
  int p_fd;

  if (is_client) {
    c_fd = s->c_fd;
    p_fd = s->p_fd;
  } else {
    p_fd = s->c_fd;
    c_fd = s->p_fd;
  }

  do {
    if ((nread = read(p_fd, buffer, MAX_BUFF)) < 0) {
      perror("Can't not read");
      break;
    }
    printf("Send to %d\n", c_fd);

    http_send_data(c_fd, buffer, nread);
  } while (0);

  return NULL;
}

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *socket_number, void (*request_handler)(int)) {

  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
        sizeof(socket_option)) == -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  if (bind(*socket_number, (struct sockaddr *) &server_address,
        sizeof(server_address)) == -1) {
    perror("Failed to bind on socket");
    exit(errno);
  }

  if (listen(*socket_number, 1024) == -1) {
    perror("Failed to listen on socket");
    exit(errno);
  }

  printf("Listening on port %d with %d threads...\n", server_port, num_threads);

  threadpool* thpool = thread_pool_init(num_threads, &work_queue, request_handler);
  if (thpool == NULL) {
    perror("Can't init threadpool");
    exit(errno);
  }

  while (1) {
    client_socket_number = accept(*socket_number,
        (struct sockaddr *) &client_address,
        (socklen_t *) &client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d\n",
        inet_ntoa(client_address.sin_addr),
        client_address.sin_port);

    thread_pool_add(thpool, client_socket_number);
  }

  thread_pool_shutdown(thpool);
  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

int server_fd;
void signal_callback_handler(int signum) {
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char *USAGE =
  "Usage: ./httpserver --files www_directory/ --port 8000 [--num-threads 5]\n"
  "       ./httpserver --proxy inst.eecs.berkeley.edu:80 --port 8000 [--num-threads 5]\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  signal(SIGINT, signal_callback_handler);

  /* Default settings */
  server_port = 8000;
  void (*request_handler)(int) = NULL;

  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      free(server_files_directory);
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char *proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char *colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char *server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char *num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (!num_threads) {
    num_threads = 1;
  }

  if (server_files_directory == NULL && server_proxy_hostname == NULL) {
    fprintf(stderr, "Please specify either \"--files [DIRECTORY]\" or \n"
                    "                      \"--proxy [HOSTNAME:PORT]\"\n");
    exit_with_usage();
  }

  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}
