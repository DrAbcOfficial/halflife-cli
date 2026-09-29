#pragma once

#include <cctype>
#include <string>
#include <vector>

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

	// Splits a comma-separated list, trimming spaces/tabs around items and
	// skipping empty items (so "" yields an empty list — callers rely on
	// that: an empty whitelist must stay empty, not "match everything").
	inline std::vector<std::string> SplitCsv(const std::string& csv)
	{
		std::vector<std::string> out;
		size_t pos = 0;
		while (pos <= csv.size())
		{
			size_t comma = csv.find(',', pos);
			if (comma == std::string::npos)
				comma = csv.size();
			std::string item = csv.substr(pos, comma - pos);
			while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
			while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
			if (!item.empty())
				out.push_back(std::move(item));
			pos = comma + 1;
		}
		return out;
	}
}
