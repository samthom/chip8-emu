#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "SDL.h"
#include "SDL_audio.h"
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
	SDL_AudioSpec want, have;
	SDL_AudioDeviceID dev;
} sdl_t;

typedef struct {
	uint32_t window_height;		 // SDL Window height
	uint32_t window_width;		 // SDL Window width
	uint32_t fg_color;				 // Foreground color RGBA8888
	uint32_t bg_color;				 // background color RGBA8888
	uint32_t scale_factor;		 // Amount to scale a CHIP8 pixels
	bool pixel_outline;				 // Outinline effect for pixels
	uint32_t insts_per_second; // CHIP8 CPU "clock rate" or hz
	uint32_t square_wave_freq; //  Frequency of square wave sound eg. 440hz for
														 //  middle A
	uint32_t audio_sample_rate;
	int16_t volume; // How loud the sound
} config_t;

typedef enum {
	QUIT,
	RUNNING,
	PAUSED,
} emulator_state_t;

typedef struct {
	uint16_t opcode;
	uint16_t NNN; // 12 bit address/constand
	uint8_t NN;		// 8 bit constant
	uint8_t N;		// 4 bit constant
	uint8_t X;		// 4 bit register identifier
	uint8_t Y;		// 4 bit register identifier
} instruction_t;

typedef struct {
	emulator_state_t state;
	uint8_t ram[4096];
	bool display[64 * 32]; // CHIP8 original resolution
	uint16_t stack[16];		 // Subroutine stack
	uint16_t *stack_ptr;
	uint8_t V[16];				// V0-VF Data registers
	uint16_t I;						// Index register
	uint16_t PC;					// Program Counter
	uint8_t delay_timer;	// Decrease at 60hz per second when > 0
	uint8_t sound_timer;	// Decrease at 60hz per second and play tone when > 0
	bool keypad[16];			// Hexadecimal keypad
	const char *rom_name; // Currently running ROM
	instruction_t inst;		// Currently executing inst
} chip8_t;

void audio_callback(void *userdata, uint8_t *stream, int len) {
	config_t *config = (config_t *)userdata;

	// Fill data with some squre wave
	int16_t *audio_data = (int16_t *)stream;
	static uint32_t running_sample_index = 0;

	const int32_t square_wave_period =
			config->audio_sample_rate / config->square_wave_freq;
	const int32_t half_square_wave_period = square_wave_period / 2;

	for (int i = 0; i < len / 2; i++) {
		audio_data[i] = ((running_sample_index++ / half_square_wave_period) % 2)
												? config->volume
												: -config->volume;
	}
}

bool init_sdl(sdl_t *sdl, config_t config) {
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

	// Initialize audio config
	sdl->want = (SDL_AudioSpec){
			.freq = 44100,					// 44100hz "CD" Quality
			.format = AUDIO_S16LSB, // Signed 16bite little endian
			.channels = 1,					// Mono .samples =
			.samples = 512,
			.callback = audio_callback,
			.userdata = &config, // Userdata passed to the audio callback
	};

	sdl->dev = SDL_OpenAudioDevice(NULL, 0, &sdl->want, &sdl->have, 0);
	if (!(sdl->dev > 0)) {
		SDL_Log("Could not get an audio device %s\n", SDL_GetError());
		return false;
	}

	if ((sdl->want.format != sdl->have.format) ||
			(sdl->want.channels != sdl->have.channels)) {
		SDL_Log("Could not get desired audio spec \n");
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
			.bg_color = 0x000000FF, // black
			.scale_factor = 20,			// Default resolution
			.pixel_outline = true,
			.insts_per_second = 700,		// Number of inst to emulate in one second
			.square_wave_freq = 440,		// 440hz for middle A
			.audio_sample_rate = 44100, // CD Quality
			.volume = 3000,							// INT16_MAX would be max volume
	};

	// Override defaults form passed in arguments
	for (int i = 1; i < argc; i++) {
		(void)argv[i];
	}
	// ...
	return true;
}

