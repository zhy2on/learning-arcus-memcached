# arcus-memcached 메모리 모델

`set`이나 `allocate` 흐름을 보다 보면 `slab class`, `LRU`, `small LRU`, `slabs_clsid`, `LRU_CLSID_FOR_SMALL` 같은 이름이 자주 나온다.

이 문서는 그 메모리 관련 개념들을 한 번에 정리하기 위한 메모다.

---

## 한 줄 요약

Arcus default engine은 같은 item을 두 가지 관점으로 관리한다.

- 메모리 할당 관점: 어느 `slab class`에서 메모리 블록을 꺼낼지
- 캐시 관리 관점: 어느 `LRU` 그룹에서 recency / reclaim / eviction을 관리할지

즉 "메모리를 어디서 받는가"와 "그 item을 어떤 LRU 큐에서 관리하는가"는 서로 연결되어 있지만 같은 개념은 아니다.

---

## slab memory란?

slab allocator는 비슷한 크기의 객체를 같은 class에 묶어 관리하는 메모리 할당 방식이다.

`slabs.h`의 `slabclass_t`를 보면 각 class가 대략 이런 정보를 가진다.

- `size`
  이 class가 다루는 chunk 크기
- `perslab`
  slab page 하나에 몇 개의 chunk가 들어가는지
- `slots`
  현재 free 상태인 chunk 목록
- `slab_list`
  실제 slab page 목록

즉 slab class는:

- "이 크기의 item은 이 class에서 할당한다"

는 식으로 메모리 블록을 나눠 관리하는 단위다.

실제 할당 시에는:

```c
unsigned int id = slabs_clsid(ntotal);
it = slabs_alloc(ntotal, id);
```

처럼 item 전체 크기 `ntotal`에 맞는 class를 고른 뒤, 그 class에서 chunk 하나를 받아온다.

> [!NOTE]
> Arcus에는 이 일반 slab allocator 외에, 작은 item을 위한 `small memory` allocator도 따로 있다. 즉 "메모리 할당" 관점에서도 Arcus는 하나의 방식만 쓰는 것이 아니다.

관련 코드 위치:

