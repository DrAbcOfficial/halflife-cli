#pragma once

#include <string>
#include <toml++/toml.hpp>

// TOML loading through the engine filesystem (paths are mod-relative), shared
// by the plugin config and the UserMsg schema loader.
namespace toml_file
{
	// Reads and parses `path`. Returns false and fills `err` ("file not found"
	// or "parse error on line N: ...") on failure. A UTF-8 BOM, which the TOML
	// spec forbids but Windows editors commonly leave, is stripped first.
	bool Read(const std::string& path, toml::table& out, std::string& err);
}
