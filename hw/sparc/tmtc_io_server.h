#ifndef __IOTCPSERVER_H__
#define __IOTCPSERVER_H__


#define IO_TCP_SERVER_TC_SOURCE_PORT 3333
#define IO_TCP_SERVER_LISTENING_PORT 2221       // port at which this io module will listen and receive input TC data
#define IO_TCP_SERVER_INPUT_BUFFER_SIZE 1024
#define IO_TCP_SERVER_BASE_PORT		20000

int send_tm_packet(unsigned char* data, int dataLength);
void init_tmtc_server(void (*in_tc_received_callback)(unsigned char*, int));

#endif
