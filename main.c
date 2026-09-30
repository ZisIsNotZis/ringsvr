#define _GNU_SOURCE
#include <stdio.h>
#include <liburing.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#define DBG
void wrn(const char* t, const char* s) { fprintf(stderr, "%s %s\n", s, t); }

[[noreturn]] void err(const char* t, const char* s) {
    fprintf(stderr, "%s %s\n", s, t);
    exit(1);
}

int errif(int n, const char* s) {
    if (n < 0) err(strerror(-n), s);
    return n;
}

int wrnif(int n, const char* s) {
    if (n < 0)wrn(strerror(-n), s);
    return n;
}

struct io_uring_sqe* getsq(struct io_uring* ring, int i, int job) {
    struct io_uring_sqe* sq = io_uring_get_sqe(ring);
    if (!sq)err("", "sq");
    sq->user_data = (size_t)i << 4 | job;
    return sq;
}

unsigned clamp(unsigned long n) { return n > 0xFFFFFFFF ? 0xFFFFFFFF : n; }

enum Job {
    UNKNOWN,
    ACCEPT,
    RECV,
    OPEN,
    STAT,
    SEND,
    SPLICE,
    SPLICE2,
    SEND404,
    OPENW,
    WRITE,
    SPLICEW,
    SPLICEW2,
    SENDW,
    CLOSEFILE,
    CLOSE
};

const char* JOBSTR[] = {
    "UNKNOWN", "ACCEPT", "RECV", "OPEN", "STAT", "SEND", "SPLICE", "SPLICE2", "SEND404", "OPENW", "WRITE", "SPLICEW", "SPLICEW2", "SENDW", "CLOSEFILE", "CLOSE"
};

struct {
    int file;
    int pipe[2];
    int sock;
    size_t srcn;
    size_t pipen;
    size_t dstn;
    char buf[512];
    struct statx stat;
    unsigned short bufi;
    unsigned short bufn;
} TH[256];

const char MSG404[] = "HTTP/1.1 404 NOT FOUND\r\nContent-Length:0\r\n\r\n";
const char MSG200[] = "HTTP/1.1 200 OK\r\nContent-Length:0\r\n\r\n";

