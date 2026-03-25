# arcus-memcached GET 흐름

```mermaid
flowchart TD
    A["클라이언트: 'get mykey\r\n'"] --> B

    subgraph process_command_ascii
        B["응답 버퍼 초기화\n(msgcurr/msgused/iovused = 0)"]
        B --> C["add_msghdr(c)\n응답 봉투 준비"]
        C --> D["tokenize_command()\n'get' / 'mykey' 토큰 분리"]
        D --> E{"cmd[0] == 'g'\nclen == 3"}
    end

    E -->|"process_get_command()"| F

    subgraph process_get_command
        F["key_token = &tokens[KEY_TOKEN]"]
        F --> G

        subgraph "멀티 키 루프 (do-while)"
            G{"key_token->length != 0"}
            G -->|"yes"| H["process_get_single()"]
            H --> I["key_token++"]
            I --> G
            G -->|"no: 이번 배치 소진"| J{"key_token->value != NULL\n아직 못 파싱한 원문 있나?"}
            J -->|"yes"| K["tokenize_command() 재호출\nkey_token = tokens 리셋"]
            K --> G
            J -->|"no"| L["루프 종료"]
        end

        L --> M["c->icurr = c->ilist\nc->suffixcurr = c->suffixlist"]
        M --> N["add_iov(c, 'END\r\n', 5)"]
        N --> O["conn_set_state(c, conn_mwrite)"]
    end

    subgraph process_get_single["process_get_single() — 키 하나"]
        H --> P["mc_engine.v1->get()\n엔진에서 item 조회"]
        P --> Q{"히트?"}
        Q -->|"미스"| R["STATS_MISSES\nreturn ENGINE_SUCCESS"]
        Q -->|"히트"| S["get_item_info()\n→ c->hinfo에 메타 정보 추출"]
        S --> T["suffix 버퍼 구성\n' flags bytes\r\n'"]
        T --> U["add_iov() x4\n'VALUE ' + key + suffix + data"]
        U --> V["ilist에 item 저장\n(전송 완료 후 release용)\nileft++"]
        V --> W["STATS_HITS\nreturn ENGINE_SUCCESS"]
    end

    O --> X["sendmsg()\niov 조각들 전송"]
    X --> Y["전송 완료 후\nilist 순회 → mc_engine.v1->release()\nrefcount--"]
```

---

## conn — 클라이언트 연결 하나를 표현하는 구조체

한 클라이언트 연결 = `conn` 하나. 명령 파싱에서 응답 전송까지, 한 클라이언트의 모든 I/O 상태와 중간 데이터를 담는 컨텍스트 객체다.

명령 하나가 처리되는 동안 `conn`은 계속 업데이트된다.

1. **명령 수신** → `rcurr`, `rbytes` 업데이트
2. **명령 파싱** → `cmd`, `store_op`, `coll_op` 등 세팅
3. **작업 처리** → `item`, `iov`, `ilist` 등 채워넣기
4. **응답 전송** → `wbuf`, `wcurr`, `wbytes` 소진
5. **완료** → 다음 명령을 위해 초기화

`process_command_ascii` 시작 부분에서 `c->msgcurr = 0; c->msgused = 0; c->iovused = 0;`으로 리셋하는 것도 그 흐름의 일부 — 이전 명령의 흔적을 지우고 새로 시작하는 것.

> [!NOTE]
> `set`처럼 데이터 본문을 따로 읽어야 하는 명령은 중간 상태(`conn_nread`)로 `conn.state`를 바꿔두고 함수를 리턴한다. 나중에 데이터가 도착했을 때 다시 깨워서 이어서 처리하는 것. 그때 중간 정보(`item`, `rlbytes` 등)를 `conn`에서 꺼내서 쓴다. `conn`은 비동기 처리의 컨텍스트를 유지하는 역할도 한다.

주요 필드 요약:

| 필드 | 역할 |
|---|---|
| `sfd` | 클라이언트 소켓 fd |
| `state` | 현재 상태 머신 상태 (`conn_new_cmd`, `conn_nread`, `conn_mwrite`, ...) |
| `rbuf` / `rcurr` / `rbytes` | 명령 수신 버퍼 |
| `wbuf` / `wcurr` / `wbytes` | 응답 전송 버퍼 |
| `ritem` / `rlbytes` | set 2단계: value 수신 버퍼 주소, 남은 바이트 수 |
| `item` / `store_op` | set 1단계에서 만들어둔 item과 연산 종류 |
| `iov` / `iovused` | scatter/gather I/O 조각 배열 |
| `msglist` / `msgcurr` | iov 묶음을 담는 msghdr 배열 |
| `ilist` / `ileft` / `icurr` | get 히트된 item 포인터 임시 보관 (전송 후 release용) |
| `coll_eitem` / `coll_op` | 컬렉션 명령 처리 컨텍스트 |
| `hinfo` / `einfo` | item / element 메타 정보 스냅샷 |
| `noreply` | noreply 옵션 여부 |

---

## process_command_ascii

**응답 버퍼 초기화**

```c
c->msgcurr = 0;
c->msgused = 0;
c->iovused = 0;
if (add_msghdr(c) != 0) { ... }
```

