# Redis vs Arcus-Memcached Persistence 비교

---

## 1. 캐시(Cache)와 인메모리 DB(In-Memory DB)의 차이

본론에 앞서 두 개념을 먼저 정리해두자. 혼용되는 경우가 많지만 의미가 다르다.

| 구분 | 캐시 (Cache) | 인메모리 DB (In-Memory DB) |
|---|---|---|
| 역할 | 원본 데이터의 복사본을 빠르게 제공 | 메모리가 primary 저장소 |
| 원본 데이터 | 별도 DB에 존재 | 여기가 원본 |
| 유실 허용 | 허용 (원본이 DB에 있으므로) | 불허 (여기가 원본이므로) |
| Persistence 필요성 | 선택적 (재시작 후 상태 복구 목적) | 필수 |
| 예시 | Memcached, Arcus (기본) | Redis (영속 모드), VoltDB |

**Arcus**는 기본적으로 캐시 시스템이라 데이터가 유실되어도 원본 DB에서 다시 채울 수 있다. 그래서 `use_persistence=false`가 기본값이다. 다만 Persistence를 활성화하면 서버를 껐다 켜도 이전 상태를 디스크에서 복구할 수 있어, 캐시를 다시 채우는 과정 없이 바로 운영 상태로 돌아올 수 있다.

**Redis**는 캐시뿐 아니라 인메모리 DB로도 널리 쓰인다. 인메모리 DB로 쓸 경우 Redis가 원본 데이터이므로 Persistence가 필수다. 그래서 Redis에서는 Persistence가 처음부터 핵심 기능으로 설계되었다.

---

## 2. Redis Persistence

Redis는 세 가지 방식을 제공한다.

### RDB (Redis Database Snapshot)

특정 시점의 메모리 전체를 `.rdb` 파일로 디스크에 저장하는 방식이다.

- 트리거: 설정한 조건("300초 안에 10개 이상 변경" 등) 또는 `BGSAVE` 명령
- 파일 크기가 작고 복구 속도가 빠름
- 단점: 마지막 스냅샷 이후 데이터는 유실될 수 있음 (분 단위 공백)

### AOF (Append Only File)

모든 쓰기 명령을 `.aof` 파일에 순서대로 기록하는 방식이다.

```
set foo bar  →  파일에 기록
set baz qux  →  파일에 기록
del foo      →  파일에 기록
...
```

복구할 때는 이 명령들을 처음부터 다시 실행해서(replay) 메모리 상태를 복원한다.

#### fsync 정책 — AOF의 핵심 설정

AOF에서 가장 중요한 설정이 **fsync 호출 시점**이다.

`write()` 시스템 콜은 데이터를 OS의 페이지 캐시(메모리 버퍼)에 쓴다. 실제 디스크에 내려가는 건 OS가 결정한다. `fsync()`는 이 버퍼를 강제로 디스크에 플러시하는 시스템 콜이다.

```
write() → OS 페이지 캐시(메모리) → fsync() 호출 시 → 디스크
```

`fsync()` 한 번에 SSD 기준으로도 수백 마이크로초~수 밀리초가 걸린다. 이 비용을 얼마나 자주 감당할지 선택하는 게 fsync 정책이다.

| 정책 | fsync 호출 시점 | 최대 데이터 유실 | 성능 |
|---|---|---|---|
| `always` | 매 명령 write 후 즉시 | 0 (명령 단위) | 가장 느림 |
| `everysec` (기본값) | 별도 bio 스레드가 1초마다 | 최대 1초 | 균형 |
| `no` | OS가 알아서 결정 | OS 재량 (수십 초 가능) | 가장 빠름 |

`everysec`이 기본값인 이유는 실용적인 타협점이기 때문이다. `always`는 디스크 fsync 속도에 TPS가 직접 bound되어 초당 수천 writes 정도밖에 못 된다. `everysec`는 그 사이 write는 메모리 버퍼에만 쌓이니 수십만 TPS가 가능하고, 유실은 최대 1초치로 제한된다. 대부분의 서비스에서 이 정도는 허용 가능하다.

Redis는 bio(Background I/O) 전용 스레드가 fsync를 담당한다. 메인 워커 스레드는 `write()`만 하고 fsync 지연에 블로킹되지 않는다.

#### no-appendfsync-on-rewrite 옵션

AOF 파일이 너무 커지면 Redis는 **AOF Rewrite**를 수행한다. 현재 메모리 상태 기준으로 AOF 파일을 새로 써서 크기를 줄이는 것이다. 이 과정에서 자식 프로세스가 대용량 디스크 write를 하면, 동시에 bio 스레드도 fsync를 호출할 경우 I/O 경합이 생겨 fsync가 수십 초씩 블로킹될 수 있다.

