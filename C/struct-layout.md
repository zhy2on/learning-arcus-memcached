# C 구조체 레이아웃과 패딩

---

## alignment 규칙

CPU는 데이터를 읽을 때 "자기 크기에 맞는 경계"에서 시작하는 것을 요구한다.
- `uint16_t` → 2바이트 경계
- `uint32_t` → 4바이트 경계
- `uint64_t` → 8바이트 경계

컴파일러는 이 규칙을 만족시키기 위해 두 가지 padding을 삽입한다.

---

## 내부 padding (멤버 사이)

멤버 자체의 alignment를 보장하기 위해 삽입된다. 단일 구조체에서도 필요하다.

```
struct { char a; double b; }

  offset 0: a      (1B)
  offset 1~7: padding (7B)  <- double은 8바이트 경계 필요
  offset 8: b      (8B)
  sizeof = 16
```

선언 순서가 내부 padding에 영향을 준다.
작은 타입을 먼저 묶어서 선언하면 padding을 줄일 수 있다.

```
struct { char a; char b; int c; }  -> sizeof = 8  (padding 2B)
struct { int c; char a; char b; }  -> sizeof = 8  (padding 2B, 동일)
struct { char a; int c; char b; }  -> sizeof = 12 (padding 3B + 3B)
```

---

## trailing padding (구조체 끝)

구조체 크기는 **멤버 중 가장 큰 alignment의 배수**로 맞춰진다.
배열로 사용할 때 다음 원소의 첫 멤버가 올바른 경계에 오도록 보장하기 위해서다.

```
struct { uint16_t a; uint8_t b; }

  offset 0: a      (2B)
  offset 2: b      (1B)
  offset 3: trailing padding (1B)  <- uint16_t alignment=2 기준으로 4B로 맞춤
  sizeof = 4
```

`uint8_t`만 있으면 alignment = 1이므로 trailing padding 없다.

```
struct { uint8_t a; uint8_t b; uint8_t c; }  -> sizeof = 3 (padding 없음)
```

---

## btree_elem_item(data[1])의 sizeof가 9가 아닌 10인 이유

```c
typedef struct _btree_elem_item {
    uint16_t refcount;     // offset 0, 2B
    uint8_t  slabs_clsid;  // offset 2, 1B
    uint8_t  status;       // offset 3, 1B
    uint8_t  nbkey;        // offset 4, 1B
    uint8_t  neflag;       // offset 5, 1B
    uint16_t nbytes;       // offset 6, 2B
    unsigned char data[1]; // offset 8, 1B
                           // trailing padding 1B
} btree_elem_item;         // sizeof = 10
```

`uint16_t`가 있으므로 alignment = 2. 고정 필드 합계 = 9B.
9B는 2의 배수가 아니므로 trailing padding 1B가 붙어 sizeof = 10.

---

## sizeof vs offsetof — ntotal 계산

collection elem 할당 시 ntotal을 이렇게 계산한다.

```c
size_t ntotal = sizeof(set_elem_item) + nbytes;
```

`sizeof`는 trailing padding을 포함하므로, 실제 payload 시작 위치인
`offsetof(set_elem_item, value)`보다 크다.

```
offsetof(set_elem_item, value) = 20   <- value가 실제 시작하는 위치
sizeof(set_elem_item)          = 24   <- trailing padding 4B 포함

sizeof  기반: ntotal = 24 + nbytes    (4B 초과 할당)
offsetof 기반: ntotal = 20 + nbytes   (정확)
```

단, slab allocator는 고정 크기 chunk 단위로 올림하기 때문에
이 4B 차이가 실제로 다른 slab class를 타는 경우는 드물다.
현실적으로 문제가 없어서 sizeof 방식을 그대로 유지하는 것이 일반적이다.

> [!NOTE]
> trailing padding은 heap에 단독으로 할당할 때는 실제로 필요하지 않다.
> 배열에서 다음 원소의 alignment를 보장하기 위한 것이므로,
> flexible array member 패턴에서는 offsetof 기반이 더 정확한 크기를 준다.
