# arcus-memcached Collection

---

## 공통 개념: 타입 식별 — `iflag`

```c
// item_base.h
#define ITEM_IFLAG_LIST  1
#define ITEM_IFLAG_SET   2
#define ITEM_IFLAG_MAP   3
#define ITEM_IFLAG_BTREE 4
#define ITEM_IFLAG_COLL  7   // 0b00000111

#define IS_LIST_ITEM(it)  (((it)->iflag & ITEM_IFLAG_COLL) == ITEM_IFLAG_LIST)
#define IS_COLL_ITEM(it)  (((it)->iflag & ITEM_IFLAG_COLL) != 0)
```

hash_item은 `iflag`라는 1바이트짜리 internal flag 필드를 가진다.
이 1바이트 안에 두 가지 정보가 함께 들어간다.

```
iflag (1 byte)
bit: 7        6        5        4    3    2    1    0
     WITH_CAS INTERNAL LINKED   ---  ---  [  item type  ]
     (128)    (64)     (32)               (& 0b00000111)
```

하위 3비트는 item 타입이다. 0이면 KV, 1~4이면 각 collection 타입.
상위 비트들은 item 상태 플래그다 (`ITEM_LINKED`=32, `ITEM_INTERNAL`=64, `ITEM_WITH_CAS`=128).

타입을 확인할 때 `& ITEM_IFLAG_COLL` (= `& 7`)로 마스킹하는 이유가 여기 있다.
상태 플래그 비트를 날려버리고 하위 3비트의 타입만 읽는 것이다.

---

## List

### `list_elem_item` — 데이터를 담는 노드

```c
// item_base.h
typedef struct _list_elem_item {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint32_t dummy;
    struct _list_elem_item *next;
    struct _list_elem_item *prev;
    uint32_t nbytes;
    char     value[1];
} list_elem_item;
```

list에서 실제 값을 담는 노드다. 각 필드를 하나씩 살펴보자.

**`refcount`, `slabs_clsid`**

hash_item에도 있던 것과 같은 역할이다. elem도 독립된 메모리 블록으로 할당되기 때문에,
참조 카운트와 어느 slab class에서 할당됐는지를 자기가 직접 관리한다.

**`dummy` — 명시적 padding**

`refcount`(2바이트) + `slabs_clsid`(1바이트) 다음에 포인터가 오면,
포인터는 8바이트 정렬이 필요하므로 컴파일러가 자동으로 padding을 삽입한다.
`dummy`(4바이트)는 그 padding 자리를 명시적으로 선언한 것이다.

```
without dummy (compiler inserts hidden padding):
offset 0: refcount    [##      ]  2 bytes
offset 2: slabs_clsid [#       ]  1 byte
offset 3: [padding    [#####   ]  5 bytes, invisible]
offset 8: next        [########]  8 bytes (pointer)

with dummy (padding is visible in code):
offset 0: refcount    [##      ]  2 bytes
offset 2: slabs_clsid [#       ]  1 byte
offset 3: [padding    [#       ]  1 byte, for dummy alignment]
offset 4: dummy       [####    ]  4 bytes
offset 8: next        [########]  8 bytes (pointer)
```

동작은 동일하지만, `dummy`가 있으면 구조체 레이아웃이 코드에서 바로 눈에 보인다.

**`next` / `prev` — 양방향 포인터**

set이나 map의 elem은 `next`만 있는 단방향 체인이다.
list는 `next`(tail 방향)와 `prev`(head 방향) 둘 다 있는 양방향 연결 리스트다.

양방향이 필요한 이유는 인덱스 탐색 최적화 때문이다.
index가 중간보다 뒤쪽이면 tail에서 `prev`를 따라 역방향으로 탐색한다.
단방향이었다면 항상 head부터 시작해야 해서 뒤쪽 인덱스 접근이 느려진다.

set/map이 단방향인 이유는 내부 구조 자체가 다르기 때문이다.
set/map의 `next`는 순서 탐색용 포인터가 아니라 **hash chain next**다.

```c
// set_elem_item
struct _set_elem_item *next;  /* hash chain next */

// map_elem_item
struct _map_elem_item *next;  /* hash chain next */
```

set/map은 내부적으로 해시 테이블(`set_hash_node`, `map_hash_node`)을 쓴다.
값/필드를 해시해서 버킷을 바로 찾으므로, 역방향 탐색이 필요 없다.

```
set_meta_info
  └── root: set_hash_node
              └── htab[16]          // 16-bucket hash table
                     └── elem -> elem -> ...   // collision chain (single direction)
```