이전 명령의 iov/msg 흔적을 0으로 리셋하고, 빈 `msghdr` 슬롯 하나를 확보해둔다. 이게 나중에 `add_iov()`로 응답 조각들을 담을 그릇이 된다.

**tokenize_command()**

```c
ntokens = tokenize_command(command, cmdlen, tokens, MAX_TOKENS);
```

`"get mykey\r\n"` → `tokens[0]="get"`, `tokens[1]="mykey"`, `ntokens=3`. 공백으로 쪼개는 함수.

토큰 배열을 `MAX_TOKENS+1`로 선언하는 이유는, 마지막 슬롯을 **아직 토큰화 안 된 나머지 명령의 길이 보관용**으로 예약해두기 때문. `get k1 k2 k3 ...`처럼 키가 많아서 한 번에 다 토큰화 못할 때를 대비한 설계.

**분기**

```c
cmd = tokens[COMMAND_TOKEN].value;   // "get"
clen = tokens[COMMAND_TOKEN].length; // 3

if (cmd[0] == 'g') {
    if (clen == 3 && strcmp(cmd, "get") == 0) {
        process_get_command(c, tokens, ntokens, false, false);
```

`cmd[0]`으로 1차 필터링 후 `strcmp`. `strcmp` 전에 길이로 먼저 걸러서 불필요한 문자열 비교를 줄이는 최적화. `"getattr"`은 `clen==3` 조건에서 이미 탈락.

---

## add_msghdr — 왜 하는 거야?

memcached는 응답을 보낼 때 `sendmsg()` 시스템 콜을 쓴다. 이 시스템 콜의 특징이 **scatter/gather I/O**다.

`sendmsg()`는 흩어져 있는 메모리 조각들의 주소 목록만 넘겨주면 커널이 알아서 순서대로 이어 붙여서 전송해준다. 그 "조각 목록"이 `iov` 배열이고, 그 목록을 담는 봉투가 `msghdr`다.

```
msghdr
  └─ msg_iov ──→ [ iov[0] ]  →  "VALUE "        (6 bytes)
                 [ iov[1] ]  →  "mykey"          (5 bytes)
                 [ iov[2] ]  →  " 0 5\r\n"       (7 bytes)
                 [ iov[3] ]  →  "hello\r\n"      (7 bytes)
                 [ iov[4] ]  →  "END\r\n"        (5 bytes)
```

`add_msghdr()`는 앞으로 `add_iov()`로 추가할 조각들을 담을 빈 봉투 하나를 준비하는 것.

정리하면:
- `add_msghdr()` — 봉투 준비
- `add_iov()` — 봉투에 응답 조각 추가 (포인터만 등록, 복사 없음)
- `sendmsg()` — 봉투 통째로 전송

---

## iov — Scatter/Gather I/O

iov 배열 자체는 연속이지만, 각 슬롯이 가리키는 **데이터는 메모리 곳곳에 흩어져 있다.**

```
iov[0] = { "VALUE key1 0 5\r\n", 16 }  → suffix 버퍼 어딘가
iov[1] = { "hello",              5  }  → 엔진 item 메모리
iov[2] = { "\r\n",               2  }  → 상수 문자열
iov[3] = { "VALUE key2 0 3\r\n", 16 }  → 다른 suffix 버퍼
iov[4] = { "bye",                3  }  → 다른 item 메모리
iov[5] = { "\r\n",               2  }  → 상수 문자열
iov[6] = { "END\r\n",            5  }  → 상수 문자열
```

`sendmsg()`가 이 iov 배열을 받아서 OS 커널이 알아서 gather해서 한 번에 전송. 메모리 복사 없이.

iov를 쓰지 않으면 흩어진 데이터를 보내기 위해 새 버퍼를 할당하고 memcpy로 합친 뒤 전송해야 한다. CPU 자원과 메모리 대역폭이 낭비된다. iov 방식은 커널이 직접 각 메모리 주소를 찾아가서 읽기 때문에 애플리케이션 계층에서의 복사가 없다.

---

## process_get_command

```c
static inline void process_get_command(conn *c, token_t *tokens, size_t ntokens,
                                        bool return_cas, bool should_touch)
```

`inline`이다. 호출이 매우 빈번한 hot path라서 함수 호출 오버헤드를 없애려는 것.

> [!NOTE]
> 일반 함수 호출은 인자 세팅 → call → 스택 프레임 생성 → 본문 실행 → 스택 해제 → ret 과정을 거친다. `inline`을 쓰면 컴파일러가 함수 호출 대신 **함수 본문을 호출 위치에 그대로 복사 붙여넣기** 해줘서 이 오버헤드가 사라진다. get은 memcached에서 가장 많이 호출되는 명령이라 이 차이가 누적되면 의미 있다. 단, `inline`은 힌트일 뿐이라 컴파일러가 본문이 너무 크면 무시하기도 한다.

**멀티 키 루프**

