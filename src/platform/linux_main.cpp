/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2023  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// Dear ImGui: standalone example application for SDL2 + OpenGL
// (SDL is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation, etc.)
// (GL3W is a helper library to access OpenGL functions since there is no standard header to access modern OpenGL functions easily. Alternatives are GLEW, Glad, etc.)
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

#include "common.h"
#include "platform.h"
#include "viewer.h"
#include "gui.h" // TODO: move
#include "dicom.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl.h"
#include "backends/imgui_impl_opengl3.h"
#include <stdio.h>

#include "stb_image.h"
#include "stringified_icon.h"

#include "misc/freetype/imgui_freetype.h"

// About Desktop OpenGL function loaders:
//  Modern desktop OpenGL doesn't have a standard portable header file to load OpenGL function pointers.
//  Helper libraries are often used for this purpose! Here we are supporting a few common ones (gl3w, glew, glad).
//  You may use another loader/header of your choice (glext, glLoadGen, etc.), or chose to manually implement your own.
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
#include <GL/gl3w.h>            // Initialize with gl3wInit()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
#include <GL/glew.h>            // Initialize with glewInit()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
#include <glad/glad.h>          // Initialize with gladLoadGL()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD2)
#include <glad/gl.h>            // Initialize with gladLoadGL(...) or gladLoaderLoadGL()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING2)
#define GLFW_INCLUDE_NONE       // GLFW including OpenGL headers causes ambiguity or multiple definition errors.
#include <glbinding/Binding.h>  // Initialize with glbinding::Binding::initialize()
#include <glbinding/gl/gl.h>
using namespace gl;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING3)
#define GLFW_INCLUDE_NONE       // GLFW including OpenGL headers causes ambiguity or multiple definition errors.
#include <glbinding/glbinding.h>// Initialize with glbinding::initialize()
#include <glbinding/gl/gl.h>
using namespace gl;
#else
#include IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#endif

#include <pthread.h>

#if DO_DEBUG
void stringify_icon_image() {
	const char* resource_filename = "resources/icon/icon128.png";
	mem_t* icon_file = platform_read_entire_file(resource_filename);
	if (icon_file) {
		FILE* f_output = fopen("src/stringified_icon.h", "w");

		fprintf(f_output,   "// This is a stringified version of the file %s\n"
		                    "// It is used to load the window icon on Linux.\n"
		                    "\n"
		                    "// This file is automatically generated at runtime."
		                    "\n"
		                    "#pragma once"
		                    "\n"
		                    "\n", resource_filename);

		// Adapted from bin2c:
		// https://github.com/gwilymk/bin2c

		bool32 need_comma = false;
		fprintf(f_output, "const u8 stringified_icon_bytes[%u] = {", icon_file->len);
		for (u64 i = 0; i < icon_file->len; ++i) {
			if (need_comma)
				fprintf(f_output, ",");
			else
				need_comma = 1;
			if ((i % 32) == 0)
				fprintf(f_output, "\n\t");
			fprintf(f_output, "%u", icon_file->data[i]);
		}
		fprintf(f_output, "\n};\n\n");

		fclose(f_output);
		free(icon_file);
	}
}
#endif

static void* worker_thread(void* parameter) {
    platform_thread_info_t* thread_info = (platform_thread_info_t*) parameter;

//	fprintf(stderr, "Hello from thread %d\n", thread_info->logical_thread_index);

    init_thread_memory(thread_info->logical_thread_index, &global_system_info);
	atomic_increment(&global_worker_thread_idle_count);

	for (;;) {
		if (thread_info->logical_thread_index > global_active_worker_thread_count) {
			// Worker is disabled, do nothing
			platform_sleep(100);
			continue;
		}
        if (!is_queue_work_waiting_to_start(thread_info->queue)) {
            //platform_sleep(1);
            sem_wait(thread_info->queue->semaphore);
            if (thread_info->logical_thread_index > global_active_worker_thread_count) {
                // Worker is disabled, do nothing
                platform_sleep(100);
                continue;
            }
        }
        do_worker_work(thread_info->queue, thread_info->logical_thread_index);
    }

    return 0;
}

platform_thread_info_t thread_infos[MAX_THREAD_COUNT];

