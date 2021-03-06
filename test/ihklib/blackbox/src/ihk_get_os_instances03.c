#include <limits.h>
#include <errno.h>
#include <ihklib.h>
#include "util.h"
#include "okng.h"
#include "params.h"
#include "linux.h"
#include <unistd.h>

#define INDEX_DUMMY -0x80000000

const char param[] = "dev_index";
const char *values[] = {
	"INT_MIN",
	"-1",
	"0",
	"1",
	"INT_MAX",
};

int main(int argc, char **argv)
{
	int ret;
	int i;

	params_getopt(argc, argv);

	ret = linux_insmod(0);
	INTERR(ret, "linux_insmod returned %d\n", ret);

	int dev_index_input[5] = {
		INT_MIN,
		-1,
		0,
		1,
		INT_MAX
	};
	int ret_expected[5] = {
		-ENOENT,
		-ENOENT,
		0,
		-ENOENT,
		-ENOENT,
	};
	int index_input[5][1] = {
		{ INDEX_DUMMY },
		{ INDEX_DUMMY },
		{ INDEX_DUMMY },
		{ INDEX_DUMMY },
		{ INDEX_DUMMY },
	};
	int index_expected[5][1] = {
		{ INDEX_DUMMY },
		{ INDEX_DUMMY },
		{ 0 },
		{ INDEX_DUMMY },
		{ INDEX_DUMMY },
	};

	for (i = 0; i < 5; i++) {
		int num_os_instances;

		START("test-case: %s: %s\n", param, values[i]);

		ret = ihk_create_os(0);
		INTERR(ret, "ihk_create_os returned %d\n", ret);

		ret = ihk_get_num_os_instances(0);
		INTERR(ret < 0, "ihk_create_os returned %d\n", ret);
		num_os_instances = ret;

		ret = ihk_get_os_instances(dev_index_input[i],
				index_input[i], num_os_instances);
		OKNG(ret == ret_expected[i],
		     "return value: %d, expected: %d\n",
		     ret, ret_expected[i]);
		OKNG(index_input[i][0] == index_expected[i][0],
		     "get os index as expected\n");

		ret = ihk_destroy_os(0, 0);
		INTERR(ret, "ihk_destroy_os returned %d\n", ret);
	}

	ret = 0;
out:
	linux_rmmod(0);
	return ret;
}
