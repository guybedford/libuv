/* Copyright libuv project contributors. All rights reserved.
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

/* Bind a pipe server on one loop, duplicate its OS handle, and adopt the
 * duplicate with uv_pipe_open() on a second loop, where it is listened on
 * and serves a connection. */

#include "uv.h"
#include "task.h"

#include <string.h>

#ifdef _WIN32
# include "../src/uv-common.h"
# include <io.h>
#else
# include <unistd.h>
#endif

static uv_loop_t loop_b;
static uv_pipe_t source;     /* bound or connected on the default loop */
static uv_pipe_t server;     /* server on loop_b */
static uv_pipe_t connection; /* accepted connection on loop_b */
static uv_pipe_t client;
static int transfer_fd = -1;
static uv_connect_t connect_req;
static uv_write_t server_write_req;
static uv_write_t client_write_req;

static int connection_cb_called;
static int connect_cb_called;
static int server_read_cb_called;
static int client_read_cb_called;
static int close_cb_called;

static char ping[] = "PING";


static void close_cb(uv_handle_t* handle) {
  close_cb_called++;
}


static void alloc_cb(uv_handle_t* handle, size_t size, uv_buf_t* buf) {
  static char slab[64];
  buf->base = slab;
  buf->len = sizeof(slab);
}


static void write_cb(uv_write_t* req, int status) {
  ASSERT_OK(status);
}


static void server_read_cb(uv_stream_t* stream,
                           ssize_t nread,
                           const uv_buf_t* buf) {
  uv_buf_t wbuf;

  ASSERT_EQ(4, nread);
  ASSERT_OK(memcmp(buf->base, ping, 4));
  server_read_cb_called++;

  /* Echo back. */
  wbuf = uv_buf_init(ping, 4);
  ASSERT_OK(uv_write(&server_write_req, stream, &wbuf, 1, write_cb));
}


static void client_read_cb(uv_stream_t* stream,
                           ssize_t nread,
                           const uv_buf_t* buf) {
  ASSERT_EQ(4, nread);
  ASSERT_OK(memcmp(buf->base, ping, 4));
  client_read_cb_called++;

  uv_close((uv_handle_t*) &client, close_cb);
  uv_close((uv_handle_t*) &connection, close_cb);
  uv_close((uv_handle_t*) &server, close_cb);
}


static void connection_cb(uv_stream_t* s, int status) {
  ASSERT_OK(status);
  ASSERT_PTR_EQ(s, (uv_stream_t*) &server);
  connection_cb_called++;

  ASSERT_OK(uv_pipe_init(&loop_b, &connection, 0));
  ASSERT_OK(uv_accept(s, (uv_stream_t*) &connection));
  ASSERT_OK(uv_read_start((uv_stream_t*) &connection,
                          alloc_cb,
                          server_read_cb));
}


static void connect_cb(uv_connect_t* req, int status) {
  uv_buf_t wbuf;

  ASSERT_OK(status);
  connect_cb_called++;

  ASSERT_OK(uv_read_start((uv_stream_t*) &client, alloc_cb, client_read_cb));
  wbuf = uv_buf_init(ping, 4);
  ASSERT_OK(uv_write(&client_write_req,
                     (uv_stream_t*) &client,
                     &wbuf,
                     1,
                     write_cb));
}


static void source_connect_cb(uv_connect_t* req, int status) {
  uv_os_fd_t os_fd;
#ifdef _WIN32
  HANDLE duped;
#endif

  ASSERT_OK(status);
  connect_cb_called++;

  ASSERT_OK(uv_fileno((uv_handle_t*) &source, &os_fd));
#ifdef _WIN32
  ASSERT(DuplicateHandle(GetCurrentProcess(),
                         os_fd,
                         GetCurrentProcess(),
                         &duped,
                         0,
                         FALSE,
                         DUPLICATE_SAME_ACCESS));
  transfer_fd = _open_osfhandle((intptr_t) duped, 0);
#else
  transfer_fd = dup(os_fd);
#endif
  ASSERT_GE(transfer_fd, 0);

  /* The connection has issued no I/O yet, so the source end can be closed
   * before the duplicate is adopted on the other loop. */
  uv_close((uv_handle_t*) &source, close_cb);
}


