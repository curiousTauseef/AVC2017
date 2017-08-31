#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>

#include "structs.h"
#include "curves.h"

static calib_t CALIBRATION;
static raw_example_t* RAW_DS;
static int DS_SIZE;
static char* DS_PATH;

void dataset_mean(example_t* dataset, float* mu_vec, unsigned int dims, unsigned int examples)
{
	bzero(mu_vec, sizeof(float) * dims);

	for(int i = examples; i--;)
	{
		float* ex = dataset[i].state.v;

		for(int j = dims; j--;)
		{
			mu_vec[j] += ex[j];
		}
	}

	for(int j = dims; j--;)
	{
		mu_vec[j] /= (float)examples;
	}
}


void dataset_range(example_t* dataset, range_t* ranges, unsigned int dims, unsigned int examples)
{
	float* ex = dataset[0].state.v;

	for(int i = examples; i--;)
	for(int j = dims; j--;)
	{
		ranges[j].min = dataset[i].state.v[j];
		ranges[j].max = dataset[i].state.v[j];
	}

	for(int i = examples; i--;)
	{
		float* ex = dataset[i].state.v;

		for(int j = dims; j--;)
		{
			float f = ex[j];
			if(f > ranges[j].max) ranges[j].max = f;
			if(f < ranges[j].min) ranges[j].min = f;
		}
	}
}


void dataset_raw_to_float(raw_example_t* raw_ex, example_t* ex, calib_t* cal, unsigned int examples)
{
	for(int i = examples; i--;)
	{
		raw_example_t* rs = raw_ex + i;
		example_t*     s  = ex + i;


		// Convert the state vectors
		// ------------------------
		// Convert IMU vectors
		for(int j = 3; j--;)
		{
			s->state.rot[j] = rs->state.rot_rate[j];
			s->state.acc[j] = rs->state.acc[j];
		}

		// Convert ODO values
		s->state.vel = rs->state.vel;
		s->state.distance = rs->state.distance;

		// Convert camera data
		for(int cell = LUMA_PIXELS; cell--;)
		{
			s->state.view.luma[cell] = rs->state.view.luma[cell];
		}

		for(int cell = CHRO_PIXELS; cell--;)
		{
			s->state.view.chroma[cell].cr = rs->state.view.chroma[cell].cr;
			s->state.view.chroma[cell].cb = rs->state.view.chroma[cell].cb;
		}

		// Convert the action vectors
		// -------------------------
		convert_action(&rs->action, &s->action, cal);
	}
}


void load_dataset(const char* path)
{
	int fd = open(path, O_RDONLY);

	if(fd < 0)
	{
		EXIT("Failed to open '%s'", path);
	}

	off_t size = lseek(fd, 0, SEEK_END);
	DS_SIZE = size / sizeof(raw_example_t);

	fprintf(stderr, "Reading dataset of %u examples...", DS_SIZE);

	lseek(fd, 0, SEEK_SET);
	RAW_DS = calloc(DS_SIZE, sizeof(raw_example_t));

	if(!RAW_DS)
	{
		EXIT("Failed RAW_DS allocation");
	}

	// Read whole file
	for(int i = 0; i < DS_SIZE; ++i)
	{
		if(read(fd, RAW_DS + i, sizeof(raw_example_t)) != sizeof(raw_example_t))
		{
			EXIT("Read failed");
		}
	}

	fprintf(stderr, "done!\n");

	// clean up
	close(fd);
}


void proc_opts(int argc, const char ** argv)
{
	for(;;)
	{
		int c = getopt(argc, (char *const *)argv, "i:t:s:");
		if(c == -1) break;

		int min, max;

		switch (c) {
			case 't':
				sscanf(optarg, "%d,%d", &min, &max);
				CALIBRATION.throttle.min = min;
				CALIBRATION.throttle.max = max;
				break;
			case 's':
				sscanf(optarg, "%d,%d", &min, &max);
				CALIBRATION.steering.min = min;
				CALIBRATION.steering.max = max;
				break;
			case 'i':
				load_dataset(optarg);
				DS_PATH = optarg;
				break;
		}
	}
}


int main(int argc, const char* argv[])
{
	proc_opts(argc, argv);

	example_t examples[DS_SIZE];
	range_t ranges[sizeof(state_vector_t) / 4] = {};

	dataset_raw_to_float(RAW_DS, examples, &CALIBRATION, DS_SIZE);
	dataset_range(examples, ranges, dims, DS_SIZE);

	for(int i = dims; i--;)
	{
		printf("%d [%f - %f]\n", i, ranges[i].min, ranges[i].max);
	}

	return 0;
}
