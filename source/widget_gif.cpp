#include "deck_state.hpp"
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <string_view>

namespace {
unsigned byteAt(std::string_view bytes, size_t offset) {
    return static_cast<unsigned char>(bytes[offset]);
}

int wordAt(std::string_view bytes, size_t offset) {
    return byteAt(bytes, offset) | (byteAt(bytes, offset + 1) << 8);
}

size_t paletteBytes(unsigned flags) {
    return (flags & 0x80) ? 3u << ((flags & 7) + 1) : 0;
}

bool skipSubBlocks(std::string_view bytes, size_t& offset) {
    while (offset < bytes.size()) {
        const size_t count = byteAt(bytes, offset++);
        if (count == 0) return true;
        if (count > bytes.size() - offset) return false;
        offset += count;
    }
    return false;
}

using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
}

bool DeckState::cacheAnimatedGif(const std::string& id, const std::string& bytes, SDL_Renderer* renderer) {
    const std::string_view gif(bytes);
    if (gif.size() < 14 || (gif.substr(0, 6) != "GIF89a" && gif.substr(0, 6) != "GIF87a")) return false;
    const int width = wordAt(gif, 6);
    const int height = wordAt(gif, 8);
    if (width < 1 || height < 1 || width > 512 || height > 512) return false;
    const size_t frameBytes = static_cast<size_t>(width) * height * 4;
    const size_t headerEnd = 13 + paletteBytes(byteAt(gif, 10));
    if (headerEnd >= gif.size()) return false;

    Surface canvas(nullptr, SDL_FreeSurface);
    Surface previous(nullptr, SDL_FreeSurface);
    std::vector<ArtworkFrame> frames;
    uint32_t loops = 1;
    Uint64 endsAt = 0;
    size_t control = 0;
    size_t cursor = headerEnd;
    Uint32 background = 0;
    bool finished = false;
    while (cursor < gif.size()) {
        const size_t start = cursor;
        const unsigned marker = byteAt(gif, cursor++);
        if (marker == 0x3b) {
            finished = true;
            break;
        }
        if (marker == 0x21) {
            if (cursor == gif.size()) return false;
            const auto label = byteAt(gif, cursor++);
            if (!skipSubBlocks(gif, cursor)) return false;
            if (label == 0xf9 && cursor - start == 8 && byteAt(gif, start + 2) == 4) control = start;
            if (label == 0xff && cursor - start >= 19 &&
                (gif.substr(start + 3, 11) == "NETSCAPE2.0" || gif.substr(start + 3, 11) == "ANIMEXTS1.0") &&
                byteAt(gif, start + 14) == 3 && byteAt(gif, start + 15) == 1) {
                const auto count = wordAt(gif, start + 16);
                loops = count == 0 ? 0 : count + 1;
            }
            continue;
        }
        if (marker != 0x2c || gif.size() - start < 11) return false;
        SDL_Rect frameRect{wordAt(gif, start + 1), wordAt(gif, start + 3),
                           wordAt(gif, start + 5), wordAt(gif, start + 7)};
        if (frameRect.w < 1 || frameRect.h < 1 || frameRect.x + frameRect.w > width ||
            frameRect.y + frameRect.h > height) return false;
        cursor = start + 10 + paletteBytes(byteAt(gif, start + 9));
        if (cursor >= gif.size()) return false;
        ++cursor; // LZW code size
        if (!skipSubBlocks(gif, cursor)) return false;
        if (frames.size() >= 90 || frameBytes * (frames.size() + 1) > 3 * 1024 * 1024) return false;

        const unsigned controlFlags = control ? byteAt(gif, control + 3) : 0;
        const bool transparent = (controlFlags & 1) != 0;
        const int disposal = (controlFlags >> 2) & 7;
        if (!canvas) {
            canvas.reset(SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32));
            if (!canvas) return false;
            SDL_SetSurfaceBlendMode(canvas.get(), SDL_BLENDMODE_NONE);
            const auto color = 13 + byteAt(gif, 11) * 3;
            if (!transparent && color + 2 < headerEnd) {
                background = SDL_MapRGBA(canvas->format, byteAt(gif, color),
                                         byteAt(gif, color + 1), byteAt(gif, color + 2), 255);
            }
            SDL_FillRect(canvas.get(), nullptr, background);
        }
        if (disposal == 3) {
            if (!previous) previous.reset(SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32));
            if (!previous) return false;
            SDL_SetSurfaceBlendMode(previous.get(), SDL_BLENDMODE_NONE);
            if (SDL_BlitSurface(canvas.get(), nullptr, previous.get(), nullptr) != 0) return false;
        }

        // Let SDL_image decode each image's palette, transparency, interlacing and LZW data.
        std::string image(gif.substr(0, headerEnd));
        for (int i = 0; i < 2; ++i) {
            image[6 + i] = static_cast<char>((frameRect.w >> (8 * i)) & 255);
            image[8 + i] = static_cast<char>((frameRect.h >> (8 * i)) & 255);
        }
        if (control) image.append(gif.substr(control, 8));
        const auto descriptor = image.size();
        image.append(gif.substr(start, cursor - start));
        std::fill_n(image.begin() + descriptor + 1, 4, '\0');
        image.push_back(';');
        auto* stream = SDL_RWFromConstMem(image.data(), static_cast<int>(image.size()));
        if (!stream) return false;
        Surface surface(IMG_LoadGIF_RW(stream), SDL_FreeSurface);
        SDL_RWclose(stream);
        if (!surface || surface->w != frameRect.w || surface->h != frameRect.h) return false;
        SDL_Rect dest = frameRect;
        if (SDL_BlitSurface(surface.get(), nullptr, canvas.get(), &dest) != 0) return false;

        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
            SDL_CreateTextureFromSurface(renderer, canvas.get()), SDL_DestroyTexture);
        if (!texture) return false;
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        const int delay = control ? wordAt(gif, control + 4) * 10 : 0;
        endsAt += delay <= 10 ? 100 : delay;
        frames.push_back({std::move(texture), endsAt});
        if (disposal == 2) SDL_FillRect(canvas.get(), &frameRect, transparent ? 0 : background);
        if (disposal == 3 && SDL_BlitSurface(previous.get(), nullptr, canvas.get(), nullptr) != 0) return false;
        control = 0;
    }
    if (!finished || frames.size() < 2) return false;
    auto* first = frames.front().texture.get();
    return cacheResource(id, first, width, height, 0, std::move(frames), loops);
}
