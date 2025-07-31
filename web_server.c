#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <mysql/mysql.h>
#include <microhttpd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define HTTP_PORT          8080

#define DB_HOST            "192.168.0.100"
#define DB_USER            "root"
#define DB_PASS            "1234"
#define DB_NAME            "sensor_db"

#define MOTOR_SERVER_IP    "192.168.0.100"
#define MOTOR_SERVER_PORT  5000

#define MAX_JSON           32768

static MYSQL *db_conn;

/* MySQL 연결 */
static void mysql_open(void) {
    db_conn = mysql_init(NULL);
    if (!mysql_real_connect(db_conn,
            DB_HOST, DB_USER, DB_PASS,
            DB_NAME, 0, NULL, 0)) {
        fprintf(stderr, "MySQL 연결 오류: %s\n", mysql_error(db_conn));
        exit(EXIT_FAILURE);
    }
}

/* SQL → JSON 변환 */
static int
query_to_json(const char *sql, char *buf, size_t sz)
{
    if (mysql_query(db_conn, sql)) return -1;
    MYSQL_RES *res = mysql_store_result(db_conn);
    if (!res) return -1;

    unsigned int cols = mysql_num_fields(res);
    char *p = buf, *end = buf + sz - 1;
    MYSQL_ROW row;
    unsigned long *lengths;
    unsigned long total = mysql_num_rows(res), idx = 0;

    // JSON 배열 시작
    if (p + 2 >= end) { mysql_free_result(res); return -1; }
    *p++ = '['; *p++ = '\n';

    while ((row = mysql_fetch_row(res))) {
        lengths = mysql_fetch_lengths(res);

        // 오브젝트 시작
        if (p + 3 >= end) break;
        *p++ = ' '; *p++ = ' '; *p++ = '{';

        for (unsigned int i = 0; i < cols; ++i) {
            MYSQL_FIELD *f = mysql_fetch_field_direct(res, i);
            // 키
            int w = snprintf(p, end - p,
                             "\"%s\":\"", f->name);
            if (w < 0 || p + w >= end) goto overflow;
            p += w;
            // 값
            int v = snprintf(p, end - p,
                             "%.*s", (int)lengths[i], row[i] ? row[i] : "");
            if (v < 0 || p + v >= end) goto overflow;
            p += v;
            // 닫기
            if (i+1 < cols) {
                if (p + 3 >= end) goto overflow;
                *p++ = '"'; *p++ = ','; *p++ = ' ';
            } else {
                if (p + 2 >= end) goto overflow;
                *p++ = '"';
            }
        }

        // 오브젝트 끝
        if (p + 3 >= end) break;
        *p++ = '}';

        // 쉼표 or 개행
        if (++idx < total) {
            *p++ = ','; *p++ = '\n';
        } else {
            *p++ = '\n';
        }
    }

    // JSON 배열 끝
    if (p + 2 >= end) goto overflow;
    *p++ = ']'; *p++ = '\n';
 *p   = '\0';

    mysql_free_result(res);
    return 0;

overflow:
    mysql_free_result(res);
    return -1;
}


