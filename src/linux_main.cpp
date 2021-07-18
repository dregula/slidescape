// Dear ImGui: standalone example application for SDL2 + OpenGL
// (SDL is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation, etc.)
// (GL3W is a helper library to access OpenGL functions since there is no standard header to access modern OpenGL functions easily. Alternatives are GLEW, Glad, etc.)
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

#include "common.h"
#include "platform.h"
#include "viewer.h"
#include "gui.h" // TODO: move

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>

#include "stb_image.h"
#include "stringified_icon.h"

#include "imgui_freetype.h"

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
#endif

void* worker_thread(void* parameter) {
    platform_thread_info_t* thread_info = (platform_thread_info_t*) parameter;

//	fprintf(stderr, "Hello from thread %d\n", thread_info->logical_thread_index);

    init_thread_memory(thread_info->logical_thread_index);
	atomic_increment(&global_worker_thread_idle_count);

	for (;;) {
        if (!is_queue_work_waiting_to_start(thread_info->queue)) {
            //platform_sleep(1);
            sem_wait(thread_info->queue->semaphore);
        }
        do_worker_work(thread_info->queue, thread_info->logical_thread_index);
    }

    return 0;
}

platform_thread_info_t thread_infos[MAX_THREAD_COUNT];

void linux_init_multithreading() {
	init_thread_memory(0);
    i32 semaphore_initial_count = 0;
    worker_thread_count = total_thread_count - 1;
    global_work_queue.semaphore = sem_open("/worksem", O_CREAT, 0644, semaphore_initial_count);
    global_completion_queue.semaphore = sem_open("/completionsem", O_CREAT, 0644, semaphore_initial_count);

    pthread_t threads[MAX_THREAD_COUNT] = {};

    // NOTE: the main thread is considered thread 0.
    for (i32 i = 1; i < total_thread_count; ++i) {
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

bool linux_process_input() {
    // Swap
    input_t* temp = old_input;
    old_input = curr_input;
    curr_input = temp;


    // reset the transition counts.
    // TODO: can't we just do that, instead of reinitializing the reset?


    curr_input->drag_start_xy = old_input->drag_start_xy;
    curr_input->drag_vector = old_input->drag_vector;

    ImGuiIO& io = ImGui::GetIO();
    curr_input->mouse_xy = io.MousePos;

    u32 button_count = MIN(COUNT(curr_input->mouse_buttons), COUNT(io.MouseDown));
    memset_zero(&curr_input->mouse_buttons);
    for (u32 i = 0; i < button_count; ++i) {
        curr_input->mouse_buttons[i].down = old_input->mouse_buttons[i].down;
        linux_process_button_event(&curr_input->mouse_buttons[i], io.MouseDown[i]);
    }

    memset_zero(&curr_input->keyboard);
    for (u32 i = 0; i < COUNT(curr_input->keyboard.buttons); ++i) {
        curr_input->keyboard.buttons[i].down = old_input->keyboard.buttons[i].down;

    }
    u32 key_count = MIN(COUNT(curr_input->keyboard.keys), COUNT(io.KeysDown));
    for (u32 i = 0; i < key_count; ++i) {
        curr_input->keyboard.keys[i].down = old_input->keyboard.keys[i].down;
        linux_process_button_event(&curr_input->keyboard.keys[i], io.KeysDown[i]);
    }

    curr_input->keyboard.key_shift.down = old_input->keyboard.key_shift.down;
    curr_input->keyboard.key_ctrl.down = old_input->keyboard.key_ctrl.down;
    curr_input->keyboard.key_alt.down = old_input->keyboard.key_alt.down;
    curr_input->keyboard.key_super.down = old_input->keyboard.key_super.down;
    linux_process_button_event(&curr_input->keyboard.key_shift, io.KeyShift);
    linux_process_button_event(&curr_input->keyboard.key_ctrl, io.KeyCtrl);
    linux_process_button_event(&curr_input->keyboard.key_alt, io.KeyAlt);
    linux_process_button_event(&curr_input->keyboard.key_super, io.KeySuper);

	curr_input->mouse_z = io.MouseWheel;

    v2f mouse_delta = io.MouseDelta;
//    mouse_delta.x *= window_scale_factor;
//    mouse_delta.y *= window_scale_factor;
    curr_input->drag_vector = mouse_delta;

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

extern SDL_Window* g_window;

static i32 need_check_window_focus_gained_after_frames;

// Main code
int main(int argc, const char** argv)
{

    g_argc = argc;
    g_argv = argv;
	console_printer_benaphore = benaphore_create();
    console_print("Starting up...\n");
    get_system_info();

	app_state_t* app_state = &global_app_state;
	init_app_state(app_state);
	viewer_init_options(app_state);

    linux_init_multithreading();
    add_work_queue_entry(&global_work_queue, (work_queue_callback_t*)init_openslide, NULL, 0);
    linux_init_input();

    // Setup SDL
    // (Some versions of SDL before <2.0.10 appears to have performance/stalling issues on a minority of Windows systems,
    // depending on whether SDL_INIT_GAMECONTROLLER is enabled or disabled.. updating to latest version of SDL is recommended!)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        console_print_error("Error: %s\n", SDL_GetError());
        return -1;
    }

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
    SDL_Window* window = SDL_CreateWindow("Slideviewer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, desired_window_width, desired_window_height, window_flags);
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
	stringify_icon_image(); // (re)creates stringified_icon.c
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

    if (is_macos) {
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
#if LINUX
	global_main_font = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/noto/NotoSans-Regular.ttf", 17.0f);
	if (!global_main_font) {
		global_main_font = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16.0f);
	}
	global_fixed_width_font = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/noto/NotoMono-Regular.ttf/NotoMono-Regular.ttf", 15.0f);
	if (!global_fixed_width_font) {
		global_fixed_width_font = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/noto/NotoMono-Regular.ttf", 15.0f);
		if (!global_fixed_width_font) {
			global_fixed_width_font = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 15.0f);
		}
	}
	io.Fonts->AddFontDefault();
#elif APPLE
	float font_size = 16.0f * app_state->display_scale_factor;
	global_main_font = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/SFNSText.ttf", font_size, NULL, io.Fonts->GetGlyphRangesJapanese());
//	global_fixed_width_font = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Courier.dfont", 15.0f);
	global_fixed_width_font = io.Fonts->AddFontDefault();
	io.Fonts->Build();
	global_main_font->Scale = app_state->display_points_per_pixel;
#endif
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != NULL);



	io.Fonts->FontBuilderFlags = ImGuiFreeTypeBuilderFlags_MonoHinting;

    // Our state
//    bool show_demo_window = true;
//    bool show_another_window = false;
//    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

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
	            is_program_running = false;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
	            is_program_running = false;
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
		    is_program_running = false;
	    }
	    if (was_key_pressed(curr_input, KEY_O) && curr_input->keyboard.key_ctrl.down) {
		    open_file_dialog(app_state, 0);
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
        ImGui_ImplSDL2_NewFrame(g_window);
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
