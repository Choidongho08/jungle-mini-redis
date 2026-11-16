#ifndef MINIREDIS_STATUS_H
#define MINIREDIS_STATUS_H

/*
 * 공통 결과 코드.
 *
 * 실패할 수 있는 모든 공개 함수는 mr_status 를 반환한다. 실패 시 함수는
 * "아무 일도 일어나지 않은" 상태를 보장한다(부분 변경은 롤백). 예외는 각
 * 함수 주석에 명시한다.
 */
typedef enum mr_status {
    MR_OK = 0,
    MR_NOT_FOUND,      /* 키 없음(만료되어 없는 것으로 간주된 경우 포함) */
    MR_COND_FAILED,    /* NX/XX 등 조건 불충족으로 수행하지 않음 */
    MR_NOMEM,          /* 메모리 할당 실패 */
    MR_INVALID_ARG,    /* 잘못된 인자(NULL, 범위 밖 TTL 등) */
    MR_NOT_INTEGER,    /* 값이 64비트 부호 있는 정수 표현이 아님 */
    MR_OVERFLOW,       /* 정수/크기 계산 오버플로 */
    MR_IO_ERROR,       /* 파일 I/O 실패 */
    MR_CORRUPT         /* 영속 파일 내용이 손상됨 */
} mr_status;

/*
 * 상태 코드의 사람이 읽을 수 있는 설명(정적 문자열, 해제 금지).
 * 알 수 없는 값이면 "unknown status" 를 반환한다. O(1).
 */
const char *mr_status_str(mr_status st);

#endif /* MINIREDIS_STATUS_H */
