#include <stdint.h>

uint64_t narrow_u16(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                    uint64_t e, uint64_t f, uint64_t g, uint16_t h);
int64_t narrow_i8(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                  uint64_t e, uint64_t f, uint64_t g, int8_t h);

typedef uint64_t (*wide8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t);

uint64_t mettle_abi_drive_u16(void) {
  wide8 f = (wide8)(void *)narrow_u16;
  return f(1, 0, 0, 0, 0, 0, 0, 0xDEAD00000000EA5FULL);
}

int64_t mettle_abi_drive_i8(void) {
  wide8 f = (wide8)(void *)narrow_i8;
  return (int64_t)f(1, 0, 0, 0, 0, 0, 0, 0x12345678000000FDULL);
}
