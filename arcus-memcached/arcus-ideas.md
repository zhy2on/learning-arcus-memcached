# ARCUS 공부하면서 떠오른 아이디어들

> 코드를 읽다가, Redis와 비교하다가 떠오른 개선 아이디어와 의문들을 모아두는 공간.

---

## 1. sizeof → offsetof 변환 (메모리 낭비 제거)

**관련 파일**: `coll_list.c`, `coll_set.c`, `coll_map.c`

list/set/map element 할당 시 `ntotal = sizeof(elem) + nbytes`로 계산하는데,
구조체 trailing padding 때문에 실제 필요량보다 4바이트 크게 계산된다.

SM allocator는 8바이트 단위로 슬롯 클래스를 결정하므로, 이 4바이트 오차가
클래스 경계를 넘으면 실제로 더 큰 슬롯을 사용하게 된다.

```c
// 현재 (4바이트 낭비 가능)
size_t ntotal = sizeof(list_elem_item) + nbytes;

// 개선안
size_t ntotal = offsetof(list_elem_item, value) + nbytes;
```

자세한 분석: `issue-sizeof-vs-offsetof.md`

---

## 2. 락 경합 개선 (멀티스레드 구조적 약점)

ARCUS 멀티 워커 스레드 모델의 핵심 병목 지점 4곳:

1. **해시 테이블 버킷 락** — 핫키에서 동일 버킷으로 경합 집중
2. **SM allocator 락** — 할당 요청이 많을수록 단일 락에서 대기
3. **cmdlog 링 버퍼 + 파일 쓰기 락** — 고쓰기 처리량에서 병목
4. **LRU 클래스 락** — 전체 LRU 접근 시 경합

**개선 방향**:
- 해시 테이블을 버킷 단위 fine-grained lock으로 분리 (Java `ConcurrentHashMap` 방식)
- SM allocator를 per-thread local allocator로 분리 → 할당 시 락 불필요
- cmdlog 링 버퍼를 lock-free 자료구조로 교체

---

## 3. ZooKeeper 의존성 제거 (ARCUS 전용 클러스터 프로토콜)

**현재 구조**:
```
arcus-memcached ↔ arcus-zookeeper ↔ arcus-java-client
```

ZooKeeper를 커스터마이징해서 쓰고 있지만, 여전히 ZooKeeper 앙상블을
별도로 운영해야 한다는 진입 장벽이 있다.

**개선 방향**: 노드 간 gossip 프로토콜로 자체 클러스터 구성
- arcus-zookeeper 앙상블 불필요
- 클라이언트도 ZK 없이 노드 목록 획득 가능
- ZooKeeper 버전 업그레이드에 종속되지 않음
- 캐시에 최적화된 failover 로직 직접 제어 가능

**난이도**: 높음. Raft나 gossip 프로토콜을 ARCUS에 맞게 구현해야 함.
Redis Cluster의 자체 gossip 방식이 참고 모델이 될 수 있다.

---

## 4. io_uring 기반 I/O (persistence 레이턴시 개선)

현재 cmdlog flush는 `write` + `fsync` 시스템콜 기반이다.
리눅스 io_uring을 활용하면 시스템콜 오버헤드를 줄일 수 있다.

특히 SYNC 모드에서 group commit의 효과를 더 높일 수 있다.
fsync 대기 시간이 줄어들면 waiter들의 평균 대기도 줄어든다.

---

## 5. 새로운 데이터 타입 확장 (Redis Stack 대항)

Redis가 모듈로 확장하면서 경쟁력을 높인 영역들:

| 기능 | Redis | ARCUS | 우선순위 |
|---|---|---|---|
| JSON 네이티브 | RedisJSON | 없음 | 중 |
| 풀텍스트 검색 | RediSearch | 없음 | 높음 |
| 시계열 데이터 | RedisTimeSeries | 없음 | 높음 |
| Pub/Sub / Streams | 기본 지원 | 없음 | 중 |
| 서버사이드 스크립팅 | Lua / Functions | 없음 | 낮음 |

검색과 시계열은 CPU-intensive 연산이라 ARCUS의 멀티스레드 구조와 시너지가 있다.
단일 스레드인 Redis보다 집계/검색 처리량이 높을 수 있다.

---

## 6. NUMA-aware 메모리 할당

코어가 많은 서버는 NUMA 구조다. 다른 NUMA 노드 메모리에 접근하면 레이턴시가 크게 뛴다.

**개선 방향**:
- 워커 스레드를 NUMA 노드에 고정 (CPU affinity)
- 각 스레드가 자신의 NUMA 로컬 메모리에서만 할당
- 현재 SM allocator의 전역 락과 결합하면 효과가 더 큼

---

## ARCUS의 진짜 강점 (경쟁력 포인트 정리)

Redis 대비 객관적으로 우위인 것들:

1. **단일 노드 멀티코어 처리량**: Redis는 커맨드 실행이 단일 스레드. ARCUS는 진짜 병렬 실행. 클러스터 없이 같은 스펙에서 더 높은 처리량.

2. **B+tree 범위 조회**: 하나의 캐시 아이템 안에서 바이트 기반 복합 범위 조회. Redis Sorted Set은 score 기반만 가능.

3. **SYNC 영속성 + 성능**: Redis AOF `always`는 유실 없지만 느리다. `everysec`은 빠르지만 최대 1초 유실. ARCUS SYNC는 group commit으로 **유실 없음 + 성능 저하 최소**를 동시에.
