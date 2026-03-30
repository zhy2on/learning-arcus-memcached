# arcus-memcached 메모리 모델

---

## 두 축: 할당 vs LRU

item 하나에 대해 두 가지 질문이 동시에 존재한다.

- 질문 1: 이 item의 메모리를 어디서 받는가? → **할당 축 (Allocator)**
- 질문 2: 이 item을 어느 LRU 큐에서 관리하는가? → **LRU 관리 축**

둘은 독립적이다. 같은 `ntotal` 기준으로 나뉘지만 역할이 다르다.

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

## SM allocator: 256KB 블록 + 가변 slot

`ntotal <= MAX_SM_VALUE_LEN` (≈48KB)인 item은 여기서 할당된다.

### 블록 구조

```
  SM block (256KB = SM_BLOCK_SIZE)
  ┌──────────────┬──────────┬──────────┬────────────┬──────────────────┐
  │ sm_blck_t    │  slot A  │  slot B  │   slot C   │   free slot ...  │
  │ header (32B) │  (56B)   │  (232B)  │  (2048B)   │                  │
  └──────────────┴──────────┴──────────┴────────────┴──────────────────┘
```

### slot 내부

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

tail이 slot 끝에 붙어있는 이유: 해제 시 앞쪽 인접 slot이 free인지 역방향으로 확인하기 위해서다.

크기는 8바이트 단위(`SM_SLOT_UNIT_LEN`)로 정렬. 최소 32B, 최대 48KB.

### 할당: splitting

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

Slab으로 fallback **없음**. 경로는 오직 `ntotal` 크기로 결정된다.

```
SM alloc fail -> reclaim/regain (free items from small LRU) -> retry SM alloc
```

---

## Slab allocator: slab class + 고정 chunk

`ntotal > MAX_SM_VALUE_LEN`인 item은 여기서 할당된다.

### 구조

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

같은 class 내 chunk 크기가 동일하므로, 해제된 chunk를 다음 요청에 in-place로 바로 재사용(reclaim) 가능.

### 내부 단편화 문제

```
  50B item -> slab class 1 (chunk 96B)
  ┌─────────────────────┬──────────────────────────────────┐
  │      data (50B)     │         wasted (46B, 48%)        │
  └─────────────────────┴──────────────────────────────────┘
```

small item일수록 낭비 비율이 커진다 → SM allocator가 필요한 이유.

---

## SM vs Slab 선택 기준

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

HEAD 쪽이 최근 접근 item, TAIL 쪽이 reclaim/eviction 후보다.

### small LRU에 들어가는 것

```
  small LRU
    +-- small KV hash_item    (ntotal <= MAX_SM_VALUE_LEN)
    +-- collection hash_item  (IS_COLL_ITEM: list/btree/set/map)
          +-- elem nodes (list_elem_item, etc.)  -> NOT in any LRU
```

collection 내부 노드는 **어떤 LRU에도 없다**. eviction 단위는 collection 전체(hash_item)이고, collection이 evict되면 내부 노드도 같이 해제된다.

### slab class LRU에 들어가는 것

```
  slab class N LRU
    +-- large KV hash_item  (ntotal > MAX_SM_VALUE_LEN, NOT IS_COLL_ITEM)
```

> [!NOTE]
> slab class LRU와 달리, small LRU에는 collection hash_item도 함께 관리된다. collection은 `ntotal`이 아무리 커도 `IS_COLL_ITEM` 플래그가 있으면 무조건 small LRU로 들어간다. 내부 노드들의 메모리 해제까지 포함한 eviction을 small LRU에서 일괄 처리하기 위해서다.

### LRU 내부 마커

```
  small LRU
  HEAD -- [new items] -- curMK -- [reclaim zone] -- lowMK -- TAIL
                            ^                           ^
                        scan end                   scan start
                    (do_item_mem_alloc)
```

`lowMK`~`curMK` 구간을 스캔해 invalid item을 재활용(reclaim)한다. LRU 전체를 매번 훑지 않기 위한 구조.

---

## item 하나가 연결되는 방식

item 하나는 두 자료구조에 동시에 연결된다.

```
  [ Hash Table ]                       [ LRU ]

  bucket[3]                            small LRU
     |                                 HEAD
     v                                  |
  hash_item -h_next-> hash_item        v
                                    hash_item <-> hash_item <-> ...
                                    (linked via prev/next)
```

같은 hash_item 객체가 `h_next`로 hash table에, `prev/next`로 LRU에 동시에 연결된다.

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

`do_item_mem_alloc()`이 받는 `clsid`는 caller의 힌트다. 여기서 실제 변수 두 개를 파생한다.

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

| 변수 | 역할 |
|---|---|
| `clsid_based_on_ntotal` | `slabs_alloc(ntotal, clsid)` 호출 시 slab class 지정 |
| `lruid` | reclaim/regain/eviction 대상을 어느 LRU 큐에서 찾을지 |

---

## 관리 구조체

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

`roottable`은 해시 테이블 확장 시 전체를 한 번에 재구성하지 않고 점진적으로 재배치하기 위한 상위 인덱스 구조다.

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
