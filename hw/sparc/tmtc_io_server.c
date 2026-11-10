#include "qemu/osdep.h"
#include "tmtc_io_server.h"
#include "qapi/error.h"
#include "trace.h"

//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>

/*#ifdef _WIN32
  #include <winsock.h>

  //#define errno WSAGetLastError()
  typedef DWORD WINAPI DWORD_WINAPI;

#elif defined __linux__
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <sys/errno.h>
  #include <pthread.h>

  #define TSIM_HEADER_SIZE        4

  #define SOCKET_ERROR                        -1
  #define INVALID_SOCKET                      -1

  typedef int SOCKET;
  typedef int DWORD_WINAPI;
  typedef int DWORD;


#else
  #error "unknown platform"
#endif
*/

#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#define TSIM_HEADER_SIZE 4

typedef uint32_t SOCKET;

#define IO_TCP_SERVER_HANDSHAKE_TOKEN_SIZE 12

void (*receivedTcCallback)(unsigned char *data, int length, void *opaque);
void *stixspw;
int clientHandshaked = 0;
const char handshakeToken[] = "STIX_CONTROL";
SOCKET listeningSocket;
SOCKET clientSocket = -1;

//static uint8_t packet_out[8192];
static unsigned char acReadBuffer[IO_TCP_SERVER_INPUT_BUFFER_SIZE];

static void *tcHandler(void *arg)
{
  struct sockaddr_in sinRemote;
  socklen_t nAddrSize = sizeof(sinRemote);
  SOCKET sd;
  int nReadBytes;

  while (1)
  {
    trace_stixtmtc_wait_client();
    sd = accept(listeningSocket, (struct sockaddr *)&sinRemote, &nAddrSize);
    if (sd == INVALID_SOCKET)
    {
      trace_stixtmtc_accept_failed();
      continue;
    }

    clientSocket = sd;
    trace_stixtmtc_client_connected(inet_ntoa(sinRemote.sin_addr),
                                    ntohs(sinRemote.sin_port));

    do
    {
      nReadBytes = recv(sd, acReadBuffer, IO_TCP_SERVER_INPUT_BUFFER_SIZE, 0);
      if (nReadBytes > 0)
      {
        receivedTcCallback(acReadBuffer, nReadBytes, stixspw);
      }
      else if (nReadBytes == SOCKET_ERROR)
      {
        trace_stixtmtc_recv_error(errno);
        close(sd);
        break;
      }

    } while (nReadBytes != 0); // connection has been gracefully closed (recv return value == 0)

    trace_stixtmtc_client_disconnected();

    close(sd);
    clientSocket = -1;
  }

  return 0;
}

int send_tm_packet(unsigned char *data, int data_length)
{
  trace_stixtmtc_tm_send();
  if (clientSocket == -1)
  {
    trace_stixtmtc_socket_closed();
  }
  else
  {
    /*int idx = 0;
    int packetLength = dataLength + TSIM_HEADER_SIZE;

    // TSIM length included header
    packet_out[idx++] = (packetLength >> 24) & 0xFF;
    packet_out[idx++] = (packetLength >> 16) & 0xFF;
    packet_out[idx++] = (packetLength >> 8) & 0xFF;
    packet_out[idx++] = packetLength & 0xFF;
    // TSIM header
    packet_out[idx++] = 0;
    packet_out[idx++] = 0;
    packet_out[idx++] = 0;
    packet_out[idx++] = 0;
    // Payload
    while (dataLength > 0)
    {
      packet_out[idx++] = *data++;
      dataLength--;
    }*/

    if (send(clientSocket, data, data_length, 0) != SOCKET_ERROR)
    {
      return 1;
    }
  }
  return 0;
}

/**
 *  Creates and connects socket to localhost address and REMOTE_PORT port.
 *  Through this socket, this io module will receive external TC data.
 *  Returns true on success; on failure sets @errp and returns false.
 */
bool init_tmtc_server(void (*in_tc_received_callback)(unsigned char *, int, void *), void *opaque, uint32_t tmtc_port, Error **errp)
{
  receivedTcCallback = in_tc_received_callback;
  stixspw = opaque;
  int port;
  pthread_t tid;

  // Make unique port number
  if (tmtc_port != 0)
  {
    port = tmtc_port;
  }
  else
  {
    port = IO_TCP_SERVER_BASE_PORT + getuid();
  }

  /*
   * the dead WSAStartup branch (Windows-only, nCode was always 0) and
   * the unreachable INADDR_NONE check are gone; the block just scopes
   * the socket setup
   */
  {
    //-------------------------------------------- Setup listening socket -----------------------------------
    unsigned long localhostAddress = inet_addr("127.0.0.1");
    listeningSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listeningSocket != INVALID_SOCKET)
    {
      struct sockaddr_in sinInterface;
      sinInterface.sin_family = AF_INET;
      /* security fix: bind loopback — the code computed localhostAddress but
       never used it (latent 2022 bug). A remote ground station later will
       want a bind-address property instead. */
      sinInterface.sin_addr.s_addr = localhostAddress;
      sinInterface.sin_port = htons(port);

      int optval = 1;
      setsockopt(listeningSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

      if (bind(listeningSocket, (struct sockaddr *)&sinInterface, sizeof(struct sockaddr_in)) != SOCKET_ERROR)
      {
        if (listen(listeningSocket, 1) == SOCKET_ERROR)
        {
          int err = errno;

          close(listeningSocket);
          listeningSocket = INVALID_SOCKET;
          error_setg_errno(errp, err,
                           "TMTC server: cannot listen on 127.0.0.1:%d",
                           port);
          return false;
        }

        trace_stixtmtc_listening(port);

        // Create TC thread
        pthread_create(&tid, NULL, tcHandler, NULL);
      }
      else
      {
        int err = errno;

        close(listeningSocket);
        listeningSocket = INVALID_SOCKET;
        error_setg_errno(errp, err,
                         "TMTC server: cannot bind 127.0.0.1:%d",
                         port);
        return false;
      }
    }
    else
    {
      error_setg_errno(errp, errno,
                       "TMTC server: cannot create the listening socket (port %d)",
                       port);
      return false;
    }
  }

  return true;
}
