#include "usermsg/usermsg_decoder.h"

#include <cstdio>
#include <vector>

namespace
{
	const size_t kMaxItemsShown = 16;

	void AppendHex(std::string& out, const unsigned char* p, size_t n)
	{
		char b[8];
		for (size_t i = 0; i < n; ++i)
		{
			_snprintf_s(b, sizeof(b), _TRUNCATE, " %02X", p[i]);
			out += b;
		}
	}

	void AppendNumber(std::string& out, double v, bool integral)
	{
		char b[32];
		if (integral)
			_snprintf_s(b, sizeof(b), _TRUNCATE, "%lld", (long long)v);
		else
			_snprintf_s(b, sizeof(b), _TRUNCATE, "%g", v);
		out += b;
	}

	struct Reader
	{
		const unsigned char* data = nullptr;
		size_t size = 0;
		size_t pos = 0;
		int coordSize = 2;
		bool ok = true;

		const void* Take(size_t n)
		{
			if (!ok || pos + n > size)
			{
				ok = false;
				return nullptr;
			}
			const void* p = data + pos;
			pos += n;
			return p;
		}
	};

	// Minimum bytes one item of this field consumes; bounds "*" repetition
	// and count-from-field loops.
	size_t FieldMinSize(const UserMsgField& f, int coordSize)
	{
		size_t base;
		if (f.type == "byte" || f.type == "char" || f.type == "angle")
			base = 1;
		else if (f.type == "coord")
			base = (size_t)coordSize;
		else if (f.type == "short" || f.type == "word" || f.type == "angle16")
			base = 2;
		else if (f.type == "long" || f.type == "float")
			base = 4;
		else if (f.type == "string")
			base = 1;
		else if (f.type == "vec3")
			base = 3 * (size_t)coordSize;
		else if (f.type == "group")
		{
			base = 0;
			for (const UserMsgField& c : f.children)
				if (!c.hasWhen)
					base += FieldMinSize(c, coordSize);
		}
		else
			base = 0;
		return base * (f.countRest ? 1 : (size_t)f.count);
	}

	struct Scope
	{
		std::string name;
		double value;
	};

	// Ordered (name, numeric value) pairs for when/count lookups; searched
	// newest-first, so inner scopes shadow outer ones.
	const double* LookupValue(const std::vector<Scope>& scopes, const std::string& name)
	{
		for (size_t i = scopes.size(); i-- > 0;)
		{
			if (scopes[i].name == name)
				return &scopes[i].value;
		}
		return nullptr;
	}

