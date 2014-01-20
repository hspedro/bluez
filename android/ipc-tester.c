/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <libgen.h>
#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"

#include "src/shared/tester.h"
#include "src/shared/mgmt.h"
#include "src/shared/hciemu.h"

#include "hal-msg.h"
#include <cutils/properties.h>

#define WAIT_FOR_SIGNAL_TIME 2 /* in seconds */
#define EMULATOR_SIGNAL "emulator_started"

struct test_data {
	struct mgmt *mgmt;
	uint16_t mgmt_index;
	struct hciemu *hciemu;
	enum hciemu_type hciemu_type;
	pid_t bluetoothd_pid;
	bool setup_done;
};

struct ipc_data {
	void *buffer;
	size_t len;
};

struct generic_data {
	struct ipc_data ipc_data;

	unsigned int num_services;
	int init_services[];
};

struct regmod_msg {
	struct hal_hdr header;
	struct hal_cmd_register_module cmd;
} __attribute__((packed));

#define CONNECT_TIMEOUT (5 * 1000)
#define SERVICE_NAME "bluetoothd"

static char exec_dir[PATH_MAX + 1];

static int cmd_sk = -1;
static int notif_sk = -1;

static void read_info_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct mgmt_rp_read_info *rp = param;
	char addr[18];
	uint16_t manufacturer;
	uint32_t supported_settings, current_settings;

	tester_print("Read Info callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	ba2str(&rp->bdaddr, addr);
	manufacturer = btohs(rp->manufacturer);
	supported_settings = btohl(rp->supported_settings);
	current_settings = btohl(rp->current_settings);

	tester_print("  Address: %s", addr);
	tester_print("  Version: 0x%02x", rp->version);
	tester_print("  Manufacturer: 0x%04x", manufacturer);
	tester_print("  Supported settings: 0x%08x", supported_settings);
	tester_print("  Current settings: 0x%08x", current_settings);
	tester_print("  Class: 0x%02x%02x%02x",
			rp->dev_class[2], rp->dev_class[1], rp->dev_class[0]);
	tester_print("  Name: %s", rp->name);
	tester_print("  Short name: %s", rp->short_name);

	if (strcmp(hciemu_get_address(data->hciemu), addr)) {
		tester_pre_setup_failed();
		return;
	}

	tester_pre_setup_complete();
}

static void index_added_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Added callback");
	tester_print("  Index: 0x%04x", index);

	data->mgmt_index = index;

	mgmt_send(data->mgmt, MGMT_OP_READ_INFO, data->mgmt_index, 0, NULL,
					read_info_callback, NULL, NULL);
}

static void index_removed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Removed callback");
	tester_print("  Index: 0x%04x", index);

	if (index != data->mgmt_index)
		return;

	mgmt_unregister_index(data->mgmt, data->mgmt_index);

	mgmt_unref(data->mgmt);
	data->mgmt = NULL;

	tester_post_teardown_complete();
}

static void read_index_list_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Read Index List callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	mgmt_register(data->mgmt, MGMT_EV_INDEX_ADDED, MGMT_INDEX_NONE,
					index_added_callback, NULL, NULL);

	mgmt_register(data->mgmt, MGMT_EV_INDEX_REMOVED, MGMT_INDEX_NONE,
					index_removed_callback, NULL, NULL);

	data->hciemu = hciemu_new(data->hciemu_type);
	if (!data->hciemu) {
		tester_warn("Failed to setup HCI emulation");
		tester_pre_setup_failed();
		return;
	}

	tester_print("New hciemu instance created");
}

static void test_pre_setup(const void *data)
{
	struct test_data *test_data = tester_get_data();

	if (!tester_use_debug())
		fclose(stderr);

	test_data->mgmt = mgmt_new_default();
	if (!test_data->mgmt) {
		tester_warn("Failed to setup management interface");
		tester_pre_setup_failed();
		return;
	}

	mgmt_send(test_data->mgmt, MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, 0,
				NULL, read_index_list_callback, NULL, NULL);
}

