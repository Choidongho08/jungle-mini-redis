#!/usr/bin/env bash
# sanitizer 빌드 테스트 실행 래퍼 (make asan 전용)
#
# GCC 12 의 ASan/LSan 은 ASLR 엔트로피가 큰 커널(WSL2 6.x 등)에서 프로그램이 시작되기도 전에
# 무작위로 SIGSEGV 로 죽는다. 그 경우만 골라서 다시 실행한다:
#   종료 코드 139 이고, 출력에 sanitizer 의 DEADLYSIGNAL 외에 아무 출력도 없을 때.
# 테스트가 무언가 출력한 뒤 죽었다면 진짜 오류이므로 그대로 실패시킨다.
#
# 근본 해결: 호스트(WSL)에서 sysctl -w vm.mmap_rnd_bits=28

set -u
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    out=$("$@" 2>&1)
    rc=$?
    if [ $rc -eq 139 ] && [ -z "$(printf '%s\n' "$out" | grep -v 'Sanitizer:DEADLYSIGNAL')" ]; then
        echo "(sanitizer startup crash, retry $attempt)" >&2
        continue
    fi
    printf '%s\n' "$out"
    exit $rc
done
echo "giving up after repeated sanitizer startup crashes" >&2
exit 139
