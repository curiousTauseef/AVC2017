#define _GNU_SOURCE

#include "sys.h"
#include "structs.h"
#include "dataset_hdr.h"
#include "i2c.h"
#include "drv_pwm.h"
#include "cam.h"
#include "linmath.h"
#include "deadreckon.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))


typedef enum {
	COL_MODE_NORMAL = 0,
	COL_MODE_ACT_CAL,
	COL_MODE_ROUTE,
} col_mode_t;

char* MEDIA_PATH;
int I2C_BUS;
int NORM_VIDEO;
col_mode_t MODE;
int WAIT_FOR_MOVEMENT = 1;
int READ_ACTION = 1;
calib_t CAL;
float VEL;


void proc_opts(int argc, const char ** argv)
{
	for(;;)
	{
		int c = getopt(argc, (char *const *)argv, "aircnm:");
		if(c == -1) break;

		switch (c) 
		{
			case 'c': // Calibrate
				MODE = COL_MODE_ACT_CAL;
				b_log("Calibrating action vector");
				break;
			case 'n': // Normalize video
				NORM_VIDEO = 1;
				break;
			case 'm': // Set recording media
				b_log("Using media: '%s'", optarg);
				MEDIA_PATH = optarg;
				break;
			case 'r': // Record route
				b_log("Recording route");
				MODE = COL_MODE_ROUTE;
				break;
			case 'i': // Start immediately
				WAIT_FOR_MOVEMENT = 0;
				break;
			case 'a':
				READ_ACTION = 0;
				break;
		}
	}
}


int poll_vision(raw_state_t* state, cam_t* cams)
{
	cam_wait_frame(cams);

	range_t luma_range = { 128, 128 };
	range_t cr_range = { 128, 128 };
	range_t cb_range = { 128, 128 };
	float luma_mu = 0;
	float cr_mu = 0;
	float cb_mu = 0;


	// Downsample the intensity resolution to match that of
	// the chroma
	for(int j = FRAME_H; j--;)
	{
		int bi = cams[0].buffer_info.index;
		uint32_t* row = cams[0].frame_buffers[bi] + (j * (FRAME_W << 1));
		uint8_t* luma_row = state->view.luma + (j * FRAME_W);
		chroma_t* chroma_row = state->view.chroma + (j * (FRAME_W >> 1));
		
		for(int i = FRAME_W / 2; i--;)
		{
			int li = i << 1;

			luma_row[li + 0] = row[i] & 0xFF;
			luma_row[li + 1] = (row[i] >> 16) & 0xFF;

			chroma_row[i].cr = (row[i] >> 8) & 0xFF;
			chroma_row[i].cb = (row[i] >> 24) & 0xFF;

			if(NORM_VIDEO)
			{
				for(int k = 2; k--;)
				{
					luma_range.min = MIN(luma_range.min, luma_row[li + i + k]);
					luma_range.max = MAX(luma_range.max, luma_row[li + i + k]);
					luma_mu += luma_row[li + i + k];
				}		


				cr_range.min = MIN(cr_range.min, chroma_row[i].cr);
				cr_range.max = MAX(cr_range.max, chroma_row[i].cr);
				cb_range.min = MIN(cb_range.min, chroma_row[i].cb);
				cb_range.max = MAX(cb_range.max, chroma_row[i].cb);

				cr_mu += chroma_row[i].cr;
				cb_mu += chroma_row[i].cb;
			}
		}
	}

	if(NORM_VIDEO)
	{
		luma_mu /= (FRAME_W * FRAME_H);
		cr_mu /= (FRAME_W / 2) * FRAME_H;
		cb_mu /= (FRAME_W / 2) * FRAME_H;
		float luma_spread = luma_range.max - luma_range.min;
		float cr_spread = cr_range.max - cr_range.min;
		float cb_spread = cb_range.max - cb_range.min;

		int bi = cams[0].buffer_info.index;

		for(int j = FRAME_H; j--;)
		{
			uint32_t* row = cams[0].frame_buffers[bi] + (j * FRAME_W * 2);
			uint8_t* luma_row = state->view.luma + (j * FRAME_W);
			chroma_t* chroma_row = state->view.chroma + (j * (FRAME_W >> 1));
			
			for(int i = FRAME_W / 2; i--;)
			{
				int li = i << 1;

				luma_row[li + 0] = 255 * (luma_row[li + 0] - luma_range.min) / luma_spread;
				luma_row[li + 1] = 255 * (luma_row[li + 1] - luma_range.min) / luma_spread;
			}
		}
	}


	return 0;
}


int calibration(cam_settings_t cfg)
{
	raw_action_t action = {};
		
	poll_i2c_devs(NULL, &action, NULL);

	CAL.throttle.min = CAL.throttle.max = action.throttle;
	CAL.steering.min = CAL.steering.max = action.steering;

	for(;;)
	{
		poll_i2c_devs(NULL, &action, NULL);
		calib_t last_cal = CAL;

		if(action.steering > CAL.steering.max) CAL.steering.max = action.steering;
		if(action.throttle > CAL.throttle.max) CAL.throttle.max = action.throttle;

		if(action.steering < CAL.steering.min) CAL.steering.min = action.steering;
		if(action.throttle < CAL.throttle.min) CAL.throttle.min = action.throttle;

		if(memcmp(&last_cal, &CAL, sizeof(CAL)))
		{
			b_log("throttle: [%f - %f]\nsteering: [%f - %f]",
					CAL.throttle.min, CAL.throttle.max,
					CAL.steering.min, CAL.steering.max
			       );

			// save the calibration profile
			int fd = open(ACTION_CAL_PATH, O_CREAT | O_WRONLY, 0666);
			write(fd, &CAL, sizeof(CAL));
			close(fd);
		}

		usleep(500000);
	}
}