void linux_init_multithreading() {
	init_thread_memory(0, &global_system_info);
    global_worker_thread_count = global_system_info.suggested_total_thread_count - 1;
	global_active_worker_thread_count = global_worker_thread_count;

	global_work_queue = create_work_queue("/worksem", 1024); // Queue for newly submitted tasks
	global_completion_queue = create_work_queue("/completionsem", 1024); // Message queue for completed tasks
	global_export_completion_queue = create_work_queue("/exportcompletionsem", 1024); // Message queue for export task

    pthread_t threads[MAX_THREAD_COUNT] = {};

    // NOTE: the main thread is considered thread 0.
    for (i32 i = 1; i < global_system_info.suggested_total_thread_count; ++i) {
        thread_infos[i] = (platform_thread_info_t){ .logical_thread_index = i, .queue = &global_work_queue};

        if (pthread_create(threads + i, NULL, &worker_thread, (void*)(&thread_infos[i])) != 0) {
            fprintf(stderr, "Error creating thread\n");
        }

    }

    test_multithreading_work_queue();


}

void linux_init_input() {
    old_input = &inputs[0];
    curr_input = &inputs[1];
}

void linux_process_button_event(button_state_t* new_state, bool32 down) {
    down = (down != 0);
    if (new_state->down != down) {
        new_state->down = (bool8)down;
        ++new_state->transition_count;
    }
}