| | list | set / map |
|---|---|---|
| 내부 구조 | 이중 연결 리스트 | 해시 테이블 |
| 접근 방식 | 인덱스 (위치 기반) | 값/필드 해시 (키 기반) |
| 양방향 필요성 | 있음 (tail에서 역탐색) | 없음 (해시로 버킷 직접 접근) |

```
head                                              tail
 |                                                 |
 v    next           next           next           v
[elem0]  -------->  [elem1]  -------->  [elem2]  ...
[elem0]  <--------  [elem1]  <--------  [elem2]
              prev           prev

index:   0          1               2        ...   ccnt-1
        -ccnt      -(ccnt-1)       -(ccnt-2) ...   -1
```

**`value[1]` — flexible array 패턴**

`value[1]`은 구조체의 **마지막 필드**다.
이 점을 이용해, 할당 시 뒤에 실제 데이터 공간을 추가로 붙인다.

```c
list_elem_item *elem = malloc(sizeof(list_elem_item) + nbytes);
```

`malloc`이 돌려주는 건 연속된 메모리 블록 하나다.

```
+---------------------------+---------------------------+
| list_elem_item            |  value data (nbytes)      |
| (refcount ~ value[0])     |  value[1] ~ value[n-1]    |
+---------------------------+---------------------------+
                            ^
                        elem->value
```

`elem->value`는 마지막 필드인 `value[0]`의 주소를 가리킨다.
`malloc`이 이미 그 뒤로 `nbytes`만큼 연속으로 잡아줬으니까,
`elem->value[0]`, `elem->value[1]`, ... 로 실제 데이터에 바로 접근할 수 있다.

`value[1]`이 마지막 필드가 아니었다면 뒤에 다른 필드가 오기 때문에 이 트릭을 쓸 수 없다.

---

### `list_meta_info` — collection의 루트 메타데이터

```c
typedef struct _list_meta_info {
    int32_t  mcnt;
    int32_t  ccnt;
    uint8_t  ovflact;
    uint8_t  mflags;
    uint16_t itdist;
    uint32_t stotal;
    list_elem_item *head;
    list_elem_item *tail;
} list_meta_info;
```

list collection 하나의 상태 전체를 담는 구조체다. 각 필드 역할은 다음과 같다.

**`mcnt` / `ccnt`**

`mcnt`는 최대 elem 개수, `ccnt`는 현재 elem 개수다. `mcnt`가 `int32_t`(signed)인 이유는 -1이 유효한 값이기 때문이다. -1이면 "config에 설정된 `max_list_size` 상한까지 동적으로 허용"을 의미한다. `do_list_real_maxcount()`가 이 값을 해석한다:

```c
if (maxcount < 0)                          real_maxcount = -1;
else if (maxcount == 0)                    real_maxcount = DEFAULT_LIST_SIZE; // 4000
else if (maxcount > config->max_list_size) real_maxcount = config->max_list_size;
```

**`ovflact`**

`ccnt`가 `mcnt`에 도달했을 때의 동작을 결정한다.

- `OVFL_ERROR`: 삽입 거부
- `OVFL_HEAD_TRIM`: 가장 앞 elem을 잘라내고 삽입
- `OVFL_TAIL_TRIM`: 가장 뒤 elem을 잘라내고 삽입 (기본값)

**`mflags`**

collection 상태 플래그다. sticky 여부(`COLL_META_FLAG_STICKY`)와 클라이언트가 읽을 수 있는 상태인지(`COLL_META_FLAG_READABLE`)를 담는다.

**`itdist` — hash_item 역참조**

`list_meta_info`는 hash_item의 value 영역 안에 embed된다. meta_info 포인터만 있을 때 자신이 속한 hash_item을 찾으려면 역방향으로 거리를 계산해야 한다.

```c
// 저장: meta_info와 hash_item 사이의 거리를 sizeof(size_t) 단위로 기록
info->itdist = (uint16_t)((size_t*)info - (size_t*)it);

// 복원: 저장해둔 거리만큼 빼서 hash_item 주소를 복원
#define COLL_GET_HASH_ITEM(info) ((size_t*)(info) - (info)->itdist)
```

단위가 `sizeof(size_t)`(8바이트)인 이유는 `uint16_t`로 표현 가능한 범위(65535 × 8 = 약 512KB)가 실제 거리를 충분히 커버하기 때문이다.

**`stotal`**

list 안의 모든 elem이 차지하는 총 메모리 크기다. elem 추가/삭제 시마다 업데이트된다.

