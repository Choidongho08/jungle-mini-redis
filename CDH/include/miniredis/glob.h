#ifndef MINIREDIS_GLOB_H
#define MINIREDIS_GLOB_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Redis 호환 glob 매칭(바이트 단위, 대소문자 구분).
 *
 *   *      임의 길이(0 포함) 바이트열
 *   ?      임의의 1바이트
 *   [abc]  집합, [a-z] 범위(역순 범위는 뒤집어 해석), [^a] 부정
 *   \x     x 를 리터럴로(집합 안에서도 동일). 패턴 끝의 단독 '\' 는 리터럴 '\'.
 *   닫히지 않은 '[' 는 패턴 끝에서 집합이 끝난 것으로 본다(Redis 동작).
 *
 * pattern/str 는 길이 기반이므로 NUL 을 포함할 수 있다. len 이 0 이면 포인터는
 * NULL 이어도 된다.
 *
 * 시간복잡도: O(plen * slen) 최악. Redis 의 재귀 구현은 "a*a*a*...b" 같은
 * 병적 패턴에서 지수적으로 폭발하지만, 여기서는 마지막 '*' 위치만 기억하는
 * 백트래킹을 쓴다. 앞선 '*' 로 되돌아갈 필요가 없는 이유: 뒤의 '*' 가 이미
 * 임의 길이를 흡수할 수 있어 앞선 '*' 의 선택을 바꿔 얻는 매칭은 뒤 '*' 의
 * 선택만 바꿔도 얻어지기 때문이다.
 */
bool mr_glob_match(const void *pattern, size_t plen, const void *str, size_t slen);

#endif /* MINIREDIS_GLOB_H */