static ImGuiKey hid_keycode_to_ImGuiKey(int keycode)
{
	switch (keycode)
	{
		case KEY_Tab: return ImGuiKey_Tab;
		case KEY_Left: return ImGuiKey_LeftArrow;
		case KEY_Right: return ImGuiKey_RightArrow;
		case KEY_Up: return ImGuiKey_UpArrow;
		case KEY_Down: return ImGuiKey_DownArrow;
		case KEY_PageUp: return ImGuiKey_PageUp;
		case KEY_PageDown: return ImGuiKey_PageDown;
		case KEY_Home: return ImGuiKey_Home;
		case KEY_End: return ImGuiKey_End;
		case KEY_Insert: return ImGuiKey_Insert;
		case KEY_DeleteForward: return ImGuiKey_Delete;
		case KEY_Delete: return ImGuiKey_Backspace;
		case KEY_Space: return ImGuiKey_Space;
		case KEY_Return: return ImGuiKey_Enter;
		case KEY_Escape: return ImGuiKey_Escape;
		case KEY_Quote: return ImGuiKey_Apostrophe;
		case KEY_Comma: return ImGuiKey_Comma;
		case KEY_Minus: return ImGuiKey_Minus;
		case KEY_Period: return ImGuiKey_Period;
		case KEY_Slash: return ImGuiKey_Slash;
		case KEY_Semicolon: return ImGuiKey_Semicolon;
		case KEY_Equals: return ImGuiKey_Equal;
		case KEY_LeftBracket: return ImGuiKey_LeftBracket;
		case KEY_Backslash: return ImGuiKey_Backslash;
		case KEY_RightBracket: return ImGuiKey_RightBracket;
		case KEY_Grave: return ImGuiKey_GraveAccent;
		case KEY_CapsLock: return ImGuiKey_CapsLock;
		case KEY_ScrollLock: return ImGuiKey_ScrollLock;
		case ImGuiKey_NumLock: return ImGuiKey_NumLock;
		case KEY_PrintScreen: return ImGuiKey_PrintScreen;
		case KEY_Pause: return ImGuiKey_Pause;
		case KP_0: return ImGuiKey_Keypad0;
		case KP_1: return ImGuiKey_Keypad1;
		case KP_2: return ImGuiKey_Keypad2;
		case KP_3: return ImGuiKey_Keypad3;
		case KP_4: return ImGuiKey_Keypad4;
		case KP_5: return ImGuiKey_Keypad5;
		case KP_6: return ImGuiKey_Keypad6;
		case KP_7: return ImGuiKey_Keypad7;
		case KP_8: return ImGuiKey_Keypad8;
		case KP_9: return ImGuiKey_Keypad9;
		case KP_Point: return ImGuiKey_KeypadDecimal;
		case KP_Divide: return ImGuiKey_KeypadDivide;
		case KP_Multiply: return ImGuiKey_KeypadMultiply;
		case KP_Subtract: return ImGuiKey_KeypadSubtract;
		case KP_Add: return ImGuiKey_KeypadAdd;
		case KP_Enter: return ImGuiKey_KeypadEnter;
		case KP_Equals: return ImGuiKey_KeypadEqual;
		case KEY_LeftControl: return ImGuiKey_LeftCtrl;
		case KEY_LeftShift: return ImGuiKey_LeftShift;
		case KEY_LeftAlt: return ImGuiKey_LeftAlt;
		case KEY_LeftGUI: return ImGuiKey_LeftSuper;
		case KEY_RightControl: return ImGuiKey_RightCtrl;
		case KEY_RightShift: return ImGuiKey_RightShift;
		case KEY_RightAlt: return ImGuiKey_RightAlt;
		case KEY_RightGUI: return ImGuiKey_RightSuper;
		case KEY_Menu: return ImGuiKey_Menu;
		case KEY_0: return ImGuiKey_0;
		case KEY_1: return ImGuiKey_1;
		case KEY_2: return ImGuiKey_2;
		case KEY_3: return ImGuiKey_3;
		case KEY_4: return ImGuiKey_4;
		case KEY_5: return ImGuiKey_5;
		case KEY_6: return ImGuiKey_6;
		case KEY_7: return ImGuiKey_7;
		case KEY_8: return ImGuiKey_8;
		case KEY_9: return ImGuiKey_9;
		case KEY_A: return ImGuiKey_A;
		case KEY_B: return ImGuiKey_B;
		case KEY_C: return ImGuiKey_C;
		case KEY_D: return ImGuiKey_D;
		case KEY_E: return ImGuiKey_E;
		case KEY_F: return ImGuiKey_F;
		case KEY_G: return ImGuiKey_G;
		case KEY_H: return ImGuiKey_H;
		case KEY_I: return ImGuiKey_I;
		case KEY_J: return ImGuiKey_J;
		case KEY_K: return ImGuiKey_K;
		case KEY_L: return ImGuiKey_L;
		case KEY_M: return ImGuiKey_M;
		case KEY_N: return ImGuiKey_N;
		case KEY_O: return ImGuiKey_O;
		case KEY_P: return ImGuiKey_P;
		case KEY_Q: return ImGuiKey_Q;
		case KEY_R: return ImGuiKey_R;
		case KEY_S: return ImGuiKey_S;
		case KEY_T: return ImGuiKey_T;
		case KEY_U: return ImGuiKey_U;
		case KEY_V: return ImGuiKey_V;
		case KEY_W: return ImGuiKey_W;
		case KEY_X: return ImGuiKey_X;
		case KEY_Y: return ImGuiKey_Y;
		case KEY_Z: return ImGuiKey_Z;
		case KEY_F1: return ImGuiKey_F1;
		case KEY_F2: return ImGuiKey_F2;
		case KEY_F3: return ImGuiKey_F3;
		case KEY_F4: return ImGuiKey_F4;
		case KEY_F5: return ImGuiKey_F5;
		case KEY_F6: return ImGuiKey_F6;
		case KEY_F7: return ImGuiKey_F7;
		case KEY_F8: return ImGuiKey_F8;
		case KEY_F9: return ImGuiKey_F9;
		case KEY_F10: return ImGuiKey_F10;
		case KEY_F11: return ImGuiKey_F11;
		case KEY_F12: return ImGuiKey_F12;
	}
	return ImGuiKey_None;
}

