#include "tmtc_io_server.h"
#include "qemu/osdep.h"
#include "qemu/qemu-print.h"

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

void (*receivedTcCallback)(unsigned char *data, int length);
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
    qemu_printf("Waiting for TC client...\n");
    sd = accept(listeningSocket, (struct sockaddr *)&sinRemote, &nAddrSize);
    if (sd == INVALID_SOCKET)
    {
      qemu_printf("accept failed\n");
      continue;
    }

    clientSocket = sd;
    qemu_printf("Client connected from: %s:%d\n", inet_ntoa(sinRemote.sin_addr), ntohs(sinRemote.sin_port));

    do
    {
      nReadBytes = recv(sd, acReadBuffer, IO_TCP_SERVER_INPUT_BUFFER_SIZE, 0);
      if (nReadBytes > 0)
      {
        receivedTcCallback(acReadBuffer, nReadBytes);
      }
      else if (nReadBytes == SOCKET_ERROR)
      {
        qemu_printf("receive error: %d\n", errno);
        close(sd);
        break;
      }

    } while (nReadBytes != 0); // connection has been gracefully closed (recv return value == 0)

    qemu_printf("TC client disconnected\n");

    close(sd);
    clientSocket = -1;
  }

  return 0;
}

int send_tm_packet(unsigned char *data, int data_length)
{
  qemu_printf("sending a packet!!!!\n");
  if (clientSocket == -1)
  {
    qemu_printf("Error: socket is closed\n");
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
 */
void init_tmtc_server(void (*in_tc_received_callback)(unsigned char *, int))
{
  receivedTcCallback = in_tc_received_callback;
  int nCode = 0;
  int port;
  pthread_t tid;

  // Make unique port number
  port = IO_TCP_SERVER_BASE_PORT + getuid();

  if (nCode != 0)
  {
    qemu_printf("WSAStartup() returned error code: %d", nCode);
  }
  else
  {
    //-------------------------------------------- Setup listening socket -----------------------------------
    unsigned long localhostAddress = inet_addr("127.0.0.1");
    if (localhostAddress == INADDR_NONE)
    {
      qemu_printf("error in allocating winsock address\n");
    }
    listeningSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listeningSocket != INVALID_SOCKET)
    {
      struct sockaddr_in sinInterface;
      sinInterface.sin_family = AF_INET;
      sinInterface.sin_addr.s_addr = INADDR_ANY;
      sinInterface.sin_port = htons(port);

      int optval = 1;
      setsockopt(listeningSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

      if (bind(listeningSocket, (struct sockaddr *)&sinInterface, sizeof(struct sockaddr_in)) != SOCKET_ERROR)
      {
        qemu_printf("IOTCPSERVER - Server listening on port: %d\n", port);
        listen(listeningSocket, 1);

        // Create TC thread
        pthread_create(&tid, NULL, tcHandler, NULL);
      }
      else
      {
        listeningSocket = INVALID_SOCKET;
        qemu_printf("IOTCPSERVER - Bind failed... port: %d, error: %d\n", port, errno);
        exit(1);
      }
    }
    else
    {
      qemu_printf("IOTCPSERVER - Invalid socket... port: %d, error: %d\n", port, errno);
      exit(1);
    }
  }
}