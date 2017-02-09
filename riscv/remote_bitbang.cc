#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>

#include "remote_bitbang.h"

#if 1
#  define D(x) x
#else
#  define D(x)
#endif

/////////// remote_bitbang_t

remote_bitbang_t::remote_bitbang_t(uint16_t port, jtag_dtm_t *tap) :
  tap(tap),
  socket_fd(0),
  client_fd(0)
{
  socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    fprintf(stderr, "remote_bitbang failed to make socket: %s (%d)\n",
        strerror(errno), errno);
    abort();
  }

  fcntl(socket_fd, F_SETFL, O_NONBLOCK);
  int reuseaddr = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
        sizeof(int)) == -1) {
    fprintf(stderr, "remote_bitbang failed setsockopt: %s (%d)\n",
        strerror(errno), errno);
    abort();
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(socket_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
    fprintf(stderr, "remote_bitbang failed to bind socket: %s (%d)\n",
        strerror(errno), errno);
    abort();
  }

  if (listen(socket_fd, 1) == -1) {
    fprintf(stderr, "remote_bitbang failed to listen on socket: %s (%d)\n",
        strerror(errno), errno);
    abort();
  }
}

void remote_bitbang_t::accept()
{
  client_fd = ::accept(socket_fd, NULL, NULL);
  if (client_fd == -1) {
    if (errno == EAGAIN) {
      // No client waiting to connect right now.
    } else {
      fprintf(stderr, "failed to accept on socket: %s (%d)\n", strerror(errno),
          errno);
      abort();
    }
  } else {
    fcntl(client_fd, F_SETFL, O_NONBLOCK);
  }
}

void remote_bitbang_t::tick()
{
  if (client_fd > 0) {
    execute_commands();
  } else {
    this->accept();
  }
}

void remote_bitbang_t::execute_commands()
{
  const unsigned buf_size = 64 * 1024;
  char recv_buf[buf_size];
  char send_buf[buf_size];
  unsigned total_received = 0;
  ssize_t bytes = read(client_fd, recv_buf, buf_size);
  bool quit = false;
  while (bytes > 0) {
    total_received += bytes;
    unsigned send_offset = 0;
    for (unsigned i = 0; i < bytes; i++) {
      uint8_t command = recv_buf[i];

      switch (command) {
        case 'B': /* fprintf(stderr, "*BLINK*\n"); */ break;
        case 'b': /* fprintf(stderr, "_______\n"); */ break;
        case 'r': tap->reset(); break;
        case '0': tap->set_pins(0, 0, 0); break;
        case '1': tap->set_pins(0, 0, 1); break;
        case '2': tap->set_pins(0, 1, 0); break;
        case '3': tap->set_pins(0, 1, 1); break;
        case '4': tap->set_pins(1, 0, 0); break;
        case '5': tap->set_pins(1, 0, 1); break;
        case '6': tap->set_pins(1, 1, 0); break;
        case '7': tap->set_pins(1, 1, 1); break;
        case 'R': send_buf[send_offset++] = tap->tdo() ? '1' : '0'; break;
        case 'Q': quit = true; break;
        default:
                  fprintf(stderr, "remote_bitbang got unsupported command '%c'\n",
                      command);
      }
    }
    unsigned sent = 0;
    while (sent < send_offset) {
      bytes = write(client_fd, send_buf + sent, send_offset);
      if (bytes == -1) {
        fprintf(stderr, "failed to write to socket: %s (%d)\n", strerror(errno), errno);
        abort();
      }
      sent += bytes;
    }

    if (total_received > buf_size || quit) {
      // Don't go forever, because that could starve the main simulation.
      break;
    }
    bytes = read(client_fd, recv_buf, buf_size);
  }

  if (bytes == -1) {
    if (errno == EAGAIN) {
      // We'll try again the next call.
    } else {
      fprintf(stderr, "remote_bitbang failed to read on socket: %s (%d)\n",
          strerror(errno), errno);
      abort();
    }
  }
  if (bytes == 0 || quit) {
    // The remote disconnected.
    close(client_fd);
    client_fd = 0;
  }
}