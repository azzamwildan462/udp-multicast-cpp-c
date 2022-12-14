#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <fstream>
#include <sys/time.h>
#include <sched.h>
#include <unistd.h>
#include <sstream>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include "zlib.h"
#include "lz4.h"
#include "lmdb.h"

// #define COMM_DEBUG // Activate it to debug
#define COMM_MULTICAST_TEST // Activate it to test program
// #define USE_REALTIME_PROCESS // Activate it to use realtime process

#define E(expr) CHECK((rc = (expr)) == MDB_SUCCESS, #expr)
#define RES(err, expr) ((rc = expr) == (err) || (CHECK(!rc, #expr), 0))
#define CHECK(test, msg) ((test) ? (void)0 : ((void)fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, msg, mdb_strerror(rc)), abort()))
#define PERR(txt, par...) printf("ERROR: (%s / %s): " txt "\n", __FILE__, __FUNCTION__, ##par)
#define PERRNO(txt) printf("ERROR: (%s / %s): " txt ": %s\n", __FILE__, __FUNCTION__, strerror(errno))

#define TXN_DBI_EXIST(txn, dbi, validity) \
    ((txn) && (dbi) < (txn)->mt_numdbs && ((txn)->mt_dbflags[dbi] & (validity)))

#define TTUP_US_DEFAULT 20E3 // 10Hz -> 100E3, 20Hz -> 50E3
#define TTUP_US_50Hz 10E3
#define PROGRAM_FREQUENCY 25

// Compress data
#define UNCOMPRESS 0
#define ZLIB_COMPRESS 1
#define LZ4_COMPRESS 2

// Socket
typedef struct multiSocket_tag
{
    struct sockaddr_in destAddress;
    int socketID;
    bool compressedData;
} multiSocket_t;
typedef struct nw_config
{
    char multicast_ip[16];
    char *iface;
    unsigned int port;
    uint8_t compress_type;
    char identifier[1];
} config;
multiSocket_t multiSocket;

// Config
config nw_config;

// New thread for receive data
pthread_t recv_thread;
pthread_attr_t thread_attr;
uint8_t end_signal_counter;

// Data
char data[128] = "its3123123";
// char data[128];
char its[4] = "its";
unsigned long int actual_data_size = 15;

int TTUP_US = TTUP_US_50Hz;
int timer;

static void signal_catch(int sig)
{
    if (sig == SIGALRM)
        timer++;
}

