#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <mysql/mysql.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define PORT         5000
#define BUF_SIZE     256
#define MAX_CLIENTS  10

#define SERIAL_DEV   "/dev/ttyACM0"
#define BAUDRATE     B9600

// MySQL 연결 정보
#define DB_HOST      "localhost"
#define DB_USER      "root"
#define DB_PASS      "1234"
#define DB_NAME      "sensor_db"

// 전역 핸들
static MYSQL *conn;
static int     serial_fd;

// 클라이언트 관리
static int      clients[MAX_CLIENTS];
static int      client_count = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// 직렬 통신 동기화
static pthread_mutex_t  serial_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   motor_cv         = PTHREAD_COND_INITIALIZER;
static int              motor_done_flag  = 0;

// 함수 프로토타입
void  init_db(void);
void  init_serial(void);
void *serial_reader(void *arg);
int   send_motor_cmd(int client_fd);
void *server_thread(void *arg);
void *client_handler(void *arg);
void *summary_thread(void *arg);
float get_mode(MYSQL *conn, const char *column, const char *device_id);
void  add_client(int fd);
void  remove_client(int fd);
void  broadcast_and_store(const char *line);

int main() {
    init_db();
    init_serial();

    pthread_t th_serial, th_server, th_summary;

    // 직렬 읽기 스레드 (모터 응답 + 센서 데이터 처리)
    pthread_create(&th_serial, NULL, serial_reader, NULL);

    // TCP 서버 스레드
    pthread_create(&th_server, NULL, server_thread, NULL);

    // 10분 간격 요약 스레드
    pthread_create(&th_summary, NULL, summary_thread, NULL);

    // 필요한 경우 join
    pthread_join(th_serial, NULL);
    pthread_join(th_server, NULL);
    pthread_join(th_summary, NULL);

    mysql_close(conn);
    close(serial_fd);
    return 0;
}

// DB 초기화
void init_db(void) {
    conn = mysql_init(NULL);
    if (!mysql_real_connect(conn,
            DB_HOST, DB_USER, DB_PASS, DB_NAME,
            0, NULL, 0)) {
        fprintf(stderr, "DB connect error: %s\n", mysql_error(conn));
        exit(EXIT_FAILURE);
    }
}

// 직렬 포트 초기화
void init_serial(void) {
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
void broadcast_and_store(const char *line) {
    char device[32];
    double t, h;
    if (sscanf(line, "%31[^,],%lf,%lf", device, &t, &h) != 3) {
        return;
    }

    char query[256];
    snprintf(query, sizeof(query),
        "INSERT INTO sensor_data "
        "(device_id, recorded_at, temperature, humidity) "
        "VALUES('%s', NOW(), %.2f, %.2f)",
        device, t, h);
    mysql_query(conn, query);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        dprintf(clients[i], "DATA,%s,%.2f,%.2f\n", device, t, h);
    }
    pthread_mutex_unlock(&clients_mutex);
}
//2
// 직렬 데이터 수신 + 모터 처리 스레드
void *serial_reader(void *arg) {
    char buf[BUF_SIZE];
    while (1) {
        int n = read(serial_fd, buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = '\0';

        // 모터 완료 응답인 경우
        if (strstr(buf, "MOTOR_DONE")) {
            pthread_mutex_lock(&serial_mutex);
            motor_done_flag = 1;
            pthread_cond_signal(&motor_cv);
            pthread_mutex_unlock(&serial_mutex);
        }
        // 그 외는 센서 데이터
        else {
            broadcast_and_store(buf);
        }
    }
    return NULL;
}

// 모터 명령 전송 (읽기는 serial_reader가 담당)
int send_motor_cmd(int client_fd) {
    const char *cmd = "MOTOR\n";

    pthread_mutex_lock(&serial_mutex);
    motor_done_flag = 0;

    write(serial_fd, cmd, strlen(cmd));

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;

    int rc = 0;
    while (!motor_done_flag && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&motor_cv, &serial_mutex, &ts);
    }
    pthread_mutex_unlock(&serial_mutex);

    if (motor_done_flag) {
        dprintf(client_fd, "OK,MOTOR_DONE\n");
        return 0;
    } else {
        dprintf(client_fd, "ERR,NO_RESPONSE\n");
        return -1;
    }
}

// 클라이언트 커맨드 처리 스레드
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

// 10분 간격 요약 스레드
void *summary_thread(void *arg) {
    while (1) {
        MYSQL *sum_conn = mysql_init(NULL);
        mysql_real_connect(sum_conn,
            DB_HOST, DB_USER, DB_PASS, DB_NAME,
            0, NULL, 0);

        const char *sql =
          "SELECT device_id, "
          "ROUND(AVG(temperature),2), MIN(temperature), MAX(temperature), "
          "ROUND(AVG(humidity),2), MIN(humidity), MAX(humidity) "
          "FROM sensor_data "
          "WHERE recorded_at >= NOW() - INTERVAL 10 MINUTE "
          "GROUP BY device_id";

        if (mysql_query(sum_conn, sql) == 0) {
            MYSQL_RES *res = mysql_store_result(sum_conn);
            MYSQL_ROW  row;
            while ((row = mysql_fetch_row(res))) {
                const char *device_id = row[0];
                float temp_avg = atof(row[1]);
                float temp_min = atof(row[2]);
                float temp_max = atof(row[3]);
                float hum_avg  = atof(row[4]);
                float hum_min  = atof(row[5]);
                float hum_max  = atof(row[6]);

                float temp_mode = get_mode(sum_conn, "temperature", device_id);
                float hum_mode  = get_mode(sum_conn, "humidity",    device_id);

                char insert[512];
                snprintf(insert, sizeof(insert),
                  "INSERT INTO sensor_summary "
                  "(device_id, period_start, temp_avg, temp_mode, temp_min, temp_max, "
                  " hum_avg, hum_mode, hum_min, hum_max) "
                  "VALUES('%s', NOW() - INTERVAL 10 MINUTE, "
                  "%.2f, %.2f, %.2f, %.2f, "
                  "%.2f, %.2f, %.2f, %.2f)",
                  device_id,
                  temp_avg, temp_mode, temp_min, temp_max,
                  hum_avg,  hum_mode,  hum_min,  hum_max);
                mysql_query(sum_conn, insert);
            }
            mysql_free_result(res);
        } else {
            fprintf(stderr, "Summary query error: %s\n", mysql_error(sum_conn));
        }

        mysql_close(sum_conn);
        sleep(600);
    }
    return NULL;
}

// 최빈값 계산
float get_mode(MYSQL *conn, const char *column, const char *device_id) {
    char query[256];
    snprintf(query, sizeof(query),
      "SELECT %s FROM sensor_data "
      "WHERE recorded_at >= NOW() - INTERVAL 10 MINUTE "
      "AND device_id = '%s' "
      "GROUP BY %s ORDER BY COUNT(*) DESC LIMIT 1",
      column, device_id, column);

    if (mysql_query(conn, query) == 0) {
        MYSQL_RES *res = mysql_store_result(conn);
        MYSQL_ROW  row = mysql_fetch_row(res);
        float mode = row ? atof(row[0]) : 0.0f;
        mysql_free_result(res);
        return mode;
    } else {
        fprintf(stderr, "Mode query error: %s\n", mysql_error(conn));
        return 0.0f;
    }
}