`no-appendfsync-on-rewrite=yes`를 설정하면 Rewrite가 진행되는 동안은 fsync를 건너뛴다. 그 기간 중 crash가 나면 OS 버퍼 데이터는 유실될 수 있지만, Rewrite 완료 후에는 정상 fsync가 재개된다.

### 혼합 모드 (Redis 4.0+)

AOF Rewrite 시 앞부분은 RDB 포맷으로 베이스를 저장하고, 이후 변경분만 AOF로 기록한다. 빠른 로딩(RDB)과 높은 내구성(AOF)을 동시에 확보하는 방식이다.

---

## 3. Arcus-Memcached Persistence

Arcus Persistence는 **Snapshot + Command Log** 두 레이어로 구성된다.

### Snapshot (스냅샷)

특정 시점의 메모리 전체 데이터를 디스크에 기록한다.

- 파일명: `snapshot_YYYYMMDDHHMMSS`
- Checkpoint 수행 시 생성되며, 이전 스냅샷은 이후 삭제

### Command Log (명령 로그)

모든 쓰기 명령을 파일에 순차적으로 기록한다.

- 파일명: `cmdlog_YYYYMMDDHHMMSS`
- Redis AOF와 같은 역할

#### sync vs async logging

Arcus의 logging 정책은 두 가지다.

**Sync logging** (`async_logging=false`)

```
Worker Thread
─────────────────────────────────────
1. 명령 처리 (메모리 반영)
2. cmdlog 파일에 직접 write()
3. 클라이언트에 응답 반환
```

매 명령마다 write() 후 응답한다. 데이터 유실 가능성이 최소화되지만 write 지연이 생긴다.

**Async logging** (`async_logging=true`)

```
Worker Thread              별도 Flush Thread
──────────────────         ──────────────────────────
1. 명령 처리                  로그 버퍼를 주기적으로
2. 로그를 버퍼에 쌓음            디스크에 flush
3. 즉시 응답 반환
```

Worker 스레드는 메모리 버퍼에만 쓰고 바로 응답한다. 처리량은 높아지지만 flush 전 crash 시 버퍼에 있던 데이터는 유실될 수 있다.

### Checkpoint

별도 Checkpoint 스레드가 스냅샷을 새로 생성하고, 이전 스냅샷과 이전 cmdlog 파일을 삭제한다.

Checkpoint 트리거 조건:
- `chkpt_interval_pct_snapshot`: 마지막 스냅샷 대비 cmdlog 증가 비율(%)
- `chkpt_interval_min_logsize`: cmdlog 최소 크기(MB)

두 조건을 모두 만족할 때 Checkpoint가 수행된다.

### Recovery (복구)

```
1. 가장 최근 Snapshot 파일 로드 → 메모리 복원
2. Snapshot 이후의 cmdlog 파일을 순서대로 replay
→ 복구 완료
```

---

## 4. Redis vs Arcus 핵심 비교표

| 항목 | Redis | Arcus-Memcached |
|---|---|---|
| Persistence 방식 | RDB / AOF / 혼합 | Snapshot + Command Log |
| Snapshot 수행 주체 | fork()된 자식 프로세스 | Checkpoint 전용 스레드 |
| Command Log | AOF (append) | cmdlog (append) |
| 로깅 정책 | always / everysec / no | sync / async |
| Checkpoint/Rewrite | AOF Rewrite (파일 재작성) | Snapshot 재생성 + 이전 파일 삭제 |
| 복구 방식 | RDB 로드 또는 AOF replay 또는 혼합 | Snapshot 로드 → cmdlog replay |
| 기본 활성화 | 설정에 따라 (기본 RDB 활성) | `use_persistence=false` (기본 비활성) |
| Persistence 도입 시점 | Redis 초기부터 | v1.13.0 beta (2020-12) |

---

## 5. I/O Race Condition 문제와 해결 방법

Persistence에서 Snapshot을 찍을 때 근본적인 문제가 하나 있다. **Snapshot을 디스크에 쓰는 동안 메모리가 계속 바뀐다는 것이다.**

```
Checkpoint Thread가 "foo=bar"를 디스크에 쓰는 도중
Worker Thread가 "foo"를 "zzz"로 바꿔버리면?
→ 디스크에는 어떤 값이 써져야 하나?
```

이 상황을 제어하려면 스냅샷 도중 전체 데이터에 락을 걸어야 하는데, 그러면 락을 잡는 동안 서비스가 완전히 멈춘다. Redis, RocksDB, Arcus는 각자 다른 방식으로 이 문제를 해결한다.

