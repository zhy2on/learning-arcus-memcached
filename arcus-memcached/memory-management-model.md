# arcus-memcached 메모리 관리 모델

---

## 두 축: 할당 vs LRU

arcus-memcached에서 item 하나를 다룰 때, 항상 두 가지 질문이 동시에 존재한다.

- **질문 1**: 이 item의 메모리를 어디서 받는가? → **할당 축 (Allocator)**
- **질문 2**: 이 item을 어느 LRU 큐에서 관리하는가? → **LRU 관리 축**

이 두 축은 **독립적**이다. 같은 `ntotal` 기준으로 나뉘기는 하지만, 역할이 완전히 다르다. 할당 축은 "메모리를 어디서 받느냐"를 결정하고, LRU 축은 "어디에 꽂아서 추적하느냐"를 결정한다.

```
                    ntotal <= MAX_SM_VALUE_LEN ?
                          /              \
                        YES              NO
                         |               |
           ┌─────────────┴──┐      ┌─────┴──────────────┐
           │  SM allocator  │      │   Slab allocator   │
           │ (variable slot)│      │   (fixed chunk)    │
           └────────────────┘      └────────────────────┘
                         |               |
           ┌─────────────┴──┐      ┌─────┴──────────────┐
           │   small LRU    │      │  LRU by slab class │
           │  (clsid = 0)   │      │   (clsid = 1..N)   │
           └────────────────┘      └────────────────────┘
```

---

## 왜 Small Memory allocator를 따로 뒀냐

Slab allocator는 **고정 크기 chunk** 방식이다. 크기별로 class를 나눠서 각 class의 chunk 크기에 맞게 메모리를 준다.

```
slab class 1: 96바이트짜리 chunk
slab class 2: 120바이트짜리 chunk
slab class 3: 152바이트짜리 chunk
...
```

50바이트 아이템을 저장하면 slab class 1에 들어가고, 실제로는 96바이트 chunk를 통째로 쓴다. **46바이트는 낭비**다. large item은 크기 대비 낭비 비율이 작아서 감수할 수 있지만, small item은 낭비가 너무 크다.

collection 내부 노드들은 element 값 크기에 따라 크기가 제각각이라 더 심하다. 그렇다고 slab class를 잘게 쪼개면 class가 너무 많아지고, 이번엔 class 간 단편화 문제가 생긴다.

### 단편화(fragmentation)

메모리를 쪼개서 쓰다 보니, 남는 공간이 생기는데 그걸 제대로 못 쓰는 상태를 단편화라고 한다. slab 구조에서는 두 종류가 있다.

**내부 단편화 (internal fragmentation)**: 노드 크기보다 큰 slab chunk를 써야 할 때 chunk 안에서 낭비가 생기는 것

```
노드: 70B
-> 128B slab 사용
-> 58B 낭비
```

**외부 단편화 (slab 간)**: class를 너무 많이 쪼개면 특정 class는 남고 다른 class는 부족한 상황이 생기는 것

```
class1 (64B)  -> 거의 안 씀 (남음)
class2 (72B)  -> 부족함
class3 (80B)  -> 부족함
```

총 메모리는 남는데, 필요한 크기의 class에 메모리가 없어서 못 쓰는 상황이 된다.

결국 이런 트레이드오프가 생긴다.

> class를 잘게 쪼개면 → 내부 단편화 ↓, 대신 → class 간 단편화 ↑

small item에는 이 "적당히"가 잘 맞지 않는다. SM allocator는 **가변 크기 slot** 방식으로 이 문제를 아예 다른 방향으로 해결한다.

---

### SM allocator 구조

256KB짜리 블록(`SM_BLOCK_SIZE = 262144`)을 slab class 0에서 받아서, 그 안을 가변 크기 slot으로 쪼개 쓴다.

```
[sm_blck_t header | slot A | slot B | slot C | free slot ... ]
      32바이트       가변크기  가변크기  가변크기
```

각 slot은 head와 tail을 가진다.

```
sm_slot_t (head): status, offset, length, prev, next
sm_tail_t (tail): offset, length
```

tail이 slot 끝에 붙어 있는 이유는 **인접한 free slot을 합칠 때** 앞쪽 slot의 크기를 역방향으로 알아내기 위해서다. 앞으로 순서대로 읽으면 앞쪽 slot의 끝 위치를 모르기 때문에 tail을 통해 역방향으로 크기를 확인한다.

---

### 할당 로직 비교

**Slab allocator:**

```c
// slabs_clsid()로 크기에 맞는 class 번호 찾기
// 그 class의 freelist에서 chunk 하나 꺼내기
// freelist 비었으면 새 slab page 할당
```

