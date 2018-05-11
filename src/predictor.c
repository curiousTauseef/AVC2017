#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include "sys.h"
#include "structs.h"
#include "pid.h"

#include "nn.h"

#define ROOT_MODEL_DIR "/var/model/"

int INPUT_FD = 0;

waypoint_t* WAYPOINTS;
waypoint_t* NEXT_WPT;

int FORWARD_STATE = 0;
int USE_DEADRECKONING = 1;

PID_t PID_THROTTLE = {
	.p = 2,
	.i = 0.25,
	.d = 2,
};

mat_t X;
nn_layer_t* L;

float TOTAL_DISTANCE;

void proc_opts(int argc, char* const *argv)
{
	const char* cmds = "hfr:sd:";
	const char* prog_desc = "Collects data from sensors, compiles them into system state packets. Then forwards them over stdout";
	const char* cmd_desc[] = {
		"Show this help",
		"Forward system state over stdout",
		"Path for route file to load",
		"Set one explicit waypoint that is very far away",
		"Enable or disable deadreckoning",
	};

	int c;
	while((c = getopt(argc, argv, cmds)) != -1)
	switch(c)
	{
		case 'h':
			cli_help(argv, prog_desc, cmds, cmd_desc);
		case 'f':
			FORWARD_STATE = 1;
			break;
		case 'r':
		{
			// Load the route
			int fd = open(optarg, O_RDONLY);
			if (fd < 0)
			{
				b_log("Loading route: '%s' failed", optarg);
				exit(-1);
			}

			off_t all_bytes = lseek(fd, 0, SEEK_END);
			size_t count = all_bytes / sizeof(waypoint_t);

			b_log("Loading route with %d waypoints", count);

			lseek(fd, 0, SEEK_SET);
			WAYPOINTS = (waypoint_t*)calloc(count + 1, sizeof(waypoint_t));
			size_t read_bytes = read(fd, WAYPOINTS, count * sizeof(waypoint_t));
			if (read_bytes != count * sizeof(waypoint_t))
			{
				b_log("route read of %d/%dB failed", read_bytes, all_bytes);
				exit(-1);
			}
			close(fd);

			// connect waypoint references
			for (int i = 0; i < count - 1; ++i)
			{
				WAYPOINTS[i].next = WAYPOINTS + i + 1;
			}
			WAYPOINTS[count - 1].next = NULL;

			NEXT_WPT = WAYPOINTS;
		}
			break;
		case 's':
		{
			b_log("Pointing to (0, 1E6, 0)");

			static waypoint_t up = {
				.position = { 0, 1E6, 0 },
				.heading  = { 0, 1, 0 },
				.velocity = 0.25,
			};

			WAYPOINTS = &up;
			NEXT_WPT = WAYPOINTS;
		}
			break;
		case 'd':
			USE_DEADRECKONING = optarg[0] == 'y' ? 1 : 0;
			break;

	}
}

