# Flexible Array Member와 `data[1]` 패턴

## 한 줄 요약

구조체 뒤에 가변 길이 데이터를 붙여 저장하려면,

- 과거 C89 시절에는 `data[1]` 같은 관용적 패턴을 많이 썼고
- C99부터는 flexible array member(FAM)인 `data[]`를 공식 문법으로 쓸 수 있게 됐다.

---

## 왜 이런 패턴이 필요했나?

컬렉션 element나 item 같은 구조체는 보통:

- 앞부분은 고정 크기 메타데이터
- 뒷부분은 key/value/bkey/eflag 같은 가변 길이 데이터

로 이루어진다.

이때 메모리 할당은 한 번에 하고 싶다.

예를 들어:

```c
[고정 헤더][가변 길이 데이터]
```

형태로 한 덩어리로 잡아두면:

- malloc/free 횟수가 줄고
- 캐시 locality가 좋아지고
- 포인터 관리가 단순해진다.

그래서 "구조체 마지막에 데이터가 이어진다"는 패턴이 많이 쓰였다.

---

## C89에서 자주 쓰던 `data[1]` 패턴

예전에는 이런 식으로 썼다.

```c
typedef struct {
    uint16_t refcount;
    uint8_t status;
    char data[1];
} elem_t;
```

그리고 실제 할당은:

```c
size_t ntotal = sizeof(elem_t) + payload_len - 1;
elem_t *elem = malloc(ntotal);
```

처럼 했다.

의미는:

- 선언상으로는 마지막 배열이 1바이트
- 실제로는 그 뒤를 더 크게 잡아
- `data`를 가변 길이 버퍼처럼 사용

하는 것이다.

이건 표준이 보장한 문법이라기보다, 오래된 C 코드에서 널리 쓰이던 관용구에 가깝다.

---

## `data[1]` 패턴의 특징

장점:

- C89 문법으로도 구현 가능
- 오래된 컴파일러와 호환성 좋음
- 관용적으로 널리 사용됨

주의점:

- `sizeof(elem_t)`에 마지막 `data[1]`의 1바이트가 포함된다
- alignment에 따른 trailing padding도 포함될 수 있다
- 그래서 크기 계산 시 `-1` 같은 보정이 자주 들어간다

즉 코드를 읽을 때 의도를 이해하기 어렵고,
size 계산 실수가 섞이기 쉽다.

---

## C99의 Flexible Array Member

C99에서는 이 패턴을 공식적으로 지원하는 문법이 들어왔다.

```c
typedef struct {
    uint16_t refcount;
    uint8_t status;
    char data[];
} elem_t;
```

여기서 `data[]`가 flexible array member(FAM)다.

특징은:

- 구조체의 **마지막 멤버에만** 올 수 있다
- `sizeof(elem_t)`에는 `data[]` 본체 크기가 포함되지 않는다
- 실제 할당할 때 뒤에 원하는 만큼 붙여 쓸 수 있다

예를 들어:

```c
size_t ntotal = sizeof(elem_t) + payload_len;
elem_t *elem = malloc(ntotal);
```

이렇게 더 자연스럽게 계산할 수 있다.

---

## `data[1]` vs `data[]`

### `data[1]`

```c
typedef struct {
    int len;
    char data[1];
} buf_t;
```

- 마지막 1바이트가 `sizeof`에 포함됨
- 보통 `payload_len - 1` 보정이 필요

### `data[]`

```c
typedef struct {
    int len;
    char data[];
} buf_t;
```

- `data[]` 자체는 `sizeof`에 포함되지 않음
- 보통 `payload_len`만 더하면 됨

즉 FAM 쪽이 의도가 더 명확하다.

---

## 그래도 padding은 남을 수 있다

중요한 점은 FAM을 써도 alignment 규칙 자체가 사라지는 건 아니라는 것이다.

즉:

- `data[]`는 `sizeof`에 포함되지 않지만
- 고정 헤더 부분 끝에 trailing padding은 있을 수 있다

따라서 FAM은 "`data[1]`의 1바이트 문제"를 없애주지만,
alignment에 따른 구조체 레이아웃 문제 전체를 없애주는 건 아니다.

---

## 왜 요즘은 FAM이 더 자연스러운가?

FAM은:

- 문법상 의도가 분명하고
- `sizeof` 의미가 더 깔끔하고
- size 계산이 덜 헷갈리고
- `_fixed` 같은 보조 타입이 필요 없어지는 경우가 많다

그래서 C99 이상을 쓸 수 있다면 `data[]`가 보통 더 읽기 쉽다.

---

## Arcus 같은 코드에서 읽는 포인트

구조체 마지막에:

- `value[1]`
- `data[1]`

같은 멤버가 보이면,

"이건 마지막에 가변 길이 데이터를 붙여 쓰는 오래된 C 관용구구나"
라고 읽으면 된다.

반대로:

- `value[]`
- `data[]`

가 보이면,

"이건 C99 flexible array member로 같은 의도를 더 명확하게 표현한 거구나"
라고 보면 된다.

---

## 기억할 문장

- `data[1]`는 C89 시절의 관용적 패턴
- `data[]`는 C99의 공식 flexible array member
- 둘 다 "구조체 뒤에 가변 길이 데이터를 붙여 쓴다"는 목적은 같다
- 다만 `data[]` 쪽이 `sizeof`와 의도 표현 면에서 더 깔끔하다
