#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>

CURL *easy;
CURLM *multi;
int timeout;
bool perform = true;
struct pollfd* pfd;
int num_open_fds = 0;

int socket_callback(CURL* easy, curl_socket_t s, int what, void* clientp, void* socketp) {
  switch (what) {
  case CURL_POLL_IN:
    printf("BLAH socket_cb CURL_POLL_IN %d\n", s);
    pfd[0].fd = s;
    pfd[0].events = POLLIN;
    num_open_fds = 1;
    break;
  case CURL_POLL_OUT:
    printf("BLAH socket_cb CURL_POLL_OUT %d\n", s);
    pfd[0].fd = s;
    pfd[0].events = POLLOUT;
    num_open_fds = 1;
    break;
  case CURL_POLL_INOUT:
    printf("BLAH socket_cb CURL_POLL_INOUT %d\n", s);
    pfd[0].fd = s;
    pfd[0].events = POLLIN & POLLOUT;
    num_open_fds = 1;
    break;
  case CURL_POLL_REMOVE:
    printf("BLAH socket_cb CURL_POLL_REMOVE %d\n", s);
    pfd[0].fd = s;
    pfd[0].events = 0;
    num_open_fds = 0;
    break;
  default:
    printf("BLAH unexpected what!!!\n");
    exit(1);
  }
  return 0;
}

int timer_callback(CURLM *multi, long timeout_ms, void* clientp) {
  printf("BLAH timer_cb %d\n", timeout_ms);
  timeout = timeout_ms;
  return 0;
}

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  printf("BLAH write_cb\n");
  perform = false;
  return size * nmemb;
}

void setup() {
  pfd = calloc(1, sizeof(struct pollfd));
  curl_global_init(CURL_GLOBAL_DEFAULT);
  easy = curl_easy_init();
  curl_easy_setopt(easy, CURLOPT_URL, "http://techie.net");
  multi = curl_multi_init();
  curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socket_callback);
  curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timer_callback);
}

void partial_transfer(){
  int retval;
  int running;
  int ev_bitmask;
  perform = true;
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(easy, CURLOPT_VERBOSE, 1);
  curl_multi_add_handle(multi, easy);
  curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running);
  while (perform) {
    printf("BLAH perform %d\n", running);
    retval = poll(pfd, num_open_fds, 1000);
    if (retval == 0) {
      curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running);
    } else if (retval > 0) {
      ev_bitmask = 0;
      if (pfd[0].revents & POLLIN)
	ev_bitmask |= CURL_CSELECT_IN;
      if (pfd[0].revents & POLLOUT)
	ev_bitmask |= CURL_CSELECT_OUT;
      printf("BLAH action %d\n", ev_bitmask);
      curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, pfd[0].fd, &running);
    } else {
      printf("BLAH UH OH!!!\n");
      exit(1);
    }
  }
}

int main() {
  setup();
  partial_transfer();
}