단순하다. 고정 크기 chunk를 꺼내거나 새 page를 받으면 된다.

**SM allocator (`do_smmgr_alloc`):**

1. 요청 크기를 8바이트 단위로 올림 (`slen = do_smmgr_slen(size)`)
2. 그 크기에 맞는 free slot class를 찾음 (`smid = do_smmgr_memid(slen, ...)`)
3. 해당 class에 free slot이 있으면 가져옴 — slot이 요청보다 크면 **남은 부분을 잘라서 다시 free list에 넣음**
4. 없으면 새 256KB 블록을 할당하고 그 안에서 slot 분할

**free 시에는 인접 free slot과 합침 (coalescing):**

```c
// do_smmgr_free()
// 앞쪽 slot이 free면 merge
if (prv_slot != NULL) {
    cur_offset = prv_slot->offset;
    cur_length += prv_slot->length;
}
// 뒤쪽 slot이 free면 merge
if (nxt_slot != NULL) {
    cur_length += nxt_slot->length;
}
// 블록 전체가 비었으면 slab으로 반환
if (cur_offset == SM_BHEAD_SIZE && cur_length == SM_BBODY_SIZE) {
    do_smmgr_blck_free(cur_blck);
}
```

Slab은 free 시 그냥 freelist에 넣기만 한다. SM은 인접 slot을 합쳐서 단편화를 줄이고, 블록 전체가 빈 경우 slab으로 돌려보낸다.

---

### 실제 예시를 통한 이해

**Large item이 저장되는 경우**

사용자 프로필 JSON, 상품 상세 페이지, 세션 데이터 같은 것들이다. 예를 들어 전자상거래 서버가 상품 정보를 캐싱한다면:

```
product:1001 -> {"name": "노트북", "spec": ...}  -> 80KB
product:1002 -> {"name": "모니터", "spec": ...}  -> 75KB
product:1003 -> {"name": "키보드", "spec": ...}  -> 82KB
```

이런 데이터는 **같은 종류끼리 크기가 비슷하다**. 상품 정보는 대략 비슷한 포맷이라 80KB 안팎에 모인다. slab class 하나(예: 96KB chunk)가 이 범위를 커버하고, product:1001이 evict되면 그 96KB chunk는 즉시 product:2000에 재사용된다. 외부 단편화가 없다.

만약 이걸 SM으로 관리하면 어떻게 될까?

```
[80KB used: product:1001 | 75KB used: product:1002 | 82KB used: product:1003 | ...]
```

product:1002 (75KB)가 먼저 만료되면:

```
[80KB used | 75KB free | 82KB used | ...]
```

다음 요청이 76KB짜리면 이 75KB 공간에 못 들어간다. 1KB 차이로 실패하고 새 블록을 받아야 한다. large item은 크기가 크기 때문에 자투리 공간이 재활용되기 어렵다.

**Small item이 저장되는 경우**

btree collection의 내부 노드들이다. btree는 element 값 크기에 따라 각 노드 크기가 완전히 다르다.

```
btree_elem (value 10B)  -> ntotal 약 40B
btree_elem (value 200B) -> ntotal 약 230B
btree_elem (value 2KB)  -> ntotal 약 2KB
btree_indx_node         -> 크기 고정이지만 btree_elem과 섞여 들어옴
```

이걸 slab으로 관리하면 크기가 워낙 다양하니 class가 수십 개 필요하다. 그리고 40B짜리 element는 slab class 1(96B chunk)에 들어가서 56B를 낭비한다. element 10만 개면 5MB 낭비다.

SM으로 관리하면 같은 256KB 블록 안에 이 다양한 크기들이 섞여서 들어간다.

```
[40B used | 232B used | 40B used | 2048B used | 40B used | 232B used | free ...]
```

40B짜리가 지워지면:

```
[40B free | 232B used | 40B free | 2048B used | 40B free | ...]
```

단편화처럼 보이지만, 다음에 들어오는 element가 40B짜리면 그 자리를 그대로 쓴다. **small item은 크기가 작으니까 조각난 공간도 재활용될 확률이 높다.** 40B 조각이 40B 요청에 딱 맞는 것이다. large item의 75KB 조각이 76KB 요청을 못 받는 것과 대조된다.

**정리하면, 두 방식의 차이는 "자투리 공간이 재활용될 확률"이다.**

