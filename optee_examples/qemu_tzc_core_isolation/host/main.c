// SPDX-License-Identifier: BSD-2-Clause

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define QEMU_TZC400_DEV		"/dev/qemu_tzc400"
#define QEMU_TZC400_REGION	1
#define QEMU_TZC400_FILTERS	1
#define QEMU_TZC400_SEC_ATTR_S_RDWR 3

#define QEMU_TZC400_IOCTL_MAGIC	'z'

struct qemu_tzc400_alloc {
	uint64_t phys;
	uint64_t size;
};

struct qemu_tzc400_region {
	uint32_t filters;
	uint32_t region;
	uint64_t base;
	uint64_t top;
	uint32_t sec_attr;
	uint32_t nsaid_permissions;
};

struct qemu_tzc400_touch {
	uint32_t write;
	uint32_t value;
};

#define QEMU_TZC400_IOCTL_ALLOC \
	_IOR(QEMU_TZC400_IOCTL_MAGIC, 0, struct qemu_tzc400_alloc)
#define QEMU_TZC400_IOCTL_CONFIG \
	_IOW(QEMU_TZC400_IOCTL_MAGIC, 1, struct qemu_tzc400_region)
#define QEMU_TZC400_IOCTL_TOUCH \
	_IOWR(QEMU_TZC400_IOCTL_MAGIC, 2, struct qemu_tzc400_touch)

static bool parse_uint(const char *arg, unsigned int max, unsigned int *out)
{
	char *end = NULL;
	unsigned long val;

	if (!arg || *arg == '\0')
		return false;

	errno = 0;
	val = strtoul(arg, &end, 0);
	if (errno || *end != '\0' || val > max)
		return false;

	*out = (unsigned int)val;
	return true;
}

static int pin_cpu(unsigned int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);

	if (sched_setaffinity(0, sizeof(set), &set) != 0) {
		fprintf(stderr, "sched_setaffinity(%u): %s\n",
			cpu, strerror(errno));
		return -1;
	}

	return 0;
}

static int touch_page(int fd, bool write, uint32_t *value)
{
	struct qemu_tzc400_touch touch = {
		.write = write ? 1u : 0u,
		.value = value ? *value : 0u,
	};

	if (ioctl(fd, QEMU_TZC400_IOCTL_TOUCH, &touch) != 0) {
		fprintf(stderr, "QEMU_TZC400_IOCTL_TOUCH(%s): %s\n",
			write ? "write" : "read", strerror(errno));
		return -1;
	}

	if (!write && value)
		*value = touch.value;

	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s <allowed-cpu> <denied-cpu> [allowed-nsaid]\n",
		prog);
}

int main(int argc, char *argv[])
{
	struct qemu_tzc400_alloc alloc;
	struct qemu_tzc400_region region;
	unsigned int allowed_cpu;
	unsigned int denied_cpu;
	unsigned int allowed_nsaid;
	uint32_t touch_value = 0xa5a5a5a5;
	long cpu_count;
	int fd;

	if (argc != 3 && argc != 4) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	cpu_count = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_count <= 0) {
		fprintf(stderr, "cannot determine CPU count\n");
		return EXIT_FAILURE;
	}

	if (!parse_uint(argv[1], (unsigned int)cpu_count - 1, &allowed_cpu) ||
	    !parse_uint(argv[2], (unsigned int)cpu_count - 1, &denied_cpu)) {
		usage(argv[0]);
		fprintf(stderr, "CPUs must be in range 0..%ld\n",
			cpu_count - 1);
		return EXIT_FAILURE;
	}

	if (argc == 4) {
		if (!parse_uint(argv[3], 15, &allowed_nsaid)) {
			usage(argv[0]);
			fprintf(stderr, "NSAID must be in range 0..15\n");
			return EXIT_FAILURE;
		}
	} else {
		allowed_nsaid = allowed_cpu & 0xf;
	}

	fd = open(QEMU_TZC400_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", QEMU_TZC400_DEV,
			strerror(errno));
		return EXIT_FAILURE;
	}

	if (ioctl(fd, QEMU_TZC400_IOCTL_ALLOC, &alloc) != 0) {
		fprintf(stderr, "QEMU_TZC400_IOCTL_ALLOC: %s\n",
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (alloc.size == 0 || (alloc.phys & (alloc.size - 1)) != 0) {
		fprintf(stderr, "invalid allocation: phys=0x%" PRIx64 " size=%" PRIu64 "\n",
			alloc.phys, alloc.size);
		close(fd);
		return EXIT_FAILURE;
	}

	region.filters = QEMU_TZC400_FILTERS;
	region.region = QEMU_TZC400_REGION;
	region.base = alloc.phys;
	region.top = alloc.phys + alloc.size - 1;
	region.sec_attr = QEMU_TZC400_SEC_ATTR_S_RDWR;
	region.nsaid_permissions = (1u << allowed_nsaid) |
				    (1u << (16 + allowed_nsaid));

	if (ioctl(fd, QEMU_TZC400_IOCTL_CONFIG, &region) != 0) {
		fprintf(stderr, "QEMU_TZC400_IOCTL_CONFIG: %s\n",
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (pin_cpu(allowed_cpu) != 0 ||
	    touch_page(fd, true, &touch_value) != 0) {
		close(fd);
		return EXIT_FAILURE;
	}

	puts("allowed cpu write ok");
	fflush(stdout);

	if (pin_cpu(denied_cpu) != 0) {
		close(fd);
		return EXIT_FAILURE;
	}

	puts("denied cpu read begins");
	fflush(stdout);

	if (touch_page(fd, false, &touch_value) != 0) {
		close(fd);
		return EXIT_FAILURE;
	}

	puts("denied cpu read unexpectedly ok");
	close(fd);
	return EXIT_FAILURE;
}
