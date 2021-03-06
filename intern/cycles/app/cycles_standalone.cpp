/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

#include <stdio.h>

#include "buffers.h"
#include "camera.h"
#include "device.h"
#include "scene.h"
#include "session.h"

#include "util_args.h"
#include "util_foreach.h"
#include "util_function.h"
#include "util_path.h"
#include "util_progress.h"
#include "util_string.h"
#include "util_time.h"

#ifdef WITH_CYCLES_STANDALONE_GUI
#include "util_view.h"
#endif

#include "cycles_xml.h"

CCL_NAMESPACE_BEGIN

struct Options {
	Session *session;
	Scene *scene;
	string filepath;
	int width, height;
	SceneParams scene_params;
	SessionParams session_params;
	bool quiet, show_help;
} options;

static void session_print(const string& str)
{
	/* print with carriage return to overwrite previous */
	printf("\r%s", str.c_str());

	/* add spaces to overwrite longer previous print */
	static int maxlen = 0;
	int len = str.size();
	maxlen = max(len, maxlen);

	for(int i = len; i < maxlen; i++)
		printf(" ");

	/* flush because we don't write an end of line */
	fflush(stdout);
}

static void session_print_status()
{
	int sample, tile;
	double total_time, sample_time;
	string status, substatus;

	/* get status */
	sample = options.session->progress.get_sample();
	options.session->progress.get_tile(tile, total_time, sample_time);
	options.session->progress.get_status(status, substatus);

	if(substatus != "")
		status += ": " + substatus;

	/* print status */
	status = string_printf("Sample %d   %s", sample, status.c_str());
	session_print(status);
}

static BufferParams& session_buffer_params()
{
	static BufferParams buffer_params;
	buffer_params.width = options.width;
	buffer_params.height = options.height;
	buffer_params.full_width = options.width;
	buffer_params.full_height = options.height;

	return buffer_params;
}

static void session_init()
{
	options.session = new Session(options.session_params);
	options.session->reset(session_buffer_params(), options.session_params.samples);
	options.session->scene = options.scene;
	
	if(options.session_params.background && !options.quiet)
		options.session->progress.set_update_callback(function_bind(&session_print_status));
#ifdef WITH_CYCLES_STANDALONE_GUI
	else
		options.session->progress.set_update_callback(function_bind(&view_redraw));
#endif

	options.session->start();

	options.scene = NULL;
}

static void scene_init(int width, int height)
{
	options.scene = new Scene(options.scene_params, options.session_params.device);
	xml_read_file(options.scene, options.filepath.c_str());
	
	if (width == 0 || height == 0) {
		options.width = options.scene->camera->width;
		options.height = options.scene->camera->height;
	}
}

static void session_exit()
{
	if(options.session) {
		delete options.session;
		options.session = NULL;
	}
	if(options.scene) {
		delete options.scene;
		options.scene = NULL;
	}

	if(options.session_params.background && !options.quiet) {
		session_print("Finished Rendering.");
		printf("\n");
	}
}

#ifdef WITH_CYCLES_STANDALONE_GUI
static void display_info(Progress& progress)
{
	static double latency = 0.0;
	static double last = 0;
	double elapsed = time_dt();
	string str;

	latency = (elapsed - last);
	last = elapsed;

	int sample, tile;
	double total_time, sample_time;
	string status, substatus;

	sample = progress.get_sample();
	progress.get_tile(tile, total_time, sample_time);
	progress.get_status(status, substatus);

	if(substatus != "")
		status += ": " + substatus;

	str = string_printf("%s        Time: %.2f        Latency: %.4f        Sample: %d        Average: %.4f",
						status.c_str(), total_time, latency, sample, sample_time);

	view_display_info(str.c_str());
	
	if(options.show_help)
		view_display_help();
}

static void display()
{
	options.session->draw(session_buffer_params());

	display_info(options.session->progress);
}

static void resize(int width, int height)
{
	options.width = width;
	options.height = height;

	if(options.session)
		options.session->reset(session_buffer_params(), options.session_params.samples);
}

static void keyboard(unsigned char key)
{
	if(key == 'r')
		options.session->reset(session_buffer_params(), options.session_params.samples);
	else if(key == 'h')
		options.show_help = !(options.show_help);
	else if(key == 27) // escape
		options.session->progress.set_cancel("Canceled");
}
#endif

static int files_parse(int argc, const char *argv[])
{
	if(argc > 0)
		options.filepath = argv[0];

	return 0;
}

