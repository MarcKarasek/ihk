#include <errno.h>

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

  params_getopt(argc, argv);

  struct mems mems_input = { 0 };
  struct mems mems_query = { 0 };

  int excess;
  ret = _mems_ls(&mems_input, "MemFree", 0.02, 1UL << 30);
  INTERR(ret, "mems_ls returned %d\n", ret);

  excess = mems_input.num_mem_chunks - 4;
  if (excess > 0) {
    ret = mems_shift(&mems_input, excess);
    INTERR(ret, "mems_ls returned %d\n", ret);
  }

  /* Activate and check */
  /* Precondition */
  ret = linux_insmod(0);
  INTERR(ret, "linux_insmod returned %d\n", ret);

  int fd = ihklib_device_open(0);
  INTERR(fd < 0, "ihklib_device_open returned %d\n", fd);
  int test_mode = TEST_SMP_IHK_QUERY_MEM;
  ret = ioctl(fd, IHK_DEVICE_SET_TEST_MODE, &test_mode);
  INTERR(ret, "ioctl IHK_DEVICE_SET_TEST_MODE returned %d. errno=%d\n", ret, -errno);
  close(fd); fd = -1;

  int allowed_var = 10;
	ret = ihk_reserve_mem_conf(0, IHK_RESERVE_MEM_TOTAL, &allowed_var);
	INTERR(ret, "ihk_reserve_mem_conf returned %d\n", ret);
  int ratio = 10;
  ret = ihk_reserve_mem_conf(0, IHK_RESERVE_MEM_MAX_SIZE_RATIO_ALL, &ratio);
  INTERR(ret, "ihk_reserve_mem_conf returned %d\n", ret);

  ret = ihk_reserve_mem(0, mems_input.mem_chunks, mems_input.num_mem_chunks);
  INTERR(ret, "reserve mem return value: %d, expected: 0\n", ret);

  ret = ihk_get_num_reserved_mem_chunks(0);
  int num_query_mem_chunks = ret < 0 ? 0 : ret;

  ret = mems_init(&mems_query, num_query_mem_chunks);
  INTERR(ret, "mems_init returned %d\n", ret);

  ret = ihk_query_mem(0, mems_query.mem_chunks,
    mems_query.num_mem_chunks);
  INTERR(ret != 0, "query mem return value: %d, expected: %d\n", ret, 0);

 out:
 /* Clean up */
  mems_release();

  linux_rmmod(0);
  return ret;
}