### 방법 1: fork() + Copy-on-Write (Redis)

Redis는 `fork()`로 자식 프로세스를 만들어 Snapshot을 맡긴다.

`fork()` 직후, OS는 부모와 자식이 같은 물리 페이지를 공유하도록 설정하고 해당 페이지들을 **읽기 전용**으로 마킹한다.

```
fork() 직후:
  부모 가상주소 0x1000 ──┐
                         ├──▶ 물리 페이지 [foo=bar]  (복사 없음)
  자식 가상주소 0x1000 ──┘
```

이후 부모(메인 프로세스)가 `foo`를 수정하려 하면 **Page Fault**가 발생한다. OS가 새 물리 페이지를 할당하고 원본 내용을 복사한 뒤, 부모의 가상주소를 새 페이지로 연결한다. 자식은 원본 페이지를 그대로 바라본다.

```
부모가 foo를 "zzz"로 수정한 후:
  부모 가상주소 0x1000 ──▶ 물리 페이지 [foo=zzz]  ← 부모가 새 페이지를 할당받음
  자식 가상주소 0x1000 ──▶ 물리 페이지 [foo=bar]  ← 자식은 원본 그대로
```

이것이 **Copy-on-Write(CoW)** 다. fork() 시점에 복사하는 게 아니라, 실제로 수정이 일어나는 그 순간에 복사가 트리거된다.

자식 프로세스는 fork() 시점의 완전히 일관된 스냅샷을 보고 있다. 메인이 아무리 데이터를 바꿔도 자식의 뷰는 변하지 않는다. OS의 가상 메모리 시스템이 이 격리를 보장하기 때문에, 애플리케이션 레벨에서 락을 전혀 안 써도 된다.

또한 자식은 **독립된 프로세스**이므로 I/O 스케줄링도 완전히 독립이다. 자식이 디스크에 천천히 써도 메인 프로세스의 I/O에 영향을 주지 않는다. I/O race condition이 구조적으로 존재할 수 없다.

#### 메모리 사용량 증가

fork() 직후에는 물리 메모리 복사가 전혀 없다. 실제로 수정된 페이지만큼만 메모리가 추가로 사용된다. 따라서:

- read-heavy workload: 추가 메모리 ≈ 거의 0
- write-heavy workload: 수정 페이지만큼 증가, 극단적으로는 기존 메모리와 동일한 크기가 추가 → **최대 2배**

Redis 운영에서 "메모리 여유분을 항상 50% 확보하라"는 가이드가 있는 이유다. Snapshot 중 메모리가 부족해지면 OOM Killer가 Redis를 죽이는 사고가 생긴다.

### 방법 2: Immutable MemTable (RocksDB)

RocksDB는 fork() 없이 스레드만으로 동일한 문제를 해결한다.

쓰기는 먼저 메모리의 **MemTable**(정렬된 skiplist)에 들어간다. MemTable이 가득 차면 이것을 "Immutable(불변)"로 전환하고, 새 MemTable을 만들어 이후 write를 받는다.

```
MemTable이 가득 참:

  Worker Thread           Flush Thread
  ┌─────────────┐        ┌─────────────────────────┐
  │ New MemTable│        │ Immutable MemTable      │
  │ foo=zzz     │        │ foo=bar  (변경 안 됨)     │
  │ (새 write들) │        │ baz=qux                 │
  └─────────────┘        └─────────────────────────┘
        ↓                          ↓
   계속 write 처리          SST 파일로 디스크에 씀
```

Immutable MemTable은 완전히 읽기 전용이라 락이 필요 없다. Worker는 새 MemTable에 쓰고, Flush 스레드는 Immutable MemTable을 읽는다. 서로 다른 메모리 영역을 건드리니 race condition 자체가 없다.

fork()와 달리 스냅샷 범위를 "방금 꽉 찬 MemTable 하나"로 제한해서 메모리 오버헤드를 최소화한다. 대신 WAL(Write-Ahead Log)이 crash 복구를 담당하므로 전체 메모리 스냅샷이 필요 없는 구조다.

### 방법 3: 디스크 물리 분리 (Arcus)

Arcus는 Checkpoint 스레드와 Worker 스레드가 동일 프로세스 내에서 동시에 디스크 I/O를 수행한다.

```
Arcus 프로세스
 ├── Worker Thread      → cmdlog 파일에 write  (logs_path 디스크 사용)
 └── Checkpoint Thread → snapshot 파일에 write (data_path 디스크 사용)
```

같은 디스크를 사용하면 Checkpoint가 I/O 대역폭을 독점하면서 cmdlog write가 밀리고, 이는 클라이언트 응답 레이턴시 급등으로 이어진다.

