#ifndef __BITMAP_H__
#define __BITMAP_H__

#include <bitset>
#include <cstdint>
#include <string>

#define BITMAP_MAX_SIZE 64

using Bitmap = std::bitset<BITMAP_MAX_SIZE>;

class BitmapHelper {
public:
  static uint64_t value(Bitmap bmp)
  {
    return bmp.to_ullong();
  }

  static std::string to_string(Bitmap bmp)
  {
    return bmp.to_string<char, std::string::traits_type, std::string::allocator_type>();
  }

  static uint32_t count_bits_set(Bitmap bmp)
  {
    return static_cast<uint32_t>(bmp.count());
  }

  static uint32_t count_bits_same(Bitmap bmp1, Bitmap bmp2)
  {
    return static_cast<uint32_t>((~(bmp1 ^ bmp2)).count());
  }

  static uint32_t count_bits_diff(Bitmap bmp1, Bitmap bmp2)
  {
    return static_cast<uint32_t>((bmp1 ^ bmp2).count());
  }

  static Bitmap rotate_left(Bitmap bmp, uint32_t amount)
  {
    if (amount == 0 || amount >= BITMAP_MAX_SIZE)
      return bmp;
    return (bmp << amount) | (bmp >> (BITMAP_MAX_SIZE - amount));
  }

  static Bitmap rotate_right(Bitmap bmp, uint32_t amount)
  {
    if (amount == 0 || amount >= BITMAP_MAX_SIZE)
      return bmp;
    return (bmp >> amount) | (bmp << (BITMAP_MAX_SIZE - amount));
  }

  static Bitmap bitwise_or(Bitmap bmp1, Bitmap bmp2)
  {
    return bmp1 | bmp2;
  }

  static Bitmap bitwise_and(Bitmap bmp1, Bitmap bmp2)
  {
    return bmp1 & bmp2;
  }
};

#endif /* __BITMAP_H__ */