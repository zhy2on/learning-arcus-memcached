# Persistence 성능 측정

> 이 문서는 [JAM2IN 블로그 - ARCUS 데이터 영속성 성능 측정](https://medium.com/jam2in/arcus-데이터-영속성-성능-측정-fd4c8c7e3247)을 읽고 정리한 것이다.

---

## 이 글의 핵심 질문

persistence 기능을 켜면 당연히 성능이 떨어진다. 디스크에 쓰는 작업이 추가되니까. 근데 **얼마나** 떨어지는지가 중요하다. 그 차이가 서비스에서 감당할 수 있는 수준인가?

---

## 시험 환경

### 장비 구성

ARCUS 서버와 부하 생성 클라이언트를 **별도 장비**로 분리해서 측정했다. 같은 장비에서 하면 클라이언트가 쓰는 CPU/메모리가 서버 성능에 영향을 줘서 정확한 측정이 안 되기 때문이다.

| 구분 | 사양 |
|---|---|
| ARCUS 서버 | CPU 8vCPU, 메모리 16GB, 네트워크 1Gbps, HDD 50GB×2 |
| 클라이언트 | CPU 8vCPU, 메모리 16GB, 네트워크 1Gbps, SSD 50GB |

블로그에서는 이 시험이 Naver Cloud Platform VM이 기본으로 제공하는 HDD를 사용한 것이라고 명시하며, NVMe SSD 같이 높은 IOPS를 제공하는 디스크를 사용하면 더 높은 성능을 볼 수 있다고 언급한다.

### ARCUS 구동 옵션

```bash
memcached -d -v -r -R 100 -t 6 -p 11500 -b 8192 -c 4096 -m 12000 \
  -E .../default_engine.so \
  -e config_file=.../default_engine.conf
```

| 옵션 | 의미 |
|---|---|
| `-t 6` | 워커 스레드 6개 |
| `-R 100` | IO 이벤트당 최대 처리 연산 수 100 |
| `-b 8192` | TCP backlog 큐 크기 |
| `-c 4096` | 최대 클라이언트 연결 수 |
| `-m 12000` | 최대 메모리 12GB |

### Persistence 설정

```ini
use_persistence=true
data_path=/disk2/test/arcus/ARCUS-DB   # 스냅샷 → 별도 디스크
logs_path=/home/test/arcus/ARCUS-DB    # cmdlog → 다른 디스크
```

data_path와 logs_path를 **디스크를 분리해서** 설정했다. 체크포인트가 스냅샷을 쓸 때 디스크 IO를 많이 먹는데, 같은 디스크에 있으면 cmdlog flush도 같이 지연된다. 디스크를 나누면 둘이 서로 간섭하지 않는다.

### 비교 대상

세 가지 모드를 비교했다:

| 모드 | 설명 |
|---|---|
| **OFF** | persistence 꺼진 순수 캐시 모드. 기준선 |
| **ASYNC** | 비동기 로깅. 버퍼에 쓰고 바로 응답 |
| **SYNC** | 동기 로깅. 디스크에 flush 확인 후 응답 |

---

## 시험 데이터

- 전체 5천만 건 (총 6.5GB)
- 키 크기: 9~17바이트 (`memtier-1` ~ `memtier-50000000`)
- 값 크기: 50바이트
- 아이템 평균 크기: 130바이트 (키 + 값 + 메타데이터)

---

## 시험 시나리오 및 결과

### 삽입(SET) 성능

5천만 건 전부 삽입. 100% write 워크로드라 persistence 오버헤드가 가장 크게 드러나는 시험이다.

```bash
memtier_benchmark --threads=8 --clients=50 --data-size=50 \
  --key-pattern=P:P --key-minimum=1 --key-maximum=50000000 \
  --ratio=1:0 --requests=125000 --print-percentiles=90,95,99
```

**결과 해석**

- ASYNC는 OFF(캐시)와 거의 차이 없음
- SYNC도 OFF 대비 처리량이 많이 떨어지지 않음

SYNC가 생각보다 성능이 좋은 이유는 **group commit** 때문이다. 워커 스레드가 flush를 기다리는 동안 블로킹 없이 다른 요청을 처리하고, 로그 플러시 스레드가 한 번의 fsync로 여러 워커의 대기를 한꺼번에 풀어준다. 동기지만 논블로킹으로 설계된 덕분이다.

### 조회(GET) 성능

5천만 건 삽입 후 균등 분포로 조회.

```bash
memtier_benchmark --threads=8 --clients=50 --data-size=50 \
  --key-pattern=R:R --key-minimum=1 --key-maximum=50000000 \
  --distinct-client-seed --randomize --ratio=0:1 --requests=125000 \
  --print-percentiles=90,95,99
```

**결과 해석**

- OFF, ASYNC, SYNC 모두 동일한 성능

당연하다. 조회 연산은 데이터를 변경하지 않으므로 cmdlog에 기록할 게 없다. persistence 모드와 무관하게 똑같이 동작한다.

### 혼합(SET+GET) 성능

삽입:조회 비율을 1:9, 3:7, 5:5로 바꿔가며 측정.

```bash
memtier_benchmark --threads=8 --clients=50 --data-size=50 \
  --key-pattern=R:R --key-minimum=1 --key-maximum=50000000 \
  --distinct-client-seed --randomize --ratio=1:9 --requests=125000 \
  --print-percentiles=90,95,99
```

**결과 해석**

세 비율 모두 OFF 대비 성능 차이가 크지 않다. 특히 1:9(SET 10%, GET 90%)의 경우 SYNC 모드의 처리량이 OFF보다 20K 정도밖에 차이 나지 않았다.

실서비스 워크로드는 대부분 이 혼합 형태인데, 조회 비중이 높을수록 persistence 오버헤드가 희석된다. ARCUS가 조회 성능이 좋기 때문에 조회 비중이 높은 시험에서는 persistence 모드와 무관하게 고성능을 보인다.

---

## 체크포인트 영향

5천만 건 삽입 상태에서 1:9 혼합 연산을 ASYNC 모드로 돌렸을 때의 모니터링 결과다.

- 평소 처리량: 약 150K ops/s (변경 + 조회 합산)
- 체크포인트 발동 구간(약 2분): 처리량이 일시적으로 감소
  - 이 시점에 스냅샷 파일에 약 5GB가 기록됨

체크포인트가 진행되는 동안 스냅샷 파일을 쓰는 IO 때문에 cmdlog flush가 약간 지연되고, 그게 처리량 감소로 이어진다. 블로그에서는 향후 체크포인트를 천천히 진행하도록 개선해서 처리량 감소를 더 줄일 계획이라고 언급한다.

---

## 핵심 결론

1. **ASYNC는 캐시 모드와 거의 동일한 성능** → 데이터 유실 가능성을 감수할 수 있다면 ASYNC가 합리적인 선택
2. **SYNC도 생각보다 성능이 좋다** → group commit 덕분에 동기 보장을 하면서도 오버헤드를 최소화
3. **조회 비중이 높은 실서비스에서는 persistence 오버헤드가 거의 체감되지 않는다**
4. **디스크 분리는 필수** → data_path(스냅샷)와 logs_path(cmdlog)를 다른 디스크에 두지 않으면 체크포인트 때 cmdlog flush가 같이 지연됨
5. **이 시험은 HDD 기준** → 블로그에서 NVMe SSD 같이 높은 IOPS 디스크를 사용하면 더 높은 성능을 볼 수 있다고 언급