bool init_chip8(chip8_t *chip8, const char rom_name[]) {
	const uint32_t entry_point = 0x200; // CHIP8 Roms will be loaded to 0x200
	const uint8_t font[] = {
			0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
			0x20, 0x60, 0x20, 0x20, 0x70, // 1
			0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
			0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
			0x90, 0x90, 0xF0, 0x10, 0x10, // 4
			0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
			0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
			0xF0, 0x10, 0x20, 0x40, 0x40, // 7
			0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
			0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
			0xF0, 0x90, 0xF0, 0x90, 0x90, // A
			0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
			0xF0, 0x80, 0x80, 0x80, 0xF0, // C
			0xE0, 0x90, 0x90, 0x90, 0xE0, // D
			0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
			0xF0, 0x80, 0xF0, 0x80, 0x80, // F
	};

	// Load font
	memcpy(&chip8->ram[0], font, sizeof(font));
	//  Load ROM
	// Open ROM file
	FILE *rom = fopen(rom_name, "rb");
	if (!rom) {
		SDL_Log("Rom file %s is invalid or does not exist \n", rom_name);
		return false;
	}
	// Get/Check rom size
	fseek(rom, 0, SEEK_END);
	const long rom_size = ftell(rom);
	const long max_size = sizeof chip8->ram - entry_point;
	rewind(rom);

	if (rom_size > max_size) {
		SDL_Log("Rom file %s is too big! Rom size: %zu, Max size allowed: %zu\n",
						rom_name, rom_size, max_size);
		return false;
	}

	if (fread(&chip8->ram[entry_point], rom_size, 1, rom) != 1) {
		SDL_Log("Could not read from Rom file %s in to CHIP8 memory \n", rom_name);
		return false;
	}

	fclose(rom);
	//  Set chip8 machine
	chip8->state = RUNNING;	 // Default machine state to RUNNING
	chip8->PC = entry_point; // Start program counter at ROM entry point
	chip8->rom_name = rom_name;
	chip8->stack_ptr = &chip8->stack[0];
	return true;
}

