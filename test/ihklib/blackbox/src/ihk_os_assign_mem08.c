#include <errno.h>
#include <ihklib.h>
#include "util.h"
#include "okng.h"
#include "mem.h"
#include "params.h"
#include "linux.h"

const char param[] = "Number of NUMA nodes";
const char *values[] = {
	"One node",
	"All the nodes",
};

struct mems mems_to_reserve[2];
struct mems mems_assign[2];
struct mems mems_after_assign[2];
struct mems mems_margin[2];

int main(int argc, char **argv)
{
	int ret;
	int i;

	params_getopt(argc, argv);

	/* Precondition */
	ret = linux_insmod(0);
	INTERR(ret, "linux_insmod returned %d\n", ret);

	ret = ihk_create_os(0);
	INTERR(ret, "ihk_create_os returned %d\n", ret);

	for (i = 0; i < 2; i++) {
		int excess;

		ret = mems_ls(&mems_to_reserve[i]);
		INTERR(ret, "mems_ls returned %d\n", ret);

		excess = mems_to_reserve[i].num_mem_chunks - 4;
		if (excess > 0) {
			ret = mems_shift(&mems_to_reserve[i], excess);
			INTERR(ret, "mems_shift returned %d\n", ret);
		}
	}

	/* first node */
	if (mems_to_reserve[0].num_mem_chunks > 1) {
		ret = mems_pop(&mems_to_reserve[0],
				mems_to_reserve[0].num_mem_chunks - 1);
		INTERR(ret, "mems_pop returned %d\n", ret);
	}

	for (i = 0; i < 2; i++) {
		ret = mems_copy(&mems_after_assign[i], &mems_to_reserve[i]);
		INTERR(ret, "mems_copy returned %d\n", ret);

		ret = mems_copy(&mems_margin[i], &mems_after_assign[i]);
		INTERR(ret, "mems_copy returned %d\n", ret);

		mems_fill(&mems_margin[i], 4UL << 20);
	}

	int ret_expected[2] = { 0 };

	struct mems *mems_expected[2] = {
		&mems_after_assign[0],
		&mems_after_assign[1],
	};

	/* Activate and check */
	for (i = 0; i < 2; i++) {
		START("test-case: %s: %s\n", param, values[i]);

		ret = ihk_reserve_mem(0, mems_to_reserve[i].mem_chunks,
				mems_to_reserve[i].num_mem_chunks);
		INTERR(ret, "ihk_reserve_mem returned %d\n", ret);

		ret = mems_reserved(&mems_assign[i]);
		INTERR(ret, "mems_reserved returned %d\n", ret);

		ret = ihk_os_assign_mem(0, mems_assign[i].mem_chunks,
				      mems_assign[i].num_mem_chunks);
		OKNG(ret == ret_expected[i],
		     "return value: %d, expected: %d\n",
		     ret, ret_expected[i]);

		if (mems_expected[i]) {
			ret = mems_check_assigned(mems_expected[i],
						  &mems_margin[i]);
			OKNG(ret == 0, "assigned as expected\n");

			/* Clean up */
			ret = mems_os_release();
			INTERR(ret, "mems_os_release returned %d\n",
			       ret);
		}
		ret = mems_release();
		INTERR(ret, "mems_release returned %d\n", ret);
	}

	ret = 0;
 out:
	if (ihk_get_num_os_instances(0)) {
		mems_os_release();
		ihk_destroy_os(0, 0);
	}
	mems_release();
	linux_rmmod(0);
	return ret;
}