- small item: 크기가 다양하지만 작아서 → 자투리 공간도 다른 small item에 맞을 가능성이 높음 → 가변 할당이 유리
- large item: 크기가 크고 같은 종류끼리 비슷한 크기로 모임 → 자투리 공간은 거의 재활용 못 하고, 고정 chunk를 같은 용도로 재사용하는 게 더 나음 → 고정 chunk가 유리

두 방식은 서로 다른 트레이드오프를 가지고, 각각이 유리한 크기 범위가 다르기 때문에 나눠서 쓰는 것이다.

| | Large item | Small item |
|---|---|---|
| 고정 크기 낭비 | 작다 (감수 가능) | 크다 (문제) |
| 외부 단편화 위험 | 높다 (큰 연속 공간 필요) | 낮다 (작은 조각 재활용 쉬움) |
| 결론 | Slab (고정) 유리 | SM (가변) 유리 |

| | Slab allocator | SM allocator |
|---|---|---|
| 단위 | 고정 크기 chunk (class별) | 가변 크기 slot (8바이트 단위) |
| 대상 | large item | small item, collection node |
| 장점 | 단순, 빠름 | 메모리 낭비 적음 |
| 단점 | 작은 아이템 낭비 큼 | 복잡, coalescing 비용 |
| free 시 | freelist에 추가만 | 인접 slot과 합치기 |

---

## SM allocator: 256KB 블록 + 가변 slot

`ntotal <= MAX_SM_VALUE_LEN` (≈48KB)인 item은 여기서 할당된다.

### 블록 구조

256KB 블록 하나를 받아서 그 안을 slot 단위로 쪼개 쓴다. slot 크기는 요청에 따라 그때그때 달라진다.

```
  SM block (256KB = SM_BLOCK_SIZE)
  ┌──────────────┬──────────┬──────────┬────────────┬──────────────────┐
  │ sm_blck_t    │  slot A  │  slot B  │   slot C   │   free slot ...  │
  │ header (32B) │  (56B)   │  (232B)  │  (2048B)   │                  │
  └──────────────┴──────────┴──────────┴────────────┴──────────────────┘
```

### slot 내부

slot 하나는 head + data + tail로 구성된다.

```
  slot (e.g. 56B)
  ┌──────────────┬──────────────────────────────┬──────────┐
  │  sm_slot_t   │             data             │sm_tail_t │
  │  head (8B)   │            (40B)             │  (8B)    │
  └──────────────┴──────────────────────────────┴──────────┘
                                                     ^
                                       tail: used to check if the
                                       prev slot is free (backward scan)
```

tail이 slot 끝에 붙어있는 이유: 해제 시 앞쪽 인접 slot이 free인지 역방향으로 확인하기 위해서다. 현재 slot의 시작 주소에서 1바이트 앞을 보면 앞쪽 slot의 tail이 있고, 거기서 그 slot의 크기를 알 수 있다.

크기는 8바이트 단위(`SM_SLOT_UNIT_LEN`)로 정렬. 최소 32B, 최대 48KB.

### 할당: splitting

요청 크기보다 큰 free slot이 있으면, 그 slot을 잘라서 앞부분만 쓰고 나머지는 다시 free list로 돌려보낸다.

```
  request: 40B  ->  actual slot: 48B (rounded up to 8B boundary)

  before (free slot 128B):
  ┌────────────────────────────────┐
  │            free (128B)         │
  └────────────────────────────────┘
            | split
            v
  ┌──────────┬─────────────────────┐
  │ used(48B)│     free (80B)      │  <- remainder returned to free list
  └──────────┴─────────────────────┘
```

### 해제: coalescing

slot을 해제할 때, 인접한 slot이 free면 하나로 합친다. 이렇게 하면 작은 조각들이 점점 흩어지지 않고 다시 큰 free slot으로 뭉쳐진다.

```
  before:
  ┌──────────┬──────────┬──────────┐
  │ free(48B)│ used(56B)│ free(40B)│
  └──────────┴──────────┴──────────┘

  after free -> coalesce adjacent free slots:
  ┌────────────────────────────────┐
  │           free (144B)          │
  └────────────────────────────────┘
```

Slab은 해제된 chunk를 freelist에 추가만 하지만, SM은 단편화를 줄이기 위해 인접 free slot을 합친다.

### SM이 꽉 찼을 때

SM이 꽉 차도 Slab으로 fallback하지 **않는다**. 할당 경로는 오직 `ntotal` 크기로 미리 결정되기 때문이다. 대신 small LRU에서 item을 reclaim해서 공간을 확보하고 재시도한다.

```
SM alloc fail -> reclaim/regain (free items from small LRU) -> retry SM alloc
```

---

## Slab allocator: slab class + 고정 chunk