void final_cleanup(const sdl_t sdl) {
	SDL_DestroyRenderer(sdl.renderer);
	SDL_DestroyWindow(sdl.window);
	SDL_CloseAudioDevice(sdl.dev);
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
void update_screen(const sdl_t sdl, const config_t config,
									 const chip8_t chip8) {
	SDL_Rect rect = {
			.x = 0, .y = 0, .w = config.scale_factor, .h = config.scale_factor};
	// Grab color values to draw
	const uint8_t bg_r = (config.bg_color >> 24) & 0xFF;
	const uint8_t bg_g = (config.bg_color >> 16) & 0xFF;
	const uint8_t bg_b = (config.bg_color >> 8) & 0xFF;
	const uint8_t bg_a = (config.bg_color >> 0) & 0xFF;

	const uint8_t fg_r = (config.fg_color >> 24) & 0xFF;
	const uint8_t fg_g = (config.fg_color >> 16) & 0xFF;
	const uint8_t fg_b = (config.fg_color >> 8) & 0xFF;
	const uint8_t fg_a = (config.fg_color >> 0) & 0xFF;
	// Loop through display pixels, draw a rectangle per pixel to the SDL window
	for (uint32_t i = 0; i < sizeof chip8.display; i++) {
		// 1D i value to 2D X/Y coordinates
		// X = i % window width
		// Y = i / window width
		rect.x = (i % config.window_width) * config.scale_factor;
		rect.y = (i / config.window_width) * config.scale_factor;

		if (chip8.display[i]) {
			// if pixel is on, draw foreground color
			SDL_SetRenderDrawColor(sdl.renderer, fg_r, fg_g, fg_b, fg_a);
			SDL_RenderFillRect(sdl.renderer, &rect);

			if (config.pixel_outline) {
				SDL_SetRenderDrawColor(sdl.renderer, bg_r, bg_g, bg_b, bg_a);
				SDL_RenderDrawRect(sdl.renderer, &rect);
			}
		} else {
			// if pixel is off, draw background color
			SDL_SetRenderDrawColor(sdl.renderer, bg_r, bg_g, bg_b, bg_a);
			SDL_RenderFillRect(sdl.renderer, &rect);
		}
	}
	SDL_RenderPresent(sdl.renderer);
}

// User input
// CHIP8 Keypad   QWERTY
// 123C           1234
// 456D           qwer
// 789E           asdf
// A0BF           zxcv
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
			case SDLK_SPACE:
				// space bar
				if (chip8->state == RUNNING) {
					puts("====== PAUSED ======");
					chip8->state = PAUSED;
				} else {
					puts("====== RESUME ======");
					chip8->state = RUNNING;
				}
				return;
			case SDLK_1:
				chip8->keypad[0x1] = true;
				break;
			case SDLK_2:
				chip8->keypad[0x2] = true;
				break;
			case SDLK_3:
				chip8->keypad[0x3] = true;
				break;
			case SDLK_4:
				chip8->keypad[0xC] = true;
				break;

			case SDLK_q:
				chip8->keypad[0x4] = true;
				break;
			case SDLK_w:
				chip8->keypad[0x5] = true;
				break;
			case SDLK_e:
				chip8->keypad[0x6] = true;
				break;
			case SDLK_r:
				chip8->keypad[0xD] = true;
				break;

			case SDLK_a:
				chip8->keypad[0x7] = true;
				break;
			case SDLK_s:
				chip8->keypad[0x8] = true;
				break;
			case SDLK_d:
				chip8->keypad[0x9] = true;
				break;
			case SDLK_f:
				chip8->keypad[0xE] = true;
				break;

			case SDLK_z:
				chip8->keypad[0xA] = true;
				break;
			case SDLK_x:
				chip8->keypad[0x0] = true;
				break;
			case SDLK_c:
				chip8->keypad[0xB] = true;
				break;
			case SDLK_v:
				chip8->keypad[0xF] = true;
				break;
			default:
				break;
			}
			break;
		case SDL_KEYUP:
			switch (event.key.keysym.sym) {

			case SDLK_1:
				chip8->keypad[0x1] = false;
				break;
			case SDLK_2:
				chip8->keypad[0x2] = false;
				break;
			case SDLK_3:
				chip8->keypad[0x3] = false;
				break;
			case SDLK_4:
				chip8->keypad[0xC] = false;
				break;

			case SDLK_q:
				chip8->keypad[0x4] = false;
				break;
			case SDLK_w:
				chip8->keypad[0x5] = false;
				break;
			case SDLK_e:
				chip8->keypad[0x6] = false;
				break;
			case SDLK_r:
				chip8->keypad[0xD] = false;
				break;

			case SDLK_a:
				chip8->keypad[0x7] = false;
				break;
			case SDLK_s:
				chip8->keypad[0x8] = false;
				break;
			case SDLK_d:
				chip8->keypad[0x9] = false;
				break;
			case SDLK_f:
				chip8->keypad[0xE] = false;
				break;

			case SDLK_z:
				chip8->keypad[0xA] = false;
				break;
			case SDLK_x:
				chip8->keypad[0x0] = false;
				break;
			case SDLK_c:
				chip8->keypad[0xB] = false;
				break;
			case SDLK_v:
				chip8->keypad[0xF] = false;
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
	}
}

