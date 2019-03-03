#pragma once

#include "common.h"


extern int g_argc;
extern char** g_argv;

// Platform specific function prototypes

i64 get_clock();
float get_seconds_elapsed(i64 start, i64 end);

void mouse_show();
void mouse_hide();