static void options_parse(int argc, const char **argv)
{
	options.width = 0;
	options.height = 0;
	options.filepath = "";
	options.session = NULL;
	options.quiet = false;

	/* device names */
	string device_names = "";
	string devicename = "cpu";
	bool list = false;

	vector<DeviceType>& types = Device::available_types();

	foreach(DeviceType type, types) {
		if(device_names != "")
			device_names += ", ";

		device_names += Device::string_from_type(type);
	}

	/* shading system */
	string ssname = "svm";

	/* parse options */
	ArgParse ap;
	bool help = false;

	ap.options ("Usage: cycles [options] file.xml",
		"%*", files_parse, "",
		"--device %s", &devicename, ("Devices to use: " + device_names).c_str(),
#ifdef WITH_OSL
		"--shadingsys %s", &ssname, "Shading system to use: svm, osl",
#endif
		"--background", &options.session_params.background, "Render in background, without user interface",
		"--quiet", &options.quiet, "In background mode, don't print progress messages",
		"--samples %d", &options.session_params.samples, "Number of samples to render",
		"--output %s", &options.session_params.output_path, "File path to write output image",
		"--threads %d", &options.session_params.threads, "CPU Rendering Threads",
		"--width  %d", &options.width, "Window width in pixel",
		"--height %d", &options.height, "Window height in pixel",
		"--list-devices", &list, "List information about all available devices",
		"--help", &help, "Print help message",
		NULL);
	
	if(ap.parse(argc, argv) < 0) {
		fprintf(stderr, "%s\n", ap.geterror().c_str());
		ap.usage();
		exit(EXIT_FAILURE);
	}
	else if(list) {
		vector<DeviceInfo>& devices = Device::available_devices();
		printf("Devices:\n");

		foreach(DeviceInfo& info, devices) {
			printf("    %s%s\n",
				info.description.c_str(),
				(info.display_device)? " (display)": "");
		}

		exit(EXIT_SUCCESS);
	}
	else if(help || options.filepath == "") {
		ap.usage();
		exit(EXIT_SUCCESS);
	}

	if(ssname == "osl")
		options.scene_params.shadingsystem = SceneParams::OSL;
	else if(ssname == "svm")
		options.scene_params.shadingsystem = SceneParams::SVM;
		
#ifdef WITH_CYCLES_STANDALONE_GUI
	/* Progressive rendering for GUI */
	if(!options.session_params.background)
		options.session_params.progressive = true;
#else
	/* When building without GUI, set background */
	options.session_params.background = true;
#endif

	/* find matching device */
	DeviceType device_type = Device::type_from_string(devicename.c_str());
	vector<DeviceInfo>& devices = Device::available_devices();
	DeviceInfo device_info;
	bool device_available = false;

	foreach(DeviceInfo& device, devices) {
		if(device_type == device.type) {
			options.session_params.device = device;
			device_available = true;
			break;
		}
	}

	/* handle invalid configurations */
	if(options.session_params.device.type == DEVICE_NONE || !device_available) {
		fprintf(stderr, "Unknown device: %s\n", devicename.c_str());
		exit(EXIT_FAILURE);
	}
#ifdef WITH_OSL
	else if(!(ssname == "osl" || ssname == "svm")) {
		fprintf(stderr, "Unknown shading system: %s\n", ssname.c_str());
		exit(EXIT_FAILURE);
	}
	else if(options.scene_params.shadingsystem == SceneParams::OSL && options.session_params.device.type != DEVICE_CPU) {
		fprintf(stderr, "OSL shading system only works with CPU device\n");
		exit(EXIT_FAILURE);
	}
#endif
	else if(options.session_params.samples < 0) {
		fprintf(stderr, "Invalid number of samples: %d\n", options.session_params.samples);
		exit(EXIT_FAILURE);
	}
	else if(options.filepath == "") {
		fprintf(stderr, "No file path specified\n");
		exit(EXIT_FAILURE);
	}

	/* load scene */
	scene_init(options.width, options.height);
}

CCL_NAMESPACE_END

using namespace ccl;

int main(int argc, const char **argv)
{
	path_init();
	options_parse(argc, argv);
	
#ifdef WITH_CYCLES_STANDALONE_GUI
	if(options.session_params.background) {
#endif
		session_init();
		options.session->wait();
		session_exit();
#ifdef WITH_CYCLES_STANDALONE_GUI
	}
	else {
		string title = "Cycles: " + path_filename(options.filepath);

		/* init/exit are callback so they run while GL is initialized */
		view_main_loop(title.c_str(), options.width, options.height,
			session_init, session_exit, resize, display, keyboard);
	}
#endif

	return 0;
}