**`head` / `tail`**

양방향 연결 리스트의 양 끝 포인터다. 삽입/삭제/탐색 모두 이 두 포인터에서 시작한다.

---

### `do_list_item_alloc()` — list hash_item 생성

```c
static hash_item *do_list_item_alloc(const void *key, const uint32_t nkey,
                                     item_attr *attrp, const void *cookie)
{
    uint32_t nbytes = 2; /* "\r\n" */
    uint32_t real_nbytes = META_OFFSET_IN_ITEM(nkey, nbytes)
                         + sizeof(list_meta_info) - nkey;

    hash_item *it = do_item_alloc(key, nkey, attrp->flags, attrp->exptime,
                                  real_nbytes, cookie);
    if (it != NULL) {
        it->iflag |= ITEM_IFLAG_LIST;
        it->nbytes = nbytes; /* NOT real_nbytes */
        memcpy(item_get_data(it), "\r\n", nbytes);

        list_meta_info *info = (list_meta_info *)item_get_meta(it);
        info->mcnt    = do_list_real_maxcount(attrp->maxcount);
        info->ccnt    = 0;
        info->ovflact = (attrp->ovflaction==0 ? OVFL_TAIL_TRIM : attrp->ovflaction);
        info->mflags  = 0;
        if (attrp->readable == 1) info->mflags |= COLL_META_FLAG_READABLE;
        info->itdist  = (uint16_t)((size_t*)info-(size_t*)it);
        info->stotal  = 0;
        info->head = info->tail = NULL;
    }
    return it;
}
```

`real_nbytes`로 메모리를 할당하지만, `it->nbytes`에는 2를 넣는다. 이유는 `nbytes`가 클라이언트 응답 시 value 크기로 쓰이는 필드인데, collection은 실제 value가 elem들 안에 있고 hash_item 자체에는 의미 있는 value가 없기 때문이다. 프로토콜 종결자 역할의 `"\r\n"` 2바이트만 넣는 것이다.

collection hash_item의 메모리 구조를 정리하면:

```
hash_item 생성 시:
  it->nbytes = 2  (항상 고정, 프로토콜 종결자)
  info->stotal = 0

elem 추가될 때마다:
  별도 메모리 블록으로 elem 할당
  info->stotal += elem 크기  (누적)
```

즉 collection 전체 크기는 `hash_item.nbytes`가 아니라 `list_meta_info.stotal`이 추적한다.

`item_get_meta(it)`는 hash_item 안에서 `list_meta_info`가 시작하는 위치를 계산해서 돌려주는 매크로다. `META_OFFSET_IN_ITEM`으로 계산한 위치가 바로 거기다.

---

### `do_list_elem_alloc()` — elem 메모리 확보

```c
static list_elem_item *do_list_elem_alloc(const uint32_t nbytes,
                                          const void *cookie)
{
    size_t ntotal = sizeof(list_elem_item) + nbytes;

    list_elem_item *elem = do_item_mem_alloc(ntotal, LRU_CLSID_FOR_SMALL, cookie);
    if (elem != NULL) {
        elem->slabs_clsid = slabs_clsid(ntotal);
        elem->refcount    = 0;
        elem->nbytes      = nbytes;
        elem->prev = elem->next = (list_elem_item *)ADDR_MEANS_UNLINKED;
    }
    return elem;
}
```

`do_item_mem_alloc()`에 `LRU_CLSID_FOR_SMALL`을 넘기는 이유: elem은 LRU에 등록되지 않지만 메모리는 SM allocator에서 받는다. 공간이 부족할 때 small LRU의 hash_item들을 evict해서 SM 공간을 확보하라는 의미다.

`elem->prev = elem->next = ADDR_MEANS_UNLINKED`로 초기화하는 것은 "아직 어떤 list에도 연결되지 않았다"는 표시다. 이 상태에서 `do_list_elem_link()`가 호출되면 비로소 list에 연결된다.

**elem은 eviction 단위가 아니다**

elem은 LRU에 없기 때문에 eviction 대상으로 고려되지 않는다. eviction은 항상 **hash_item 단위**로만 일어난다. hash_item이 evict되면 그 collection 전체가 제거되고, 내부 elem들도 그때 같이 해제된다.

access tracking도 hash_item 단위다. list 전체를 최근에 접근했으면 hash_item이 LRU 앞으로 오고, 오래 안 쓰면 뒤로 밀려 결국 hash_item 통째로 evict된다. "list 안의 elem 중 자주 쓰는 것만 살리고 안 쓰는 것만 골라서 evict" 같은 건 없다. collection은 전부 살거나 전부 죽거나다.

