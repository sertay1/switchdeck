#pragma once
#include <cstdint>
using Uint64 = uint64_t;
using Uint32 = uint32_t;
using Uint8 = uint8_t;
struct SDL_Texture {};
struct SDL_Renderer {};
struct SDL_Rect { int x,y,w,h; };
struct SDL_Point { int x,y; };
struct SDL_Color { Uint8 r,g,b,a; };
void SDL_DestroyTexture(SDL_Texture*);
