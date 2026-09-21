/*
   Лабораторна робота (дисципліна: «Сучасні операційні системи»)
   Тема: Режими роботи сокетів та вимірювання швидкодії
 
   Досліджуються:
     - домени: AF_INET (TCP, loopback) та AF_UNIX (локальні сокети);
     - режими:  sync  (блокуючі сокети) та
                async (неблокуючі сокети + select()/event-loop);
     - навантаження (workloads):
          W1 багато малих пакетів (50000 x 128 B),
          W2 середні пакети        (2000 x 16 КБ),
          W3 мало великих пакетів  (150 x 1 МБ);
     - метрики: message/sec (пакетів/сек), bytes/sec, час встановлення
       з'єднання, час відкриття/закриття сокета.
 
   Програма портативна (Windows/Winsock2 та POSIX).
   Збірка (Windows):  gcc sockets_bench.c -o sockets_bench -lws2_32
   Збірка (Linux):    gcc sockets_bench.c -o sockets_bench -lpthread
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Шлях UNIX-сокета формується коротким; глушимо несуттєві
 * попередження GCC про потенційне обрізання рядка. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif

/* ---------------------- Налаштування платформи ---------------------- */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <process.h>
  #include <afunix.h>                      /* struct sockaddr_un (MinGW) */
  typedef SOCKET  sock_t;
  typedef unsigned (__stdcall *threadfunc_t)(void*);
  typedef HANDLE  th_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define THREAD_FCN unsigned __stdcall
  #define SOCK_ERR() ((int)WSAGetLastError())
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/un.h>
  #include <sys/time.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <time.h>
  typedef int     sock_t;
  typedef void* (*threadfunc_t)(void*);
  typedef pthread_t th_t;
  #define INVALID_SOCK (-1)
  #define THREAD_FCN void*
  #define SOCK_ERR() ((int)errno)
#endif

/* ----------------------- Високоточний таймер ----------------------- */

static double now_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int inited = 0;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* ---------------------- Потоки (абстракція) ------------------------- */

static th_t thread_start(threadfunc_t fn, void* arg) {
#ifdef _WIN32
    uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    return (th_t)h;
#else
    pthread_t t;
    pthread_create(&t, NULL, fn, arg);
    return t;
#endif
}

static void thread_join(th_t t) {
#ifdef _WIN32
    WaitForSingleObject((HANDLE)t, INFINITE);
    CloseHandle((HANDLE)t);
#else
    pthread_join(t, NULL);
#endif
}

/* --------------------- Низькорівневі помічники ---------------------- */

static int set_nonblocking(sock_t s, int on) {
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &v);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, fl);
#endif
}

/* select(): 1 = готовий, 0 = таймаут, -1 = помилка */
static int wait_sock(sock_t s, int for_write, int timeout_ms) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    return select(0, for_write ? NULL : &fds,
                  for_write ? &fds : NULL, NULL, &tv);
#else
    return select((int)s + 1, for_write ? NULL : &fds,
                  for_write ? &fds : NULL, NULL, &tv);
#endif
}

static int sock_close(sock_t s) {
#ifdef _WIN32
    return closesocket(s);
#else
    return close(s);
#endif
}

static int sock_shutdown_send(sock_t s) {
#ifdef _WIN32
    return shutdown(s, SD_SEND);
#else
    return shutdown(s, SHUT_WR);
#endif
}

/* connect(): для sync — блокуючий; для async — із завершенням через
 * select() (WSAEWOULDBLOCK / EINPROGRESS). Повертає 0 при успіху. */
static int do_connect(sock_t c, const struct sockaddr* addr, socklen_t addrlen,
                      int nb) {
    if (!nb) return connect(c, addr, addrlen);
    if (connect(c, addr, addrlen) == 0) return 0;
#ifdef _WIN32
    if (SOCK_ERR() != WSAEWOULDBLOCK) return -1;
#else
    if (errno != EINPROGRESS) return -1;
#endif
    if (wait_sock(c, 1, 5000) <= 0) return -1;
    int soerr = 0;
#ifdef _WIN32
    int sl = sizeof(soerr);
    getsockopt(c, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl);
#else
    socklen_t sl = sizeof(soerr);
    getsockopt(c, SOL_SOCKET, SO_ERROR, &soerr, &sl);
#endif
    return soerr == 0 ? 0 : -1;
}