int main(void) {
    int fd = errif(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), "socket");
    int tmp = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &tmp, sizeof(tmp));
    struct sockaddr_in adr;
    memset(&adr, 0, sizeof(adr));
    adr.sin_family = AF_INET;
    adr.sin_addr.s_addr = INADDR_ANY;
    adr.sin_port = htons(8000);
    errif(bind(fd, (struct sockaddr*)&adr, sizeof(adr)), "bind");
    errif(listen(fd, sizeof(TH) / sizeof(*TH)), "listen");
    struct io_uring ring;
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SQPOLL;
    p.sq_thread_idle = 10;
    errif(io_uring_queue_init_params(sizeof(TH) / sizeof(*TH) * 4, &ring, &p), "init");
    struct io_uring_sqe* sq;
    for (int i = 0; i < sizeof(TH) / sizeof(*TH); i++) {
        int pipe2[2];
        pipe(pipe2);
        TH[i].pipe[0] = pipe2[1];
        TH[i].pipe[1] = pipe2[0];
        sq = getsq(&ring, i, ACCEPT);
        io_uring_prep_accept(sq, fd, NULL, NULL, 0);
    }
    while (1) {
        struct io_uring_cqe* cq = NULL;
        io_uring_peek_cqe(&ring, &cq);
        if (cq == NULL) {
            errif(io_uring_submit(&ring), "submit");
            while (io_uring_wait_cqe(&ring, &cq) < 0);
        }
        io_uring_cqe_seen(&ring, cq);
        int i = (int)(cq->user_data >> 4);
        int job = (int)(cq->user_data & 15);
        int res = wrnif(cq->res, JOBSTR[job]);
#ifdef DBG
        printf("%d %s %d file=%hu pipe=%hu,%hu sock=%hu srcn=%lu pipen=%lu dstn=%lu\n", i, JOBSTR[job], res, TH[i].file, TH[i].pipe[0], TH[i].pipe[1], TH[i].sock, TH[i].srcn, TH[i].pipen, TH[i].dstn);
#endif
        switch (job) {
        case ACCEPT:
            if (res <= 0) break;
            TH[i].sock = res;
            TH[i].pipen = TH[i].dstn = TH[i].bufi = TH[i].bufn = 0;
        RECV:
            TH[i].bufn -= TH[i].bufi;
            memmove(TH[i].buf, TH[i].buf + TH[i].bufi, TH[i].bufn);
            sq = getsq(&ring, i, RECV);
            io_uring_prep_recv(sq, TH[i].sock, TH[i].buf, sizeof(TH[i].buf) - 1 - TH[i].bufn, SOCK_NONBLOCK);
            break;
        case RECV:
            if (res <= 0) goto CLOSE;
            TH[i].bufn += res;
            TH[i].buf[TH[i].bufn] = 0;
            char* end = strstr(TH[i].buf, "\r\n\r\n");
            if (!end) {
                if (TH[i].bufn < sizeof(TH[i].buf) - 1) goto RECV;
                wrn(TH[i].buf, "head");
                goto CLOSE;
            }
            *end = 0;
            end += 4;
            TH[i].bufi = end - TH[i].buf;
            char* path = strstr(TH[i].buf, " ");
            if (path == NULL) {
                wrn("no found", "path");
                goto CLOSE;
            }
            *path = 0;
            path += 2;
            bool get;
            if (!strcmp(TH[i].buf, "GET"))get = true;
            else if (!strcmp(TH[i].buf, "POST"))get = false;
            else {
                wrn(TH[i].buf, "method");
                goto CLOSE;
            }
            char* head = strstr(path, " ");
            if (!head) {
                wrn("end no found", "path");
                goto CLOSE;
            }
            *head = 0;
            head += 1;
#ifdef DBG
            printf(get ? "get %s\n" : "post %s\n", path);
#endif
            char* t = strstr(path, "..");
            if (t) {
                wrn(path, "path");
                goto CLOSE;
            }
            if (!get) {
                head = strstr(head, "Content-Length: ");
                if (!head) {
                    wrn("no found", "length");
                    goto CLOSE;
                }
                head += 16;
                t = strstr(head, "\r");
                if (t) *t = 0;
                TH[i].srcn = atol(head);
                if (!TH[i].srcn) {
                    wrn("0", "length");
                    goto CLOSE;
                }
            }
            if (get) {
                sq = getsq(&ring, i, OPEN);
                io_uring_prep_openat(sq, AT_FDCWD, path, O_RDONLY, 0444);
            }
            else {
                sq = getsq(&ring, i, OPENW);
                io_uring_prep_openat(sq, AT_FDCWD, path, O_RDWR | O_TRUNC | O_CREAT, 0644);
            }
            break;
        case OPEN:
            if (res <= 0) {
                sq = getsq(&ring, i, SEND404);
                io_uring_prep_send(sq, TH[i].sock, MSG404, sizeof(MSG404), MSG_ZEROCOPY);
                break;
            }
            TH[i].file = res;
            sq = getsq(&ring, i, STAT);
            io_uring_prep_statx(sq, TH[i].file, "", AT_EMPTY_PATH, 0, &TH[i].stat);
            break;
        case STAT:
            if (res < 0) goto CLOSE;
            TH[i].srcn = TH[i].stat.stx_size;
            tmp = sprintf((char*)&TH[i].stat, "HTTP/1.1 200 OK\r\nContent-Length:%ld\r\n\r\n", TH[i].srcn);
            sq = getsq(&ring, i, SEND);
            io_uring_prep_send(sq, TH[i].sock, (char*)&TH[i].stat, tmp, MSG_ZEROCOPY);
            break;
        case SEND:
            if (res <= 0) goto CLOSE;
            if (TH[i].pipen < TH[i].srcn) {
                sq = getsq(&ring, i, SPLICE);
                io_uring_prep_splice(sq, TH[i].file, (long)TH[i].pipen, TH[i].pipe[0], -1, clamp(TH[i].srcn - TH[i].pipen), 0);
            }
            goto SPLICE2;
        case SPLICE:
            if (res <= 0) goto CLOSE;
            TH[i].pipen += res;
            if (TH[i].pipen >= TH[i].srcn) break;
            sq = getsq(&ring, i, SPLICE);
            io_uring_prep_splice(sq, TH[i].file, (long)TH[i].pipen, TH[i].pipe[0], -1, clamp(TH[i].srcn - TH[i].pipen), 0);
            break;
        case SPLICE2:
            if (res <= 0) goto CLOSE;
            TH[i].dstn += res;
        SPLICE2:
            if (TH[i].dstn >= TH[i].srcn) {
                sq = getsq(&ring, i, CLOSEFILE);
                io_uring_prep_close(sq, TH[i].file);
                break;
            }
            sq = getsq(&ring, i, SPLICE2);
            io_uring_prep_splice(sq, TH[i].pipe[1], -1, TH[i].sock, -1, clamp(TH[i].srcn - TH[i].dstn), 0);
            break;
        case OPENW:
            if (res <= 0) goto CLOSE;
            TH[i].file = res;
            goto WRITE;
        case WRITE:
            if (res <= 0)goto CLOSE;
            TH[i].bufi += res;
            TH[i].pipen += res;
            TH[i].dstn += res;
        WRITE:
            if (TH[i].bufi >= TH[i].bufn) {
                if (TH[i].pipen < TH[i].srcn) {
                    sq = getsq(&ring, i, SPLICEW);
                    io_uring_prep_splice(sq, TH[i].sock, -1, TH[i].pipe[0], -1, clamp(TH[i].srcn - TH[i].pipen), 0);
                }
                goto SPLICEW2;
            };
            sq = getsq(&ring, i, WRITE);
            io_uring_prep_write(sq, TH[i].file, TH[i].buf + TH[i].bufi, TH[i].bufn - TH[i].bufi, TH[i].dstn);
            break;
        case SPLICEW:
            if (res <= 0) goto CLOSE;
            TH[i].pipen += res;
            if (TH[i].pipen >= TH[i].srcn) break;
            sq = getsq(&ring, i, SPLICEW);
            io_uring_prep_splice(sq, TH[i].sock, -1, TH[i].pipe[0], -1, clamp(TH[i].srcn - TH[i].pipen), 0);
            break;
        case SPLICEW2:
            if (res <= 0) goto CLOSE;
            TH[i].dstn += res;
        SPLICEW2:
            if (TH[i].dstn >= TH[i].srcn) {
                sq = getsq(&ring, i, SENDW);
                io_uring_prep_send(sq, TH[i].sock, MSG200, sizeof(MSG200), MSG_ZEROCOPY);
                break;
            }
            sq = getsq(&ring, i, SPLICEW2);
            io_uring_prep_splice(sq, TH[i].pipe[1], -1, TH[i].file, (long)TH[i].dstn, clamp(TH[i].srcn - TH[i].dstn), 0);
            break;
        case SENDW:
            if (res <= 0) goto CLOSE;
            sq = getsq(&ring, i, CLOSEFILE);
            io_uring_prep_close(sq, TH[i].file);
            break;
        case SEND404:
        case CLOSEFILE:
            goto RECV;
        CLOSE:
            sq = getsq(&ring, i, CLOSE);
            io_uring_prep_close(sq, TH[i].sock);
            break;
        case CLOSE:
            sq = getsq(&ring, i, ACCEPT);
            io_uring_prep_accept(sq, fd, NULL, NULL, 0);
            break;
        default:
            return 1;
        }
    }
}