```c
key_token = &tokens[KEY_TOKEN];  // KEY_TOKEN == 1

do {
    while (key_token->length != 0) {
        process_get_single(c, key_token->value, key_token->length, ...);
        key_token++;
    }

    if (key_token->value != NULL) {
        ntokens = tokenize_command(key_token->value, (key_token+1)->length, tokens, MAX_TOKENS);
        key_token = tokens;
    }
} while(key_token->value != NULL);
```

`get k1 k2 k3 ...`처럼 키가 여러 개일 수 있어서 이중 루프다.

- **안쪽 while** — 지금 토큰 배열에 있는 키들을 하나씩 처리. `length == 0`이면 sentinel(배열 끝)
- **바깥쪽 do-while** — `MAX_TOKENS`를 넘어서 한 번에 토큰화 못 한 나머지가 있으면 다시 토큰화해서 반복

`key_token->value != NULL`이 "아직 처리할 키가 남아있다"는 뜻인 이유: `tokenize_command` 종료 시 원문 끝까지 다 파싱했으면 마지막 슬롯의 `value`를 `NULL`로, MAX_TOKENS에 걸려서 중간에 멈췄으면 남은 원문의 시작 포인터로 세팅해두기 때문.

**루프 종료 후**

```c
c->icurr = c->ilist;
c->suffixcurr = c->suffixlist;
add_iov(c, "END\r\n", 5);
conn_set_state(c, conn_mwrite);
```

`ilist`/`suffixlist` 커서를 처음으로 되돌려 전송 완료 후 순회를 처음부터 할 수 있게 한다. 모든 키 처리가 끝나면 `"END\r\n"`을 iov에 추가하고 `conn_mwrite`로 전환. iov에 쌓인 응답들이 전송되기 시작한다.

---

## process_get_single

키 하나에 대한 실제 get 처리.

**엔진 조회**

```c
ret = mc_engine.v1->get(mc_engine.v0, c, &it, key, nkey, 0);
```

엔진에 키 조회를 요청한다. 히트면 `it`에 item 포인터, 미스면 `it = NULL`.

**응답 조각 등록**

```c
mc_engine.v1->get_item_info(mc_engine.v0, c, it, &c->hinfo);

char *suffix = get_suffix_buffer(c);
snprintf(suffix, SUFFIX_SIZE, " %u %u\r\n", htonl(c->hinfo.flags), c->hinfo.nbytes - 2);

add_iov(c, "VALUE ", 6);
add_iov(c, c->hinfo.key, c->hinfo.nkey);
add_iov(c, suffix, suffix_len);
add_iov_hinfo_ascii(c, &c->hinfo);  // 실제 value 데이터
```

`get_item_info()`로 item 메타 정보를 `c->hinfo`에 꺼내고, 응답 조각들을 iov에 등록한다. 메모리 복사 없이 포인터만 등록하는 것. `nbytes - 2`는 item에 `\r\n`이 포함되어 있어 클라이언트에게는 그걸 빼고 알려줘야 하기 때문.

> [!NOTE]
> `c->hinfo`는 단순 캐시가 아니라 **엔진 추상화 인터페이스**다. memcached 서버는 item의 내부 구조를 직접 알지 못한다. 엔진마다 item 구조체가 다를 수 있기 때문에, `get_item_info()`를 통해 표준화된 뷰(`hinfo`)를 받아서 사용한다. `mc_engine.v0/v1` union이 엔진을 추상화하는 것과 같은 맥락.

**ilist에 item 저장**

```c
*(c->ilist + c->ileft) = it;
c->ileft++;
```

item 포인터를 `ilist`에 저장한다. 전송 완료 후 `mc_engine.v1->release()`로 엔진에 돌려줘야 하기 때문. 엔진이 item의 refcount를 관리하는데 `get()` 호출 시 refcount가 올라가 있어서 명시적으로 release해줘야 한다.

---

## 서버 vs 엔진

이 코드에서 서버와 엔진은 역할이 분리되어 있다.

- **서버** (`memcached.c`) — 네트워크 프로토콜 처리, 연결 관리, 명령 파싱, 응답 조립
- **엔진** (`engines/default/*.c`) — 실제 데이터 저장/조회/삭제, 메모리 관리, 컬렉션 구현

get 명령을 예로 들면, 서버가 하는 일은 소켓에서 명령 읽기, 토큰 분해, 어느 엔진 API를 호출할지 결정, 응답 문자열 조립, 클라이언트로 write다. 실제 조회는 엔진에 위임한다.

```c
mc_engine.v1->get(...)       // 조회
mc_engine.v1->allocate(...)  // 메모리 할당
mc_engine.v1->store(...)     // 저장
mc_engine.v1->release(...)   // 참조 해제
```

엔진은 인터페이스를 구현한 플러그인처럼 붙는다. 인터페이스 정의는 `engine.h`의 `ENGINE_HANDLE_V1`에 있고, default engine은 그 함수 포인터를 실제 구현으로 채운다.

RDBMS로 비유하면:
- `memcached.c` → MySQL server layer (SQL 파싱, 클라이언트 프로토콜, 실행 제어)
- `default engine` → InnoDB (실제 저장 엔진, 메모리/인덱스/버퍼 관리)

다만 InnoDB처럼 디스크 페이지를 다루는 게 아니라, RAM 안의 item/collection을 직접 관리한다.