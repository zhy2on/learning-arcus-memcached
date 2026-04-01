#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _btree_elem_item_fixed {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint8_t  status;
    uint8_t  nbkey;
    uint8_t  neflag;
    uint16_t nbytes;
} btree_elem_item_fixed;

typedef struct _btree_elem_item_data1 {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint8_t  status;
    uint8_t  nbkey;
    uint8_t  neflag;
    uint16_t nbytes;
    unsigned char data[1];
} btree_elem_item_data1;

typedef struct _btree_elem_item_flex {
    uint16_t refcount;
    uint8_t  slabs_clsid;
    uint8_t  status;
    uint8_t  nbkey;
    uint8_t  neflag;
    uint16_t nbytes;
    unsigned char data[];
} btree_elem_item_flex;

#define PRINT_OFFSET(type, field) \
    printf("  offsetof(%-30s, %-12s) = %zu\n", #type, #field, offsetof(type, field))

#define PRINT_SIZE(type) \
    printf("  sizeof(%-32s)   = %zu\n", #type, sizeof(type))

int main(void) {
    printf("=== offsetof ===\n\n");

    printf("[btree_elem_item_fixed]\n");
    PRINT_OFFSET(btree_elem_item_fixed, refcount);
    PRINT_OFFSET(btree_elem_item_fixed, slabs_clsid);
    PRINT_OFFSET(btree_elem_item_fixed, status);
    PRINT_OFFSET(btree_elem_item_fixed, nbkey);
    PRINT_OFFSET(btree_elem_item_fixed, neflag);
    PRINT_OFFSET(btree_elem_item_fixed, nbytes);
    printf("\n");

    printf("[btree_elem_item data[1]]\n");
    PRINT_OFFSET(btree_elem_item_data1, refcount);
    PRINT_OFFSET(btree_elem_item_data1, slabs_clsid);
    PRINT_OFFSET(btree_elem_item_data1, status);
    PRINT_OFFSET(btree_elem_item_data1, nbkey);
    PRINT_OFFSET(btree_elem_item_data1, neflag);
    PRINT_OFFSET(btree_elem_item_data1, nbytes);
    PRINT_OFFSET(btree_elem_item_data1, data);
    printf("\n");

    printf("[btree_elem_item data[]]\n");
    PRINT_OFFSET(btree_elem_item_flex, refcount);
    PRINT_OFFSET(btree_elem_item_flex, slabs_clsid);
    PRINT_OFFSET(btree_elem_item_flex, status);
    PRINT_OFFSET(btree_elem_item_flex, nbkey);
    PRINT_OFFSET(btree_elem_item_flex, neflag);
    PRINT_OFFSET(btree_elem_item_flex, nbytes);
    PRINT_OFFSET(btree_elem_item_flex, data);
    printf("\n");

    printf("=== sizeof ===\n\n");
    PRINT_SIZE(btree_elem_item_fixed);
    PRINT_SIZE(btree_elem_item_data1);
    PRINT_SIZE(btree_elem_item_flex);

    printf("\n=== trailing padding test ===\n\n");
    printf("uint16_t alignment = %zu\n\n", _Alignof(uint16_t));

    /* 1-byte씩 추가하면서 sizeof 변화 관찰 */
    struct { uint16_t a; } s1;
    struct { uint16_t a; uint8_t b; } s2;
    struct { uint16_t a; uint8_t b; uint8_t c; } s3;
    struct { uint16_t a; uint8_t b; uint8_t c; uint8_t d; } s4;

    printf("  { uint16_t }                   sizeof = %zu\n", sizeof(s1));
    printf("  { uint16_t + uint8_t }         sizeof = %zu  <- trailing padding 1B\n", sizeof(s2));
    printf("  { uint16_t + uint8_t*2 }       sizeof = %zu  <- padding 없음\n", sizeof(s3));
    printf("  { uint16_t + uint8_t*3 }       sizeof = %zu  <- trailing padding 1B\n", sizeof(s4));

    printf("\n");
    printf("  btree_elem_item fixed part (8B) + data[1] (1B) = 9B\n");
    printf("  9B -> trailing padding 1B -> sizeof = 10B\n");

    printf("\n=== alignment by largest member ===\n\n");

    struct { uint8_t a; uint8_t b; uint8_t c; } only_u8;
    struct { uint16_t a; uint8_t b; } has_u16;
    struct { uint32_t a; uint8_t b; } has_u32;
    struct { uint64_t a; uint8_t b; } has_u64;

    struct { uint8_t a; uint8_t b; uint8_t c;
             uint8_t d; uint8_t e; uint8_t f;
             uint8_t g; uint8_t h; uint8_t i; } only_u8_9;
    printf("  { uint8_t*9  }              sizeof = %zu  (align 1, no padding)\n", sizeof(only_u8_9));
    printf("  { uint8_t*3  }              sizeof = %zu  (align 1, no padding)\n", sizeof(only_u8));
    printf("  { uint16_t + uint8_t }      sizeof = %zu  (align 2, +1B padding)\n", sizeof(has_u16));
    printf("  { uint32_t + uint8_t }      sizeof = %zu  (align 4, +3B padding)\n", sizeof(has_u32));
    printf("  { uint64_t + uint8_t }      sizeof = %zu  (align 8, +7B padding)\n", sizeof(has_u64));

    return 0;
}
