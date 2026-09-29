#pragma once

#include <cctype>
#include <string>

// Small string helpers shared across modules.
namespace text
{
	// ASCII-lowercase copy. cvar/command and usermsg names are ASCII, so a
	// locale-independent tolower on each byte is the right normalization.
	inline std::string Lowercase(const char* s)
	{
		std::string out(s ? s : "");
		for (size_t i = 0; i < out.size(); ++i)
			out[i] = (char)tolower((unsigned char)out[i]);
		return out;
	}

	inline std::string Lowercase(const std::string& s)
	{
		return Lowercase(s.c_str());
	}
}