Arcus가 `data_path`와 `logs_path`를 **물리적으로 다른 디스크**에 두도록 권장하는 이유다. 같은 디스크에 경로만 다르게 설정하면 결국 같은 I/O 채널을 공유해서 의미가 없다.

### 세 방법 비교

| 방법 | 구현 복잡도 | I/O 격리 | 메모리 오버헤드 |
|---|---|---|---|
| fork() + CoW (Redis) | 낮음 (OS가 다 해줌) | 완전 분리 | 최대 2배 |
| Immutable MemTable (RocksDB) | 매우 높음 | 완전 해결 | MemTable 크기만큼 |
| 디스크 물리 분리 (Arcus) | 낮음 (인프라 해결) | 물리 분리 | 없음 |

세 방법 모두 "스냅샷 시점의 데이터를 immutable하게 만드는 것"이 핵심이다. 그걸 OS에게 맡기느냐(fork), 애플리케이션이 직접 구현하느냐(RocksDB), 인프라로 우회하느냐(Arcus)의 차이다.

---

## 6. 성능 비교 정리

| 상황 | 유리한 쪽 | 이유 |
|---|---|---|
| 일반 쓰기 처리량 (TPS) | Redis (everysec) | fsync를 1초에 1번으로 제한, bio 스레드 분리 |
| Checkpoint 중 레이턴시 | Redis | fork()로 메인 스레드가 디스크 I/O에서 완전히 독립 |
| 메모리 사용 효율 | Arcus | fork() 없으므로 메모리 2배 문제 없음 |
| 대용량 메모리 환경 | Arcus | fork()는 메모리가 클수록 CoW 오버헤드 커짐 |
| 복구 속도 | 비슷 | 둘 다 Snapshot + Log replay 구조 |

Redis의 가장 큰 강점은 fork() + bio 스레드 덕분에 Persistence를 켜도 메인 스레드의 레이턴시가 거의 오르지 않는다는 점이다. 다만 메모리가 크면 fork() 비용과 CoW 페이지 복사 비용이 무시 못 할 수준이 된다.

Arcus는 메모리 효율이 좋고 스레드 기반이라 구현이 단순하지만, Checkpoint 중 I/O 경합 문제를 인프라(디스크 물리 분리)로 해결해야 한다는 운영 부담이 있다.

---

## 7. exptime 저장 방식

ARCUS와 Redis는 아이템 만료 시각을 저장하는 방식이 다르다.

ARCUS는 내부적으로 상대값으로 관리한다. `current_time`이라는 전역 변수에 "서버 시작 후 경과 초"를 누적하고, 아이템의 exptime도 "서버 시작 기준으로 몇 초 뒤"로 저장한다. 만료 체크는 `item->exptime < current_time` 정수 비교 하나로 끝난다. `current_time`은 1초마다 갱신되는 전역 변수라 캐시에 올라있어 시스템 콜 없이 메모리 읽기만으로 처리된다.

Redis는 처음부터 절대 unix timestamp(밀리초 단위)로 저장한다. 클라이언트가 `EXPIRE key 300`처럼 상대값을 보내도 내부에서 `now + 300000ms`로 변환해서 절대값으로 저장한다. 덕분에 RDB 스냅샷이나 AOF 로그에 저장할 때 별도 변환이 필요 없다.

ARCUS가 상대값을 쓰는 이유는 memcached 프로토콜 자체가 TTL을 상대값으로 주고받도록 설계됐기 때문이다. 내부를 절대값으로 바꾸면 프로토콜 호환성을 지키면서 변환 지점을 여러 곳에 추가해야 하고, upstream memcached와의 머지 비용도 늘어난다. 숫자가 작고 비교가 단순하다는 성능 이점도 있다. 캐시 성능을 우선시하는 시스템에서 현재 방식이 목적에 맞다.

이 상대값이 persistence 앞에서 문제가 된다. 재시작하면 `current_time`이 0부터 다시 시작하기 때문이다. ARCUS는 persistence 레이어에서만 변환한다. 로그에 저장할 때 `CONVERT_ABS_EXPTIME`으로 절대 unix timestamp로 변환하고, redo 시 `CONVERT_REL_EXPTIME`으로 다시 상대값으로 복원한다. 변경 범위를 persistence 레이어로 국소화한 현실적인 선택이다. Redis는 절대값으로 저장하기 때문에 이 변환 과정 자체가 없다. 설계 시점에 persistence를 염두에 뒀는지 아닌지의 차이가 여기서 드러난다.

---

## 8. 싱글 스레드 vs 멀티 스레드: 락 경합

