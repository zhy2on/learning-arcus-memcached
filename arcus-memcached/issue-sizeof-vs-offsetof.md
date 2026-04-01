# Issue: sizeof를 이용한 ntotal 계산의 정확성 문제

## 요약

list, set, map의 element 할당 시 `ntotal = sizeof(elem_item) + nbytes`로 계산하는데,
구조체 trailing padding 때문에 실제 필요한 크기보다 크게 계산된다.
이 오차가 Small Memory(SM) allocator에서 실제로 더 큰 슬롯 클래스를 선택하게 만들 수 있다.

---

## 문제 상황

### 현재 코드

**coll_list.c:126**
```c
size_t ntotal = sizeof(list_elem_item) + nbytes;
list_elem_item *elem = do_item_mem_alloc(ntotal, LRU_CLSID_FOR_SMALL, cookie);
```

**coll_set.c:217**
```c
size_t ntotal = sizeof(set_elem_item) + nbytes;
set_elem_item *elem = do_item_mem_alloc(ntotal, LRU_CLSID_FOR_SMALL, cookie);
```

**coll_map.c:170**
```c
size_t ntotal = sizeof(map_elem_item) + nfield + nbytes;
map_elem_item *elem = do_item_mem_alloc(ntotal, LRU_CLSID_FOR_SMALL, cookie);
```

`do_list_elem_ntotal`(해제 시 사용)도 동일한 방식으로 계산한다:
```c
static inline uint32_t do_list_elem_ntotal(list_elem_item *elem)
{
    return sizeof(list_elem_item) + elem->nbytes;
}
```

### 구조체 레이아웃과 sizeof vs offsetof

각 구조체의 마지막 필드는 `value[1]` (또는 `data[1]`) — trailing data 패턴이다.

**list_elem_item (item_base.h:167)**
```
uint16_t refcount    2바이트
uint8_t  slabs_clsid 1바이트
[패딩]               1바이트
uint32_t dummy       4바이트
*next                8바이트
*prev                8바이트
uint32_t nbytes      4바이트
char     value[1]    1바이트
[패딩]               3바이트  ← 8바이트 정렬 맞추기 위한 trailing padding
─────────────────────────────
sizeof = 32, offsetof(value) = 28 → 차이: 4바이트
```

**set_elem_item (item_base.h:178)**
```
uint16_t refcount    2바이트
uint8_t  slabs_clsid 1바이트
[패딩]               1바이트
uint32_t hval        4바이트
*next                8바이트
uint32_t nbytes      4바이트
char     value[1]    1바이트
[패딩]               3바이트  ← trailing padding
─────────────────────────────
sizeof = 24, offsetof(value) = 20 → 차이: 4바이트
```

**map_elem_item (item_base.h:188)**
```
uint16_t refcount    2바이트
uint8_t  slabs_clsid 1바이트
[패딩]               1바이트
uint32_t hval        4바이트
*next                8바이트
uint8_t  nfield      1바이트
uint16_t nbytes      2바이트
[패딩]               1바이트
unsigned char data[1]1바이트
[패딩]               3바이트  ← trailing padding
─────────────────────────────
sizeof = 24, offsetof(data) = 20 → 차이: 4바이트
```

세 구조체 모두 `sizeof - offsetof(trailing data field) = 4바이트` 차이가 있다.

---

## 왜 문제가 되는가

### 메모리 할당 경로

```
do_list_elem_alloc(nbytes)
  ntotal = sizeof(list_elem_item) + nbytes   // 실제 필요량보다 4바이트 큼
  └── do_item_mem_alloc(ntotal, LRU_CLSID_FOR_SMALL, ...)
        └── do_slabs_alloc(ntotal, ...)
              size <= MAX_SM_VALUE_LEN(약 48K)이므로:
              └── do_smmgr_alloc(ntotal)       // SM allocator 사용
                    slen = do_smmgr_slen(ntotal)
                    targ = do_smmgr_memid(slen, true)
                    → slen 기반으로 슬롯 클래스 결정
```

### SM allocator의 슬롯 클래스 결정 (slabs.c:371)

```c
static inline int do_smmgr_slen(int size)
{
    int slen = (int)size + sizeof(sm_tail_t);
    slen = (((slen-1) / 8) + 1) * 8;  // 8바이트 단위로 올림
    if (slen < SM_MIN_SLOT_SIZE) slen = 32;
    return slen;
}
```

SM allocator는 요청 크기를 **8바이트 단위로 올림**해서 슬롯 클래스를 결정한다.
슬롯 클래스는 8바이트 간격으로 나뉘므로(1KB 이하 범위 기준), 4바이트 오차가
클래스 경계를 넘기면 실제로 더 큰 슬롯을 사용하게 된다.

### 구체적인 예시 (list_elem_item 기준)

SM allocator 내부적으로 `slen = ntotal + sizeof(sm_tail_t)`를 계산한다.
`sizeof(sm_tail_t)`를 T라 하면:

| nbytes | sizeof 방식 ntotal | offsetof 방식 ntotal | slen(sizeof) | slen(offsetof) | 슬롯 클래스 차이 |
|--------|-------------------|---------------------|--------------|----------------|-----------------|
| 4      | 36                | 32                  | ceil((36+T)/8)*8 | ceil((32+T)/8)*8 | T에 따라 달라짐 |
| 12     | 44                | 40                  | ceil((44+T)/8)*8 | ceil((40+T)/8)*8 | 마찬가지 |
| ...    | ...               | ...                 | ...          | ...            | ...             |

`nbytes % 8`이 5~8 범위에 해당하는 경우, 4바이트 차이가 slen을 다음 8바이트 경계로 넘겨
**한 단계 위 슬롯 클래스를 사용하게 된다.** 이는 실질적인 메모리 낭비다.

---

## 올바른 계산 방법

```c
// 정확한 크기: trailing padding을 포함하지 않는다
size_t ntotal = offsetof(list_elem_item, value) + nbytes;
size_t ntotal = offsetof(set_elem_item,  value) + nbytes;
size_t ntotal = offsetof(map_elem_item,  data)  + nfield + nbytes;
```

`offsetof`는 해당 필드가 시작하는 바이트 오프셋을 반환하므로,
trailing data가 실제로 시작하는 위치 + 데이터 크기가 정확한 ntotal이다.

---

## 참고: sizeof를 써도 문제없는 경우

일반 slab allocator(large item)는 미리 정의된 size class 단위로 청크를 할당하므로,
ntotal이 동일한 size class 경계 안에 있는 한 4바이트 오차는 실질적인 낭비로 이어지지 않는다.

하지만 SM allocator는 **8바이트 단위의 세밀한 슬롯 클래스**를 사용하기 때문에
4바이트 오차가 실제 슬롯 클래스 선택에 영향을 줄 수 있다.

---

## 관련 코드 위치

| 파일 | 라인 | 내용 |
|------|------|------|
| `engines/default/coll_list.c` | 57, 126 | `sizeof(list_elem_item) + nbytes` |
| `engines/default/coll_set.c`  | 116, 217 | `sizeof(set_elem_item) + nbytes` |
| `engines/default/coll_map.c`  | 76, 170 | `sizeof(map_elem_item) + nfield + nbytes` |
| `engines/default/item_base.h` | 167–196 | list/set/map element 구조체 정의 |
| `engines/default/slabs.c`     | 371–377 | `do_smmgr_slen` — SM 슬롯 크기 계산 |
| `engines/default/slabs.c`     | 1341–1348 | `do_slabs_alloc` — SM vs slab 분기 |
