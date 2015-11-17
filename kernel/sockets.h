#include "includes/kernel/sockets.h"

typedef struct
{
    uint16_t source;
    uint16_t destination;
    uint32_t sequence;
    uint32_t acknowledgement;
        unsigned char offset:4;
        unsigned char reserved:3;
        unsigned char ecn:3;
        unsigned char control:6;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_header;

socket* create_socket();
void close_socket(socket* s);
void destroy_sockets(uint64_t pid);
void connect(socket *s, uint32_t ip, uint16_t port);
uint64_t isconnected(socket* s);