bool linux_process_input() {
    // Swap
    input_t* temp = old_input;
    old_input = curr_input;
    curr_input = temp;

    curr_input->drag_start_xy = old_input->drag_start_xy;
    curr_input->drag_vector = old_input->drag_vector;

    ImGuiIO& io = ImGui::GetIO();
	// Make sure we have the latest events processed
	ImGui::UpdateInputEvents(false);

	curr_input->mouse_xy = io.MousePos;

	SDL_PumpEvents();
	u32 mouse_buttons = SDL_GetMouseState(NULL, NULL);
	u32 button_count = MIN(COUNT(curr_input->mouse_buttons), 5);
    memset_zero(&curr_input->mouse_buttons);
    for (u32 i = 0; i < button_count; ++i) {
        curr_input->mouse_buttons[i].down = old_input->mouse_buttons[i].down;
        linux_process_button_event(&curr_input->mouse_buttons[i], mouse_buttons & SDL_BUTTON(i + 1));
    }

    memset_zero(&curr_input->keyboard);
    for (u32 i = 0; i < COUNT(curr_input->keyboard.buttons); ++i) {
        curr_input->keyboard.buttons[i].down = old_input->keyboard.buttons[i].down;

    }
    u32 key_count = COUNT(curr_input->keyboard.keys);
    for (u32 i = 0; i < key_count; ++i) {
        curr_input->keyboard.keys[i].down = old_input->keyboard.keys[i].down;
		ImGuiKey key = hid_keycode_to_ImGuiKey(i);
		if (key != ImGuiKey_None) {
			linux_process_button_event(&curr_input->keyboard.keys[i], ImGui::IsKeyDown(hid_keycode_to_ImGuiKey(i)));
		}
    }

    curr_input->keyboard.key_shift.down = old_input->keyboard.key_shift.down;
    curr_input->keyboard.key_ctrl.down = old_input->keyboard.key_ctrl.down;
    curr_input->keyboard.key_alt.down = old_input->keyboard.key_alt.down;
    curr_input->keyboard.key_super.down = old_input->keyboard.key_super.down;
    linux_process_button_event(&curr_input->keyboard.key_shift, io.KeyShift);
    linux_process_button_event(&curr_input->keyboard.key_ctrl, io.KeyCtrl);
    linux_process_button_event(&curr_input->keyboard.key_alt, io.KeyAlt);
    linux_process_button_event(&curr_input->keyboard.key_super, io.KeySuper);

	curr_input->keyboard.modifiers = 0;
	if (curr_input->keyboard.key_ctrl.down) curr_input->keyboard.modifiers |= KMOD_CTRL;
	if (curr_input->keyboard.key_alt.down) curr_input->keyboard.modifiers |= KMOD_ALT;
	if (curr_input->keyboard.key_shift.down) curr_input->keyboard.modifiers |= KMOD_SHIFT;
	if (curr_input->keyboard.key_super.down) curr_input->keyboard.modifiers |= KMOD_GUI;

	curr_input->mouse_z = io.MouseWheel;

	i32 mouse_x = 0, mouse_y = 0;
	SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
	v2f mouse_delta = (v2f){(float)mouse_x, (float)mouse_y};
    curr_input->drag_vector = mouse_delta;

	curr_input->mouse_moved = (mouse_delta.x != 0.0f || mouse_delta.y != 0.0f);
    if (cursor_hidden && !curr_input->mouse_buttons[0].down && curr_input->mouse_moved) {
	    mouse_show();
    }

    curr_input->are_any_buttons_down = false;
    for (u32 i = 0; i < COUNT(curr_input->keyboard.buttons); ++i) {
        curr_input->are_any_buttons_down = (curr_input->are_any_buttons_down) || curr_input->keyboard.buttons[i].down;
    }
    for (u32 i = 0; i < COUNT(curr_input->keyboard.keys); ++i) {
        curr_input->are_any_buttons_down = (curr_input->are_any_buttons_down) || curr_input->keyboard.keys[i].down;
    }
    for (u32 i = 0; i < COUNT(curr_input->mouse_buttons); ++i) {
        curr_input->are_any_buttons_down = (curr_input->are_any_buttons_down) || curr_input->mouse_buttons[i].down;
    }


    bool did_idle = false;
    return did_idle;
}

// TODO: refactor
void load_openslide_task(int logical_thread_index, void* userdata) {
	is_openslide_available = init_openslide();
	if (is_openslide_available) {
		// TODO: register file associations under Linux
//		if (!win32_registry_add_to_open_with_list("svs")) return;
//		if (!win32_registry_add_to_open_with_list("ndpi")) return;
//		if (!win32_registry_add_to_open_with_list("vms")) return;
//		if (!win32_registry_add_to_open_with_list("scn")) return;
//		if (!win32_registry_add_to_open_with_list("mrxs")) return;
//		if (!win32_registry_add_to_open_with_list("bif")) return;
	}
	is_openslide_loading_done = true;
}