/* Повний send (з обробкою часткових записів і EWOULDBLOCK) */
static int send_full(sock_t s, const char* buf, int len, int nb) {
    int off = 0;
    while (off < len) {
        if (nb) {
            if (wait_sock(s, 1, 10000) <= 0) return -1;
        }
        int n = send(s, buf + off, len - off, 0);
        if (n < 0) {
#ifdef _WIN32
            if (SOCK_ERR() == WSAEWOULDBLOCK) continue;
            return -1;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
#endif
        }
        off += n;
    }
    return 0;
}

/* recv (з підтримкою select):
 *   >0 байт | 0 = EOF | -1 = помилка | -2 = таймаут */
static int recv_like(sock_t s, char* buf, int len, int nb, int to_ms) {
    if (nb) {
        int r = wait_sock(s, 0, to_ms);
        if (r == 0) return -2;
        if (r < 0)  return -1;
    }
    return recv(s, buf, len, 0);
}

/* ------------------- Створення слухаючих сокетів -------------------- */

struct listener_info {
    sock_t sock;
    char   unix_path[256];
    int    port;
};

static sock_t make_listener(int domain, struct listener_info* li) {
    memset(li, 0, sizeof(*li));
    li->sock = socket(domain, SOCK_STREAM, 0);
    if (li->sock == INVALID_SOCK) return INVALID_SOCK;

    if (domain == AF_INET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;                       /* ефемерний порт */
#ifdef _WIN32
        if (bind(li->sock, (struct sockaddr*)&addr, (int)sizeof(addr)) != 0)
            return INVALID_SOCK;
        int alen = sizeof(addr);
        getsockname(li->sock, (struct sockaddr*)&addr, &alen);
#else
        if (bind(li->sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
            return INVALID_SOCK;
        socklen_t alen = sizeof(addr);
        getsockname(li->sock, (struct sockaddr*)&addr, &alen);
#endif
        li->port = (int)ntohs(addr.sin_port);
    } else {
        static unsigned unix_counter = 0;
        const char* tmpdir = getenv("TEMP_TMPDIR_VAR");
        (void)tmpdir; /* платформозалежне значення нижче */
#ifdef _WIN32
        tmpdir = getenv("TEMP");
#else
        tmpdir = getenv("TMPDIR");
#endif
        if (!tmpdir)
#ifdef _WIN32
            tmpdir = "C:\\Windows\\Temp";
#else
            tmpdir = "/tmp";
#endif
        snprintf(li->unix_path, sizeof(li->unix_path), "%s%slab1sock_%06u_%u.sock",
                 tmpdir,
#ifdef _WIN32
                 "\\",
#else
                 "/",
#endif
                 (unsigned)getpid(), unix_counter++);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", li->unix_path);
        if (bind(li->sock, (struct sockaddr*)&sa, (int)sizeof(sa)) != 0)
            return INVALID_SOCK;
    }
    if (listen(li->sock, 8) != 0) return INVALID_SOCK;
    return li->sock;
}

static void listener_destroy(struct listener_info* li) {
    if (li->sock != INVALID_SOCK) sock_close(li->sock);
    if (li->unix_path[0]) remove(li->unix_path);
}

/* ------------------- Потоковий сервер: передача --------------------- */

struct transfer_arg {
    sock_t listen_sock;
    int    nb;
    unsigned long long expect_bytes;
    unsigned long long recvd;
};

static THREAD_FCN server_transfer_thread(void* p) {
    struct transfer_arg* a = (struct transfer_arg*)p;
    sock_t s = accept(a->listen_sock, NULL, NULL);
    if (s == INVALID_SOCK) return 1;
    if (a->nb) set_nonblocking(s, 1);

    /* квитанція: сервер підтверджує встановлення з'єднання */
    send_full(s, "O", 1, a->nb);

    char buf[131072];
    unsigned long long tot = 0;
    while (tot < a->expect_bytes) {
        int n = recv_like(s, buf, sizeof(buf), a->nb, 10000);
        if (n == 0) break;                  /* EOF */
        if (n == -2) {                      /* таймаут */
            if (tot < a->expect_bytes) continue;
            break;
        }
        if (n < 0) break;
        tot += (unsigned long long)n;
    }
    a->recvd = tot;
    sock_close(s);
    return 0;
}

/* ------------------------- Тест передачі ---------------------------- */

static void run_transfer(const char* name, int domain, int nb, int nodelay,
                         const char* wl_name,
                         long long msg_count, size_t msg_size) {
    struct listener_info li;
    struct transfer_arg ta;
    sock_t c = INVALID_SOCK;
    th_t th = (th_t)0;
    int cres = -1;
    int an = -1;

    if (make_listener(domain, &li) == INVALID_SOCK) {
        printf("FAIL,%s,%s,listen_err=%d\n", name, wl_name, SOCK_ERR());
        return;
    }

    ta.listen_sock   = li.sock;
    ta.nb            = nb;
    ta.expect_bytes  = (unsigned long long)msg_count * (unsigned long long)msg_size;
    ta.recvd         = 0;
    th = thread_start((threadfunc_t)server_transfer_thread, &ta);

    /* адреса для connect */
    double t_conn_start = now_sec();
    if (domain == AF_INET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((unsigned short)li.port);
        c = socket(AF_INET, SOCK_STREAM, 0);
        if (c != INVALID_SOCK) {
            if (nb) set_nonblocking(c, 1);
            if (nodelay) {
                int one = 1;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                           (const char*)&one, sizeof(one));
            }
            cres = do_connect(c, (struct sockaddr*)&addr,
                             (socklen_t)sizeof(addr), nb);
        }
    } else {
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", li.unix_path);
        c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (c != INVALID_SOCK) {
            if (nb) set_nonblocking(c, 1);
            cres = do_connect(c, (struct sockaddr*)&sa,
                             (socklen_t)sizeof(sa), nb);
        }
    }

    /* чекаємо квитанцію — з'єднання встановлено (у т.ч. для async) */
    char ack[1];
    if (cres == 0) {
        do { an = recv_like(c, ack, 1, nb, 10000); } while (an == -2);
    }
    double t_conn_end = now_sec();
    double estab_sec = t_conn_end - t_conn_start;

    /* --- передача даних --- */
    double t_start = now_sec();
    if (cres == 0 && an == 1) {
        char* buf = (char*)malloc(msg_size);
        if (buf) {
            memset(buf, 0xAB, msg_size);
            long long i;
            for (i = 0; i < msg_count; i++) {
                if (send_full(c, buf, (int)msg_size, nb) != 0) break;
            }
            free(buf);
        }
    }
    if (c != INVALID_SOCK) sock_shutdown_send(c);
    thread_join(th);
    double t_end = now_sec();

    double dt = (t_end - t_start) > 0 ? (t_end - t_start) : 1e-9;
    double bytes = (double)msg_count * (double)msg_size;
    unsigned long long got = ta.recvd;

    printf("TRANSFER,%s,%s,%lld,%zu,%lld,%llu,%.6f,%.2f,%.0f,%.0f,%.2f\n",
           name, wl_name,
           msg_count, msg_size,
           (long long)bytes, got,
           t_end - t_start,               /* s */
           bytes / dt / 1e6,              /* MB/s */
           (double)msg_count / dt,        /* msg/s */
           bytes / dt,                    /* B/s */
           estab_sec * 1e6);              /* connect+accept, us */

    if (c != INVALID_SOCK) sock_close(c);
    listener_destroy(&li);
}

/* ----------------- Відкриття/закриття сокета ------------------------ */

static void bench_open_close(int domain, const char* name, int k) {
    double t0 = now_sec();
    int i;
    for (i = 0; i < k; i++) {
        sock_t s = socket(domain, SOCK_STREAM, 0);
        if (s == INVALID_SOCK) {
            printf("FAIL,open_close,%s\n", name);
            return;
        }
        sock_close(s);
    }
    double dt = now_sec() - t0;
    printf("OPENCLOSE,%s,%d,%.6f,%.0f,%.0f\n",
           name, k, dt, (double)k / dt, dt * 1e9 / (double)k);
}

/* ----------------- Встановлення з'єднання --------------------------- */

struct accept_arg {
    sock_t listen_sock;
    int k;
};

static THREAD_FCN accept_loop_thread(void* p) {
    struct accept_arg* a = (struct accept_arg*)p;
    int i;
    for (i = 0; i < a->k; i++) {
        sock_t s = accept(a->listen_sock, NULL, NULL);
        if (s == INVALID_SOCK) break;
        send(s, "O", 1, 0);         /* квитанція */
        sock_close(s);
    }
    return 0;
}

static void bench_connect(int domain, int nb, const char* name, int k) {
    struct listener_info li;
    if (make_listener(domain, &li) == INVALID_SOCK) {
        printf("FAIL,connect,%s\n", name);
        return;
    }
    struct accept_arg aa;
    aa.listen_sock = li.sock;
    aa.k = k;
    th_t th = thread_start((threadfunc_t)accept_loop_thread, &aa);

    double sum = 0;
    int i, ok = 0;
    for (i = 0; i < k; i++) {
        sock_t c = socket(domain, SOCK_STREAM, 0);
        if (c == INVALID_SOCK) break;
        if (nb) set_nonblocking(c, 1);
        double t0 = now_sec();
        int cres;
        if (domain == AF_INET) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons((unsigned short)li.port);
            cres = do_connect(c, (struct sockaddr*)&addr,
                              (socklen_t)sizeof(addr), nb);
        } else {
            struct sockaddr_un sa;
            memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", li.unix_path);
            cres = do_connect(c, (struct sockaddr*)&sa,
                              (socklen_t)sizeof(sa), nb);
        }
        char ack[1];
        int an = 0;
        if (cres == 0) {
            do { an = recv_like(c, ack, 1, nb, 10000); } while (an == -2);
        }
        double t1 = now_sec();
        if (cres == 0 && an == 1) { sum += (t1 - t0); ok++; }
        sock_close(c);
    }
    thread_join(th);
    listener_destroy(&li);
    double avg_us = ok ? (sum / ok) * 1e6 : 0.0;
    printf("CONNECT,%s,%d,%.2f\n", name, ok, avg_us);
}

