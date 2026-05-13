#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "common/oxbow.h"

#define LOAD_CONFIG_STR(var) g_sd_conf.var = getenv(#var);
#define LOAD_CONFIG_INT(var) g_sd_conf.var = get_val_from_env(#var);
#define PRINT_CONFIG_STR(var)                                                  \
	do {                                                                   \
		printf(#var "=%s\n", g_sd_conf.var);                           \
	} while (0)
#define PRINT_CONFIG_INT(var)                                                  \
	do {                                                                   \
		printf(#var "=%d\n", g_sd_conf.var);                           \
	} while (0)

struct secure_daemon_config g_sd_conf = { 0 };

static int get_val_from_env(char *conf_name)
{
	return getenv(conf_name) ? atoi(getenv(conf_name)) : 0; // default is 0
}

/* Strip leading/trailing whitespace in-place and return the new start. */
static char *strtrim(char *s)
{
	char *end;
	if (!s)
		return s;
	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

/* Parse a whitespace-separated list of PCIe BDFs into
 * g_sd_conf.pcie_nvme_addr_list[] and set g_sd_conf.pcie_nvme_nr_vfs.
 *
 * Whitespace-only separation is intentional: the same env string is also
 * consumed by SPDK setup scripts (e.g. PCI_ALLOWED), which require a
 * whitespace-separated list. Commas are rejected up-front so users don't
 * get a silent mismatch between the daemon and the setup scripts.
 *
 * Each token is strdup'd so the parsed list is independent of the
 * (possibly env-owned) raw string.
 */
static void parse_pcie_nvme_addr_list(void)
{
	const char *raw = g_sd_conf.pcie_nvme_addr;
	char *dup, *tok, *save = NULL, *trimmed;
	int n = 0;

	g_sd_conf.pcie_nvme_nr_vfs = 0;
	for (int i = 0; i < MAX_NVME_VFS; i++)
		g_sd_conf.pcie_nvme_addr_list[i] = NULL;

	if (!raw || !*raw)
		return;

	if (strchr(raw, ',')) {
		fprintf(stderr,
			"[config] pcie_nvme_addr must be whitespace-separated "
			"(got comma in '%s'). Use e.g. "
			"\"0000:d8:00.1 0000:d8:00.2\" to match the format "
			"consumed by SPDK setup scripts.\n",
			raw);
		return;
	}

	dup = strdup(raw);
	if (!dup) {
		fprintf(stderr, "[config] strdup fail while parsing "
				"pcie_nvme_addr\n");
		return;
	}

	tok = strtok_r(dup, " \t", &save);
	while (tok && n < MAX_NVME_VFS) {
		trimmed = strtrim(tok);
		if (*trimmed) {
			g_sd_conf.pcie_nvme_addr_list[n] = strdup(trimmed);
			if (!g_sd_conf.pcie_nvme_addr_list[n]) {
				fprintf(stderr,
					"[config] strdup fail for BDF '%s'\n",
					trimmed);
				break;
			}
			n++;
		}
		tok = strtok_r(NULL, " \t", &save);
	}

	if (tok && n >= MAX_NVME_VFS) {
		fprintf(stderr,
			"[config] pcie_nvme_addr has more than MAX_NVME_VFS=%d "
			"entries; truncating.\n",
			MAX_NVME_VFS);
	}

	g_sd_conf.pcie_nvme_nr_vfs = n;
	free(dup);
}

void load_secure_daemon_configs(void)
{
	LOAD_CONFIG_STR(pcie_nvme_addr);
	parse_pcie_nvme_addr_list();
	LOAD_CONFIG_STR(rpc_rdma_ip_addr);
	LOAD_CONFIG_INT(rpc_rdma_port);
	LOAD_CONFIG_INT(data_fetcher_rdma_port);
	LOAD_CONFIG_INT(data_fetcher_rdma_port_second);
	// LOAD_CONFIG_INT(spdk_max_io_requests_in_qpair);
	LOAD_CONFIG_INT(storage_engine_thread_num);
	LOAD_CONFIG_INT(read_worker_thread_num);
	LOAD_CONFIG_INT(rpc_rdma_thread_num);
	LOAD_CONFIG_INT(rpc_shmem_thread_num);
	LOAD_CONFIG_INT(snapshot_thread_num);
	LOAD_CONFIG_STR(filesystem);
	LOAD_CONFIG_INT(bg_journaling);
	LOAD_CONFIG_INT(journal_mode);
}

int validate_secure_daemon_configs(void)
{
	int total_rd_workers = g_sd_conf.storage_engine_thread_num +
			       g_sd_conf.read_worker_thread_num;

	if (g_sd_conf.read_worker_thread_num > OXBOW_RD_WORKER_NR_MAX) {
		fprintf(stderr,
			"[config] read_worker_thread_num(%d) must be <= "
			"OXBOW_RD_WORKER_NR_MAX(%d).\n",
			g_sd_conf.read_worker_thread_num,
			OXBOW_RD_WORKER_NR_MAX);
		return -1;
	}

	if (total_rd_workers > OXBOW_TOTAL_IO_THREAD_NR_MAX) {
		fprintf(stderr,
			"[config] storage_engine_thread_num(%d) + "
			"read_worker_thread_num(%d) must be <= "
			"OXBOW_TOTAL_IO_THREAD_NR_MAX(%d).\n",
			g_sd_conf.storage_engine_thread_num,
			g_sd_conf.read_worker_thread_num,
			OXBOW_TOTAL_IO_THREAD_NR_MAX);
		return -1;
	}

	return 0;
}

void print_secure_daemon_configs(void)
{
	printf("---- Secure Daemon Configurations ----\n");
	PRINT_CONFIG_STR(pcie_nvme_addr);
	printf("pcie_nvme_nr_vfs=%d\n", g_sd_conf.pcie_nvme_nr_vfs);
	for (int i = 0; i < g_sd_conf.pcie_nvme_nr_vfs; i++) {
		printf("pcie_nvme_addr_list[%d]=%s\n", i,
		       g_sd_conf.pcie_nvme_addr_list[i]);
	}
	PRINT_CONFIG_STR(rpc_rdma_ip_addr);
	PRINT_CONFIG_INT(rpc_rdma_port);
	PRINT_CONFIG_INT(data_fetcher_rdma_port);
	PRINT_CONFIG_INT(data_fetcher_rdma_port_second);
	// PRINT_CONFIG_INT(spdk_max_io_requests_in_qpair);
	PRINT_CONFIG_INT(storage_engine_thread_num);
	PRINT_CONFIG_INT(read_worker_thread_num);
	PRINT_CONFIG_INT(rpc_rdma_thread_num);
	PRINT_CONFIG_INT(rpc_shmem_thread_num);
	PRINT_CONFIG_INT(snapshot_thread_num);
	PRINT_CONFIG_STR(filesystem);
	PRINT_CONFIG_INT(bg_journaling);
	PRINT_CONFIG_INT(journal_mode);
	printf("--------------------------------------\n");
}