void load_dicom_task(int logical_thread_index, void* userdata) {
	is_dicom_available = dicom_init();
	is_dicom_loading_done = true;
}

extern SDL_Window* g_window;

static i32 need_check_window_focus_gained_after_frames;

// Main code
int main(int argc, const char** argv)
{

    g_argc = argc;
    g_argv = argv;

    app_command_t app_command = app_parse_commandline(argc, argv);
    if (app_command.exit_immediately) {
        app_command_execute_immediately(&app_command);
        exit(0);
    }
    bool verbose_console = true;//!app_command.headless;

	console_printer_benaphore = benaphore_create();
    if (verbose_console) console_print("Starting up...\n");
    get_system_info(verbose_console);

	app_state_t* app_state = &global_app_state;
	init_app_state(app_state, app_command);
	viewer_init_options(app_state);

    linux_init_multithreading();

    if (app_command.headless) {
        is_openslide_available = init_openslide();
        is_openslide_loading_done = true;
        return app_command_execute(app_state);
    }

    add_work_queue_entry(&global_work_queue, (work_queue_callback_t*)load_openslide_task, NULL, 0);
    add_work_queue_entry(&global_work_queue, (work_queue_callback_t*)load_dicom_task, NULL, 0);
    linux_init_input();

	/*i32 num_video_drivers = SDL_GetNumVideoDrivers();
	const char* chosen_driver = NULL;
	for (i32 i = 0; i < num_video_drivers; ++i) {
		const char* driver = SDL_GetVideoDriver(i);
		console_print("SDL video driver %d: %s\n", i, driver);
		if (strcmp(driver, "wayland") == 0) {
			chosen_driver = driver;
		}
	}*/
	SDL_VideoInit(NULL);

    // Setup SDL
    // (Some versions of SDL before <2.0.10 appears to have performance/stalling issues on a minority of Windows systems,
    // depending on whether SDL_INIT_GAMECONTROLLER is enabled or disabled.. updating to latest version of SDL is recommended!)
	i64 clock_sdl_begin = get_clock();
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        console_print_error("Error: %s\n", SDL_GetError());
        return -1;
    }
	float seconds_elapsed_sdl_init = get_seconds_elapsed(clock_sdl_begin, get_clock());
	console_print_verbose("Initialized SDL in %g seconds\n", seconds_elapsed_sdl_init);

    // Decide GL+GLSL versions
#ifdef __APPLE__
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    u32 window_flags = (SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window_start_maximized) {
    	window_flags |= SDL_WINDOW_MAXIMIZED;
    }
	window_flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window* window = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, desired_window_width, desired_window_height, window_flags);
    g_window = window;
	app_state->main_window = window;

	{
		i32 gl_w, gl_h;
		SDL_GL_GetDrawableSize(window, &gl_w, &gl_h);
		i32 window_w, window_h;
		SDL_GetWindowSize(window, &window_w, &window_h);
		app_state->display_scale_factor = (float) gl_w / (float) window_w;
		app_state->display_points_per_pixel = (float) window_w / (float) gl_w;
	}

    // Load icon
#if DO_DEBUG
	if (!is_macos) {
		stringify_icon_image(); // (re)creates stringified_icon.h
	}
#endif
	if constexpr(sizeof(stringified_icon_bytes) > 1) {
		int x = 0;
		int y = 0;
		int channels_in_file = 0;
		u8* pixels = stbi_load_from_memory(stringified_icon_bytes, sizeof(stringified_icon_bytes), &x, &y, &channels_in_file, 4);
		if (pixels) {
			SDL_Surface* icon = SDL_CreateRGBSurfaceFrom(pixels, x, y, 32, x*4, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
			SDL_SetWindowIcon(window, icon);
			SDL_FreeSurface(icon);
		}
	}

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);

    char* version_string = (char*)glGetString(GL_VERSION);
    console_print("OpenGL supported version: %s\n", version_string);

    if (global_system_info.is_macos) {
	    is_vsync_enabled = 1; // prevent stutter (?)
    } else {
    	is_vsync_enabled = 0;
    }
    SDL_GL_SetSwapInterval(is_vsync_enabled); // Enable vsync

    // Initialize OpenGL loader
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
    bool err = gl3wInit() != 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
    bool err = glewInit() != GLEW_OK;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
    bool err = gladLoadGL() == 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD2)
    bool err = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress) == 0; // glad2 recommend using the windowing library loader instead of the (optionally) bundled one.
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING2)
    bool err = false;
    glbinding::Binding::initialize();
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING3)
    bool err = false;
    glbinding::initialize([](const char* name) { return (glbinding::ProcAddress)SDL_GL_GetProcAddress(name); });