- [`slabs.h:65`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/slabs.h#L65)
  `MAX_SM_VALUE_LEN` 선언
- [`slabs.c:195`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/slabs.c#L195)
  `MAX_SM_VALUE_LEN` 초기화

---

## `slabs_clsid(size)`는 뭘 하나?

`slabs_clsid(size)`는 주어진 크기의 item을 어떤 slab class에서 할당해야 하는지 계산하는 함수다.

즉:

- 입력: item 전체 크기
- 출력: 그 크기를 담을 수 있는 slab class id

이다.

크기가 너무 커서 어떤 class에도 안 들어가면 `0`을 반환하고, 이 경우 `allocate()`는 `ENGINE_E2BIG` 또는 `NULL`로 실패한다.

관련 코드 위치:

- [`slabs.h:84`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/slabs.h#L84)
  `slabs_clsid()` 선언

---

## LRU는 뭐냐?

LRU는 캐시 item들을 최근 사용 순서로 관리하는 doubly linked list다.

`item_link_q()` / `item_unlink_q()`를 보면 item을 LRU 큐 head/tail에 연결하거나 제거하는 식으로 동작한다.

즉 LRU는:

- 어떤 item이 최근에 쓰였는지
- tail 쪽에서 어떤 item을 reclaim / evict 후보로 볼지

를 관리하는 자료구조다.

slab class가 메모리 블록 관리용이라면, LRU는 item 생명주기와 recency 관리용이라고 보면 된다.

관련 코드 위치:

- [`item_base.c:453`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L453)
  `item_link_q()`
- [`item_base.c:497`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L497)
  `item_unlink_q()`

---

## small LRU란?

Arcus는 LRU를 항상 item의 `slabs_clsid` 그대로 쓰지 않는다.

`item_link_q()`를 보면:

```c
int clsid = it->slabs_clsid;
if (IS_COLL_ITEM(it) || ITEM_ntotal(it) <= MAX_SM_VALUE_LEN) {
    clsid = LRU_CLSID_FOR_SMALL;
}
```

즉:

- collection item이거나
- item 전체 크기 `ITEM_ntotal(it)`이 `MAX_SM_VALUE_LEN` 이하이면

해당 item은 일반 class별 LRU 대신 `LRU_CLSID_FOR_SMALL` 그룹에서 관리된다.

이게 흔히 말하는 `small LRU`다.

즉 `small LRU`는:

- 작은 크기 item들
- collection item들

을 따로 모아 관리하는 LRU 그룹이다.

관련 코드 위치:

- [`item_base.c:462`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L462)
  link 시 small item / collection item이면 `LRU_CLSID_FOR_SMALL`
- [`item_base.c:505`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L505)
  unlink 시에도 같은 기준 사용

---

## small memory란?

`small memory`는 작은 item을 위한 **메모리 allocator 영역**이다.

즉 `small LRU`처럼 recency를 관리하는 리스트가 아니라, 작은 item이 들어갈 실제 메모리 블록을 좀 더 촘촘하게 관리하기 위한 전용 할당 방식이다.

`slabs.h`를 보면:

```c
/* Maximum value length for using the small memory allocator */
extern int MAX_SM_VALUE_LEN;
```

라고 되어 있고, `slabs.c`에서도 small memory allocator 관련 자료구조와 통계가 따로 존재한다.

실제로 allocator 레벨에서도 분기가 분명히 보인다.

```c
if (size <= MAX_SM_VALUE_LEN) {
    return do_smmgr_alloc(size);
}
```

free 쪽도 마찬가지로:

```c
if (size <= MAX_SM_VALUE_LEN) {
    do_smmgr_free(ptr, size);
    return;
}
```

처럼 small memory allocator 경로를 별도로 탄다.

즉 Arcus는 작은 item에 대해:

- 일반 slab class 기반 allocator만 쓰는 것이 아니라
- `MAX_SM_VALUE_LEN` 이하 크기에서는 small memory allocator 경로를 사용할 수 있게

설계되어 있다.

한 줄로 말하면:

- `small memory`
  작은 item용 메모리 할당기
- `small LRU`
  작은 item용 LRU 관리 그룹

이다.

즉 둘은 이름은 비슷하지만 같은 개념이 아니다.

관련 코드 위치:

- [`slabs.c:1346`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/slabs.c#L1346)
  작은 크기면 `do_smmgr_alloc()`
- [`slabs.c:1396`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/slabs.c#L1396)
  작은 크기면 `do_smmgr_free()`

---

## 왜 small LRU를 따로 두나?

작은 item들은 수가 많고, 접근 패턴이나 reclaim / eviction 압박도 다를 수 있다.

그래서 Arcus는 작은 item들을 한 그룹으로 모아:

- recency 관리
- reclaim 대상 탐색
- eviction 대상 탐색

을 별도로 수행할 수 있게 해둔다.

이 때문에 `do_item_mem_alloc()`에서도 small memory shortage일 때 `do_item_regain()`을 먼저 small LRU 기준으로 수행한다.

즉 small LRU는 "작은 item 전용 recency/정리 큐"라고 보면 된다.

---

## `small memory`와 `small LRU`는 어떻게 다른가?

이 둘은 모두 "small item"을 다루지만, 보는 관점이 다르다.

| 구분 | small memory | small LRU |
| --- | --- | --- |
| 성격 | 메모리 allocator | LRU 관리 그룹 |
| 역할 | 작은 item이 들어갈 실제 메모리 블록 관리 | 작은 item의 recency / reclaim / eviction 관리 |
| 질문으로 보면 | "메모리를 어디서 받을까?" | "이 item을 어떤 LRU 큐에서 관리할까?" |

즉 작은 item 하나에 대해:

- 메모리는 `small memory` allocator 경로에서 확보될 수 있고
- 캐시 관리상으로는 `small LRU`에 연결될 수 있다

고 이해하면 된다.

코드 기준으로 보면:

- allocator 층:
  [`slabs.c:1346`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/slabs.c#L1346)
- LRU 층:
  [`item_base.c:462`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L462),
  [`item_base.c:505`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L505)

에서 각각 분기되는 것이 보인다.

---

## `slab class id`와 `lruid`

코드를 보면 `do_item_mem_alloc()` 안에서:

```c
unsigned int clsid_based_on_ntotal;
unsigned int lruid;
```

를 따로 둔다.

여기서:

- `clsid_based_on_ntotal`
  실제 메모리를 어느 slab class에서 할당할지를 뜻하는 변수
- `lruid`
  reclaim / eviction 대상을 어느 LRU 그룹에서 찾을지를 뜻하는 변수

즉:

- `slab class id`
  메모리 할당 단위
- `lruid`
  LRU 관리 단위

라고 보면 된다.

관련 코드 위치:

- [`item_base.c:714`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L714)
  `clsid == LRU_CLSID_FOR_SMALL`일 때 `clsid_based_on_ntotal = slabs_clsid(ntotal)`
- [`item_base.c:719`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L719)
  `ntotal <= MAX_SM_VALUE_LEN`이면 `lruid = LRU_CLSID_FOR_SMALL`

---

## item 하나는 어떻게 보나?

item 하나를 예로 들면, 같은 객체가 이런 정보를 동시에 가진다.

1. 메모리 관점
   이 item은 어떤 `slab class`에서 할당되었는가

2. LRU 관점
   이 item은 어떤 LRU 그룹에서 관리되는가

예를 들어:

- `ITEM_ntotal(it)`이 작으면
  메모리는 특정 slab class에서 할당되지만
  LRU는 `LRU_CLSID_FOR_SMALL`에서 관리될 수 있다

즉 하나의 item에 대해:

- 메모리 출처
- LRU 관리 위치

를 별도로 생각해야 한다.

---

## `item_link_q()` / `item_unlink_q()`에서 보면

이 함수들이 `it->slabs_clsid`만 그대로 쓰지 않고, small item인지 다시 확인하는 이유도 여기 있다.

```c
int clsid = it->slabs_clsid;
if (IS_COLL_ITEM(it) || ITEM_ntotal(it) <= MAX_SM_VALUE_LEN) {
    clsid = LRU_CLSID_FOR_SMALL;
}
```

즉:

- 메모리는 `slabs_clsid` 기준으로 잡혔더라도
- LRU 연결은 small LRU 쪽으로 갈 수 있다

는 뜻이다.

이 포인트를 놓치면:

- "왜 이미 `slabs_clsid`가 있는데 또 LRU용 class를 다시 계산하지?"

가 헷갈리기 쉽다.

관련 코드 위치:

- [`item_base.c:462`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L462)
- [`item_base.c:505`](/Users/zhy2on/Documents/arcus/arcus-memcached/engines/default/item_base.c#L505)

---

## `do_item_mem_alloc()`에서 보면

`do_item_mem_alloc()`은 이 두 축을 동시에 사용한다.

- `clsid_based_on_ntotal`
  새 raw 메모리 블록을 slab allocator에서 받을 때 사용
- `lruid`
  reclaim / eviction / regain 대상을 tail에서 찾을 때 사용

즉 메모리 확보 정책은:

- 할당은 slab 기준으로 하고
- 희생양 탐색은 LRU 기준으로 한다

고 이해하면 된다.

---

## 헷갈리지 않게 정리하면

- `slab`
  메모리 블록 관리 구조
- `slab class`
  비슷한 크기 객체를 묶는 메모리 class
- `slabs_clsid`
  item 크기에 맞는 slab class id
- `LRU`
  최근 사용 순서를 관리하는 리스트
- `small LRU`
  작은 item / collection item을 따로 관리하는 LRU 그룹
- `lruid`
  reclaim / eviction을 어느 LRU 그룹에서 수행할지 나타내는 id

---

## 마지막 정리

Arcus 메모리 모델을 읽을 때는:

- slab class는 "메모리를 어디서 받을지"
- LRU group은 "그 item을 어떤 recency/정리 큐에서 관리할지"

라는 식으로 나눠 생각하면 가장 덜 헷갈린다.

특히 `small LRU`는 작은 item들을 별도로 관리하기 위한 LRU 그룹이고, 이 때문에 어떤 item은 메모리는 특정 slab class에서 할당되더라도 LRU는 `LRU_CLSID_FOR_SMALL`에서 관리될 수 있다.