	// Reads one item of the field and appends its rendering; array fields
	// call this repeatedly and wrap the items in brackets.
	bool ReadOne(const UserMsgField& f, Reader& r, std::vector<Scope>& scopes, std::string& out, size_t maxString)
	{
		char b[64];
		auto push = [&](double v)
		{
			if (!f.name.empty())
				scopes.push_back({ f.name, v });
		};

		if (f.type == "byte" || f.type == "char")
		{
			const void* p = r.Take(1);
			if (!p) return false;
			int v = f.type == "byte" ? *(const unsigned char*)p : *(const char*)p;
			push((double)v);
			AppendNumber(out, (double)v, true);
		}
		else if (f.type == "short" || f.type == "word" || f.type == "angle16")
		{
			const void* p = r.Take(2);
			if (!p) return false;
			unsigned short u = *(const unsigned short*)p;
			push(f.type == "short" ? (double)*(const short*)p : (double)u);
			if (f.type == "angle16")
			{
				_snprintf_s(b, sizeof(b), _TRUNCATE, "%.2f", (double)u * 360.0 / 65536.0);
				out += b;
			}
			else
				AppendNumber(out, (double)(short)u, true);
		}
		else if (f.type == "long" || f.type == "float")
		{
			const void* p = r.Take(4);
			if (!p) return false;
			double v = f.type == "long" ? (double)*(const long*)p : (double)*(const float*)p;
			push(v);
			AppendNumber(out, v, f.type == "long");
		}
		else if (f.type == "coord" || f.type == "angle")
		{
			const void* p = r.Take(f.type == "coord" ? (size_t)r.coordSize : 1);
			if (!p) return false;
			double v;
			if (f.type == "coord")
				v = (r.coordSize == 4 ? (double)*(const long*)p : (double)*(const short*)p) * 0.125;
			else
				v = (double)*(const char*)p * 360.0 / 256.0;
			push(v);
			_snprintf_s(b, sizeof(b), _TRUNCATE, "%.3f", v);
			out += b;
		}
		else if (f.type == "string")
		{
			const char* start = (const char*)r.data + r.pos;
			size_t avail = r.size - r.pos;
			size_t len = 0;
			while (len < avail && start[len] != '\0')
				++len;
			r.Take(len < avail ? len + 1 : len);  // consume the NUL too, when present
			if (!r.ok) return false;
			push(0.0);  // strings only test as 0 in when-counters
			out += '"';
			bool cut = len > maxString;
			size_t shown = cut ? maxString : len;
			char eb[8];
			for (size_t i = 0; i < shown; ++i)
			{
				unsigned char c = (unsigned char)start[i];
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20 || c == 0x7F)
					{
						_snprintf_s(eb, sizeof(eb), _TRUNCATE, "\\x%02X", c);
						out += eb;
					}
					else
						out += (char)c;
				}
			}
			out += cut ? "...\"" : "\"";
		}
		else if (f.type == "vec3")
		{
			const void* p = r.Take(3 * (size_t)r.coordSize);
			if (!p) return false;
			double v[3];
			if (r.coordSize == 4)
			{
				const long* l = (const long*)p;
				v[0] = l[0] * 0.125; v[1] = l[1] * 0.125; v[2] = l[2] * 0.125;
			}
			else
			{
				const short* s = (const short*)p;
				v[0] = s[0] * 0.125; v[1] = s[1] * 0.125; v[2] = s[2] * 0.125;
			}
			if (!f.name.empty())
			{
				scopes.push_back({ f.name + ".x", v[0] });
				scopes.push_back({ f.name + ".y", v[1] });
				scopes.push_back({ f.name + ".z", v[2] });
			}
			_snprintf_s(b, sizeof(b), _TRUNCATE, "(%.3f,%.3f,%.3f)", v[0], v[1], v[2]);
			out += b;
		}
		else  // group reaches WalkField, never here
			return false;
		return true;
	}

	void WalkFields(const std::vector<UserMsgField>& fields, Reader& r,
		std::vector<Scope>& scopes, std::string& out, size_t maxString);

	void WalkField(const UserMsgField& f, Reader& r, std::vector<Scope>& scopes, std::string& out, size_t maxString)
	{
		const size_t base = out.size();

		if (f.hasWhen)
		{
			const double* v = LookupValue(scopes, f.whenField);
			if (!v)
				return;
			bool match = f.whenNotEqual ? (*v != f.whenValue) : (*v == f.whenValue);
			if (!match)
				return;
		}

		// Resolve the repeat count: "*", a named field, or a fixed value.
		bool isArray = f.countRest || f.count > 1 || f.countFromField;
		size_t count = 1;
		size_t minSize = 0;
		if (f.countRest)
		{
			minSize = FieldMinSize(f, r.coordSize);
			count = minSize ? (size_t)-1 : 0;  // bounded by the reader below
		}
		else if (f.countFromField)
		{
			const double* v = LookupValue(scopes, f.countField);
			count = v ? (size_t)(long long)*v : 0;
		}
		else
			count = (size_t)f.count;

		if (!f.name.empty())
		{
			out += ' ';
			out += f.name;
			out += '=';
		}
		if (isArray)
			out += '[';

		size_t shown = 0;
		for (size_t i = 0; i < count; ++i)
		{
			if (minSize && r.size - r.pos < minSize)
				break;
			if (isArray && shown)
				out += ',';
			if (isArray && shown >= kMaxItemsShown)
			{
				out += "...";
				break;
			}
			if (f.type == "group")
			{
				const size_t scopeBase = scopes.size();
				WalkFields(f.children, r, scopes, out, maxString);
				scopes.resize(scopeBase);
			}
			else
			{
				std::string item;
				if (!ReadOne(f, r, scopes, item, maxString))
					break;
				out += item;
			}
			++shown;
		}

		if (isArray)
			out += ']';
		if (!r.ok)
			out.resize(base);
	}

	void WalkFields(const std::vector<UserMsgField>& fields, Reader& r,
		std::vector<Scope>& scopes, std::string& out, size_t maxString)
	{
		for (const UserMsgField& f : fields)
		{
			if (!r.ok)
				break;
			WalkField(f, r, scopes, out, maxString);
		}
	}
}

std::string UserMsgDecoder::Format(const UserMsgSchema& schema, const UserMsgDef& def,
	int iSize, const void* pbuf, size_t maxString)
{
	char b[64];
	_snprintf_s(b, sizeof(b), _TRUNCATE, " size=%d", iSize);
	std::string line = "[usermsg] " + def.name + b;

	Reader r;
	r.data = (const unsigned char*)pbuf;
	r.size = (size_t)iSize;
	r.coordSize = schema.coord_size;

	if (def.raw)
	{
		size_t shown = r.size < 32 ? r.size : 32;
		line += " raw";
		AppendHex(line, r.data, shown);
		if (r.size > shown)
			line += " ...";
	}
	else
	{
		std::vector<Scope> scopes;
		WalkFields(def.fields, r, scopes, line, maxString);
		if (!r.ok)
			line += " truncated";
		if (r.ok && r.pos < r.size)
		{
			line += " tail";
			size_t shown = (r.size - r.pos) < 16 ? (r.size - r.pos) : 16;
			AppendHex(line, r.data + r.pos, shown);
			if (r.size - r.pos > 16)
				line += " ...";
		}
	}
	return line;
}