#else
    bool err = false; // If you use IMGUI_IMPL_OPENGL_LOADER_CUSTOM, your loader is likely to requires some form of initialization.
#endif
    if (err)
    {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return 1;
    }

    // Setup Dear ImGui context
    imgui_create_context();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/ProggyTiny.ttf", 10.0f);
	static const ImWchar ranges[] = {
			0x0020, 0x00FF, // Basic Latin + Latin Supplement
			0x0370, 0x03FF, // Greek
			0,
	};
#if LINUX

	// Query default monitor resolution
	float ddpi, hdpi, vdpi;
	if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0) {
		fprintf(stderr, "Failed to obtain DPI information for display 0: %s\n", SDL_GetError());
		exit(1);
	}
	float dpi_scaling = ddpi / 72.f;
	float font_scale_factor = 1.0f;
	if (dpi_scaling > 1.0f) {
		font_scale_factor += ((dpi_scaling - 1.0f) * 0.5f);
	}

	const char* main_ui_font_filename = "/usr/share/fonts/noto/NotoSans-Regular.ttf";
	if (file_exists(main_ui_font_filename)) {
		global_main_font = io.Fonts->AddFontFromFileTTF(main_ui_font_filename, 17.0f * font_scale_factor, NULL, ranges);
	} else {
		main_ui_font_filename = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
		if (file_exists(main_ui_font_filename)) {
			global_main_font = io.Fonts->AddFontFromFileTTF(main_ui_font_filename, 16.0f * font_scale_factor, NULL, ranges);
		}
	}

	const char* fixed_width_font_filename = "/usr/share/fonts/noto/NotoMono-Regular.ttf/NotoMono-Regular.ttf";
	if (file_exists(fixed_width_font_filename)) {
		global_fixed_width_font = io.Fonts->AddFontFromFileTTF(fixed_width_font_filename, 15.0f * font_scale_factor, NULL, ranges);
	} else {
		fixed_width_font_filename = "/usr/share/fonts/noto/NotoMono-Regular.ttf";
		if (file_exists(fixed_width_font_filename)) {
			global_fixed_width_font = io.Fonts->AddFontFromFileTTF(fixed_width_font_filename, 15.0f * font_scale_factor, NULL, ranges);
		} else {
			fixed_width_font_filename = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
			if (file_exists(fixed_width_font_filename)) {
				global_fixed_width_font = io.Fonts->AddFontFromFileTTF(fixed_width_font_filename, 15.0f * font_scale_factor, NULL, ranges);
			}
		}
	}
#elif APPLE
	float font_size = 16.0f * app_state->display_scale_factor;
	const char* main_ui_font_filename = "/System/Library/Fonts/SFNSText.ttf";
	if (file_exists(main_ui_font_filename)) {
		global_main_font = io.Fonts->AddFontFromFileTTF(main_ui_font_filename, font_size, NULL, ranges);
	} else {
        main_ui_font_filename = "/System/Library/Fonts/SFNS.ttf";
        if (file_exists(main_ui_font_filename)) {
            global_main_font = io.Fonts->AddFontFromFileTTF(main_ui_font_filename, font_size, NULL, ranges);
        }
    }

	const char* fixed_width_font_filename = "/System/Library/Fonts/Menlo.ttc";
	if (file_exists(fixed_width_font_filename)) {
		global_fixed_width_font = io.Fonts->AddFontFromFileTTF(fixed_width_font_filename, 14.0f * app_state->display_scale_factor, NULL, ranges);
		if (global_fixed_width_font) {
			global_fixed_width_font->Scale = app_state->display_points_per_pixel;
		}
	}