static void test_post_teardown(const void *data)
{
	struct test_data *test_data = tester_get_data();

	if (test_data->hciemu) {
		hciemu_unref(test_data->hciemu);
		test_data->hciemu = NULL;
	}
}

static void bluetoothd_start(int hci_index)
{
	char prg_name[PATH_MAX + 1];
	char index[8];
	char *prg_argv[4];

	snprintf(prg_name, sizeof(prg_name), "%s/%s", exec_dir, "bluetoothd");
	snprintf(index, sizeof(index), "%d", hci_index);

	prg_argv[0] = prg_name;
	prg_argv[1] = "-i";
	prg_argv[2] = index;
	prg_argv[3] = NULL;

	if (!tester_use_debug())
		fclose(stderr);

	execve(prg_argv[0], prg_argv, NULL);
}

static void emulator(int pipe, int hci_index)
{
	static const char SYSTEM_SOCKET_PATH[] = "\0android_system";
	char buf[1024];
	struct sockaddr_un addr;
	struct timeval tv;
	int fd;
	ssize_t len;

	fd = socket(PF_LOCAL, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		goto failed;

	tv.tv_sec = WAIT_FOR_SIGNAL_TIME;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, SYSTEM_SOCKET_PATH, sizeof(SYSTEM_SOCKET_PATH));

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Failed to bind system socket");
		goto failed;
	}

	len = write(pipe, EMULATOR_SIGNAL, sizeof(EMULATOR_SIGNAL));

	if (len != sizeof(EMULATOR_SIGNAL))
		goto failed;

	memset(buf, 0, sizeof(buf));

	len = read(fd, buf, sizeof(buf));
	if (len <= 0 || (strcmp(buf, "ctl.start=bluetoothd")))
		goto failed;

	close(pipe);
	close(fd);
	bluetoothd_start(hci_index);

failed:
	close(pipe);
	close(fd);
}

static int accept_connection(int sk)
{
	int err;
	struct pollfd pfd;
	int new_sk;

	memset(&pfd, 0 , sizeof(pfd));
	pfd.fd = sk;
	pfd.events = POLLIN;

	err = poll(&pfd, 1, CONNECT_TIMEOUT);
	if (err < 0) {
		err = errno;
		tester_warn("Failed to poll: %d (%s)", err, strerror(err));
		return -errno;
	}

	if (err == 0) {
		tester_warn("bluetoothd connect timeout");
		return -errno;
	}

	new_sk = accept(sk, NULL, NULL);
	if (new_sk < 0) {
		err = errno;
		tester_warn("Failed to accept socket: %d (%s)",
							err, strerror(err));
		return -errno;
	}

	return new_sk;
}

static bool init_ipc(void)
{
	struct sockaddr_un addr;

	int sk;
	int err;

	sk = socket(AF_LOCAL, SOCK_SEQPACKET, 0);
	if (sk < 0) {
		err = errno;
		tester_warn("Failed to create socket: %d (%s)", err,
							strerror(err));
		return false;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	memcpy(addr.sun_path, BLUEZ_HAL_SK_PATH, sizeof(BLUEZ_HAL_SK_PATH));

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = errno;
		tester_warn("Failed to bind socket: %d (%s)", err,
								strerror(err));
		close(sk);
		return false;
	}

	if (listen(sk, 2) < 0) {
		err = errno;
		tester_warn("Failed to listen on socket: %d (%s)", err,
								strerror(err));
		close(sk);
		return false;
	}

	/* Start Android Bluetooth daemon service */
	if (property_set("ctl.start", SERVICE_NAME) < 0) {
		tester_warn("Failed to start service %s", SERVICE_NAME);
		close(sk);
		return false;
	}

	cmd_sk = accept_connection(sk);
	if (cmd_sk < 0) {
		close(sk);
		return false;
	}

	notif_sk = accept_connection(sk);
	if (notif_sk < 0) {
		close(sk);
		close(cmd_sk);
		cmd_sk = -1;
		return false;
	}

	tester_print("bluetoothd connected");

	close(sk);

	return true;
}

static void cleanup_ipc(void)
{
	if (cmd_sk < 0)
		return;

	close(cmd_sk);
	cmd_sk = -1;
}

