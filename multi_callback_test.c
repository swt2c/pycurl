#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <uv.h>

struct datauv {
  uv_timer_t timeout;
  uv_loop_t *loop;
  CURLM *multi;
};
 
typedef struct curl_context_s {
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
  struct datauv *uv;
} curl_context_t;

static curl_context_t *create_curl_context(curl_socket_t sockfd,
                                           struct datauv *uv)
{
  curl_context_t *context;
 
  context = (curl_context_t *) malloc(sizeof(*context));
 
  context->sockfd = sockfd;
  context->uv = uv;
 
  uv_poll_init_socket(uv->loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;
 
  return context;
}

/* callback from libuv on socket activity */
static void on_uv_socket(uv_poll_t *req, int status, int events)
{
  printf("BLAH socket_cb %d %d\n", status, events);
  int running_handles;
  int flags = 0;
  curl_context_t *context = (curl_context_t *) req->data;
  (void)status;
  if(events & UV_READABLE)
    flags |= CURL_CSELECT_IN;
  if(events & UV_WRITABLE)
    flags |= CURL_CSELECT_OUT;
 
  curl_multi_socket_action(context->uv->multi, context->sockfd, flags,
                           &running_handles);
  //check_multi_info(context);
}
 
/* callback from libuv when timeout expires */
static void on_uv_timeout(uv_timer_t *req)
{
  printf("BLAH uv_timeout\n");
  curl_context_t *context = (curl_context_t *) req->data;
  if(context) {
    int running_handles;
    curl_multi_socket_action(context->uv->multi, CURL_SOCKET_TIMEOUT, 0,
                             &running_handles);
    //check_multi_info(context);
  }
}

/* callback from libcurl to update the timeout expiry */
static int cb_timeout(CURLM *multi, long timeout_ms,
                      struct datauv *uv)
{
  printf("BLAH cb_timeout %d\n", timeout_ms);
  (void)multi;
  if(timeout_ms < 0)
    uv_timer_stop(&uv->timeout);
  else {
    if(timeout_ms == 0)
      timeout_ms = 1; /* 0 means call curl_multi_socket_action asap but NOT
                         within the callback itself */
    uv_timer_start(&uv->timeout, on_uv_timeout, (uint64_t)timeout_ms,
                   0); /* do not repeat */
  }
  return 0;
}

static void curl_close_cb(uv_handle_t *handle)
{
  curl_context_t *context = (curl_context_t *) handle->data;
  free(context);
}

static void destroy_curl_context(curl_context_t *context)
{
  uv_close((uv_handle_t *) &context->poll_handle, curl_close_cb);
}

/* callback from libcurl to update socket activity to wait for */
static int cb_socket(CURL *easy, curl_socket_t s, int action,
                     struct datauv *uv,
                     void *socketp)
{
  printf("BLAH cb_socket %d %d\n", s, action);
  curl_context_t *curl_context;
  int events = 0;
  (void)easy;
 
  switch(action) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    curl_context = socketp ?
      (curl_context_t *) socketp : create_curl_context(s, uv);
 
    curl_multi_assign(uv->multi, s, (void *) curl_context);
 
    if(action != CURL_POLL_IN)
      events |= UV_WRITABLE;
    if(action != CURL_POLL_OUT)
      events |= UV_READABLE;
 
    uv_poll_start(&curl_context->poll_handle, events, on_uv_socket);
    break;
  case CURL_POLL_REMOVE:
    if(socketp) {
      uv_poll_stop(&((curl_context_t*)socketp)->poll_handle);
      destroy_curl_context((curl_context_t*) socketp);
      curl_multi_assign(uv->multi, s, NULL);
    }
    break;
  default:
    abort();
  }
 
  return 0;
}

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

size_t write_callback(char *ptr, size_t size, size_t nmemb, CURL *easy) {
  printf("BLAH write_cb\n");
  exit(0);
  perform = false;
  return size * nmemb;
}

void setup() {
  //pfd = calloc(1, sizeof(struct pollfd));
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
  struct datauv uv = { 0 };
  int running_handles;
 
  curl_global_init(CURL_GLOBAL_ALL);
 
  uv.loop = uv_default_loop();
  uv_timer_init(uv.loop, &uv.timeout);
 
  uv.multi = curl_multi_init();
  curl_multi_setopt(uv.multi, CURLMOPT_SOCKETFUNCTION, cb_socket);
  curl_multi_setopt(uv.multi, CURLMOPT_SOCKETDATA, &uv);
  curl_multi_setopt(uv.multi, CURLMOPT_TIMERFUNCTION, cb_timeout);
  curl_multi_setopt(uv.multi, CURLMOPT_TIMERDATA, &uv);

  CURL* easy = curl_easy_init();
  curl_easy_setopt(easy, CURLOPT_URL, "http://techie.net");
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, easy);
  curl_easy_setopt(easy, CURLOPT_VERBOSE, 1);
  curl_multi_add_handle(uv.multi, easy);

  /* kickstart the thing */
  curl_multi_socket_action(uv.multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
  uv_run(uv.loop, UV_RUN_DEFAULT);
  curl_multi_cleanup(uv.multi);
 
  return 0;
}