---

### `do_list_elem_find()` — 인덱스로 elem 탐색

> `engines/default/coll_list.c:158`

```c
static list_elem_item *do_list_elem_find(list_meta_info *info, int index)
{
    list_elem_item *elem;

    if (index <= (info->ccnt/2)) {
        assert(index >= 0);
        elem = info->head;
        for (int i = 0; i < index && elem != NULL; i++) {
            elem = elem->next;
        }
    } else {
        assert(index < info->ccnt);
        elem = info->tail;
        for (int i = info->ccnt-1; i > index && elem != NULL; i--) {
            elem = elem->prev;
        }
    }
    return elem;
}
```

index가 `ccnt/2`보다 작거나 같으면 head에서 `next`로 앞에서부터, 크면 tail에서 `prev`로 뒤에서부터 탐색한다. 최악의 경우에도 리스트 절반만 순회하면 되는 O(n/2) 최적화다.

이 함수가 받는 `index`는 항상 양수다. 음수 인덱스(-1, -2 등) 변환은 호출 전에 이미 완료되어 있다. 예를 들어 index -1은 `ccnt - 1`로 변환돼서 들어온다.

---

### `do_list_elem_link()` — list에 elem 연결

> `engines/default/coll_list.c:178`

```c
static ENGINE_ERROR_CODE do_list_elem_link(list_meta_info *info,
                                           const int index,
                                           list_elem_item *elem)
{
    list_elem_item *prev, *next;

    assert(index >= 0);
    if (index == 0) {
        prev = NULL;
        next = info->head;
    } else if (index >= info->ccnt) {
        prev = info->tail;
        next = NULL;
    } else {
        prev = do_list_elem_find(info, (index-1));
        next = prev->next;
    }

    elem->prev = prev;
    elem->next = next;
    if (prev == NULL) info->head = elem;
    else              prev->next = elem;
    if (next == NULL) info->tail = elem;
    else              next->prev = elem;
    info->ccnt++;

    if (1) { /* apply memory space */
        size_t stotal = slabs_space_size(do_list_elem_ntotal(elem));
        do_coll_space_incr((coll_meta_info *)info, ITEM_TYPE_LIST, stotal);
    }
    return ENGINE_SUCCESS;
}
```

index 위치에 맞게 `prev`/`next`를 찾아 양방향으로 연결하는 표준 연결 리스트 삽입이다. 세 케이스로 나뉘는 이유는 **경계 케이스 처리** 때문이다.

만약 세 케이스로 나누지 않고 항상 `do_list_elem_find(index-1)`로 prev를 찾으려 하면:

- `index == 0`일 때: `do_list_elem_find(-1)`을 호출하게 되는데, 그 함수는 `assert(index >= 0)`이 있어서 음수를 받으면 터진다.
- `index >= ccnt`일 때: `do_list_elem_find(ccnt-1)`을 호출하면 동작하긴 하지만, `info->tail`을 바로 쓰면 O(1)인데 굳이 O(n/2) 탐색을 할 필요가 없다.

| 케이스 | 이유 |
|---|---|
| `index == 0` | 경계 처리 — `find(-1)` 호출 불가 |
| `index >= ccnt` | 경계 처리 + 최적화 — `info->tail` 바로 사용 |
| 그 외 | 일반 케이스 — `do_list_elem_find()`로 탐색 |

`if (1)` 블록은 `stotal`을 업데이트하는 부분으로, 항상 실행된다. 나중에 조건을 추가하거나 이 블록을 on/off할 수 있도록 의도적으로 남긴 코드 스타일이다.

---

### `do_list_elem_unlink()` — list에서 elem 제거

> `engines/default/coll_list.c:210`

```c
static void do_list_elem_unlink(list_meta_info *info, list_elem_item *elem,
                                enum elem_delete_cause cause)
{
    {
        if (elem->prev == NULL) info->head = elem->next;
        else                    elem->prev->next = elem->next;
        if (elem->next == NULL) info->tail = elem->prev;
        else                    elem->next->prev = elem->prev;
        elem->prev = elem->next = (list_elem_item *)ADDR_MEANS_UNLINKED;
        info->ccnt--;

        if (info->stotal > 0) {
            size_t stotal = slabs_space_size(do_list_elem_ntotal(elem));
            do_coll_space_decr((coll_meta_info *)info, ITEM_TYPE_LIST, stotal);
        }

        if (elem->refcount == 0) {
            do_list_elem_free(elem);
        }
    }
}
```