`ntotal > MAX_SM_VALUE_LEN`인 item은 여기서 할당된다.

### 구조

slab class마다 고정 크기의 chunk를 관리한다. 크기가 다른 item은 각자 맞는 class로 들어간다.

```
  slab class 1 (chunk = 96B, fixed size)
  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
  │ item(50B)│ │   free   │ │ item(80B)│ │   free   │
  └──────────┘ └──────────┘ └──────────┘ └──────────┘

  slab class 5 (chunk = 400B, fixed size)
  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
  │  item(320B)  │ │     free     │ │  item(390B)  │
  └──────────────┘ └──────────────┘ └──────────────┘
```

같은 class 안에서 chunk 크기가 모두 동일하기 때문에, 해제된 chunk를 다음 요청에 in-place로 바로 재사용(reclaim)할 수 있다. 크기를 따질 필요가 없다.

### 내부 단편화 문제

고정 크기 chunk를 쓰기 때문에 item 크기가 chunk보다 작으면 나머지가 낭비된다.

```
  50B item -> slab class 1 (chunk 96B)
  ┌─────────────────────┬──────────────────────────────────┐
  │      data (50B)     │         wasted (46B, 48%)        │
  └─────────────────────┴──────────────────────────────────┘
```

small item일수록 낭비 비율이 커진다. 이것이 SM allocator가 필요한 이유다.

---

## SM vs Slab 선택 기준

할당 방식은 `ntotal` 하나로만 결정된다. 코드 어디서 호출하든 상관없이, ntotal이 기준값 이하면 SM, 초과면 Slab이다.

```
                  ntotal
                    |
          +---------+---------+
          |                   |
  <= MAX_SM_VALUE_LEN    > MAX_SM_VALUE_LEN
    (approx. 48KB)
          |                   |
     SM allocator        Slab allocator
    (variable slot)      (fixed chunk)
          |                   |
    small LRU (0)       LRU by slab class
```

| 구분 | Small item | Large item |
|---|---|---|
| 내부 단편화 (slab 낭비) | 크다 → SM이 유리 | 감수 가능 |
| 외부 단편화 위험 (SM 약점) | 낮다 (작은 조각도 재활용됨) | 높다 (큰 연속 공간 필요) |
| 할당 방식 | SM: 가변 slot | Slab: 고정 chunk |
| LRU | small LRU (0) | slab class별 LRU |

---

## LRU 구조

LRU는 slab class 개수만큼 있고, class 0번이 small LRU다. 모든 LRU는 doubly linked list 구조로, HEAD 쪽이 최근 접근한 item, TAIL 쪽이 오래된 item이다.

```
  small LRU (LRU_CLSID_FOR_SMALL = 0)
  HEAD ---- itemA <-> itemB <-> itemC <-> itemD ---- TAIL
  (recent)                                (oldest, eviction candidate)

  slab class 2 LRU
  HEAD ---- itemX <-> itemY <-> itemZ ---- TAIL

  slab class 5 LRU
  HEAD ---- itemP <-> itemQ ---- TAIL

  ...
```

item에 접근할 때마다 HEAD 쪽으로 옮겨진다. 메모리가 부족하면 TAIL 쪽부터 eviction 후보가 된다.

### small LRU에 들어가는 것

small LRU에는 두 가지가 들어간다.

```
  small LRU
    +-- small KV hash_item    (ntotal <= MAX_SM_VALUE_LEN)
    +-- collection hash_item  (IS_COLL_ITEM: list/btree/set/map)
          +-- elem nodes (list_elem_item, etc.)  -> NOT in any LRU
```

collection 내부 노드(elem)는 **어떤 LRU에도 없다**. eviction 단위는 collection 전체(hash_item)다. collection이 evict되면 내부 노드도 연쇄적으로 같이 해제된다.

collection hash_item은 `ntotal`이 아무리 커도 `IS_COLL_ITEM` 플래그가 있으면 무조건 small LRU로 들어간다. 내부 노드들의 메모리 해제까지 포함한 eviction을 small LRU에서 일괄 처리하기 위해서다.

### slab class LRU에 들어가는 것

```
  slab class N LRU
    +-- large KV hash_item  (ntotal > MAX_SM_VALUE_LEN, NOT IS_COLL_ITEM)
```

### LRU 내부 마커

LRU 전체를 매번 처음부터 끝까지 훑으면 비효율적이다. 그래서 두 개의 마커를 써서 스캔 범위를 제한한다.

