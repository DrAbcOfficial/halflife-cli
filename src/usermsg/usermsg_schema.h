#pragma once

#include <map>
#include <string>
#include <vector>

// One field descriptor of a UserMsg wire format, loaded from the TOML schema.
//
//	field = [
//	  { name = "state", type = "byte" },
//	  { name = "rgb", type = "byte", count = 3, when = { field = "enable", ne = 0 } },
//	  { name = "items", type = "group", count = "numItems", fields = [ ... ] },
//	]
struct UserMsgField
{
	std::string name;          // display name; empty -> auto "arg<n>"
	std::string type;          // byte char short word long float coord angle angle16 string vec3 group
	int count = 1;             // fixed repeat count
	bool countFromField = false;
	std::string countField;    // name of a previously-read numeric field holding the count
	bool countRest = false;    // "*" -> repeat while the buffer still holds one item
	bool hasWhen = false;
	std::string whenField;
	bool whenNotEqual = false; // false: field == whenValue, true: field != whenValue
	double whenValue = 0.0;
	std::vector<UserMsgField> children;  // type == "group"
	std::string note;
};

struct UserMsgDef
{
	std::string name;
	std::string channel;       // event-ring channel (functional group); see UserMsgSchema::Load
	std::vector<UserMsgField> fields;
	bool raw = false;          // print size + hex dump instead of parsing fields
	std::string note;
};

struct UserMsgSchema
{
	// Channel a message lands in when its definition (and any inherited base)
	// does not name one.
	static constexpr const char* kDefaultChannel = "usermsg";

	// Missing: the root schema file does not exist (a missing extends base
	// counts as Error — the mod schema is there, the install is broken).
	enum class LoadResult { Loaded, Missing, Error };

	int coord_size = 2;        // bytes of "coord": 2 = short*1/8 (hl, cstrike), 4 = long*1/8 (svencoop)
	std::vector<UserMsgDef> messages;
	std::map<std::string, size_t> index;  // lowercased name -> position in messages

	// Loads "<mod>/metahook/configs/halflifecli/usermsgs/<file>", following
	// "extends" chains (a child message replaces the same-name base
	// message). Every message carries the functional group named by its
	// optional "channel" key; an override without one inherits the base's,
	// and whatever is left empty falls back to kDefaultChannel.
	// Details of failures are printed through Con_Printf.
	LoadResult Load(const std::string& file);
};