#define BUCKET_SIZE 32
float avoider(raw_state_t* state, float* confidence)
{
	color_t rgb[FRAME_W * FRAME_H];
	const int CHROMA_W = FRAME_W / 2;
	const int HIST_W = FRAME_W / BUCKET_SIZE;
	float hist[FRAME_W / BUCKET_SIZE] = {};

	yuv422_to_rgb(state->view.luma, state->view.chroma, rgb, FRAME_W, FRAME_H);

	for (int ci = 0; ci < HIST_W; ++ci)
	{
		float col_sum = 0;
		int c = ci * BUCKET_SIZE;

		const int start = 70;
		const int height = 64;
		int r_stride = 4;

		for (int r = 0; r < height;)
		{
			for (int kr = 16; kr--;)
			for (int kc = 16; kc--;)
			{
				color_t color = rgb[((r + start + kr) * FRAME_W) + c + kc];
				X.data.f[(kr * 48) + kc * 3 + 0] = (color.r / 255.0f) - 0.5f;
				X.data.f[(kr * 48) + kc * 3 + 1] = (color.g / 255.0f) - 0.5f;
				X.data.f[(kr * 48) + kc * 3 + 2] = (color.b / 255.0f) - 0.5f;
			}

			mat_t y = *nn_predict(L, &X);
			col_sum += (-(y.data.f[0] + y.data.f[1]) + (y.data.f[2]));

			if (FORWARD_STATE)
			for (int kr = r_stride; kr--;)
			for (int kc = BUCKET_SIZE; kc--;)
			{
				float chroma_v  __attribute__ ((vector_size(8))) = {};
				float magenta_none __attribute__ ((vector_size(8))) = { 1, 1 };
				float orange_hay  __attribute__ ((vector_size(8))) = { -1, 1 };
				float green_asph  __attribute__ ((vector_size(8))) = { -1, -1 };

				chroma_v = y.data.f[0] * magenta_none + y.data.f[1] * orange_hay + y.data.f[2] * green_asph;
				chroma_v = (chroma_v + 1.f) / 2.f;
				// state->view.luma[(r + start + kr) * FRAME_W + (c + kc)] *= 0.5;
				state->view.chroma[(r + start + kr) * CHROMA_W + (c + kc) / 2].cr = chroma_v[0] * 255;
				state->view.chroma[(r + start + kr) * CHROMA_W + (c + kc) / 2].cb = chroma_v[1] * 255;

			}

			r += r_stride;
			r_stride *= 2;
		}

		hist[ci] = col_sum;// / samples;
	}

	float best = hist[HIST_W-1];
	int cont_r[2] = { HIST_W, HIST_W };

	// Here we pick the best, most contigious horizontal range of
	// the frame with the smallest sum of 'bad' colors, or the
	// largest sum of good colors if they are present.
	for (int j = HIST_W + 1; j--;)
	{
		float cost = 0;
		int cont_start = j;

		// From the current column, slide all the way to the left.
		for (int i = j; i--;)
		{
			// Accumulate cost for this region
			cost += hist[i];

			// The score for this region also factors in the width
			// so as the region grows without accumulating badness
			// it becomes more attractive. If it is found to have a better
			// score than the best, then set it as the new best.
			const float width_weight = 1;
			float total_cost = cost / (1 + (j - i) * width_weight);
			if (total_cost > best)
			{
				cont_r[0] = i;
				cont_r[1] = cont_start;
				best = total_cost;
			}
		}
	}


	int cont_width = (cont_r[1] - cont_r[0]);
	int target_idx;

	// Decide which place in the region we should steer toward
	if (cont_r[0] > 0 && cont_r[1] == FRAME_H)
	{
		target_idx = cont_r[0];// + cont_width * 0.75f;
	}
	else if (cont_r[0] == 0 && cont_r[1] < FRAME_H)
	{
		target_idx = cont_r[0];// + cont_width * 0.25f;
	}
	else
	{
		target_idx = cont_r[0] + (cont_width >> 1);
	}



	// float weight = biggest / (float)FRAME_H;
	float fp = (target_idx / (float)HIST_W);
	static float ap;

	{
		int ci = fp * FRAME_W;
		for (int i = FRAME_H; i--;)
		{
			state->view.luma[i * FRAME_W + ci] = 0;
		}
	}


	// *confidence = MIN(weight + 0.5, 1);
	ap =  0.8 * ap + 0.2 * (1 - fp);

	b_log("steer: %f", fp);

	return fp;
}

time_t LAST_SECOND;
raw_action_t predict(raw_state_t* state, waypoint_t* goal)
{

	quat q = { 0, 0, sin(M_PI / 4), cos(M_PI / 4) };
	raw_action_t act = { 117, 117 };
	vec3 goal_vec, dist_vec = {};
	vec3 left;
	float p = 0.5;

	// Visual obstacle avoidance
	float conf = 0;
	float avd_p = avoider(state, &conf);
	float inv_conf = 1 - conf;

	// Pre recorded path guidance
	if (USE_DEADRECKONING && goal)
	{
		if (vec3_len(state->heading) == 0)
		{
			return act;
		}

		// Compute the total delta vector between the car, and the goal
		// then normalize the delta to get the goal heading
		// rotate the heading vector about the z-axis to get the left vector
		vec3_sub(dist_vec, goal->position, state->position);
		vec3_norm(goal_vec, dist_vec);
		quat_mul_vec3(left, q, state->heading);

		// Determine if we are pointing toward the goal with the 'coincidence' value
		// Project the goal vector onto the left vector to determine steering direction
		// remap range to [0, 1]
		float coincidence = vec3_mul_inner(state->heading, goal_vec);
		p = (vec3_mul_inner(left, goal_vec) + 1) / 2;

		// If pointing away, steer all the way to the right or left, so
		// p will be either 1 or 0
		if (coincidence < 0)
		{
			p = roundf(p);
		}

		p = inv_conf * p + conf * avd_p;
	}
	else
	{
		p = avd_p;
	}

	// Lerp between right and left.
	act.steering = 255 * p; //CAL.steering.max * (1 - p) + CAL.steering.min * p;

	// Use a pid controller to regulate the throttle to match the speed driven
	int throttle_temp = 117 + PID_control(&PID_THROTTLE, inv_conf, state->vel);
	act.throttle = MAX(117, throttle_temp);

	//time_t now = time(NULL);
	//if (LAST_SECOND != now)
	{
		// b_log("throttle: %d", act.throttle);
	//	LAST_SECOND = now;
	}

	return act;
}


