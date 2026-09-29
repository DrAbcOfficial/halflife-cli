#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>

// Named ring buffers of one-line records ("channels"), keyed by string, that
// back the cli.* query paths. Sequence numbers come from one counter shared
// by all channels, so they stay ordered and comparable across channels while
// each channel independently keeps only its newest entries. The whole
// user-message stream is the single "usermsg" channel for now; future
// recorders can add their own channels without new storage or query plumbing.
//
// Not thread-safe: the monitor calls it from the engine thread only.
class EventLog
{
public:
	explicit EventLog(size_t maxPerChannel = 512)
		: maxPerChannel_(maxPerChannel)
	{
	}

	// Append one line to `channel` (case-sensitive key); returns its sequence
	// number. Drops the channel's oldest entry when it is full.
	uint64_t Record(const char* channel, std::string line)
	{
		uint64_t seq = nextSeq_++;
		auto& ring = channels_[channel];
		ring.push_back({ seq, std::move(line) });
		while (ring.size() > maxPerChannel_)
			ring.pop_front();
		return seq;
	}

	// Newest sequence number handed out so far (0 when nothing was recorded).
	uint64_t NewestSeq() const
	{
		return nextSeq_ - 1;
	}

	// Calls visit(seq, line) for each recorded event of `channel` with
	// seq > since, oldest first.
	void ForEach(const std::string& channel, uint64_t since,
		const std::function<void(uint64_t seq, const std::string& line)>& visit) const
	{
		auto it = channels_.find(channel);
		if (it == channels_.end())
			return;
		for (const Event& e : it->second)
		{
			if (e.seq > since)
				visit(e.seq, e.line);
		}
	}

	void Clear()
	{
		channels_.clear();
		nextSeq_ = 1;
	}

private:
	struct Event
	{
		uint64_t seq;
		std::string line;
	};

	size_t maxPerChannel_;
	std::map<std::string, std::deque<Event>> channels_;
	uint64_t nextSeq_ = 1;
};