/* MOTOR 접두어 붙여 서버로 전송 */
static int send_motor_command(const char *action) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(MOTOR_SERVER_PORT),
    };
    if (inet_pton(AF_INET, MOTOR_SERVER_IP, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    // action에 "MOTOR\n" 형태로 전달한다
    int len = strlen(action);
    if (write(fd, action, len) != len) {
        perror("write");
        close(fd);
        return -1;
    }

    /* 간단 ACK 대기 */
    char resp[16] = {0};
    int r = read(fd, resp, sizeof(resp)-1);
    if (r < 0) {
        perror("read");
        close(fd);
        return -1;
    }
    close(fd);

    // 아두이노가 "OK"로 응답해야 성공으로 본다
    return (r > 0 && strncmp(resp, "OK", 2) == 0) ? 0 : -1;
}


static const char index_page[] =
"<!DOCTYPE html>\n"
"<html lang=\"ko\">\n"
"<head>\n"
"  <meta charset=\"UTF-8\">\n"
"  <title>4분할 대시보드</title>\n"
"  <script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>\n"
"  <style>\n"
"    body, html { margin:0; padding:0; height:100%; font-family:Arial, sans-serif; }\n"
"    #grid {\n"
"      display:grid;\n"
"      grid-template-columns:1fr 1fr;\n"
"      grid-template-rows:1fr 1fr;\n"
"      gap:12px;\n"
"      padding:12px;\n"
"      box-sizing:border-box;\n"
"      height:100%;\n"
"    }\n"
"    .panel {\n"
"      background:#fafafa;\n"
"      border:1px solid #ddd;\n"
"      padding:12px;\n"
"      display:flex;\n"
"      flex-direction:column;\n"
"      box-sizing:border-box;\n"
"    }\n"
"    .panel h2 { margin:0 0 8px; font-size:1.1em; }\n"
"    .panel canvas { flex:1; }\n"
"    #dailySummary table { width:100%; border-collapse:collapse; text-align:center; }\n"
"    #dailySummary th, #dailySummary td { border:1px solid #ccc; padding:6px; font-size:0.9em; }\n"
"    #controls { margin-top:auto; text-align:center; }\n"
"    #controls button {\n"
"      width:80%; padding:12px; font-size:1em; cursor:pointer;\n"
"    }\n"
"    #motorStatus { margin-top:12px; font-weight:bold; text-align:center; }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div id=\"grid\">\n"
"    <div id=\"tempPanel\" class=\"panel\">\n"
"      <h2>실시간 온도 (℃)</h2>\n"
"      <canvas id=\"tempChart\"></canvas>\n"
"    </div>\n"
"    <div id=\"humPanel\" class=\"panel\">\n"
"      <h2>실시간 습도 (%)</h2>\n"
"      <canvas id=\"humChart\"></canvas>\n"
"    </div>\n"
"    <div id=\"dailySummary\" class=\"panel\">\n"
"      <h2>일별 요약</h2>\n"
"      <table>\n"
"        <thead>\n"
"          <tr>\n"
"            <th>날짜</th><th>평균 온도</th><th>최저 온도</th>\n"
"            <th>최고 온도</th><th>평균 습도</th>\n"
"          </tr>\n"
"        </thead>\n"
"        <tbody></tbody>\n"
"      </table>\n"
"    </div>\n"
"    <div id=\"motorPanel\" class=\"panel\">\n"
"      <h2>모터 제어</h2>\n"
"      <div id=\"controls\">\n"
"        <button id=\"btn-motor\">모터</button>\n"
"      </div>\n"
"      <div id=\"motorStatus\">모터 상태: 알 수 없음</div>\n"
"    </div>\n"
"  </div>\n"
"  <script>\n"
"    const tempCtx = document.getElementById('tempChart').getContext('2d');\n"
"    const humCtx  = document.getElementById('humChart').getContext('2d');\n"
"\n"
"    const tempChart = new Chart(tempCtx, {\n"
"      type:'line',\n"
"      data:{ labels:[], datasets:[{\n"
"        label:'온도', data:[],\n"
"        borderColor:'tomato', backgroundColor:'rgba(255,99,71,0.2)',\n"
"        tension:0.3, fill:true\n"
"      }]},\n"
"      options:{\n"
"        scales:{ y:{ min:20, max:40, title:{ display:true, text:'℃' } }, x:{ display:true } },\n"
"        plugins:{ legend:{ display:false } }\n"
"      }\n"
"    });\n"
"\n"
"    const humChart = new Chart(humCtx, {\n"
"      type:'line',\n"
"      data:{ labels:[], datasets:[{\n"
"        label:'습도', data:[],\n"
"        borderColor:'skyblue', backgroundColor:'rgba(135,206,235,0.2)',\n"
"        tension:0.3, fill:true\n"
"      }]},\n"
"      options:{\n"
"        scales:{ y:{ min:30, max:70, title:{ display:true, text:'%' } }, x:{ display:true } },\n"
"        plugins:{ legend:{ display:false } }\n"
"      }\n"
"    });\n"
"\n"
"    async function refreshRealtime() {\n"
"      try {\n"
"        const res = await fetch('/api/realtime');\n"
"        const arr = await res.json();\n"
"        const times = arr.map(o => o.recorded_at.split(' ')[1]);\n"
"        tempChart.data.labels = humChart.data.labels = times;\n"
"        tempChart.data.datasets[0].data = arr.map(o => parseFloat(o.temperature));\n"
"        humChart.data.datasets[0].data = arr.map(o => parseFloat(o.humidity));\n"
"        tempChart.update(); humChart.update();\n"
"      } catch(e) { console.error(e); }\n"
"    }\n"
"\n"
"    async function refreshSummary() {\n"
"      try {\n"
"        const res = await fetch('/api/summary');\n"
"        const arr = await res.json();\n"
"        const tbody = document.querySelector('#dailySummary tbody');\n"
"        tbody.innerHTML = '';\n"
"        arr.forEach(d => {\n"
"          const tr = document.createElement('tr');\n"
"          tr.innerHTML =\n"
"            `<td>${d.date}</td>` +\n"
"            `<td>${parseFloat(d.avg_temp).toFixed(1)}</td>` +\n"
"            `<td>${parseFloat(d.min_temp).toFixed(1)}</td>` +\n"
"            `<td>${parseFloat(d.max_temp).toFixed(1)}</td>` +\n"
"            `<td>${parseFloat(d.avg_humidity).toFixed(1)}</td>`;\n"
"          tbody.appendChild(tr);\n"
"        });\n"
"      } catch(e) { console.error(e); }\n"
"    }\n"
"\n"
"    async function controlMotor() {\n"
"      try {\n"
"        const res = await fetch('/api/motor');\n"
"        const j   = await res.json();\n"
"        document.getElementById('motorStatus').textContent =\n"
"          `모터 상태: ${j.status}`;\n"
"      } catch(e) { alert('모터 제어 실패: ' + e); }\n"
"    }\n"
"\n"
"    document.getElementById('btn-motor').onclick = controlMotor;\n"
"\n"
"    refreshRealtime();\n"
"    refreshSummary();\n"
"    setInterval(refreshRealtime, 10000);\n"
"    setInterval(refreshSummary, 300000);\n"
"  </script>\n"
"</body>\n"
"</html>\n";
// 2/2
/* HTTP 요청 핸들러: version 인자 추가, 메모리 관리 통일, SQL 오류 수정 */
static int
handle_request(void *cls,
               struct MHD_Connection *con,
               const char *url,
               const char *method,
               const char *version,
               const char *upload_data,
               size_t *upload_data_size,
               void **ptr)
{
    static int aptr;
    struct MHD_Response *resp;
    int ret;

    printf("REQ URL = [%s]\n", url);

    /* 첫 호출일 때 컨텍스트 세팅 */
    if (*ptr == NULL) {
        *ptr = &aptr;
        return MHD_YES;
    }
    *ptr = NULL;

    /* GET 이외 메서드 거부 */
    if (0 != strcmp(method, "GET"))
        return MHD_NO;

    /* 1) 최신 데이터 조회 (/api/latest) */
    if (0 == strcmp(url, "/api/latest")) {
        char *json = malloc(MAX_JSON);
        if (!json) return MHD_NO;

        /* 온도와 습도를 함께 조회 */
        const char *sql =
            "SELECT recorded_at, temperature, humidity "
            "FROM sensor_data "
            "ORDER BY recorded_at DESC "
            "LIMIT 1;";
        if (query_to_json(sql, json, MAX_JSON) < 0) {
            free(json);
            return MHD_NO;
        }

        resp = MHD_create_response_from_buffer(
                   strlen(json),
                   json,
                   MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        ret = MHD_queue_response(con, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }
    /* 2) 일별 요약 조회 (/api/summary) */
    else if (0 == strcmp(url, "/api/summary")) {
        char *json = malloc(MAX_JSON);
        if (!json) return MHD_NO;

        /* avg_humidity로 alias 변경, 불필요 컬럼 제거 */
        const char *sql =
         "SELECT "
           "  DATE(recorded_at)       AS date, "
           "  AVG(temperature)        AS avg_temp, "
           "  MIN(temperature)        AS min_temp, "
           "  MAX(temperature)        AS max_temp, "
           "  AVG(humidity)           AS avg_humidity "
           "FROM sensor_data "
           "GROUP BY DATE(recorded_at) "
           "ORDER BY date DESC "
           "LIMIT 7;";


        if (query_to_json(sql, json, MAX_JSON) < 0) {
            free(json);
            return MHD_NO;
        }

        resp = MHD_create_response_from_buffer(
                   strlen(json),
                   json,
                   MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        ret = MHD_queue_response(con, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }
    /* 3) 실시간 시계열 조회 (/api/realtime) */
    else if (0 == strcmp(url, "/api/realtime")) {
        printf("[realtime] branch entered\n");

        char *json = malloc(MAX_JSON);
        if (!json) return MHD_NO;

        /* humidity 컬럼 추가 */
                const char *sql =
            "SELECT recorded_at, temperature, humidity "
            "FROM sensor_data "
            "ORDER BY recorded_at DESC "
            "LIMIT 200;";

        printf("[realtime] running query…\n");
        int qret = query_to_json(sql, json, MAX_JSON);
        printf("[realtime] query_to_json returned %d\n", qret);

        if (qret < 0) {
            const char *err = "{\"error\":\"internal server error\"}";
            resp = MHD_create_response_from_buffer(
                       strlen(err),
                       (void *)err,
                       MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            ret = MHD_queue_response(con, MHD_HTTP_INTERNAL_SERVER_ERROR, resp);
            MHD_destroy_response(resp);
            free(json);
            return ret;
        }

        printf("[realtime] JSON length = %zu bytes\n", strlen(json));
        resp = MHD_create_response_from_buffer(
                   strlen(json),
                   json,
                   MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        ret = MHD_queue_response(con, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }
	
     else if (0 == strncmp(url, "/api/motor", 10)) {
    struct MHD_Response *resp;
    int ret;

    // 쿼리 파라미터 cmd 무시, 항상 MOTOR 명령만 보냄
    int rc = send_motor_command("MOTOR\n");

    // JSON 응답 메시지 생성
    char out[64];
    snprintf(out, sizeof(out),
             "{\"status\":\"%s\"}",
             rc == 0 ? "ok" : "fail");

    // 응답 전송
    resp = MHD_create_response_from_buffer(strlen(out),
                                           (void *)out,
                                           MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp,
                            "Content-Type",
                            "application/json");
    ret = MHD_queue_response(con,
                             rc == 0
                               ? MHD_HTTP_OK
                               : MHD_HTTP_INTERNAL_SERVER_ERROR,
                             resp);
    MHD_destroy_response(resp);
    return ret;
    }
    
    else if (strcmp(url, "/") == 0) {
    struct MHD_Response *resp;
    resp = MHD_create_response_from_buffer(
        strlen(index_page),
        (void *)index_page,
        MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, "Content-Type", "text/html");
    ret = MHD_queue_response(con, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}
{
        struct MHD_Response *resp;
        const char *msg = "{\"error\":\"not found\"}";
        int ret;

        resp = MHD_create_response_from_buffer(
            strlen(msg),
            (void*)msg,
            MHD_RESPMEM_PERSISTENT
        );
        MHD_add_response_header(resp, "Content-Type", "application/json");
        ret = MHD_queue_response(con, MHD_HTTP_NOT_FOUND, resp);
        MHD_destroy_response(resp);
        return ret;
    }

}


int main(void) {
    mysql_open();

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        HTTP_PORT,
        NULL, NULL,
        &handle_request, NULL,
        MHD_OPTION_END
    );
    if (!daemon) {
        fprintf(stderr,"HTTP 서버 시작 실패\n");
        return 1;
    }

    printf("웹 서버 실행 중: http://0.0.0.0:%d/\n", HTTP_PORT);
    getchar();  /* 엔터로 종료 */

    MHD_stop_daemon(daemon);
    mysql_close(db_conn);
    return 0;
}