#endif

    ImFont* font_default = io.Fonts->AddFontDefault();
    if (!global_main_font) {
        console_print_error("Cannot load main UI font, defaulting to built-in font.\n");
        global_main_font = font_default;
    }
    if (!global_fixed_width_font) {
		global_fixed_width_font = font_default;
	}

	io.Fonts->FontBuilderFlags = ImGuiFreeTypeBuilderFlags_MonoHinting;
	io.Fonts->Build();
	global_main_font->Scale = app_state->display_points_per_pixel;

    init_opengl_stuff(app_state);

    // Load a slide from the command line or through the OS (double-click / drag on executable, etc.)
    if (g_argc > 1) {
        const char* filename = g_argv[1];
        load_generic_file(app_state, filename, 0);
    }

    // Main loop
    is_program_running = true;
    i64 last_clock = get_clock();
    while (is_program_running) {
        i64 current_clock = get_clock();
        app_state->last_frame_start = current_clock;
        float delta_t = (float)(current_clock - last_clock) / (float)1e9;
//        printf("delta_t = %g, clock = %lld\n", delta_t, current_clock);
        last_clock = current_clock;
        delta_t = CLAMP(delta_t, 0.00001f, 2.0f / 60.0f); // prevent physics overshoot at lag spikes

        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
	            need_quit = true;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
	            need_quit = true;
            } else if (event.type == SDL_DROPFILE) {
	            u32 filetype_hint = load_next_image_as_overlay ? FILETYPE_HINT_OVERLAY : 0;
            	if (load_generic_file(app_state, event.drop.file, filetype_hint)) {
		            // Bring the window to the foreground / set input-focus.
		            // This makes it possible to immediately interact with the scene.
		            SDL_RaiseWindow(window);
		            // NOTE: on KDE there is a system setting 'focus stealing prevention setting', preventing us from getting the window focus.
		            // Solution: change this system setting from 'Low' (the default in my case) to 'None'.
		            // https://stackoverflow.com/questions/28782681/sdl2-how-to-raise-window-on-top-of-calling-terminal
		            // To warn the user this is happening, we try to detect this situation and write an error message to the console (see code below).
		            need_check_window_focus_gained_after_frames = 10;

            	}
	            SDL_free(event.drop.file);
            }
        }

        linux_process_input();

	    if (was_key_pressed(curr_input, KEY_F4) && curr_input->keyboard.key_alt.down) {
		    need_quit = true;
	    }
	    if (was_key_pressed(curr_input, KEY_O) && curr_input->keyboard.key_ctrl.down) {
		    open_file_dialog(app_state, OPEN_FILE_DIALOG_LOAD_GENERIC_FILE, 0);
	    }
	    if (was_key_pressed(curr_input, KEY_F11) || (was_key_pressed(curr_input, KEY_Return) && curr_input->keyboard.key_alt.down)) {
		    toggle_fullscreen(app_state->main_window);
	    }

	    u32 current_window_flags = SDL_GetWindowFlags(window);
        int w, h;
        int display_w, display_h;
        SDL_GetWindowSize(window, &w, &h);
        if (current_window_flags & SDL_WINDOW_MINIMIZED) {
	        w = h = 0;
	        display_w = display_h = 0;
        }
        SDL_GL_GetDrawableSize(window, &display_w, &display_h);

        // After dragging a file onto the window to load it, we want to gain the window focus.
        // Detect if this was successfully done (see event handling code above) and warn the user if it failed.
        if (need_check_window_focus_gained_after_frames > 0) {
	        --need_check_window_focus_gained_after_frames;
	        if (need_check_window_focus_gained_after_frames == 0) {
		        if (!(current_window_flags & SDL_WINDOW_INPUT_FOCUS)) {
			        console_print_error("Could not gain window focus (maybe need to adjust the 'focus stealing prevention' setting on your system?)\n");
		        }
	        }
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Update and render our application
        viewer_update_and_render(app_state, curr_input, display_w, display_h, delta_t);

        // Finish up by rendering the UI
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);

        float frame_time = get_seconds_elapsed(last_clock, get_clock());

        float target_frame_time = 0.002f;
        float time_to_sleep = target_frame_time - frame_time;
        if (time_to_sleep > 0) {
	        platform_sleep_ns((i64)(time_to_sleep * 1e9));
        }

//	    printf("Frame time: %g\n", get_seconds_elapsed(last_clock, get_clock()));

    }

    autosave(app_state, true); // save any unsaved changes

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
