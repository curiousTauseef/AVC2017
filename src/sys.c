#include "sys.h"
#include <stdio.h>


void timegate_open(timegate_t* tg)
{
	gettimeofday(&tg->start, NULL);
}


long diff_us(struct timeval then, struct timeval now)
{
	long us = (now.tv_sec - then.tv_sec) * 1e6;

	if(us)
	{
		us = (us - then.tv_usec) + now.tv_usec;
	}
	else
	{
		us = now.tv_usec - then.tv_usec;
	}

	return us;
}


void timegate_close(timegate_t* tg)
{
	struct timeval now = {};
	gettimeofday(&now, NULL);

	long residual = diff_us(tg->start, now);
	residual = tg->interval_us - residual;

	if(residual < 0) return;
	usleep(residual);
}


int calib_load(const char* path, calib_t* cal)
{
	int cal_fd = open(ACTION_CAL_PATH, O_RDONLY);

	if(cal_fd < 0)
	{
		return -1;
	}

	if(read(cal_fd, cal, sizeof(calib_t)) != sizeof(calib_t))
	{
		return -2;
	}

	close(cal_fd);
	return 0;
}

const char* PROC_NAME;
void b_log(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, (size_t)sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[%s] %s (%d)\n", PROC_NAME, buf, errno);
}


void b_good(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, (size_t)sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, AVC_TERM_GREEN "[%s]" AVC_TERM_COLOR_OFF " %s (%d)\n", PROC_NAME, buf, errno);
}


void b_bad(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, (size_t)sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, AVC_TERM_RED "[%s]" AVC_TERM_COLOR_OFF " %s (%d)\n", PROC_NAME, buf, errno);
}


void yuv422_to_rgb(uint8_t* luma, chroma_t* uv, color_t* rgb, int w, int h)
{
	for(int yi = h; yi--;)
	for(int xi = w; xi--;)
	{
		int i = yi * w + xi;
		int j = yi * (w >> 1) + (xi >> 1);

		rgb[i].r = clamp(luma[i] + 1.14 * (uv[j].cb - 128));
		rgb[i].g = clamp(luma[i] - 0.395 * (uv[j].cr - 128) - (0.581 * (uv[j].cb - 128)));
		rgb[i].b = clamp(luma[i] + 2.033 * (uv[j].cr - 128));
	}
}


float clamp(float v)
{
	v = v > 255 ? 255 : v;
	return  v < 0 ? 0 : v;
}


void cli_help(char* const argv[], const char* prog_desc, const char* cmds, const char* cmd_desc[])
{
	int cmd_idx = 0;
	printf("%sn", argv[0]);
	printf("%sn", (prog_desc));
	for (int i = 0; i < strlen((cmds)); i++)
	{
		if ((cmds)[i] == ':') continue;
		printf("-%c\t%s\n", (cmds)[i], (cmd_desc)[cmd_idx]);
		cmd_idx++;
	}
	exit(0);
}
