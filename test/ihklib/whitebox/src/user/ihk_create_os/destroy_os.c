#include <errno.h>
#include <stdlib.h>

#include <ihklib.h>
#include <user/ihklib_private.h>
#include <user/okng_user.h>

#include <blackbox/include/util.h>
#include <blackbox/include/cpu.h>
#include <blackbox/include/mem.h>
#include <blackbox/include/params.h>
#include <blackbox/include/linux.h>

int main(int argc, char **argv)
{
  int ret;
  int os_index = atoi(argv[1]);
  params_getopt(argc, argv);

  // char mode[6] = "\0";
  // sprintf(mode, "%d", TEST_IHK_CREATE_OS);
  // ret = setenv(IHKLIB_TEST_MODE_ENV_NAME, mode, 1);
  // INTERR(ret, "setenv returned %d. errno=%d\n", ret, -errno);

  /* Activate and check */
  /* Precondition */
//  ret = linux_insmod(0);
  INTERR(ret, "linux_insmod returned %d\n", ret);
  printf("%d", os_index);
  if (ihk_get_num_os_instances(0)) {
		ret = ihk_destroy_os(0, os_index);
	}


 out:

//  linux_rmmod(0);
  return ret;
}