표준 연결 리스트 삭제 로직이다. KV와 다른 패턴은 없고, 마지막의 `refcount` 확인만 주목할 부분이다.

`do_list_elem_get()`으로 elem을 조회할 때 refcount를 올려두기 때문에, 응답 전송 중에 unlink가 일어나더라도 refcount가 남아있으면 메모리를 바로 해제하지 않는다. 응답 전송이 끝나고 `do_list_elem_release()`가 호출될 때 비로소 해제된다.

---

## Set

### list와의 핵심 차이

set은 **중복을 허용하지 않는 무순서 집합**이다. list처럼 인덱스로 접근하는 게 아니라, 값 자체를 해시해서 버킷을 바로 찾는다. 그래서 내부 구조가 연결 리스트가 아니라 해시 테이블이다.

### 구조체

```c
// item_base.h

typedef struct _set_elem_item {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint32_t hval;                // hash value of the element value
    struct _set_elem_item *next;  // hash chain next (single direction)
    uint32_t nbytes;
    char     value[1];
} set_elem_item;

#define SET_HASHTAB_SIZE 16

typedef struct _set_hash_node {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint8_t  hdepth;              // depth of this node in the hash tree
    uint32_t tot_elem_cnt;        // total elem count under this node
    int16_t  hcnt[SET_HASHTAB_SIZE];   // elem count per bucket
    void    *htab[SET_HASHTAB_SIZE];   // bucket array (elem or child node)
} set_hash_node;

typedef struct _set_meta_info {
    int32_t  mcnt;
    int32_t  ccnt;
    uint8_t  ovflact;
    uint8_t  mflags;
    uint16_t itdist;
    uint32_t stotal;
    set_hash_node *root;          // list의 head/tail 대신 해시 트리 루트
} set_meta_info;
```

list의 `head`/`tail` 포인터 자리에 `set_hash_node *root`가 온다. elem들이 연결 리스트로 이어지는 게 아니라 해시 트리 구조로 관리된다.

### 내부 구조

```
set_meta_info
  └── root: set_hash_node (depth=0)
              ├── htab[0] --> set_elem_item -> set_elem_item -> ...
              ├── htab[1] --> set_hash_node (depth=1, 충돌 많을 때 확장)
              │                 ├── htab[0] --> set_elem_item -> ...
              │                 └── ...
              ├── htab[2] --> set_elem_item
              └── ...
```

버킷 하나에 `SET_MAX_HASHCHAIN_SIZE`(64)개를 초과하면 그 버킷을 자식 `set_hash_node`로 확장한다. 이런 식으로 해시 트리가 깊어진다.

### split / collapse

elem이 늘어나면 노드를 분할(split)하고, 줄어들면 다시 병합(collapse)한다.

```
split: 버킷 하나에 elem이 64개를 넘으면 자식 노드로 분리
  htab[3] -> [elem, elem, ..., elem]  (64개 초과)
  ->
  htab[3] -> set_hash_node (depth+1)
               ├── htab[x] -> [elem, ...]
               └── htab[y] -> [elem, ...]

collapse: 자식 노드 아래 elem이 32개 미만이면 부모로 흡수
  htab[3] -> set_hash_node (tot_elem_cnt=5)
  ->
  htab[3] -> [elem, elem, elem, elem, elem]
```

collapse 조건: `child_node->tot_elem_cnt < SET_MAX_HASHCHAIN_SIZE/2`

### list_elem_item과의 차이

| | list_elem_item | set_elem_item |
|---|---|---|
| `dummy` | 있음 (명시적 padding) | 없음 (`hval`이 그 자리를 채움) |
| `next`/`prev` | 양방향 | `next`만 (hash chain) |
| 추가 필드 | 없음 | `hval` (미리 계산해둔 해시값) |

`hval`을 elem에 저장해두는 이유는 중복 검사나 탐색 시 매번 해시를 다시 계산하지 않기 위해서다.

---

## Map

### set과의 핵심 차이

map은 set에 **field(이름)**가 추가된 구조다. set은 값 자체가 키이지만, map은 `field → value` 쌍을 저장한다. 내부 구조(해시 테이블)는 set과 거의 동일하다.

### 구조체

