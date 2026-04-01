# `sizeof`와 padding

## 한 줄 요약

`sizeof`는 멤버 크기의 단순 합이 아니라, **alignment와 padding까지 반영한 실제 객체 크기**를 반환한다.

즉 구조체의 `sizeof` 값에는:

- 멤버 자체 크기
- 멤버 사이의 내부 padding
- 구조체 끝의 trailing padding

이 모두 포함될 수 있다.

---

## 왜 padding이 생기나?

CPU는 보통 특정 타입이 특정 정렬 단위에 맞게 놓이길 기대한다.

예를 들어:

- `char`: 보통 1바이트 정렬
- `short`: 보통 2바이트 정렬
- `int`: 보통 4바이트 정렬
- `double`: 보통 8바이트 정렬

그래서 컴파일러는 구조체 레이아웃을 만들 때, 각 멤버가 자기 alignment를 만족하도록 중간에 빈 공간을 넣을 수 있다.

이 빈 공간이 padding이다.

---

## 내부 padding 예시

```c
struct S {
    char c;
    int i;
};
```

보통 메모리는 이렇게 배치된다.

```text
offset 0: c        (1 byte)
offset 1-3: padding
offset 4-7: i      (4 bytes)

sizeof(struct S) == 8
```

여기서는 `c` 다음에 `i`를 4바이트 경계에 맞추기 위해 내부 padding이 들어간다.

---

## trailing padding 예시

```c
struct T {
    int i;
    char c;
};
```

멤버 크기만 더하면 `4 + 1 = 5`지만, `sizeof(struct T)`는 보통 8이다.

```text
offset 0-3: i      (4 bytes)
offset 4:   c      (1 byte)
offset 5-7: trailing padding

sizeof(struct T) == 8
```

왜 끝에도 padding이 붙을까?

구조체 배열을 생각하면 이해가 쉽다.

```c
struct T arr[2];
```

이때 `arr[1]`도 `int` 정렬 조건을 만족하는 주소에서 시작해야 하므로,
컴파일러가 `struct T` 전체 크기를 alignment 단위에 맞춰 반올림한다.

이 마지막 padding이 trailing padding이다.

---

## 항상 4의 배수인가?

아니다.

정확히는, **그 구조체의 alignment 요구사항의 배수**가 되도록 `sizeof`가 정해진다.

예를 들어:

- alignment 1이면 `sizeof`도 1의 배수
- alignment 2면 `sizeof`도 2의 배수
- alignment 4면 `sizeof`도 4의 배수
- alignment 8이면 `sizeof`도 8의 배수

즉 "구조체 크기는 항상 4의 배수"가 아니라,
"구조체 크기는 보통 그 구조체 alignment의 배수"가 맞는 표현이다.

---

## `sizeof`가 의미하는 것

`sizeof(x)`는 "이 타입의 객체 하나를 메모리에 배치하려면 몇 바이트가 필요한가"에 가깝다.

그래서:

- 소스 코드에 적힌 멤버만 보고 계산한 크기
- 실제 객체 하나가 차지하는 메모리 크기

가 다를 수 있다.

이 차이를 만드는 주된 이유가 padding이다.

---

## 가변 길이 데이터와 연결해서 볼 때

구조체 뒤에 가변 길이 데이터를 붙여 쓰는 패턴에서는 `sizeof(struct)`를 고정 헤더 크기처럼 쓰는 경우가 많다.

이때 주의할 점은:

- `sizeof`에는 trailing padding이 포함될 수 있고
- 마지막 멤버가 `data[1]`이면 그 1바이트도 포함된다는 점이다.

그래서 고정 헤더 크기를 다룰 때는:

- `sizeof`
- `offsetof`
- flexible array member

의 차이를 같이 이해해야 한다.

---

## 기억할 문장

- `sizeof`는 padding을 포함할 수 있다.
- trailing padding은 구조체 끝에도 붙을 수 있다.
- 구조체 크기는 항상 4의 배수가 아니라, 보통 alignment의 배수다.