int if_NameToIndex(char *ifname, char *address)
{
    int fd;
    struct ifreq if_info;
    int if_index;

    memset(&if_info, 0, sizeof(if_info));
    strncpy(if_info.ifr_name, ifname, IFNAMSIZ - 1);

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
    {
        PERRNO("socket");
        return -1;
    }
    if (ioctl(fd, SIOCGIFINDEX, &if_info) == -1)
    {
        PERRNO("ioctl");
        close(fd);
        return -1;
    }
    if_index = if_info.ifr_ifindex;

    if (ioctl(fd, SIOCGIFADDR, &if_info) == -1)
    {
        PERRNO("ioctl");
        close(fd);
        return -1;
    }

    close(fd);

    sprintf(address, "%d.%d.%d.%d\n",
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[2],
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[3],
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[4],
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[5]);
#ifdef COMM_DEBUG
    printf("**** Using device %s -> Ethernet %s\n", if_info.ifr_name, address);
#endif

    return if_index;
}
int openSocket()
{
    struct sockaddr_in multicastAddress;
    struct ip_mreqn mreqn;
    struct ip_mreq mreq;
    int opt;
    char address[16]; // IPV4: xxx.xxx.xxx.xxx\0

    bzero(&multicastAddress, sizeof(struct sockaddr_in));
    multicastAddress.sin_family = AF_INET;
    multicastAddress.sin_port = htons(nw_config.port);
    multicastAddress.sin_addr.s_addr = INADDR_ANY;

    bzero(&multiSocket.destAddress, sizeof(struct sockaddr_in));
    multiSocket.destAddress.sin_family = AF_INET;
    multiSocket.destAddress.sin_port = htons(nw_config.port);
    multiSocket.destAddress.sin_addr.s_addr = inet_addr(nw_config.multicast_ip);

    if ((multiSocket.socketID = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        PERRNO("socket");
        return -1;
    }

    memset((void *)&mreqn, 0, sizeof(mreqn));
    mreqn.imr_ifindex = if_NameToIndex(nw_config.iface, address);
    if ((setsockopt(multiSocket.socketID, SOL_IP, IP_MULTICAST_IF, &mreqn, sizeof(mreqn))) == -1)
    {
        PERRNO("setsockopt 1");
        return -1;
    }

    opt = 1;
    if ((setsockopt(multiSocket.socketID, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) == -1)
    {
        PERRNO("setsockopt 2");
        return -1;
    }

    memset((void *)&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(nw_config.multicast_ip);
    mreq.imr_interface.s_addr = inet_addr(address);
    // fprintf(stderr, "Index: %d (port %d, %s / %s)\n", multiSocket.socketID, 4321, "224.168.1.80", address);

    if ((setsockopt(multiSocket.socketID, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) == -1)
    {
        PERRNO("setsockopt 3");
        printf("\nerrno %d\n", errno);
        return -1;
    }

    /* Disable reception of our own multicast */
    opt = 0;
    if ((setsockopt(multiSocket.socketID, IPPROTO_IP, IP_MULTICAST_LOOP, &opt, sizeof(opt))) == -1)
    {
        PERRNO("setsockopt");
        return -1;
    }

    if (bind(multiSocket.socketID, (struct sockaddr *)&multicastAddress, sizeof(struct sockaddr_in)) == -1)
    {
        PERRNO("bind");
        return -1;
    }

    return 0;
}
void closeSocket()
{
    if (multiSocket.socketID != -1)
        shutdown(multiSocket.socketID, SHUT_RDWR);
}
void loadConfig()
{
    // Hardcode
    nw_config.iface = "wlp0s20f3";
    strcpy(nw_config.multicast_ip, "224.16.32.80");
    nw_config.port = 1026;
    nw_config.compress_type = UNCOMPRESS;
    uint8_t identifier_buffer = 0;
    sprintf(nw_config.identifier, "%d", identifier_buffer);
}

int saveData(MDB_env *env, MDB_dbi *dbi, char *data_recvd)
{
    // int8_t byte1;
    // int16_t byte2;

    // memcpy(&byte1, data_recvd + 0, 1);
    // memcpy(&byte2, data_recvd + 1, 2);

    // printf("1: %d | 2: %d\n", byte1, byte2);

    char awek[10];
    memcpy(awek, data_recvd, 4);
    // printf("awek: %s\n", awek);

    printf("int awek: %d\n", atoi(awek));

    // Header check
    if (data_recvd[0] != 'i' || data_recvd[1] != 't' || data_recvd[2] != 's')
    {
        printf("Data invalid\n");
        return -2;
    }
    // Create init variables
    int rc;
    MDB_val key, value;
    MDB_txn *txn;

    // Data to be saved
    char identifier[1];
    char keys[13][32] = {"odom_x", "odom_y", "odom_theta", "status_bola",
                         "bola_x_lap", "bola_y_lap", "robot_condition", "target_umpan", "status_algo",
                         "status_sub_algo", "status_sub_sub_algo", "status_sub_sub_sub_algo"};
    // key_size = actual + 1 (identifier)
    int key_size[13] = {7, 7, 11, 12, 11, 11, 16, 13, 12, 16, 20, 24};
    int value_pos[13] = {4, 6, 8, 10, 11, 13, 15, 17, 18, 20, 22, 24};
    int value_size[13] = {2, 2, 2, 1, 2, 2, 2, 1, 2, 2, 2, 2};
    int total_items = 12;

    // Get identifier
    memcpy(&identifier, data_recvd + 3, 1);

    if (identifier[0] == '0')
        return -3;

    // Push data to DB
    E(mdb_txn_begin(env, NULL, 0, &txn));
    E(mdb_dbi_open(txn, NULL, 0, dbi));
    char data_buffer[1024];
    for (int i = 0; i < total_items; i++)
    {
        memcpy(keys[i] + key_size[i] - 1, data_recvd + 3, 1);
        // printf("key: %s\n", keys[i]);
        key.mv_size = key_size[i];
        key.mv_data = keys[i];
        value.mv_size = value_size[i];
        memcpy(&data_buffer, data_recvd + value_pos[i], value_size[i]);
        // int16_t debug;
        // memcpy(&debug, data_recvd + value_pos[i], value_size[i]);
        // printf("key: %s | val %d\n", keys[i], debug);
        value.mv_data = data_buffer;
        // printf("txn_dbi: %d\n", TXN_DBI_EXIST(txn, *dbi, 0x10));
        // rc = mdb_put(txn, *dbi, &key, &value, 0);
        // printf("rc: %d\n", rc);

        if (RES(MDB_PROBLEM, mdb_put(txn, *dbi, &key, &value, 0)) != 0)
        {
            mdb_txn_abort(txn);
            return -1;
        }
    }
    E(mdb_txn_commit(txn));

    return 0;
}
void loadDataFrame(MDB_env *env, MDB_dbi *dbi)
{
    int rc;
    MDB_val key, value;
    MDB_txn *txn;
    // char keyname[8] = "from_js";
    char keyname[8] = "header";

    uint16_t nmux1;
    int16_t x_offset;

    // E(mdb_txn_begin(env, NULL, 0, &txn));
    // key.mv_size = 7;
    // key.mv_data = keyname;
    // if (RES(MDB_PROBLEM, mdb_get(txn, *dbi, &key, &value)) != 0)
    // {
    //     mdb_txn_abort(txn);
    //     return;
    // }
    // E(mdb_txn_commit(txn));
    // memcpy(data, value.mv_data, value.mv_size);
    // memcpy(&nmux1, data + 24, 2);
    // memcpy(&x_offset, data + 12, 2);
    // printf("x_offset: %d\n", x_offset);
    // printf("nmux1: %d\n", nmux1);
    // printf("from_js: %s\n", value.mv_data);

    E(mdb_txn_begin(env, NULL, 0, &txn));
    key.mv_size = 6;
    key.mv_data = keyname;
    if (RES(MDB_PROBLEM, mdb_get(txn, *dbi, &key, &value)) != 0)
    {
        mdb_txn_abort(txn);
        return;
    }
    E(mdb_txn_commit(txn));
    memcpy(&x_offset, value.mv_data, value.mv_size);
    printf("data: %s\n", value.mv_data);
    printf("atoi: %d\n", atoi((char *)value.mv_data));
    printf("x_offset: %d\n", x_offset);
}

void loadData(MDB_env *env, MDB_dbi *dbi)
{
    int dead;
    mdb_reader_check(env, &dead);
    if (dead != 0)
        printf("[LMDB] Cleared %d reader stalls\n", dead);
    // Create init variables
    int rc;
    MDB_txn *txn;

    // Data to be loaded
    // char identifier[1];
    char keys[30][16] = {"header", "command", "style", "bola_x", "bola_y", "auto_kal", "offset_x",
                         "offset_y", "offset_th", "manual_x", "manual_y", "manual_th", "nrbtmux1", "nrbtmux2", "kec_rbt1",
                         "kec_rbt2", "kec_rbt3", "kec_rbt4", "kec_rbt5", "kec_sdt_rbt1", "kec_sdt_rbt2", "kec_sdt_rbt3",
                         "kec_sdt_rbt4", "kec_sdt_rbt5", "kec_tndg_rbt1", "kec_tndg_rbt2", "kec_tndg_rbt3", "kec_tndg_rbt4",
                         "kec_tndg_rbt5"};
    int key_size[30] = {6, 7, 5, 6, 6, 8, 8, 8, 9, 8, 8, 9, 8, 8, 8, 8, 8, 8, 8, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13};
    int data_pos[30] = {4, 5, 6, 7, 9, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42};
    int data_size[30] = {1, 1, 1, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    int total_items = 29;

    bzero(data, actual_data_size);

    // Get data from DB
    E(mdb_txn_begin(env, NULL, 0, &txn));
    // E(mdb_dbi_open(txn, NULL, 0, dbi));

    // char data_buffer[1024];
    int value_pos = 4;
    for (int i = 0; i < total_items; i++)
    {
        // strcat(keys[i], nw_config.identifier);
        // bzero(value.mv_data, value.mv_size);
        MDB_val key, value;

        key.mv_size = key_size[i];
        key.mv_data = keys[i];
        if (RES(MDB_PROBLEM, mdb_get(txn, *dbi, &key, &value)) != 0)
        {
            mdb_txn_abort(txn);
            return;
        }
        char buf_buf[value.mv_size];
        memcpy(buf_buf, value.mv_data, (int)value.mv_size);
        int buf = atoi(buf_buf);
        // printf("key: %s | %d %d %d -> %s -> %d\n", key.mv_data, data_pos[i], atoi((char *)value.mv_data), value.mv_size, value.mv_data, buf);
        memcpy(data, its, 3);                           // Header
        memcpy(data + 3, nw_config.identifier, 1);      // identifier
        memcpy(data + data_pos[i], &buf, data_size[i]); // data
    }
    E(mdb_txn_commit(txn));
    actual_data_size = data_pos[total_items - 1] + data_size[total_items - 1];

    // uint16_t qwe;
    // memcpy(&qwe, data + 24, 2);
    // printf("qwe: %d\n", qwe);
    // printf("data_to_send: %s | size %d\n", data, actual_data_size);
}

int sendData()
{
    if (nw_config.compress_type == UNCOMPRESS)
    {
        // Just send
        int nsent = sendto(multiSocket.socketID, data, actual_data_size, 0, (struct sockaddr *)&multiSocket.destAddress, sizeof(struct sockaddr));
#ifdef COMM_MULTICAST_TEST
        // printf("[send] size %d, data %s\n", nsent, data);
#endif
        if (nsent == actual_data_size)
            return 0;
        else
            return -1;
    }
    else if (nw_config.compress_type == ZLIB_COMPRESS)
    {
        // Compress with ZLIB before send
        unsigned long int compressed_data_size = compressBound(actual_data_size);
        char compressed_data[compressed_data_size];
#ifdef COMM_DEBUG
        printf("Before: %d and %d -> %s\n", sizeof(data), sizeof(compressed_data), data);
#endif
        compress((Bytef *)compressed_data, &compressed_data_size, (Bytef *)data, (unsigned long)actual_data_size);
#ifdef COMM_DEBUG
        printf("After: %d and %d -> %s\n", sizeof(data), sizeof(compressed_data), compressed_data);
        // Debug
        unsigned long int data_raw_size = compressBound(compressed_data_size);
        char data_raw[data_raw_size];
        uncompress((Bytef *)data_raw, &data_raw_size, (Bytef *)compressed_data, (unsigned long)compressed_data_size);
        printf("Uncompress: %d and %d -> %s\n", sizeof(data_raw), sizeof(compressed_data), data_raw);
#endif
        int nsent = sendto(multiSocket.socketID, compressed_data, compressed_data_size, 0, (struct sockaddr *)&multiSocket.destAddress, sizeof(struct sockaddr));
#ifdef COMM_MULTICAST_TEST
        printf("[send] size %d, data %s\n", compressed_data_size, compressed_data);
#endif
        if (nsent == compressed_data_size)
            return 0;
        else
            return -1;
    }
    else if (nw_config.compress_type == LZ4_COMPRESS)
    {
        // Compress with LZ4 before send
        uint8_t compressed_data_size = LZ4_compressBound(actual_data_size);
        char compressed_data[compressed_data_size];
#ifdef COMM_DEBUG
        printf("Before: %d and %d -> %s\n", sizeof(data), sizeof(compressed_data), data);
#endif
        compressed_data_size = LZ4_compress_default(data, &compressed_data[0], actual_data_size, compressed_data_size);
#ifdef COMM_DEBUG
        printf("After: %d and %d -> %s\n", sizeof(data), sizeof(compressed_data), compressed_data);
        // Debug
        unsigned long int data_raw_size = LZ4_compressBound(compressed_data_size);
        char data_raw[data_raw_size];
        data_raw_size = LZ4_decompress_safe(compressed_data, &data_raw[0], compressed_data_size, data_raw_size);
        printf("Uncompress: %d and %d -> %s\n", sizeof(data_raw), sizeof(compressed_data), data_raw);
#endif
        int nsent = sendto(multiSocket.socketID, compressed_data, compressed_data_size, 0, (struct sockaddr *)&multiSocket.destAddress, sizeof(struct sockaddr));
#ifdef COMM_MULTICAST_TEST
        printf("[send] size %d, data %s\n", compressed_data_size, compressed_data);
#endif
        if (nsent == compressed_data_size)
            return 0;
        else
            return -1;
    }

    return 0;
}
void *receiveData(void *arg)
{
    unsigned long int max_recv_data_size = 128;
    char recv_data[128];
    unsigned long int recv_data_size;
    multiSocket_t *socket = (multiSocket_t *)arg;

    // Prepare for DB
    int rc;
    MDB_env *env;
    MDB_dbi dbi;
    MDB_txn *txn;
    // int dead;
    // int err = mdb_reader_check(env, &dead);

    E(mdb_env_create(&env));
    E(mdb_env_set_maxreaders(env, 5));
    E(mdb_env_set_mapsize(env, 10485760)); // 10 MB
    E(mdb_env_open(env, "dependencies/lmdb/libraries/liblmdb/cpp_js", MDB_WRITEMAP | MDB_NOSYNC | MDB_NOMETASYNC, 0664));
    E(mdb_txn_begin(env, NULL, 0, &txn));
    E(mdb_dbi_open(txn, NULL, 0, &dbi));
    // E(mdb_dbi_open(txn, NULL, MDB_DUPSORT, &dbi));
    mdb_txn_commit(txn);

    while (end_signal_counter == 0)
    {
        struct sockaddr src_addr;
        socklen_t addr_len;

        char recv_buffer[recv_data_size];
        int nrecv = recvfrom(socket->socketID, (void *)recv_buffer, max_recv_data_size, 0, &src_addr, &addr_len);
#ifdef COMM_DEBUG
        printf("Buffer, nrecv: %d -> %s\n", nrecv, recv_buffer);
#endif
        if (nrecv > -1)
        {
            bzero(recv_data, max_recv_data_size);
            if (nw_config.compress_type == UNCOMPRESS)
            {
                // strncpy(recv_data, recv_buffer, nrecv);
                memcpy(recv_data, recv_buffer, nrecv);
                recv_data_size = nrecv;
            }
            else if (nw_config.compress_type == ZLIB_COMPRESS)
            {
                recv_data_size = compressBound(nrecv);
                uncompress((Bytef *)recv_data, &recv_data_size, (Bytef *)recv_buffer, (unsigned long)nrecv);
            }
            else if (nw_config.compress_type == LZ4_COMPRESS)
            {
                // recv_data_size = LZ4_compressBound(nrecv);
                recv_data_size = nrecv;
                printf("size: %d and %d\n", recv_data_size, nrecv);
                recv_data_size = LZ4_decompress_safe(recv_buffer, &recv_data[0], nrecv, recv_data_size);
            }
            // Save data to DB
            if (saveData(env, &dbi, recv_data) == -1)
            {
                pthread_exit(NULL);
                return NULL;
            }

#if defined(COMM_DEBUG) || defined(COMM_MULTICAST_TEST)
            printf("[recv] nrecv: %d -> %s \n", recv_data_size, recv_data);
#endif
        }
    }
    pthread_exit(NULL);
    return NULL;
}

void signalHandler(int sig)
{
    printf("Terminate with custom signal handler\n");
    closeSocket();
    end_signal_counter++;
    if (end_signal_counter == 3) // Ketika gagal memberi sinyal terimate untuk recv_thread
        abort();
}

void loadDummyData()
{
    bzero(data, actual_data_size);
    memcpy(data, its, 3);
    memcpy(data + 3, nw_config.identifier, 1);
    // Deklare
    int8_t dummy_header = 'i';
    int8_t dummy_command = 'S';
    int8_t dummy_style = 'E';
    int16_t dummy_bola_x_pada_lapangan = 123;
    int16_t dummy_bola_y_pada_lapangan = 200;
    int8_t dummy_auto_kalibrasi = 1;
    int16_t dummy_offset_bs_x = 90;
    int16_t dummy_offset_bs_y = 120;
    int16_t dummy_offset_bs_theta = 90;
    int16_t dummy_target_manual_x = 180;
    int16_t dummy_target_manual_y = 90;
    int16_t dummy_target_manual_theta = 45;
    uint16_t dummy_data_n_robot_mux1 = 46655;
    uint16_t dummy_data_n_robot_mux2 = 46655;
    uint8_t dummy_trim_kecepatan_robot[5] = {12, 23, 34, 45, 56};
    uint8_t dummy_trim_kecepatan_sudut_robot[5] = {10, 5, 14, 7, 14};
    uint8_t dummy_trim_kecepatan_tendang_robot[5] = {17, 9, 10, 16, 18};

    // Ngisi frame
    memcpy(data + 4, &dummy_header, 1);
    memcpy(data + 5, &dummy_command, 1);
    memcpy(data + 6, &dummy_style, 1);
    memcpy(data + 7, &dummy_bola_x_pada_lapangan, 2);
    memcpy(data + 9, &dummy_bola_y_pada_lapangan, 2);
    memcpy(data + 11, &dummy_auto_kalibrasi, 1);
    memcpy(data + 12, &dummy_offset_bs_x, 2);
    memcpy(data + 14, &dummy_offset_bs_y, 2);
    memcpy(data + 16, &dummy_offset_bs_theta, 2);
    memcpy(data + 18, &dummy_target_manual_x, 2);
    memcpy(data + 20, &dummy_target_manual_y, 2);
    memcpy(data + 22, &dummy_target_manual_theta, 2);
    memcpy(data + 24, &dummy_data_n_robot_mux1, 2);
    memcpy(data + 26, &dummy_data_n_robot_mux2, 2);
    memcpy(data + 28, dummy_trim_kecepatan_robot, 5);
    memcpy(data + 33, dummy_trim_kecepatan_sudut_robot, 5);
    memcpy(data + 38, dummy_trim_kecepatan_tendang_robot, 5);

    int16_t get_buf;
    memcpy(&get_buf, data + 18, 2);

    actual_data_size = 43;

    // printf("data_send: %d | %s\n", actual_data_size, data);
}

int main()
{
    // loadConfig();
    // loadDummyData();
    // return 0;
    // int rc;
    // MDB_env *env;
    // MDB_dbi dbi;
    // MDB_txn *txn;

    // loadConfig();

    // E(mdb_env_create(&env));
    // E(mdb_env_set_maxreaders(env, 5));
    // E(mdb_env_set_mapsize(env, 10485760)); // 10 MB
    // E(mdb_env_open(env, "dependencies/lmdb/libraries/liblmdb/js_cpp", MDB_WRITEMAP | MDB_NOSYNC | MDB_NOMETASYNC, 0664));
    // E(mdb_txn_begin(env, NULL, 0, &txn));
    // E(mdb_dbi_open(txn, NULL, 0, &dbi));
    // // E(mdb_dbi_open(txn, NULL, MDB_DUPSORT, &dbi));
    // mdb_txn_commit(txn);

    // saveData(env, &dbi, data);
    // saveData(env, &dbi, data);
    // saveData(env, &dbi, data);
    // saveData(env, &dbi, data);
    // saveData(env, &dbi, data);
    // saveData(env, &dbi, data);

    // printf("Success\n");
    // return 0;

    struct sched_param proc_sched;
    struct itimerval it;
    struct timeval tempTimeStamp;

    TTUP_US = 1E6 / ((float)PROGRAM_FREQUENCY);
    printf("Start..\n");

    loadConfig();

    // // LMDB load data
    // int rc;
    // MDB_env *env;
    // MDB_dbi dbi;
    // MDB_txn *txn;
    // int dead;
    // int err = mdb_reader_check(env, &dead);

    // E(mdb_env_create(&env));
    // E(mdb_env_set_maxreaders(env, 5));
    // E(mdb_env_set_mapsize(env, 10485760)); // 10 MB
    // E(mdb_env_open(env, "dependencies/lmdb/libraries/liblmdb/js_cpp", MDB_FIXEDMAP | MDB_NOSYNC, 0664));
    // E(mdb_txn_begin(env, NULL, 0, &txn));
    // E(mdb_dbi_open(txn, NULL, 0, &dbi));
    // mdb_txn_commit(txn);

    // loadData(env, &dbi);
    // // loadData(env, &dbi);
    // // loadData(env, &dbi);
    // // loadData(env, &dbi);
    // // loadDataFrame(env, &dbi);
    // return 0;

    signal(SIGINT, signalHandler);

    timer = 0;
    if (signal(SIGALRM, signal_catch) == SIG_ERR)
    {
        PERRNO("signal");
        return -1;
    }

#ifdef USE_REALTIME_PROCESS
    proc_sched.sched_priority = 60;
    if ((sched_setscheduler(getpid(), SCHED_FIFO, &proc_sched)) < 0)
    {
        PERRNO("setscheduler");
        return -1;
    }
#endif

    if (openSocket() == -1)
    {
        PERR("openMulticastSocket");
        return -1;
    }

    end_signal_counter = 0;

    // Create new thread to receive data
    multiSocket_t recv_sckt;
    recv_sckt.socketID = multiSocket.socketID;
    pthread_attr_init(&thread_attr);
    pthread_attr_setinheritsched(&thread_attr, PTHREAD_INHERIT_SCHED);
    if (pthread_create(&recv_thread, &thread_attr, receiveData, (void *)&recv_sckt) != 0)
    {
        PERRNO("pthread_create");
        closeSocket();
        return -1;
    }

    // LMDB load data
    int rc;
    MDB_env *env;
    MDB_dbi dbi;
    MDB_txn *txn;
    // int dead;
    // int err = mdb_reader_check(env, &dead);

    E(mdb_env_create(&env));
    E(mdb_env_set_maxreaders(env, 5));
    E(mdb_env_set_mapsize(env, 10485760));                                                               // 10 MB
    E(mdb_env_open(env, "dependencies/lmdb/libraries/liblmdb/js_cpp", MDB_FIXEDMAP | MDB_NOSYNC, 0664)); //
    E(mdb_txn_begin(env, NULL, 0, &txn));
    E(mdb_dbi_open(txn, NULL, 0, &dbi));
    mdb_txn_commit(txn);

    /* Set itimer to reactivate the program */
    it.it_value.tv_usec = (__suseconds_t)(TTUP_US);
    it.it_value.tv_sec = 0;
    it.it_interval.tv_usec = (__suseconds_t)(TTUP_US);
    it.it_interval.tv_sec = 0;
    setitimer(ITIMER_REAL, &it, NULL);

    int counter;
    while (end_signal_counter == 0)
    {
        double waitTime = (2 * (((double)rand()) / ((double)RAND_MAX)) - 1) * TTUP_US * 0.05 + TTUP_US;
        usleep(waitTime);
        loadData(env, &dbi);
        // loadDummyData();
        if (sendData() == -1)
        {
            PERR("sendData");
            return -1;
        }
    }

    printf("Success\n");

    return 0;
}