```
  small LRU
  HEAD -- [new items] -- curMK -- [reclaim zone] -- lowMK -- TAIL
                            ^                           ^
                        scan end                   scan start
                    (do_item_mem_alloc)
```

메모리가 부족할 때 `lowMK`부터 `curMK` 방향으로 스캔하면서 만료됐거나 invalid한 item을 재활용(reclaim)한다. 마커 밖은 건드리지 않는다.

---

## item 하나가 연결되는 방식

item 하나는 두 자료구조에 **동시에** 연결된다. 하나의 hash_item 객체가 hash table에도 꽂혀 있고, LRU에도 꽂혀 있는 것이다.

```
  [ Hash Table ]                       [ LRU ]

  bucket[3]                            small LRU
     |                                 HEAD
     v                                  |
  hash_item -h_next-> hash_item        v
                                    hash_item <-> hash_item <-> ...
                                    (linked via prev/next)
```

같은 hash_item 객체 안에 hash table용 포인터(`h_next`)와 LRU용 포인터(`prev/next`)가 함께 들어있다.

```
  hash_item {
      khash          // hash value (cached for fast comparison in assoc_find)
      h_next         // hash bucket collision chain
      prev, next     // LRU doubly linked list
      slabs_clsid    // memory source (slab class id)
      nkey, nbytes   // key/value size
      ...
  }
```

---

## clsid / lruid / clsid_based_on_ntotal

`do_item_mem_alloc()`은 `clsid`를 인자로 받는다. 이 값은 caller가 넘겨주는 힌트인데, 여기서 실제로 쓰이는 변수 두 개를 파생한다.

- `clsid_based_on_ntotal`: 실제 메모리 할당 시 `slabs_alloc(ntotal, clsid)`에 넘기는 slab class 번호
- `lruid`: reclaim/regain/eviction 대상을 어느 LRU 큐에서 찾을지 결정하는 번호

이 두 변수가 파생되는 방식은 caller가 collection 내부 노드를 할당하는 경우와 hash_item을 직접 할당하는 경우에 따라 달라진다.

```
  caller input: clsid  (or LRU_CLSID_FOR_SMALL)
                    |
          +---------+------------------+
          |                            |
  clsid == LRU_CLSID_FOR_SMALL        else  (direct hash_item alloc)
  (collection node alloc path)
          |                            |
  clsid_based_on_ntotal = slabs_clsid(ntotal)
  lruid  = LRU_CLSID_FOR_SMALL        clsid_based_on_ntotal = clsid
                                       lruid = (ntotal <= MAX_SM_VALUE_LEN)
                                                 ? LRU_CLSID_FOR_SMALL
                                                 : clsid
```

collection 노드 경로(`LRU_CLSID_FOR_SMALL`)로 들어오면 항상 small LRU를 쓴다. hash_item 직접 할당 경로면 ntotal 크기에 따라 small LRU 또는 slab class LRU로 갈라진다.

| 변수 | 역할 |
|---|---|
| `clsid_based_on_ntotal` | `slabs_alloc(ntotal, clsid)` 호출 시 slab class 지정 |
| `lruid` | reclaim/regain/eviction 대상을 어느 LRU 큐에서 찾을지 |

---

## 관리 구조체

LRU와 hash table은 각각 별도의 구조체로 관리된다.

```
  [ LRU management ]                [ Hash Table management ]
  struct items  (itemsp)            struct assoc  (assocp)
  +----------------------+          +---------------------------+
  | heads[0] = small LRU |          | roottable[0].hashtable[]  |
  | tails[0]             |          | roottable[1].hashtable[]  |
  | lowMK[0], curMK[0]   |          | ...                       |
  | heads[1] = class 1   |          +---------------------------+
  | tails[1]             |
  | lowMK[1], curMK[1]   |
  | ...                  |
  +----------------------+
```

`roottable`은 hash table이 커질 때 전체를 한 번에 재구성하지 않고 점진적으로 재배치하기 위한 상위 인덱스 구조다. 재배치 중에도 조회가 정상적으로 동작할 수 있게 해준다.

---

## 한눈에 정리

```
  one item
    |
    +-- memory: ntotal <= 48KB  ->  SM allocator   (variable slot, 256KB block)
    |           ntotal  > 48KB  ->  Slab allocator (fixed chunk, by class)
    |
    +-- LRU:    ntotal <= 48KB  or  IS_COLL_ITEM  ->  small LRU   (id=0)
    |           ntotal  > 48KB  (KV only)          ->  slab class LRU (id=N)
    |
    +-- Hash table: bucket + h_next collision chain
    |
    +-- collection elem nodes -> allocated from SM, NOT in any LRU
```