Redis는 싱글 스레드 이벤트 루프 기반이라 락 경합이 없다. 구조가 단순하고 메모리 접근 패턴이 예측 가능하다. 대신 코어를 하나밖에 못 쓴다.

ARCUS는 멀티 워커 스레드로 멀티코어를 활용한다. 대신 공유 자원에 락이 필요하다. 락을 최대한 잘게 쪼개서 경합을 줄이는 방향으로 설계했다. ARCUS에서 병목이 될 수 있는 락 경합 지점은 크게 네 곳이다.

**해시테이블 락** — 버킷 단위로 쪼개져 있어 다른 버킷끼리는 경합이 없다. 같은 키에 요청이 몰리는 hot key 상황에서 여러 워커가 같은 버킷 락을 두고 경합한다.

**SM allocator 락** — small memory allocator가 슬롯을 할당/해제할 때 락을 잡는다. 워커 스레드가 많고 아이템 생성/삭제가 빈번한 워크로드에서 병목이 될 수 있다.

**cmdlog 링버퍼 + 파일 쓰기 락** — 워커 스레드가 로그를 링버퍼에 쓸 때, 플러시 스레드가 파일에 쓸 때 락이 있다. SYNC 모드에서는 waiter 큐 락까지 추가된다. write throughput이 높을수록 여기가 좁아진다.

**LRU 락** — LRU 클래스마다 락이 있다. 아이템에 접근할 때마다 LRU 순서를 갱신해야 해서 락을 잡는다. 조회가 많은 워크로드에서 경합이 생길 수 있다.

Redis의 싱글 스레드가 이 모든 락을 없애는 대신 코어를 하나밖에 못 쓰는 것과의 트레이드오프다. 단순 get/set이 대부분이면 Redis의 싱글 스레드 구조가 오히려 유리할 수 있다. ARCUS가 실질적으로 유리한 지점은 컬렉션 연산이다. B+Tree, List, Set, Map 같은 자료구조에서 범위 조회, 조건 필터링, 정렬된 결과를 서버에서 처리해서 클라이언트가 데이터를 전부 가져와서 처리하는 것보다 네트워크 비용이 줄어든다. 결국 어느 게 더 빠르냐는 워크로드에 따라 다르다.

---

## 9. 스냅샷과 cmdlog의 포맷이 같은 이유

개념적으로 스냅샷과 cmdlog는 다르다. 스냅샷은 특정 시점의 메모리 상태를 그대로 찍은 것이고, cmdlog는 운영 중 발생한 변경 명령을 순서대로 쌓은 로그다.

Redis는 이 둘을 완전히 다른 포맷으로 저장한다. RDB는 메모리 상태를 직렬화한 바이너리고, AOF는 `set foo bar` 같은 명령어 텍스트를 그대로 쌓는다. 포맷도 다르고 복구 코드도 다르다.

ARCUS는 다른 선택을 했다. 스냅샷도, cmdlog도 둘 다 같은 바이너리 레코드 포맷으로 저장한다. 명령어 원문이 아니라 파싱이 끝난 구조체 형태로, "어떤 상태를 만들어라"는 결과 중심의 레코드다. `set foo bar`라는 명령어를 저장하는 게 아니라 `IT_LINK: key=foo, value=bar, exptime=..., cas=...`처럼 실행 결과를 구조체로 저장한다.

스냅샷에 담기는 레코드는 두 종류뿐이다. 아이템 하나당 `IT_LINK` 레코드 하나, 컬렉션이면 뒤에 `SNAPSHOT_ELEM` 레코드들이 이어진다. 그게 전부다. unlink도 없고 setattr도 없다. 스캔 시점에 메모리에 살아있는 아이템만 찍으니 이미 삭제되거나 만료된 아이템은 아예 포함되지 않는다. "지금 존재하는 아이템을 전부 다시 link하면 스냅샷 시점과 동일한 상태가 된다"는 발상이다.

cmdlog는 다르다. 운영 중 발생한 모든 변경을 순서대로 쌓으니 `IT_LINK`, `IT_UNLINK`, `IT_SETATTR`, `IT_FLUSH` 등 다양한 레코드가 섞여있다.

포맷을 통일했기 때문에 복구할 때 스냅샷이든 cmdlog든 `lrec_redo_from_record` 하나로 처리할 수 있다. redo 코드가 중복 없이 공유된다. Redis AOF처럼 명령어 텍스트를 쌓으면 사람이 읽기 쉽고 디버그가 편하지만, ARCUS 방식은 redo가 빠르고 복구 코드가 단순해지는 대신 바이너리라 직접 읽기 어렵다.
