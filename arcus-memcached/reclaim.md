# arcus-memcached reclaim 흐름

reclaim은 `do_item_mem_alloc()` 안에서 `slabs_alloc()` 직접 할당이 어려울 때,
LRU에서 이미 죽은(invalid) item을 찾아 그 메모리를 재활용하는 과정이다.

---

## LRU 구조와 마커

새 item은 HEAD에 추가된다. 스캔은 TAIL → HEAD 방향(`->prev`)으로 진행된다.

```
TAIL(oldest)                                                   HEAD(newest)
    |                                                               |
    v                                                               v
TAIL -- [exptime=0] -- lowMK -- [zone 1] -- curMK -- [zone 2] -- HEAD
                          |----  step 1 --->|----  step 2  --->|
```

- `->next`: HEAD → TAIL 방향
- `->prev`: TAIL → HEAD 방향 (스캔 방향)
- `lowMK`: step 1 시작점. TAIL 쪽에 위치. exptime=0 item이 오면 HEAD 방향으로 밀어냄
- `curMK`: step 2 프런티어. HEAD 방향으로 전진하며 새 영역 탐색. HEAD 도달 시 lowMK로 리셋

TAIL ~ lowMK 사이: exptime=0인 영구 item들. 절대 만료 안 되므로 스캔 범위 밖으로 밀려남.

---

## 마커 초기화 / 업데이트 시점

| 시점 | 동작 |
|---|---|
| `item_link_q` — 최초 expirable item 추가 | `lowMK = curMK = it` (1회만, exptime > 0) |
| `item_unlink_q` — 마커가 가리키는 item 제거 | dangling 방지: `lowMK/curMK = it->prev` |
| step 1 — lowMK 위치 item이 exptime==0 | `lowMK = lowMK->prev` (HEAD 방향 1칸 전진) |
| step 2 — 매 iteration | `curMK = curMK->prev` (HEAD 방향 1칸 전진) |

업데이트 후 새 lowMK가 가리키는 item의 exptime은 보장되지 않는다.

---

## step 1: zone 1 재검사

zone 1(lowMK ~ curMK)은 step 2가 이미 한 번 지나간 구간이다.
그 당시엔 valid했지만 시간이 지나 TTL이 끝났을 수 있어 다시 확인한다.

```
lowMK부터 curMK 직전까지 최대 10번 탐색:

  각 item에 대해:
    - 죽어있으면 (refcount==0 이고 invalid):
        reclaim 시도
        성공하면 반환
    - 살아있으면:
        lowMK 자신이 exptime==0 이면 lowMK를 한 칸 HEAD 방향으로 밀고
        다음 item으로 이동

10번 안에 못 찾으면 step 2로 넘어감
```

---

## step 2: zone 2 신규 탐색

zone 2(curMK ~ HEAD)는 아직 한 번도 스캔하지 않은 새 영역이다.
curMK를 HEAD 방향으로 밀면서 탐색한다.

```
curMK부터 HEAD 방향으로 최대 30번 탐색:

  각 item에 대해:
    curMK를 한 칸 HEAD 방향으로 전진시키고
    현재 item이 죽어있으면:
        reclaim 시도
        성공하면 반환

curMK가 HEAD에 도달(NULL)하면:
    curMK = lowMK 로 리셋 (다음 사이클에서 처음부터 다시)

못 찾으면 slabs_alloc() 재시도 → eviction으로 넘어감
```

---

## 왜 두 마커가 필요한가

**curMK만 있으면?**

step 2가 HEAD까지 다 밀고 난 뒤 리셋할 위치를 모른다.
그리고 "step 2가 이미 지나간 구간"을 재검사할 시작점이 없어진다.

**lowMK가 있으면?**

```
할당 1: step 2가 curMK를 A까지 전진
TAIL -- lowMK --------- curMK=A ------------ HEAD

할당 2: step 1이 lowMK ~ A 재검사 (그 사이 TTL 끝난 item 찾기)
        step 2가 curMK를 A → B로 확장
TAIL -- lowMK ------- A ------- curMK=B ----- HEAD

할당 3: step 1이 lowMK ~ B 재검사
        step 2가 curMK를 B → C로 확장
        ...

curMK가 HEAD 도달 → curMK = lowMK 리셋 → 다음 사이클 시작
```

매 할당마다 LRU 전체를 훑지 않고, step 1은 이미 지나간 구간을 재확인하고
step 2는 새 구간을 조금씩 탐색하는 방식으로 비용을 분산한다.

step 1은 최대 10번, step 2는 최대 20번으로 tries를 제한하기 때문에
한 번의 할당 시도에서 최악의 경우에도 30번만 보고 넘어간다.
대신 다음 할당 시도 때 curMK가 이어서 진행하므로, 전체 LRU는
여러 번의 할당에 걸쳐 커버된다. 모든 공간을 보지 않더라도 효율적으로
reclaim 후보를 찾을 수 있는 이유다.

---

## 전체 흐름 요약

```
do_item_mem_alloc()
    |
    +-- [ENABLE_STICKY_ITEM] flushed sticky item reclaim
    |
    +-- curMK != NULL 이면:
    |       |
    |       +-- step 1: lowMK ~ curMK 재검사 (최대 10번)
    |       |       invalid item 발견 → reclaim → 반환
    |       |       lowMK 위치 exptime=0 → lowMK HEAD 방향 전진
    |       |
    |       +-- step 1 실패 → step 2: curMK ~ HEAD 탐색 (최대 30번)
    |               invalid item 발견 → reclaim → 반환
    |               curMK HEAD 도달 → curMK = lowMK 리셋
    |
    +-- slabs_alloc() 재시도
    |
    +-- 실패 → eviction
```
