#ifndef MINIREDIS_SERVER_H
#define MINIREDIS_SERVER_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "miniredis/aof.h"
#include "miniredis/clock.h"
#include "miniredis/resp.h"
#include "miniredis/status.h"

/*
 * 싱글 스레드 RESP2 TCP 서버.
 *
 * 동시성 모델
 *  - 스레드·fork·타이머 스레드를 쓰지 않는다. poll() 이벤트 루프 하나가 소켓 I/O,
 *    명령 실행, 능동 만료(sweep), AOF fsync tick 을 모두 순서대로 처리한다.
 *  - 명령 하나는 끝까지 실행된 뒤 다음 명령이 시작되므로 명령 단위로 원자적이고
 *    락이 필요 없다. 100 개 클라이언트가 같은 키를 INCR 해도 합이 정확하다.
 *  - 대가: 느린 명령(큰 KEYS, BGREWRITEAOF 등)은 그동안 모든 클라이언트를 멈춘다.
 *    한 연결이 루프를 독점하지 않도록 틱마다 연결당 읽기 바이트·명령 수를 제한한다.
 *
 * 테스트 용이성
 *  - mr_server_run_once() 로 한 틱만 돌릴 수 있어, 테스트가 같은 스레드에서 서버
 *    틱과 클라이언트 소켓 조작을 번갈아 할 수 있다(스레드 없이 결정적).
 *  - port 0 으로 임의 포트에 바인드하고 mr_server_port() 로 실제 포트를 얻는다.
 *  - clock 을 주입하면 TTL·sweep·idle timeout 이 가짜 시계를 따른다.
 */

typedef enum mr_log_level {
    MR_LOG_DEBUG = 0,
    MR_LOG_INFO,
    MR_LOG_WARN,
    MR_LOG_ERROR,
    MR_LOG_SILENT /* 아무것도 출력하지 않음(테스트용) */
} mr_log_level;

/*
 * stderr 에 "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] pid 메시지" 한 줄을 쓴다. level 이 threshold
 * 미만이면 무시. 타임스탬프는 주입된 시계가 아니라 실제 시스템 시각이다.
 */
void mr_log(mr_log_level threshold, mr_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* "debug"|"info"|"warn"|"error" -> 레벨. 알 수 없으면 -1. */
int mr_log_level_parse(const char *s);

typedef struct mr_server_config {
    const char *bind_addr;        /* 기본 "127.0.0.1". 호스트 이름·IPv6 리터럴 허용 */
    uint16_t port;                /* 0 이면 OS 가 고른 임의 포트 */
    const char *aof_path;         /* NULL 이면 AOF 비활성 */
    mr_aof_fsync appendfsync;     /* 기본 EVERYSEC */
    size_t maxclients;            /* 동시 연결 상한. 기본 10000 */
    int64_t idle_timeout_ms;      /* 0 이면 끔. 이 시간 동안 요청이 없으면 연결 종료 */
    size_t max_query_bytes;       /* 연결당 미처리 입력 상한. 기본 128MB */
    size_t max_output_bytes;      /* 연결당 미전송 출력 상한(느린 클라이언트). 기본 256MB */
    mr_resp_limits proto;         /* 0 필드는 RESP 기본 상한 */
    size_t max_cmds_per_tick;     /* 한 틱에서 연결당 실행할 명령 수. 기본 1000 */
    size_t max_read_per_tick;     /* 한 틱에서 연결당 읽을 바이트. 기본 1MB */
    int64_t cron_interval_ms;     /* sweep·AOF tick·timeout 검사 주기. 기본 100 */
    size_t sweep_samples;         /* cron 1 회당 만료 검사 원소 수. 기본 1000 */
    int64_t sweep_budget_ms;      /* cron 1 회당 sweep 시간 예산. 기본 5 */
    const mr_clock *clock;        /* NULL 이면 시스템 시계. 서버보다 오래 살아야 함 */
    mr_log_level log_level;       /* 기본 INFO */
    volatile sig_atomic_t *stop_flag; /* NULL 허용. 0 이 아니면 mr_server_run 종료(시그널용) */
} mr_server_config;

typedef struct mr_server mr_server;

/* 위 주석의 기본값으로 채운다. */
void mr_server_config_default(mr_server_config *cfg);

/*
 * 저장소를 만들고(AOF 가 있으면 재생) 리스닝 소켓을 연다.
 * 실패 시 errbuf 에 사람이 읽을 설명(예: "bind 127.0.0.1:6379: Address already in use").
 * 반환: MR_OK, MR_INVALID_ARG(설정 오류), MR_IO_ERROR(소켓/AOF I/O), MR_CORRUPT(AOF 손상),
 * MR_NOMEM. errbuf NULL 허용.
 */
mr_status mr_server_create(const mr_server_config *cfg, mr_server **out, char *errbuf,
                           size_t errlen);

/*
 * 이벤트 루프 한 틱: 최대 timeout_ms 동안 poll 하고 준비된 이벤트를 처리하며, 주기가
 * 되었으면 cron 작업을 수행한다. 처리 대기 중인 입력이 남아 있으면 기다리지 않는다.
 * 반환: MR_OK 또는 poll 자체의 실패 MR_IO_ERROR.
 */
mr_status mr_server_run_once(mr_server *s, int timeout_ms);

/* stop 요청(또는 stop_flag) 이 올 때까지 run_once 반복. */
mr_status mr_server_run(mr_server *s);

/* 루프 종료 요청. sig_atomic_t 대입만 하므로 어디서든(시그널 문맥 포함) 안전. */
void mr_server_request_stop(mr_server *s);

/* 실제 바인드된 포트(port 0 으로 만든 경우 확인용). */
uint16_t mr_server_port(const mr_server *s);

/* 현재 연결된 클라이언트 수. */
size_t mr_server_client_count(const mr_server *s);

/*
 * 남은 출력을 가능한 만큼(블록하지 않고) 보내고 모든 연결을 닫은 뒤 AOF 를 fsync·닫고
 * 해제한다. NULL 이면 MR_OK. 반환은 AOF 닫기 결과(데이터 유실 가능성 보고용).
 */
mr_status mr_server_destroy(mr_server *s);

#endif /* MINIREDIS_SERVER_H */