int set_recording_media(int* fd, const char* ext)
{
	if(MEDIA_PATH)
	{
		char path_buf[PATH_MAX];

		snprintf(path_buf, sizeof(path_buf), "%s/%ld.%s", MEDIA_PATH, time(NULL), ext);

		*fd = open(path_buf, O_CREAT | O_WRONLY, 0666);
		b_log("Writing data to '%s'", path_buf);

		if(*fd < 0)
		{
			b_log("Error: couldn't open/create %s", path_buf);
			exit(-1);
		}
	}

	return 0;
}


int start_pose_thread(raw_example_t* ex)
{
	pthread_t pose_thread;
	return pthread_create(&pose_thread, NULL, pose_estimator, (void*)ex);
}


pthread_mutex_t STATE_LOCK;

int route()
{
	int res = 0, fd = 1;
	time_t now;
	int started = 0;
	dataset_header_t hdr = { MAGIC, 2 };

	pthread_mutex_init(&STATE_LOCK, NULL);

	set_recording_media(&fd, "route");

	// write the header first
	write(fd, &hdr, sizeof(hdr));

	now = time(NULL);
	raw_example_t ex = { };
	start_pose_thread(&ex);

	// wait for the bot to start moving
	if(WAIT_FOR_MOVEMENT)
	while(ex.state.vel <= 0)
	{
		usleep(100000);
	}

	
	vec3 last_pos;
	vec3_copy(last_pos, ex.state.position);

	for(;;)
	{
		vec3 diff = {};
		vec3_sub(diff, ex.state.position, last_pos);
		if(vec3_len(diff) >= 1)
		{
			waypoint_t wp = {
				.velocity = ex.state.vel
			};

			vec3_copy(wp.position, ex.state.position);
			vec3_copy(wp.heading, ex.state.heading);

			pthread_mutex_lock(&STATE_LOCK);
			if(write(fd, &wp, sizeof(wp)) != sizeof(wp))
			{
				b_log("Error writing waypoint sample");
				return -3;
			}
			pthread_mutex_unlock(&STATE_LOCK);

			vec3_copy(last_pos, ex.state.position);
		}

	
		if(ex.state.vel == 0 && WAIT_FOR_MOVEMENT)
		{
			exit(0);
		}	
	}
}


int collection(cam_t* cam)
{
	int res = 0, fd = 1;
	time_t now;
	int started = 0, updates = 0;
	dataset_header_t hdr = { MAGIC, 1 };

	pthread_mutex_init(&STATE_LOCK, NULL);

	set_recording_media(&fd, "train");

	if(READ_ACTION)
	{
		pwm_set_echo(0x6);
	}

	// write the header first
	write(fd, &hdr, sizeof(hdr));

	now = time(NULL);
	raw_example_t ex = { };
	start_pose_thread(&ex);

	// wait for the bot to start moving
	if(WAIT_FOR_MOVEMENT)
	while(ex.state.vel <= 0)
	{
		usleep(100000);
	}

	for(;;)
	{
		cam_request_frame(cam);

		// Do something while we wait for our
		// next frame to come in...
		++updates;
		if(now != time(NULL))
		{
			b_log("%dHz (%f %f %f) %fm/s",
				updates,
				ex.state.position[0],
				ex.state.position[1],
				ex.state.position[2],
				ex.state.vel
			);
			updates = 0;
			now = time(NULL);
		}

		if(poll_vision(&ex.state, cam))
		{
			b_log("Error capturing frame");
			return -2;
		}

		pthread_mutex_lock(&STATE_LOCK);
		if(write(fd, &ex, sizeof(ex)) != sizeof(ex))
		{
			b_log("Error writing state-action pair");
			return -3;
		}
		pthread_mutex_unlock(&STATE_LOCK);

	
		if(ex.state.vel == 0 && WAIT_FOR_MOVEMENT)
		{
			exit(0);
		}	
	}
}


int main(int argc, const char* argv[])
{
	int res;
	cam_settings_t cfg = {
		.width  = 160,
		.height = 120
	};

	PROC_NAME = argv[0];

	proc_opts(argc, argv);

	b_log("Sensors...");

	cam_t cam[2] = {
		cam_open("/dev/video0", &cfg),
		//cam_open("/dev/video1", &cfg),
	};

	ioctl(cam[0].fd, VIDIOC_S_PRIORITY, V4L2_PRIORITY_RECORD);

	if((res = i2c_init("/dev/i2c-1")))
	{
		b_log("I2C init failed (%d)", res);
		//return -1;
	}
	
	pwm_reset();
	b_log("OK");

	// Use the round-robin real-time scheduler
	// with a high priority
	struct sched_param sch_par = {
		.sched_priority = 50,
	};
	assert(sched_setscheduler(0, SCHED_RR, &sch_par) == 0);

	// Check to see if that action calibration file
	// exists. If not, switch to calibration mode. Otherwise
	// load the calibration values
	struct stat st;
	if(stat(ACTION_CAL_PATH, &st))
	{
		MODE = COL_MODE_ACT_CAL;
		b_log("No %s file found. Calibrating...", ACTION_CAL_PATH);
	}
	else
	{
		assert(calib_load(ACTION_CAL_PATH, &CAL) == 0);
	}
	
	switch(MODE)
	{
		case COL_MODE_ACT_CAL:
			calibration(cfg);
			break;
		case COL_MODE_ROUTE:
			route();
		default:
			res = collection(cam);
	}


	return res;
}
