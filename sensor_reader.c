// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_reader.c — 讀 /dev/sensor_sample 的 user space 程式
 *
 * 對應 kernel 端 struct sample_data（三個欄位、共 16 bytes）。
 * read() 每次回傳一筆；沒資料時會阻塞睡等（kernel 的 wait_event）。
 *
 * 編譯（在板子上或用 cross toolchain）：
 *   arm-ca9-linux-gnueabihf-gcc -O2 -Wall sensor_reader.c -o sensor_reader
 * 或板子上有 gcc 就：
 *   gcc -O2 -Wall sensor_reader.c -o sensor_reader
 *
 * 用法：
 *   ./sensor_reader            # 一直讀，直到 Ctrl-C
 *   ./sensor_reader 10         # 只讀 10 筆就結束
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#define DEV_PATH "/dev/sensor_sample"

/* ★ 必須跟 kernel 端 struct sample_data 完全一致（欄位順序/型別/大小）*/
struct sample_data {
	uint32_t seq;            /* 序號 */
	uint32_t value;          /* 模擬讀數 */
	uint64_t timestamp_ns;   /* 採樣時間戳（單調時間，奈秒）*/
};

static volatile int running = 1;

static void on_sigint(int sig)
{
	(void)sig;
	running = 0;   /* Ctrl-C → 停止迴圈 */
}

int main(int argc, char *argv[])
{
	int fd;
	long want = -1;              /* -1 = 無限讀；否則讀 want 筆 */
	long got = 0;
	struct sample_data s;
	uint64_t prev_ts = 0;

	if (argc >= 2)
		want = strtol(argv[1], NULL, 10);
	
	struct sigaction sa = {0};
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) < 0) {
		fprintf(stderr, "sigaction 失敗: %s\n", strerror(errno));
		return 1;
	}

	// signal(SIGINT, on_sigint);   /* 讓 Ctrl-C 能優雅結束 */

	fd = open(DEV_PATH, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s 失敗: %s\n", DEV_PATH, strerror(errno));
		fprintf(stderr, "（確認 insmod 了 sensor_sample_todo.ko、/dev/%s 存在）\n",
			"sensor_sample");
		return 1;
	}

	printf("讀取 %s（Ctrl-C 結束）...\n", DEV_PATH);
	printf("%-8s %-8s %-16s %-12s\n", "seq", "value", "timestamp_us", "間隔_us");

	while (running && (want < 0 || got < want)) {
		ssize_t r = read(fd, &s, sizeof(s));

		if (r < 0) {
			if (errno == EINTR)      /* 被 Ctrl-C 打斷的 read */
				break;
			if (errno == ENODEV) {   /* kernel rmmod 了（dying）*/
				fprintf(stderr, "\n設備已卸載 (ENODEV)，結束\n");
				break;
			}
			fprintf(stderr, "read 失敗: %s\n", strerror(errno));
			break;
		}
		//if (r == 0)                  /* 競爭：被別的 reader 搶走，重試 */
			//continue;
		if (r != sizeof(s)) {        /* 應該剛好一筆；不是就是對不上 */
			fprintf(stderr, "read 回傳 %zd bytes（預期 %zu）— struct 對不上？\n",
				r, sizeof(s));
			break;
		}

		/* timestamp 是奈秒 → 轉微秒顯示；算跟上一筆的間隔（驗證採樣週期）*/
		uint64_t ts_us = s.timestamp_ns / 1000;
		uint64_t delta_us = prev_ts ? (s.timestamp_ns - prev_ts) / 1000 : 0;
		prev_ts = s.timestamp_ns;

		printf("%-8u 0x%-6x %-16llu %-12llu\n",
		       s.seq, s.value,
		       (unsigned long long)ts_us,
		       (unsigned long long)delta_us);

		got++;
	}

	printf("共讀了 %ld 筆\n", got);
	close(fd);
	return 0;
}