static gboolean check_for_daemon(gpointer user_data)
{
	int status;
	struct test_data *data = user_data;

	if ((waitpid(data->bluetoothd_pid, &status, WNOHANG))
							!= data->bluetoothd_pid)
		return true;

	if (data->setup_done) {
		if (WIFEXITED(status) &&
				(WEXITSTATUS(status) == EXIT_SUCCESS)) {
			tester_test_passed();
			return false;
		}
		tester_test_failed();
	} else {
		tester_setup_failed();
		test_post_teardown(data);
	}

	tester_warn("Unexpected Daemon shutdown with status %d", status);
	return false;
}

static void setup(const void *data)
{
	struct test_data *test_data = tester_get_data();
	int signal_fd[2];
	char buf[1024];
	pid_t pid;
	int len;

	if (pipe(signal_fd))
		goto failed;

	pid = fork();

	if (pid < 0) {
		close(signal_fd[0]);
		close(signal_fd[1]);
		goto failed;
	}

	if (pid == 0) {
		if (!tester_use_debug())
			fclose(stderr);

		close(signal_fd[0]);
		emulator(signal_fd[1], test_data->mgmt_index);
		exit(0);
	}

	close(signal_fd[1]);
	test_data->bluetoothd_pid = pid;

	len = read(signal_fd[0], buf, sizeof(buf));
	if (len <= 0 || (strcmp(buf, EMULATOR_SIGNAL))) {
		close(signal_fd[0]);
		goto failed;
	}

	g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, check_for_daemon, test_data,
									NULL);

	if (!init_ipc()) {
		tester_warn("Cannot initialize IPC mechanism!");
		goto failed;
	}
	/* TODO: register modules */

	test_data->setup_done = true;
	return;

failed:
	g_idle_remove_by_data(test_data);
	tester_setup_failed();
	test_post_teardown(data);
}

static void teardown(const void *data)
{
	struct test_data *test_data = tester_get_data();

	g_idle_remove_by_data(test_data);
	cleanup_ipc();

	if (test_data->bluetoothd_pid)
		waitpid(test_data->bluetoothd_pid, NULL, 0);

	tester_teardown_complete();
}

static void ipc_send_tc(const void *data)
{
	const struct generic_data *generic_data = data;
	const struct ipc_data *ipc_data = &generic_data->ipc_data;

	if (ipc_data->len) {
		if (write(cmd_sk, ipc_data->buffer, ipc_data->len) < 0)
			tester_test_failed();
	}
}

#define service_data(args...) { args }

#define gen_data(writelen, writebuf, servicelist...) \
	{								\
		.ipc_data = {						\
			.buffer = writebuf,				\
			.len = writelen,				\
		},							\
		.init_services = service_data(servicelist),		\
		.num_services = sizeof((const int[])			\
					service_data(servicelist)) /	\
					sizeof(int),			\
	}

#define test_generic(name, test, setup, teardown, buffer, writelen, \
							services...) \
	do {								\
		struct test_data *user;					\
		static const struct generic_data data =			\
				gen_data(writelen, buffer, services);	\
		user = g_malloc0(sizeof(struct test_data));		\
		if (!user)						\
			break;						\
		user->hciemu_type = HCIEMU_TYPE_BREDRLE;		\
		tester_add_full(name, &data, test_pre_setup, setup,	\
				test, teardown, test_post_teardown,	\
				3, user, g_free);			\
	} while (0)

struct regmod_msg register_bt_msg = {
	.header = {
		.service_id = HAL_SERVICE_ID_CORE,
		.opcode = HAL_OP_REGISTER_MODULE,
		.len = sizeof(struct hal_cmd_register_module),
		},
	.cmd = {
		.service_id = HAL_SERVICE_ID_BLUETOOTH,
		},
};

int main(int argc, char *argv[])
{
	snprintf(exec_dir, sizeof(exec_dir), "%s", dirname(argv[0]));

	tester_init(&argc, &argv);

	test_generic("Too small data",
				ipc_send_tc, setup, teardown,
				&register_bt_msg, 1);

	return tester_run();
}
