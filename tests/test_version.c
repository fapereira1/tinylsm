#include "tinylsm/version.h"
#include <assert.h>
#include <string.h>

int main(void) {
  const char *ver = tinylsm_version();
  assert(ver != NULL);
  assert(strcmp(ver, "0.1.0") == 0);
  return 0;
}