TEST_IMPL(pipe_server_transfer) {
#if defined(NO_SELF_CONNECT)
  RETURN_SKIP(NO_SELF_CONNECT);
#endif
  uv_os_fd_t source_fd;
  int fd;

  ASSERT_OK(uv_loop_init(&loop_b));

  ASSERT_OK(uv_pipe_init(uv_default_loop(), &source, 0));
  ASSERT_OK(uv_pipe_bind(&source, TEST_PIPENAME));
  ASSERT_OK(uv_fileno((uv_handle_t*) &source, &source_fd));

#ifdef _WIN32
  {
    HANDLE duped;
    ASSERT(DuplicateHandle(GetCurrentProcess(),
                           source_fd,
                           GetCurrentProcess(),
                           &duped,
                           0,
                           FALSE,
                           DUPLICATE_SAME_ACCESS));
    fd = _open_osfhandle((intptr_t) duped, 0);
    ASSERT_GE(fd, 0);
  }

  /* A bound instance carries no IOCP association, so the source can be
   * closed before the duplicate is adopted on the other loop. */
  uv_close((uv_handle_t*) &source, close_cb);
  ASSERT_OK(uv_run(uv_default_loop(), UV_RUN_DEFAULT));
#else
  fd = dup(source_fd);
  ASSERT_GE(fd, 0);
  /* Keep the source open; closing it would unlink the socket path. */
#endif

  ASSERT_OK(uv_pipe_init(&loop_b, &server, 0));
  ASSERT_OK(uv_pipe_open(&server, fd));
  ASSERT_OK(uv_listen((uv_stream_t*) &server, 0, connection_cb));

#ifdef _WIN32
  /* The adopted server must be on the native IOCP path. */
  ASSERT_OK(server.flags & UV_HANDLE_EMULATE_IOCP);
#endif

  ASSERT_OK(uv_pipe_init(&loop_b, &client, 0));
  uv_pipe_connect(&connect_req, &client, TEST_PIPENAME, connect_cb);

  ASSERT_OK(uv_run(&loop_b, UV_RUN_DEFAULT));

  ASSERT_EQ(1, connection_cb_called);
  ASSERT_EQ(1, connect_cb_called);
  ASSERT_EQ(1, server_read_cb_called);
  ASSERT_EQ(1, client_read_cb_called);
#ifdef _WIN32
  ASSERT_OK(connection.flags & UV_HANDLE_EMULATE_IOCP);
  ASSERT_EQ(4, close_cb_called);
#else
  ASSERT_EQ(3, close_cb_called);
#endif

  ASSERT_OK(uv_loop_close(&loop_b));
  MAKE_VALGRIND_HAPPY(uv_default_loop());
  return 0;
}


TEST_IMPL(pipe_client_transfer) {
#if defined(NO_SELF_CONNECT)
  RETURN_SKIP(NO_SELF_CONNECT);
#endif
  uv_buf_t wbuf;

  ASSERT_OK(uv_loop_init(&loop_b));

  ASSERT_OK(uv_pipe_init(&loop_b, &server, 0));
  ASSERT_OK(uv_pipe_bind(&server, TEST_PIPENAME));
  ASSERT_OK(uv_listen((uv_stream_t*) &server, 0, connection_cb));

  /* Connect on the default loop, without starting any reads or writes. */
  ASSERT_OK(uv_pipe_init(uv_default_loop(), &source, 0));
  uv_pipe_connect(&connect_req, &source, TEST_PIPENAME, source_connect_cb);
  ASSERT_OK(uv_run(uv_default_loop(), UV_RUN_DEFAULT));
  ASSERT_EQ(1, connect_cb_called);
  ASSERT_GE(transfer_fd, 0);

  /* Adopt the unused connection on the second loop and use it there. */
  ASSERT_OK(uv_pipe_init(&loop_b, &client, 0));
  ASSERT_OK(uv_pipe_open(&client, transfer_fd));
  ASSERT_OK(uv_read_start((uv_stream_t*) &client, alloc_cb, client_read_cb));
  wbuf = uv_buf_init(ping, 4);
  ASSERT_OK(uv_write(&client_write_req,
                     (uv_stream_t*) &client,
                     &wbuf,
                     1,
                     write_cb));

  ASSERT_OK(uv_run(&loop_b, UV_RUN_DEFAULT));

  ASSERT_EQ(1, connection_cb_called);
  ASSERT_EQ(1, server_read_cb_called);
  ASSERT_EQ(1, client_read_cb_called);
#ifdef _WIN32
  /* The adopted client must be on the native IOCP path. */
  ASSERT_OK(client.flags & UV_HANDLE_EMULATE_IOCP);
  ASSERT_OK(connection.flags & UV_HANDLE_EMULATE_IOCP);
#endif
  ASSERT_EQ(4, close_cb_called);

  ASSERT_OK(uv_loop_close(&loop_b));
  MAKE_VALGRIND_HAPPY(uv_default_loop());
  return 0;
}