#ifdef DEBUG
void print_debug_info(chip8_t *chip8) {
	printf("Address: 0x%04X, Opcode: 0x%04X Desc: ", chip8->PC - 2,
				 chip8->inst.opcode);
	switch ((chip8->inst.opcode >> 12) & 0x0F) {
	case 0x00:
		if (chip8->inst.NN == 0xE0) {
			// 0x00E0: clear screen
			printf("Clear screen\n");
		} else if (chip8->inst.NN == 0xEE) {
			// 0x00EE: return from subroutine
			// Grab last address from sub routine stack (pop from stack)
			// set program counter to last address on stack
			printf("Return from subroutine to address 0x%04X\n",
						 *(chip8->stack_ptr - 1));
		} else {
			printf("Unimplemented Opcode.\n");
		}
		break;
	case 0x01:
		// 0x1NNN: Jump to address NNN
		printf(
				"Jump to address NNN (0x%04X)\n",
				chip8->inst.NNN); // Set program counter so that next opcode is from NNN
	case 0x02:
		// 0x2NNN: Call Subroutine at NNN
		printf("Call subroutine at NNN (0x%04X) \n", chip8->inst.NNN);
		break;
	case 0x03:
		// 0x3XNN: Skip to next instruction if Vx == KK
		printf("Increment PC by two if V%X(0x%02X) == NN(0x%02X)\n", chip8->inst.X,
					 chip8->V[chip8->inst.X], chip8->inst.NN);
		break;
	case 0x04:
		// 0x4XNN: Skip to next instruction if Vx != KK
		printf("Increment PC by two if V%X(0x%02X) != NN(0x%02X)\n", chip8->inst.X,
					 chip8->V[chip8->inst.X], chip8->inst.NN);
		break;
	case 0x05:
		// 0x5XY0: Skip to next instruction if Vx == Vy
		printf("Increment PC by two if V%X(0x%02X) == V%X(0x%02X)\n", chip8->inst.X,
					 chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
		break;
	case 0x06:
		// 0x6XNN: Set register VX to NN
		printf("Set register V%X = NN(%02X)\n", chip8->inst.X, chip8->inst.NN);
		break;
	case 0x07:
		// 0x7XNN: Set register VX += NN
		printf("Set register V%X (0x%02X) += NN(%02X), Result 0x%02X\n",
					 chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.NN,
					 chip8->V[chip8->inst.X] + chip8->inst.NN);
		break;
	case 0x08:
		switch (chip8->inst.N) {
		case 0x0:
			// 0x8XY0: Set Vx = Vy
			printf("Set register V%X (0x%02X) = V%X (0x%02X)\n", chip8->inst.X,
						 chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
			break;
		case 0x1:
			// 0x8XY1: Set Vx = Vx OR Vy
			printf("Set register V%X (0x%02X) |= V%X (0x%02X)\n", chip8->inst.X,
						 chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
			break;
		case 0x2:
			// 0x8XY2: Set Vx = Vx AND Vy
			printf("Set register V%X (0x%02X) &= V%X (0x%02X)\n", chip8->inst.X,
						 chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
			break;
		case 0x3:
			// 0x8XY3: Set Vx = Vx XOR Vy
			printf("Set register V%X (0x%02X) ^= V%X (0x%02X)\n", chip8->inst.X,
						 chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
			break;
		case 0x4:
			// 0x8XY4: Add VX + VY, set VF = carry
			printf("Set V%X (0x%02X) += V%X (0x%02X), ie 0x%X VF is set if there is "
						 "overflow  \n",
						 chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.Y,
						 chip8->V[chip8->inst.Y],
						 (uint16_t)(chip8->V[chip8->inst.X] + chip8->V[chip8->inst.Y]));
			break;
		case 0x5:
			// 0x8XY5: Set VX = VX - VY, Set VF = NOT borrow
			printf(
					"Set V%X (0x%02X) -= V%X (0x%02X), ie 0x%X VF is set if VX > VY  \n",
					chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.Y,
					chip8->V[chip8->inst.Y],
					chip8->V[chip8->inst.X] - chip8->V[chip8->inst.Y]);
			break;
		case 0x6:
			// 0x8XY6: Set VX = VX SHR 1
			// If the least significant bit of Vx is 1, then VF is set 1, otherwise 0,
			// Vx is divided 2
			printf("if lsb of V%X (0x%X) == 1, VF is set 1, V%X /= 2\n",
						 chip8->inst.X, chip8->V[chip8->inst.X] & 0x1,
						 chip8->V[chip8->inst.X]);
			break;
		case 0x7:
			// 0x8XY7: Set VX = VY - VX, Set VF = NOT borrow
			printf(
					"Set V%X (0x%02X) -= V%X (0x%02X), ie 0x%X VF is set if VX < VY  \n",
					chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.Y,
					chip8->V[chip8->inst.Y],
					chip8->V[chip8->inst.X] - chip8->V[chip8->inst.Y]);
			break;
		case 0xe:
			// 0x8XY6: Set VX = VX SHL 1
			// If the most significant bit of Vx is 1, then VF is set to 1, otherwise
			// 0. Then Vx is multiplied by 2
			printf("if lsb of V%X (0x%X) == 1, VF is set 1, V%X /= 2\n",
						 chip8->inst.X, chip8->V[chip8->inst.X] >> 7 & 0x1,
						 chip8->V[chip8->inst.X]);
			break;
		default:
			printf("Unimplemented Opcode\n");
			break;
		}
		break;
	case 0x09:
		// 0x9XY0: Skip to next instruction if Vx != Vy
		printf("Increment PC by two if V%X(0x%02X) != V%X(0x%02X)\n", chip8->inst.X,
					 chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
		break;
	case 0x0A:
		// 0xANNN: Set index register I to NNN
		printf("Set I to NNN (0x%04X)\n", chip8->inst.NNN);
		break;
	case 0x0B:
		// 0xBNNN: Jump to location nnn + V0 (PC = V0 + NNN)
		printf("Set PC to V0 (0x%02X) + NNN (0x%04X) = 0x%04X\n", chip8->V[0],
					 chip8->inst.NNN, chip8->V[0] + chip8->inst.NNN);
		break;
	case 0x0C:
		// 0xCXNN: Sets register VX = rand() % 256 & NN (bitwise AND)
		printf("Set V%X = rand() %% 256 & NN (0x%02X)\n", chip8->inst.X,
					 chip8->inst.NN);
		break;
	case 0x0D:
		// 0xDXYN: Draw N-height sprite at coords X,Y; Read from location I;
		printf("Draw N (%u) height sprite at coords V%X (0x%02X), V%X (0x%02X) "
					 "from memory location I (0x%04X). Set VF = 1 if any pixels are "
					 "turned off.\n",
					 chip8->inst.N, chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.Y,
					 chip8->V[chip8->inst.Y], chip8->I);
		break;
	case 0x0E:
		if (chip8->inst.NN == 0x9E) {
			// 0xEX9E: Skip next instruction if the key with the value of VX is
			// pressed
			printf("Skip next instruction if the key in V%X (0x%02X) is pressed; ",
						 chip8->inst.X, chip8->V[chip8->inst.X]);
		} else if (chip8->inst.NN == 0xA1) {
			// 0xEXA1: Skip next instruction if the key with the value of VX is not
			// pressed
			printf(
					"Skip next instruction if the key in V%X (0x%02X) is not pressed; ",
					chip8->inst.X, chip8->V[chip8->inst.X]);
		}
		break;
	case 0x0F:
		switch (chip8->inst.NN) {
		case 0x0A:
			// 0xFX0A: Wait for a key press and store the value of the key in Vx.
			// All execution stops until a key is pressed, then the value of the key
			// is stored in Vx.
			printf("Waiting for key press; Store key in V%X\n", chip8->inst.X);
			break;
		case 0x1E:
			// 0xFX1Ea: Set I = I + Vx;
			printf("I(0x%04X) +=V%X (0x%02X)\n", chip8->I, chip8->inst.X,
						 chip8->V[chip8->inst.X]);
			break;
		case 0x15:
			// 0xFX15: Set delay time = Vx
			printf("delay_timer(0x%02X) = V%X(0x%02X)\n", chip8->delay_timer,
						 chip8->inst.X, chip8->V[chip8->inst.X]);
			break;
		case 0x07:
			// 0xFX07: Set Vx = delay timer value.
			printf("V%X (0x%02X)= 0x%02X\n", chip8->inst.X, chip8->V[chip8->inst.X],
						 chip8->delay_timer);
			break;
		case 0x18:
			// 0xFX18: Set sound timer = Vx
			printf("sound_timer(0x%02X) = V%X(0x%02X)\n", chip8->sound_timer,
						 chip8->inst.X, chip8->V[chip8->inst.X]);
			break;
		case 0x29:
			// 0xFX29: Set I = location of sprite for digit Vx
			printf("Set I to location of sprite for digit V%X(%02X), ie %02X \n",
						 chip8->inst.X, chip8->V[chip8->inst.X],
						 chip8->V[chip8->inst.X] * 5);
			break;
		case 0x33:
			printf("Store BCD representation of V%X (0x%02X) at memory from I "
						 "(0x%04X)\n",
						 chip8->inst.X, chip8->V[chip8->inst.X], chip8->I);
			break;
		case 0x55:
			// 0xFX55: Store registers V0 through VX in memory starting at location I
			// The interpreter copies the values of registers V0 through VX into
			// memory, starting at the address I
			printf("Store registers V0 through V%X in memory starting at location "
						 "0x%04X",
						 chip8->inst.X, chip8->I);
			break;
		case 0x65:
			// 0xFX65: Read registers V0 through VX from memory starting at locaation
			// I The interpreter reads values from memory starting at location I into
			// registers V0 through VX
			printf("Read registers V0 through V%X from memory starting at location "
						 "0x%04X",
						 chip8->inst.X, chip8->I);
			break;
		default:
			printf("Unimplemented Opcode\n");
			break;
		}
		break;
	default:
		printf("Unimplemented Opcode.\n");
		break; // Unimplemented or invalid opcode
	}
}
#endif

// Emulate 1 CHIP8 instruction
void emulate_instruction(chip8_t *chip8, const config_t config) {
	// Get next opcode from ram
	chip8->inst.opcode = chip8->ram[chip8->PC] << 8 | chip8->ram[chip8->PC + 1];
	chip8->PC += 2; // Pre increment pc for next opcode

	// Fill out current instruction format
	// DXYN
	chip8->inst.NNN = chip8->inst.opcode & 0x0FFF;
	chip8->inst.NN = chip8->inst.opcode & 0x0FF;
	chip8->inst.N = chip8->inst.opcode & 0x0F;
	chip8->inst.X = (chip8->inst.opcode >> 8) & 0x0F;
	chip8->inst.Y = (chip8->inst.opcode >> 4) & 0x0F;

#ifdef DEBUG
	print_debug_info(chip8);
#endif

	uint8_t X_coord;
	uint8_t Y_coord;
	// Emulate opcode
	switch ((chip8->inst.opcode >> 12) & 0x0F) {
	case 0x00:
		if (chip8->inst.NN == 0xE0) {
			// 0x00E0: clear screen
			memset(&chip8->display[0], false, sizeof chip8->display);
		} else if (chip8->inst.NN == 0xEE) {
			// 0x00EE: return from subroutine
			// Grab last address from sub routine stack (pop from stack)
			// set program counter to last address on stack
			chip8->PC = *--chip8->stack_ptr;
		} else {
			// Unimplemented/invalid opcode, may be 0xNNN for calling machine code
		}
		break;
	case 0x01:
		// 0x1NNN: Jump to address NNN
		chip8->PC =
				chip8->inst.NNN; // Set program counter so that next opcode is from NNN
		break;
	case 0x02:
		// 0x2NNN: Call Subroutine at NNN
		// subroutine stack (push to the stack)
		*chip8->stack_ptr++ = chip8->PC; // Store current address to return to the
		chip8->PC = chip8->inst.NNN; // Store the subroutine address to the PC so it
																 // will get executed next
		break;
	case 0x03:
		// 0x3XNN: Skip to next instruction if Vx == NN
		if (chip8->V[chip8->inst.X] == chip8->inst.NN)
			chip8->PC += 2;
		break;
	case 0x04:
		// 0x4XNN: Skip to next instruction if Vx != KK
		if (chip8->V[chip8->inst.X] != chip8->inst.NN)
			chip8->PC += 2;
		break;
	case 0x05:
		// 0x5XY0: Skip to next instruction if Vx == Vy
		if (chip8->inst.N != 0)
			break; // Wrong Opcode
		if (chip8->V[chip8->inst.X] == chip8->V[chip8->inst.Y])
			chip8->PC += 2;
		break;
	case 0x06:
		// 0x6XNN: Set register VX to NN
		chip8->V[chip8->inst.X] = chip8->inst.NN;
		break;
	case 0x07:
		// 0x7XNN: Set register VX += NN
		chip8->V[chip8->inst.X] += chip8->inst.NN;
		break;
	case 0x08:
		switch (chip8->inst.N) {
		case 0x0:
			// 0x8XY0: Set Vx = Vy
			chip8->V[chip8->inst.X] = chip8->V[chip8->inst.Y];
			break;
		case 0x1:
			// 0x8XY1: Set Vx = Vx OR Vy
			chip8->V[chip8->inst.X] |= chip8->V[chip8->inst.Y];
			break;
		case 0x2:
			// 0x8XY2: Set Vx = Vx AND Vy
			chip8->V[chip8->inst.X] &= chip8->V[chip8->inst.Y];
			break;
		case 0x3:
			// 0x8XY3: Set Vx = Vx XOR Vy
			chip8->V[chip8->inst.X] ^= chip8->V[chip8->inst.Y];
			break;
		case 0x4:
			// 0x8XY4: Add VX + VY, set VF = carry
			if ((uint16_t)(chip8->V[chip8->inst.X] + chip8->V[chip8->inst.Y]) > 255)
				chip8->V[0xF] = 1;
			chip8->V[chip8->inst.X] += chip8->V[chip8->inst.Y];
			break;
		case 0x5:
			// 0x8XY5: Set VX = VX - VY, Set VF = NOT borrow
			chip8->V[0xF] = chip8->V[chip8->inst.X] >= chip8->V[chip8->inst.Y];
			chip8->V[chip8->inst.X] -= chip8->V[chip8->inst.Y];
			break;
		case 0x6:
			// 0x8XY6: Set VX = VX SHR 1
			// If the least significant bit of Vx is 1, then VF is set 1, otherwise 0,
			// Vx is divided 2
			chip8->V[0xF] = chip8->V[chip8->inst.X] & 1;
			chip8->V[chip8->inst.X] >>= 1;
			break;
		case 0x7:
			// 0x8XY7: Set VX = VY - VX, Set VF = NOT borrow
			chip8->V[0xF] = chip8->V[chip8->inst.X] <= chip8->V[chip8->inst.Y];
			chip8->V[chip8->inst.X] =
					chip8->V[chip8->inst.Y] - chip8->V[chip8->inst.X];
			break;
		case 0xe:
			// 0x8XY6: Set VX = VX SHL 1
			// If the most significant bit of Vx is 1, then VF is set to 1, otherwise
			// 0. Then Vx is multiplied by 2
			chip8->V[0xF] = chip8->V[chip8->inst.X] >> 7 & 1;
			chip8->V[chip8->inst.X] <<= 1;
			break;
		default:
			// Wrong opcode
			break;
		}
		break;
	case 0x09:
		// 0x9XY0: Skip to next instruction if Vx != Vy
		if (chip8->V[chip8->inst.X] != chip8->V[chip8->inst.Y])
			chip8->PC += 2;
		break;
	case 0x0A:
		// 0xANNN: Set index register I to NNN
		chip8->I = chip8->inst.NNN;
		break;
	case 0x0B:
		// 0xBNNN: Jump to location nnn + V0 (PC = V0 + NNN)
		chip8->PC = chip8->V[0] + chip8->inst.NNN;
		break;
	case 0x0C:
		// 0xCXNN: Sets register VX = rand() % 256 & NN (bitwise AND)
		chip8->V[chip8->inst.X] = (rand() % 256) & chip8->inst.NN;
		break;
	case 0x0D:
		// 0xDXYN: Draw N-height sprite at coords X,Y; Read from location I;
		// Screen pixels are XOR'd with sprite bits,
		// VF (Carry flag) is set if any screen pixels are set off; This is useful
		// for collision detection or other reasons.
		X_coord = chip8->V[chip8->inst.X] % config.window_width;
		Y_coord = chip8->V[chip8->inst.Y] % config.window_height;

		const uint8_t og_X = X_coord;

		chip8->V[0xF] = 0; // Init carry flag to 0

		for (uint8_t i = 0; i < chip8->inst.N; i++) {
			// Get next byte/row of sprite data
			const uint8_t sprite_data = chip8->ram[chip8->I + i];
			X_coord = og_X; // Reset X for next row to draw

			for (int8_t j = 7; j >= 0; j--) {
				// If sprite pixel/bit is on and display pixel is on, set carry flag
				bool *pixel = &chip8->display[Y_coord * config.window_width + X_coord];
				const bool sprite_bit = (sprite_data & (1 << j));

				if (sprite_bit && *pixel) {
					chip8->V[0xF] = 1;
				}

				// XOR display pixel with sprite pixel/bit
				*pixel ^= sprite_bit;

				// Stop drawing if hit right edge of screen
				if (++X_coord >= config.window_width)
					break;
			}
			// Stop drawing entire sprite if hit bottom edge of screen
			if (++Y_coord >= config.window_height)
				break;
		}
		break;
	case 0x0E:
		if (chip8->inst.NN == 0x9E) {
			// 0xEX9E: Skip next instruction if the key with the value of VX is
			// pressed
			if (chip8->keypad[chip8->V[chip8->inst.X]])
				chip8->PC += 2;
		} else if (chip8->inst.NN == 0xA1) {
			// 0xEXA1: Skip next instruction if the key with the value of VX is not
			// pressed
			if (!chip8->keypad[chip8->V[chip8->inst.X]])
				chip8->PC += 2;
		}
		break;
	case 0x0F:
		switch (chip8->inst.NN) {
		case 0x0A: {
			// 0xFX0A: Wait for a key press and store the value of the key in Vx.
			// All execution stops until a key is pressed, then the value of the key
			// is stored in Vx.
			bool key_pressed = false;
			for (uint8_t i = 0; i < sizeof chip8->keypad; i++)
				if (chip8->keypad[i]) {
					chip8->V[chip8->inst.X] = i;
					key_pressed = true;
					break;
				}
			// If no key has been pressed yet, keep getting the current opcode &
			// running this instruction
			if (!key_pressed)
				chip8->PC -= 2;
		}
		case 0x1E:
			// 0xFX1E: Set I = I + Vx;
			chip8->I += chip8->V[chip8->inst.X];
			break;
		case 0x15:
			// 0xFX15: Set delay time = Vx
			chip8->delay_timer = chip8->V[chip8->inst.X];
			break;
		case 0x07:
			// 0xFX07: Set Vx = delay timer value.
			chip8->V[chip8->inst.X] = chip8->delay_timer;
			break;
		case 0x18:
			// 0xFX18: Set sound timer = Vx
			chip8->sound_timer = chip8->V[chip8->inst.X];
			break;
		case 0x29:
			// 0xFX29: Set I = location of sprite for digit Vx
			chip8->I = chip8->V[chip8->inst.X] * 5;
			break;
		case 0x33: {
			// 0xFX33: Store BCD representation of VX in memory location I, I+1, I+2
			// I = hundred's place, I+1 = tent's palce, I+2 = one's place
			uint8_t bcd = chip8->V[chip8->inst.X]; // 123
			chip8->ram[chip8->I + 2] = bcd % 10;
			bcd /= 10;
			chip8->ram[chip8->I + 1] = bcd % 10;
			bcd /= 10;
			chip8->ram[chip8->I] = bcd;
			break;
		}
		case 0x55:
			// 0xFX55: Store registers V0 through VX in memory starting at location I
			// The interpreter copies the values of registers V0 through VX into
			// memory, starting at the address I
			for (uint8_t i = 0; i <= chip8->inst.X; i++)
				chip8->ram[chip8->I + i] = chip8->V[i];
			break;
		case 0x65:
			// 0xFX65: Read registers V0 through VX from memory starting at locaation
			// I The interpreter reads values from memory starting at location I into
			// registers V0 through VX
			for (uint8_t i = 0; i <= chip8->inst.X; i++)
				chip8->V[i] = chip8->ram[chip8->I + i];
			break;
		}
		break;
	default:
		break; // Unimplemented or invalid opcode
	}
}

void update_timers(const sdl_t sdl, chip8_t *chip8) {
	if (chip8->delay_timer > 0)
		chip8->delay_timer--;
	if (chip8->sound_timer > 0) {
		chip8->sound_timer--;
		// Play sound
		SDL_PauseAudioDevice(sdl.dev, 0);
	} else {
		// Stop playing sound
		SDL_PauseAudioDevice(sdl.dev, 1);
	}
}

int main(int argc, char **argv) {
	// Default usage message for args
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <rom_name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

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
	const char *rom_name = argv[1];
	if (!init_chip8(&chip8, rom_name))
		exit(EXIT_FAILURE);

	// Initial screen clear
	clear_screen(config, sdl);

	// Seed the random number generator
	srand(time(NULL));

	// main emulator loop
	while (chip8.state != QUIT) {
		// Handle user input
		handle_input(&chip8);
		if (chip8.state == PAUSED)
			continue;

		// Get_time(); before running instruction
		const uint64_t start_frame_time = SDL_GetPerformanceCounter();

		// emulate CHIP8 Instructions for this emulator frame (60hz)
		for (uint32_t i = 0; i < config.insts_per_second / 60; i++)
			emulate_instruction(&chip8, config);

		// Get_time() elapsed since last get_time(); elapsed time after instruction
		const uint64_t end_frame_time = SDL_GetPerformanceCounter();

		const double time_elapsed =
				(double)((end_frame_time - start_frame_time) * 1000) /
				SDL_GetPerformanceFrequency();
		// delay for approximately 60hz/60fps (16.67ms)
		SDL_Delay(16.67f > time_elapsed ? 16.67f - time_elapsed : 0);
		// update window with changes
		update_screen(sdl, config, chip8);
		// update delay and sound timers (60hz)
		update_timers(sdl, &chip8);
	}

	// Final cleanup
	final_cleanup(sdl);

	exit(EXIT_SUCCESS);
}
