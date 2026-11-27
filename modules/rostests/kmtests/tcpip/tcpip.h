
#define IOCTL_TEST_TDI      1
#define IOCTL_TEST_CONNECT  2

/* For the TDI_CONNECT test */
#define TEST_CONNECT_SERVER_PORT 12345
#define TEST_CONNECT_CLIENT_PORT 54321

#define IOCTL_TEST_MDL_CHAIN 3

/* For the MDL chain test */
#define TEST_MDL_CHAIN_UDP_SENDER_PORT   54323
#define TEST_MDL_CHAIN_UDP_RECEIVER_PORT 54324
#define TEST_MDL_CHAIN_TCP_SERVER_PORT   12347
#define TEST_MDL_CHAIN_TCP_CLIENT_PORT   54325
/* What the kernel side sends from three pieces, and the answer it receives
 * into three */
#define TEST_MDL_CHAIN_TCP_DATA   "ABCDEFGHIJKL"
#define TEST_MDL_CHAIN_TCP_ANSWER "mnopqrstuvwx"

#define IOCTL_TEST_SET_INFO 4

/* For the IOCTL_TCP_SET_INFORMATION_EX test */
#define TEST_SET_INFO_OTHER_PORT   54330
#define TEST_SET_INFO_ADDRESS_PORT 54331