/* ------------------------------ Main -------------------------------- */

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);  /* щоб кирилиця у printf виводилась коректно */
#endif

    printf("SOCKET BENCHMARK (domain x mode x workload)\n");
    printf("  domains: INET = AF_INET/TCP loopback,  UNIX = AF_UNIX\n");
    printf("  modes:   sync = blocking,  async = non-blocking + select()\n\n");

    /* 1. Відкриття/закриття сокета */
    printf("== 1) Відкриття/закриття сокета ==\n");
    bench_open_close(AF_INET, "inet", 20000);
    bench_open_close(AF_UNIX, "unix", 20000);

    /* 2. Встановлення з'єднання */
    printf("\n== 2) Встановлення з'єднання (connect + accept + ack) ==\n");
    bench_connect(AF_INET, 0, "inet_sync",  2000);
    bench_connect(AF_INET, 1, "inet_async", 2000);
    bench_connect(AF_UNIX, 0, "unix_sync",  2000);
    bench_connect(AF_UNIX, 1, "unix_async", 2000);

    /* 3. Передача даних: 5 конфігурацій x 3 навантаження x 2 повтори */
    printf("\n== 3) Передача даних (msg/s, MB/s) ==\n");

    struct wl { const char* nm; long long cnt; size_t sz; };
    struct wl wl_list[] = {
        { "W1_small", 50000, 128     },
        { "W2_med",   2000,  16384   },
        { "W3_large", 150,   1048576 },
    };
    int nwl = (int)(sizeof(wl_list) / sizeof(wl_list[0]));
    int rep, i;

    for (rep = 0; rep < 2; rep++) {
        printf("\n--- повтор %d ---\n", rep + 1);
        for (i = 0; i < nwl; i++) {
            run_transfer("inet_sync",         AF_INET, 0, 0,
                         wl_list[i].nm, wl_list[i].cnt, wl_list[i].sz);
            run_transfer("inet_sync_nodelay", AF_INET, 0, 1,
                         wl_list[i].nm, wl_list[i].cnt, wl_list[i].sz);
            run_transfer("inet_async",        AF_INET, 1, 0,
                         wl_list[i].nm, wl_list[i].cnt, wl_list[i].sz);
            run_transfer("unix_sync",         AF_UNIX, 0, 0,
                         wl_list[i].nm, wl_list[i].cnt, wl_list[i].sz);
            run_transfer("unix_async",        AF_UNIX, 1, 0,
                         wl_list[i].nm, wl_list[i].cnt, wl_list[i].sz);
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    printf("\nDone.\n");
    return 0;
}