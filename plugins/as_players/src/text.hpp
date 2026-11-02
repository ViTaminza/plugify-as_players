// as_players — the chat text helpers of the plugin.
//
// The admin_system phrase file is written for the Pisex/cs2-menus renderer: a phrase such as
//
//     "AdminKillMessage" { "en" "Admin {LIGHTRED}%s{DEFAULT} has killed player ..." }
//
// carries {TAG} colour words that the Metamod plugin replaced with the chat control bytes of
// cs2-menus' colors_text[] table before it sent the line. s2sdk's PrintToChat sends the message
// as it is, so the replacement has to happen here, with exactly the same bytes, otherwise the
// port's chat output would change (the tags would be shown literally).
//
// The table is copied from libs/pisexcompat/src/phrases.cpp (itself copied from cs2-menus'
// utils.h) instead of linking pisexcompat: this plugin is self-contained (PLUGIN_STANDARD §7).
// The last entry is the chat line break: the phrase files spell it "\n" (backslash + n) and a
// chat line wants U+2029, the engine's line separator.
//
// StripColorTags() is the server-console variant: the console has no colour codes, so the tags
// are dropped and "\n" becomes a real newline (pisexcompat did the same for its META_CONPRINT
// replacement).
//
// Header-only and engine-free, so the tests can check the exact bytes without an engine.

#pragma once

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace as_players {

namespace detail {

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

// cs2-menus' colors_text[]/colors_hex[], same order and same bytes.
inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

} // namespace detail

// The phrase text with the colour tags turned into their chat control bytes.
inline std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const detail::ColorTag& color : detail::kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

// The phrase text for a plain-text destination (the server console): drop the colour words and
// turn the chat line break into a real newline.
inline std::string StripColorTags(std::string_view text) {
	std::string out(text);
	for (const detail::ColorTag& color : detail::kColorTags) {
		const std::string_view replacement =
			color.tag == "\\n" ? std::string_view("\n") : std::string_view();
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

// printf into a bounded buffer, like the format step of the Pisex printers (2048 bytes, never
// cut inside the engine). FormatV() is the va_list form the plugin's phrase printers forward to.
inline std::string FormatV(const char* format, va_list args) {
	constexpr std::size_t kBufferSize = 2048;
	std::array<char, kBufferSize> buffer{};
	const int written = std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	if (written <= 0) {
		return {};
	}
	const std::size_t size = static_cast<std::size_t>(written) < buffer.size() - 1
								 ? static_cast<std::size_t>(written)
								 : buffer.size() - 1;
	return std::string(buffer.data(), size);
}

inline std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline std::string Format(const char* format, ...) {
	va_list args;
	va_start(args, format);
	std::string out = FormatV(format, args);
	va_end(args);
	return out;
}

} // namespace as_players