void sig_handler(int sig)
{
	b_log("Caught signal %d", sig);
	exit(0);
}


waypoint_t* best_waypoint(raw_state_t* state)
{
	waypoint_t* next = NEXT_WPT;
	waypoint_t* best = NEXT_WPT;
	float dist_sum = 0;

	while(next)
	{
		vec3 delta;  // difference between car pos and waypoint pos

		if (USE_DEADRECKONING)
		{
			vec3 dir;    // unit vector direction to waypoint from car pos
			float cost;
			float co;    // coincidence with heading
			float dist;
			float lowest_cost = 1E10;

			vec3_sub(delta, next->position, state->position);
			vec3_norm(dir, delta);
			co = vec3_mul_inner(dir, state->heading);
			dist = vec3_len(delta);

			cost = dist + (2 - (co + 1));

			if (dist > 0.5)
			{
				if (cost < lowest_cost)
				{
					best = next;
					lowest_cost = cost;
				}
			}
			else if (next->next == NULL)
			{
				return NULL;
			}

		}
		else
		{
			if (!next->next) return NULL;

			vec3_sub(delta, next->position, next->next->position);
			dist_sum += vec3_len(delta);

			if (dist_sum > state->distance)
			{
				return next;
			}
		}

		next = next->next; // lol
	}

	return best;
}


int main(int argc, char* const argv[])
{
	PROC_NAME = argv[0];

	signal(SIGINT, sig_handler);

	mat_t x = {
		.dims = { 1, 768 },
	};
	X = x;

	nn_layer_t l[] = {
		{
			.w = nn_mat_load(ROOT_MODEL_DIR "dense.kernel"),
			.b = nn_mat_load(ROOT_MODEL_DIR "dense.bias"),
			.activation = nn_act_relu
		},
		{
			.w = nn_mat_load(ROOT_MODEL_DIR "dense_1.kernel"),
			.b = nn_mat_load(ROOT_MODEL_DIR "dense_1.bias"),
			.activation = nn_act_softmax
		},
		{}
	};
	L = l;

	proc_opts(argc, argv);

	assert(nn_mat_init(&X) == 0);
	assert(nn_fc_init(L + 0, &X) == 0);
	assert(nn_fc_init(L + 1, L[0].A) == 0);

	b_log("Waiting...");

	while(1)
	{
		payload_t payload = {};

		if (!read_pipeline_payload(&payload, PAYLOAD_STATE))
		{
			raw_state_t* state = &payload.payload.state;
			raw_action_t act = predict(state, NEXT_WPT);

			payload.header.type = PAYLOAD_ACTION;
			payload.payload.pair.action = act;

			if (USE_DEADRECKONING)
			{
				b_log("Reckoning...");
				waypoint_t* next = best_waypoint(state);
				if (next != NEXT_WPT)
				{
					NEXT_WPT = next;
					b_log("next waypoint: %lx", (unsigned int)NEXT_WPT);
				}

				if (NEXT_WPT == NULL)
				{
					b_bad("No waypoints loaded");
					exit(-2);
				}
			}

			if (FORWARD_STATE)
			{
				payload.header.type = PAYLOAD_PAIR;
			}

			if (write_pipeline_payload(&payload))
			{
				b_bad("Failed to write payload");
				return -1;
			}
		}
		else
		{
			b_bad("read error");

			return -1;
		}
	}

	b_bad("terminating");

	return 0;
}