```c
// item_base.h

typedef struct _map_elem_item {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint32_t hval;                // hash value of the field
    struct _map_elem_item *next;  // hash chain next
    uint8_t  nfield;              // field length (bytes)
    uint16_t nbytes;              // value length (bytes)
    unsigned char data[1];        // data: <field bytes | value bytes>
} map_elem_item;

typedef struct _map_hash_node {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint8_t  hdepth;
    uint16_t cur_elem_cnt;        // set과 달리 현재 노드의 elem 수만 추적
    uint16_t cur_hash_cnt;        // 현재 노드의 자식 hash_node 수
    int16_t  hcnt[MAP_HASHTAB_SIZE];
    void    *htab[MAP_HASHTAB_SIZE];
} map_hash_node;

typedef struct _map_meta_info {
    int32_t  mcnt;
    int32_t  ccnt;
    uint8_t  ovflact;
    uint8_t  mflags;
    uint16_t itdist;
    uint32_t stotal;
    map_hash_node *root;
} map_meta_info;
```

### set_elem_item과의 차이

| | set_elem_item | map_elem_item |
|---|---|---|
| 해시 대상 | value | field |
| 데이터 필드 | `value[1]` | `data[1]` (field + value 연속 저장) |
| 크기 필드 | `nbytes` (value 크기) | `nfield` + `nbytes` (field/value 각각) |

`data[1]` 안에 field와 value가 연속으로 붙어있다. field를 꺼낼 때는 `data[0..nfield-1]`, value를 꺼낼 때는 `data[nfield..nfield+nbytes-1]`로 접근한다.

### split / collapse

set과 동일하게 elem이 늘어나면 split, 줄어들면 collapse한다.
set과의 차이는 collapse 조건이다.

```
collapse 조건: 자식 노드가 없고(cur_hash_cnt==0) 직접 달린 elem이 32개 미만
  child_node->cur_hash_cnt == 0 &&
  child_node->cur_elem_cnt < MAP_MAX_HASHCHAIN_SIZE/2
```

자식 노드가 있는 경우(`cur_hash_cnt > 0`)는 collapse하지 않는다.
자식까지 합산한 tot_elem_cnt를 보는 set과 달리, map은 노드 자체가 leaf인지 먼저 확인한다.

### set_hash_node와 map_hash_node의 차이

set과 map의 collapse(노드 병합) 조건이 다르기 때문에 추적 방식이 달라졌다.

set은 자식 노드 아래 전체 elem 수만 보면 collapse 여부를 판단할 수 있어서 `tot_elem_cnt` 하나로 충분하다.

```c
// set: 자식 노드 아래 elem이 적으면 병합
if (child_node->tot_elem_cnt < (SET_MAX_HASHCHAIN_SIZE/2))
```

map은 "자식 노드가 없는 노드인지"도 같이 봐야 하기 때문에 `cur_elem_cnt`(현재 노드에 직접 달린 elem 수)와 `cur_hash_cnt`(자식 노드 수)를 따로 관리한다.

```c
// map: 자식 노드가 없고 직접 달린 elem이 적으면 병합
if (child_node->cur_hash_cnt == 0 &&
    child_node->cur_elem_cnt < (MAP_MAX_HASHCHAIN_SIZE/2))
```

---

## list / set / map 비교

| | list | set | map |
|---|---|---|---|
| 내부 구조 | 이중 연결 리스트 | 해시 테이블 (`set_hash_node`) | 해시 테이블 (`map_hash_node`) |
| elem 접근 방식 | 인덱스 (위치 기반) | 값 해시 (키 기반) | 필드 해시 (키 기반) |
| elem 포인터 방향 | 양방향 (`next` + `prev`) | 단방향 (`next`, hash chain) | 단방향 (`next`, hash chain) |
| elem 키 | 없음 (위치가 식별자) | value 자체가 키 | field가 키, value는 별도 |
| 중복 허용 | 허용 | 불허 (set 정의) | field 기준 불허 |
| unlinked 상태 표현 | `prev = ADDR_MEANS_UNLINKED` | `next = ADDR_MEANS_UNLINKED` | `next = ADDR_MEANS_UNLINKED` |
| 내부 노드 통계 | 없음 (단순 연결 리스트) | `tot_elem_cnt` (노드 아래 전체 elem 수) | `cur_elem_cnt` + `cur_hash_cnt` (노드 단위) |
| split | 없음 | 버킷 elem 64개 초과 시 자식 노드로 분리 | 버킷 elem 64개 초과 시 자식 노드로 분리 |
| collapse | 없음 | `tot_elem_cnt < 32` | `cur_hash_cnt == 0` && `cur_elem_cnt < 32` |
