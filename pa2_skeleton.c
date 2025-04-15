/*
 * CS371 PA2: Error control, Flow control
 * 
 * Group Members:
 *   - Student #1: Matthew Hendrix
 *   - Student #2: Mason Wooldridge
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_EVENTS 64
#define PACKET_SIZE 16
#define PAYLOAD_SIZE 8
#define DEFAULT_CLIENT_THREADS 4

// Global configuration (modifiable via command-line arguments)
char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;  // number of requests per client thread

typedef struct __attribute__((packed)) {
    int client_id;
    unsigned int seq_num;
    char payload[PAYLOAD_SIZE];
} udp_packet_t;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long tx_cnt;
    long rx_cnt;
    int client_id;
} client_thread_data_t;

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    struct timeval start, end;
    int ret, nfds;
    
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if(epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("client epoll_ctl");
        pthread_exit(NULL);
    }
    
    data->total_rtt = 0;
    data->total_messages = 0;
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    
    for (unsigned int seq = 0; seq < (unsigned int) num_requests; seq++) {
        udp_packet_t packet;
        packet.client_id = data->client_id;
        packet.seq_num = seq;
        memcpy(packet.payload, "ABCDEFGH", PAYLOAD_SIZE);
        
        char recv_buf[PACKET_SIZE];
        
        if(gettimeofday(&start, NULL) < 0) {
            perror("gettimeofday");
            break;
        }
        
        data->tx_cnt++;
        
        while (1) {
            ret = send(data->socket_fd, &packet, sizeof(packet), 0);
            if(ret < 0) {
                perror("send");
                break;
            }
            
            nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 5000);
            if(nfds <= 0) {
                continue;
            }
            
            ret = recv(data->socket_fd, recv_buf, sizeof(recv_buf), 0);
            if(ret < 0) {
                perror("recv");
                continue;
            }
            
            udp_packet_t *rpacket = (udp_packet_t *)recv_buf;
            
            if(rpacket->client_id == data->client_id && rpacket->seq_num == seq) {
                if(gettimeofday(&end, NULL) < 0) {
                    perror("gettimeofday");
                    break;
                }
              
                long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
                data->total_rtt += rtt;
                data->total_messages++;
                data->rx_cnt++;
                break;
            }
        }
    }
    
    if(data->total_rtt > 0 && data->total_messages > 0)
        data->request_rate = (data->total_messages * 1000000.0) / data->total_rtt;
    else
        data->request_rate = 0;
    
    pthread_exit(NULL);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;
    int ret;
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    if(inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }
    
    for (int i = 0; i < num_client_threads; i++) {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if(sockfd < 0) {
            perror("socket");
            exit(EXIT_FAILURE);
        }
        
        if(connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect");
            exit(EXIT_FAILURE);
        }
        
        int epfd = epoll_create1(0);
        if(epfd < 0) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
        
        thread_data[i].socket_fd = sockfd;
        thread_data[i].epoll_fd = epfd;
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].request_rate = 0.0;
        thread_data[i].tx_cnt = 0;
        thread_data[i].rx_cnt = 0;
        thread_data[i].client_id = i;
        
        ret = pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
        if(ret != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    
    for(int i = 0; i < num_client_threads; i++){
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        
        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }
    
    if(total_messages > 0)
        printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Total initial transmissions (tx_cnt): %ld\n", total_tx);
    printf("Total successful responses (rx_cnt): %ld\n", total_rx);
    printf("Lost packets count: %ld\n", total_tx - total_rx);
}

void run_server() {
    int udp_sock, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct epoll_event ev, events[MAX_EVENTS];
    
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(udp_sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    
    int opt = 1;
    if(setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    
    if(set_nonblocking(udp_sock) < 0) {
        perror("set_nonblocking");
        exit(EXIT_FAILURE);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);
    
    if(bind(udp_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    
    epoll_fd = epoll_create1(0);
    if(epoll_fd < 0) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
    
    ev.events = EPOLLIN;
    ev.data.fd = udp_sock;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udp_sock, &ev) < 0) {
        perror("epoll_ctl: udp_sock");
        exit(EXIT_FAILURE);
    }
    
    printf("UDP Server is listening on %s:%d\n", server_ip, server_port);
    
    char buffer[PACKET_SIZE];
    
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if(nfds < 0) {
            perror("epoll_wait");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if(events[i].data.fd == udp_sock) {
                ssize_t nread = recvfrom(udp_sock, buffer, PACKET_SIZE, 0,
                                         (struct sockaddr *)&client_addr, &client_len);
                if(nread < 0) {
                    if(errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("recvfrom");
                    }
                    continue;
                }
                ssize_t nsent = sendto(udp_sock, buffer, nread, 0,
                                       (struct sockaddr *)&client_addr, client_len);
                if(nsent < 0) {
                    perror("sendto");
                }
            }
        }
    }
    
    close(udp_sock);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);
        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }
    
    return 0;
}
