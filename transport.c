// Krzysztof Szymański nr ind 338068
// C standard library
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <stdbool.h>
#include <poll.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <sys/param.h>
#include <stdlib.h>
#include <string.h>

// My libraries
#include "config.h"

/*###############################################################################
                               DATA UTILITIES
###############################################################################*/
typedef struct segment {
    uint8_t* data;
    bool     rcvd;
    struct timespec sent;
} Segment;

static int sockfd;
static uint32_t addr;
static uint16_t port;
static Segment* segs;

_Noreturn static void ERROR(const char* str) {
    fprintf(stderr, "%s: %s\n", str, strerror(errno));  // NOLINT(*-err33-c)
    exit(EXIT_FAILURE);
}

void dump(uint32_t i, FILE* fd, uint32_t len) {
    ssize_t written = fwrite (
        segs[i].data, 1, len, fd
    );
    if (written < 0)
        ERROR("write()");
}

static uint64_t timedelta(struct timespec t1, struct timespec t2) {
    return (t1.tv_sec - t2.tv_sec)*1000 + (t1.tv_nsec - t2.tv_nsec)/1000000;
}

static uint64_t nextTurn(struct timespec curr, struct timespec last) {
    return (timedelta(curr,last) < TIMEOUT_MS) ? TIMEOUT_MS - timedelta(curr,last) : 0;
}

void safe_clock_gettime(clockid_t clockid, struct timespec *tp) {
    if (clock_gettime(clockid, tp) != 0)
        ERROR("clock_gettime()");
}

/*###############################################################################
                             NETWORK UTILITIES
###############################################################################*/
void openSocket() {

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        ERROR("socket()");

    int yes = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0)
        ERROR("setsockopt() reuseaddr");
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) != 0)
        ERROR("setsockopt() reuseport");
}

void requestSegment(uint32_t fst, uint32_t lst) {

    uint8_t msg[25];
    sprintf((char*) msg, "GET %d %d\n", fst, lst - fst);
    ssize_t len = strlen((char*) msg);

    struct sockaddr_in address = { 0 };
        address.sin_family = AF_INET;
        address.sin_port   = htons(port);
        address.sin_addr.s_addr   = htonl(addr);

    ssize_t sent = sendto(
        sockfd,
        msg, 
        len, 
        0, 
        (struct sockaddr*) &address, 
        sizeof(address)
    );

    if (sent != len)
        ERROR("sendto(): ");

}

void receive() {

    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    uint8_t buffer[IP_MAXPACKET+1];

    while (true) {
        ssize_t packet_len = recvfrom(
            sockfd,
            buffer,
            IP_MAXPACKET,
            MSG_DONTWAIT,
            (struct sockaddr*) &sender,
            &sender_len
        );

        if (packet_len < 0)
            break;

        if  (ntohl(sender.sin_addr.s_addr) != addr
          || ntohs(sender.sin_port) != port)
            continue;

        uint32_t start, len;
        int matched = sscanf((char*) buffer, "DATA %" SCNu32 " %" SCNu32, &start, &len);

        if (matched == 2 && !segs[start/1000].rcvd) {
            uint8_t *data = memchr(buffer, '\n', 30) + 1;
            memcpy(segs[start/1000].data, data, len);
            segs[start/1000].rcvd = true;
        }

    }
}

int main(int argc, char **argv) {
    
    /*###############################################################################
                             INPUT PARSING & VALIDATION                                     
    ###############################################################################*/
    if (argc != 5) {
        printf("Wrong number of arguemnts!\nProvide IP, port, fname and size\n");
        return -1;
    }

    if (sscanf(argv[2], "%hu", &port) != 1) {
        printf("sscanf(): port reading error\n");
        return -1;
    }

    int conv = inet_pton(
        AF_INET, argv[1], &addr);
    if (conv != 1) {
        printf("IP Adress invalid\n");
        return -1;
    }
    addr = ntohl(addr);

    uint32_t sz, segments;
    if (sscanf(argv[4], "%u", &sz) != 1) {
        printf("sscanf: size reading error");
        return -1;
    }
    segments = (sz + 999)/1000; // ceil(sz/1000) = floor((sz+999)/1000)

    char mode = 'w';
    FILE* fd = fopen(argv[3], &mode);
    if (fd == NULL)
        ERROR("fopen()");

    openSocket();

    segs = malloc(segments * sizeof(Segment));
    uint32_t fst = 0, lst = 0;
    while (fst < segments) {

        for (; lst < MIN(fst + WINDOW_SIZE, segments); lst++) {
            requestSegment(lst*1000, MIN(lst*1000 + 1000, sz));
            safe_clock_gettime(CLOCK_REALTIME, &segs[lst].sent);
            segs[lst].data = malloc(1000);
        }

        struct timespec curr_time; 
        safe_clock_gettime(CLOCK_REALTIME, &curr_time);

        uint64_t wait = TIMEOUT_MS;
        for (uint32_t i = fst; i < lst; i++) {
            if (segs[i].rcvd) continue;
            uint64_t t = nextTurn(curr_time, segs[i].sent);          
            if (t < wait)
                wait = t;
        }

        struct pollfd ps;
            ps.fd = sockfd;
            ps.events = POLLIN;
            ps.revents = 0;
                
        int rcvd = poll(&ps, 1, wait);
        if (rcvd < 0)
            ERROR("poll()");
        else if (rcvd > 0 && ((ps.revents & POLLIN) != 0)) {
            receive();
            while (segs[fst].rcvd && fst < segments) {
                dump(fst, fd, MIN(fst*1000 + 1000, sz) - fst*1000);
                free(segs[fst].data);
                fst++;
            }
            if (fst >= segments)
                break;
        }

        for (uint32_t i = fst; i < lst; i++)
            if (!segs[i].rcvd && timedelta(segs[i].sent, curr_time) > TIMEOUT_MS) {
                requestSegment(i*1000, MIN(i*1000 + 1000, sz));
                safe_clock_gettime(CLOCK_REALTIME, &segs[i].sent);
            }
    }

    free(segs);
    if (close(sockfd) != 0)
        ERROR("close(sockfd)");
    if (fclose(fd) != 0) 
        ERROR("fclose()");

    return 0;

}
