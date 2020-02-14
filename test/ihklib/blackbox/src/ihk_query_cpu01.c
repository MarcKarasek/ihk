#include <errno.h>
#include <ihklib.h>
#include "util.h"
#include "okng.h"
#include "cpu.h"
#include "params.h"
#include "linux.h"

const char param[] = "exsitence of IHK device file";
const char *messages[] = {
	"without IHK device file",
	"with IHK device file",
};

int main(int argc, char **argv)
{
	int ret;
	int i;

	params_getopt(argc, argv);

	/* All of McKernel CPUs */
	struct cpus cpus_input[2] = { 0 };

	for (i = 1; i < 2; i++) {
		ret = cpus_ls(&cpus_input[i]);
		INTERR(ret, "cpus_ls returned %d\n", ret);

		ret = cpus_shift(&cpus_input[i], 2);
		INTERR(ret, "cpus_shift returned %d\n", ret);
	}

	int ret_expected_reserve_cpu[] = { -ENOENT, 0 };
	int ret_expected_get_num_reserved_cpus[] = {
		-ENOENT,
		cpus_input[1].ncpus
	};
	int ret_expected[] = { -ENOENT, 0 };
	struct cpus *cpus_expected[] = { NULL, &cpus_input[1] };

	/* Activate and check */
	for (i = 0; i < 2; i++) {
		struct cpus cpus;

		/* Precondition */
		if (i == 1) {
			ret = linux_insmod(0);
			INTERR(ret, "linux_insmod returned %d\n", ret);
		}

		START("test-case: %s: %s\n", param, messages[i]);

		ret = ihk_reserve_cpu(0, cpus_input[i].cpus,
				      cpus_input[i].ncpus);
		INTERR(ret != ret_expected_reserve_cpu[i],
		       "ihk_reserve_cpu returned %d\n", ret);

		ret = ihk_get_num_reserved_cpus(0);
		INTERR(ret != ret_expected_get_num_reserved_cpus[i],
		       "ihk_get_num_reserved_cpus returned %d\n", ret);

		if (!cpus_expected[i]) {
			ret = cpus_init(&cpus, 1);
			INTERR(ret, "cpus_init returned %d\n", ret);

			ret = ihk_query_cpu(0, cpus.cpus, cpus.ncpus);
			OKNG(ret == ret_expected[i],
			     "return value: %d, expected: %d\n",
			     ret, ret_expected[i]);
		} else {
			cpus.ncpus = ret;

			ret = cpus_init(&cpus, cpus.ncpus);
			INTERR(ret, "cpus_init returned %d\n", ret);

			ret = ihk_query_cpu(0, cpus.cpus, cpus.ncpus);
			OKNG(ret == ret_expected[i],
			     "return value: %d, expected: %d\n",
			     ret, ret_expected[i]);

			ret = cpus_compare(&cpus, cpus_expected[i]);
			OKNG(ret == 0, "query result matches input\n");

			/* Clean up */
			ret = ihk_release_cpu(0, cpus_input[i].cpus,
					      cpus_input[i].ncpus);
			INTERR(ret, "ihk_release_cpu returned %d\n", ret);
		}
	}

	ret = 0;
 out:
	linux_rmmod(0);
	return ret;
}