#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_keycode.h"
#include "SDL_log.h"
#include "SDL_render.h"
#include "SDL_timer.h"
#include "SDL_video.h"

typedef struct {
	SDL_Window *window;
	SDL_Renderer *renderer;
} sdl_t;

typedef struct {
	uint32_t window_height; // SDL Window height
	uint32_t window_width;	// SDL Window width
	uint32_t fg_color;			// Foreground color RGBA8888
	uint32_t bg_color;			// background color RGBA8888
	uint32_t scale_factor;	// Amount to scale a CHIP8 pixels
} config_t;

typedef enum {
	QUIT,
	RUNNING,
	PAUSED,
} emulator_state_t;

typedef struct {
	emulator_state_t state;
} chip8_t;

bool init_sdl(sdl_t *sdl, const config_t config) {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
		SDL_Log("Could not init SDL subsystems! %s\n", SDL_GetError());
		return false;
	}
	sdl->window = SDL_CreateWindow("CHIP8 Emulator", SDL_WINDOWPOS_CENTERED,
																 SDL_WINDOWPOS_CENTERED,
																 config.window_width * config.scale_factor,
																 config.window_height * config.scale_factor, 0);
	if (!sdl->window) {
		SDL_Log("Could not create window %s\n", SDL_GetError());
		return false;
	}

	sdl->renderer = SDL_CreateRenderer(sdl->window, -1, SDL_RENDERER_ACCELERATED);
	if (!sdl->renderer) {
		SDL_Log("Could not create renderer %s\n", SDL_GetError());
		return false;
	}
	return true;
}

// Setup initial emulator configuration from passed in arguments
bool set_config_from_args(config_t *config, const int argc, char **argv) {
	// set default
	*config = (config_t){
			.window_width = 64,			// CHIP8 original X resolution
			.window_height = 32,		// CHIP8 original Y resolution
			.fg_color = 0xFFFFFFFF, // white
			.bg_color = 0xFFFF00FF, // yellow
			.scale_factor = 20,			// Default resolution
	};

	// Override defaults form passed in arguments
	for (int i = 1; i < argc; i++) {
		(void)argv[i];
	}
	// ...
	return true;
}

bool init_chip8(chip8_t *chip8) {
	chip8->state = RUNNING; // Default machine state to RUNNING
	return true;
}

void final_cleanup(const sdl_t sdl) {
	SDL_DestroyRenderer(sdl.renderer);
	SDL_DestroyWindow(sdl.window);
	SDL_Quit(); // shutdown SDL subsystems
}

// Clear screen / SDL window to background color
void clear_screen(const config_t config, const sdl_t sdl) {
	const uint8_t r = (config.bg_color >> 24) & 0xFF;
	const uint8_t g = (config.bg_color >> 16) & 0xFF;
	const uint8_t b = (config.bg_color >> 8) & 0xFF;
	const uint8_t a = (config.bg_color >> 0) & 0xFF;
	SDL_SetRenderDrawColor(sdl.renderer, r, g, b, a);
	SDL_RenderClear(sdl.renderer);
}

// Update window with any changes
void update_screen(const sdl_t sdl) { SDL_RenderPresent(sdl.renderer); }

// User input
void handle_input(chip8_t *chip8) {
	SDL_Event event;

	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			// Exit window; End program
			chip8->state = QUIT;
			return;
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym) {
			case SDLK_ESCAPE:
				chip8->state = QUIT;
				return;
			default:
				break;
			}
			break;
		case SDL_KEYUP:
			break;
		default:
			break;
		}
	}
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	// Init emulator config
	config_t config = {0};
	if (!set_config_from_args(&config, argc, argv))
		exit(EXIT_FAILURE);

	// Initialize SDL
	sdl_t sdl = {0};
	if (!init_sdl(&sdl, config))
		exit(EXIT_FAILURE);

	// Init chip8 machine
	chip8_t chip8 = {0};
	if (!init_chip8(&chip8))
		exit(EXIT_FAILURE);

	// Initial screen clear
	clear_screen(config, sdl);

	// main emulator loop
	while (chip8.state != QUIT) {
		// Handle user input
		handle_input(&chip8);
		// if (chip8.state== PAUSED) continue;

		// Get_time();
		// emulate CHIP8 Instructions
		// Get_time() elapsed since last get_time();

		// delay for approximately 60hz/60fps (16.67ms)
		SDL_Delay(16);
		// update window with changes
		update_screen(sdl);
	}

	// Final cleanup
	final_cleanup(sdl);

	exit(EXIT_SUCCESS);
}
