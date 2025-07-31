#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mysql/mysql.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <time.h>

#define PORT         5000
#define BUF_SIZE     256
#define MAX_CLIENTS  10

#define SERIAL_DEV   "/dev/ttyACM0"
#define BAUDRATE     B9600

// MySQL 연결
static MYSQL *conn;

// 직렬 포트 핸들
static int serial_fd;

// 연결된 클라이언트 소켓 목록
static int clients[MAX_CLIENTS];
static int client_count = 0;

// 뮤텍스
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t serial_mutex  = PTHREAD_MUTEX_INITIALIZER;

// DB 초기화
void init_db() {
    conn = mysql_init(NULL);
    if (!mysql_real_connect(conn,
            "localhost", "root", "1234", "sensor_db",
            0, NULL, 0)) {
        fprintf(stderr, "DB connect err: %s\n", mysql_error(conn));
        exit(EXIT_FAILURE);
    }
}

// 직렬 포트 초기화
void init_serial() {
    struct termios tty;
    serial_fd = open(SERIAL_DEV, O_RDWR | O_NOCTTY);
    if (serial_fd < 0) {
        perror("Serial open");
        exit(EXIT_FAILURE);
    }
    tcgetattr(serial_fd, &tty);
    cfsetispeed(&tty, BAUDRATE);
    cfsetospeed(&tty, BAUDRATE);
    tty.c_cflag |= (CLOCAL | CREAD | CS8);
    tty.c_cflag &= ~(PARENB | CSTOPB);
    tcsetattr(serial_fd, TCSANOW, &tty);
}

// 클라이언트 등록
void add_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    if (client_count < MAX_CLIENTS) {
        clients[client_count++] = fd;
    }
    pthread_mutex_unlock(&clients_mutex);
}

// 클라이언트 제거
void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i] == fd) {
            clients[i] = clients[--client_count];
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// 센서 데이터 DB 저장 및 클라이언트 푸시
void broadcast_and_store(char *line) {
    char device[32];
    double t, h;
    if (sscanf(line, "%31[^,],%lf,%lf", device, &t, &h) != 3) {
        return;
    }

    // DB 삽입
    char query[256];
    snprintf(query, sizeof(query),
        "INSERT INTO sensor_data "
        "(device_id, recorded_at, temperature, humidity) "
        "VALUES('%s', NOW(), %.2f, %.2f)",
        device, t, h);
    mysql_query(conn, query);

    // 연결된 모든 클라이언트로 푸시
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        dprintf(clients[i], "DATA,%s,%.2f,%.2f\n", device, t, h);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// MOTOR 명령 전송 및 응답 처리
int send_motor_cmd(int client_fd) {
    const char *cmd = "MOTOR\n";
    char resp[128];
    int n;

    pthread_mutex_lock(&serial_mutex);
    write(serial_fd, cmd, strlen(cmd));

    // 최대 2초 대기
    struct timeval tv = {2, 0};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(serial_fd, &rfds);
    if (select(serial_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        n = read(serial_fd, resp, sizeof(resp) - 1);
        if (n > 0) {
            resp[n] = '\0';
            if (strstr(resp, "MOTOR_DONE")) {
                dprintf(client_fd, "OK,MOTOR_DONE\n");
                pthread_mutex_unlock(&serial_mutex);
                return 0;
            }
        }
    }
    pthread_mutex_unlock(&serial_mutex);
    dprintf(client_fd, "ERR,NO_RESPONSE\n");
    return -1;
}

// 센서 데이터 수신 스레드
void *sensor_thread(void *arg) {
    char buf[BUF_SIZE];
    while (1) {
        int n = read(serial_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            broadcast_and_store(buf);
        }
    }
    return NULL;
}

// 각 클라이언트 커맨드 처리 스레드
void *client_handler(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    add_client(fd);

    char buf[BUF_SIZE];
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        char *line = strtok(buf, "\n");
        while (line) {
            if (strcmp(line, "MOTOR") == 0) {
                send_motor_cmd(fd);
            }
            // 향후 확장 명령 처리 가능
            line = strtok(NULL, "\n");
        }
    }

    remove_client(fd);
    close(fd);
    return NULL;
}

// TCP 서버 스레드
void *server_thread(void *arg) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 5);

    while (1) {
        int *pclient = malloc(sizeof(int));
        *pclient = accept(srv, NULL, NULL);
        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, pclient);
        pthread_detach(tid);
    }
    close(srv);
    return NULL;
}

int main() {
    init_db();
    init_serial();

    pthread_t t1, t2;
    pthread_create(&t1, NULL, sensor_thread, NULL);
    pthread_create(&t2, NULL, server_thread, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    mysql_close(conn);
    close(serial_fd);
    return 0;
}
