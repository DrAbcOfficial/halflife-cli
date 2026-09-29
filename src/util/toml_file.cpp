#include "util/toml_file.h"

#include "core/plugins.h"

#include <metahook.h>

namespace toml_file
{
	bool Read(const std::string& path, toml::table& out, std::string& err)
	{
		FileHandle_t fp = FILESYSTEM_ANY_OPEN(path.c_str(), "rb");
		if (!fp)
		{
			err = "file not found";
			return false;
		}

		int size = FILESYSTEM_ANY_SIZE(fp);
		std::string text;
		if (size > 0 && size < (1 << 20))
		{
			text.resize(size);
			FILESYSTEM_ANY_READ(&text[0], size, fp);
		}
		FILESYSTEM_ANY_CLOSE(fp);

		// Windows editors commonly leave a UTF-8 BOM; the TOML spec forbids it.
		if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
			text.erase(0, 3);

		try
		{
			out = toml::parse(text);
		}
		catch (const toml::parse_error& e)
		{
			err = "parse error on line " + std::to_string((unsigned)e.source().begin.line) + ": " + std::string(e.description());
			return false;
		}
		return true;
	}
}
