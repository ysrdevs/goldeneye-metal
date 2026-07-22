#include "ge_crash_guards.h"

#include <cstdint>

int main() {
  using ge::crash_guards::RecoverPackedDataPureVirtualDispatch;

  uint64_t result = 0xFEDCBA9876543210ull;
  if (RecoverPackedDataPureVirtualDispatch(0, result) ||
      result != 0xFEDCBA9876543210ull) {
    return 1;
  }

  if (RecoverPackedDataPureVirtualDispatch(0x823D5E48u, result) ||
      result != 0xFEDCBA9876543210ull) {
    return 2;
  }

  if (!RecoverPackedDataPureVirtualDispatch(0x823EDF20u, result) || result != 0) {
    return 3;
  }
  return 0;
}
