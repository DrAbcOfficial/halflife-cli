#pragma once

#include <cstdint>

// SDL-independent cursor and focus state shared by native input, the focus
// lock and their regression tests.
//
// Positions are kept in game-window coordinates, the space SDL reports and
// the engine converts to VGUI coordinates itself. Automation addresses the
// cursor in original screenshot pixels (the video mode size); the geometry
// converts between the two.
namespace InputState
{
	// The engine headers define a clamp macro, so keep this small numeric
	// operation independent of <algorithm>.
	template<class T>
	constexpr T LimitCoordinate(T value, T low, T high)
	{
		return value < low ? low : value > high ? high : value;
	}

	struct MousePosition
	{
		int x = 0;
		int y = 0;
	};

	struct MouseGeometry
	{
		int windowWidth = 0;
		int windowHeight = 0;
		int imageWidth = 0;
		int imageHeight = 0;

		bool Valid() const
		{
			return windowWidth > 0 && windowHeight > 0 && imageWidth > 0 && imageHeight > 0;
		}

		bool SameSize(const MouseGeometry& other) const
		{
			return windowWidth == other.windowWidth && windowHeight == other.windowHeight &&
				imageWidth == other.imageWidth && imageHeight == other.imageHeight;
		}

		MousePosition ClampScreen(int64_t x, int64_t y) const
		{
			return { static_cast<int>(LimitCoordinate<int64_t>(x, 0, imageWidth - 1)),
				static_cast<int>(LimitCoordinate<int64_t>(y, 0, imageHeight - 1)) };
		}

		MousePosition ClampWindow(MousePosition point) const
		{
			return { LimitCoordinate(point.x, 0, windowWidth - 1), LimitCoordinate(point.y, 0, windowHeight - 1) };
		}

		MousePosition ScreenToWindow(MousePosition point) const
		{
			point = ClampScreen(point.x, point.y);
			return { static_cast<int>(int64_t(point.x) * windowWidth / imageWidth),
				static_cast<int>(int64_t(point.y) * windowHeight / imageHeight) };
		}

		MousePosition WindowToScreen(MousePosition point) const
		{
			point = ClampWindow(point);
			return { static_cast<int>(int64_t(point.x) * imageWidth / windowWidth),
				static_cast<int>(int64_t(point.y) * imageHeight / windowHeight) };
		}
	};

	// An injected motion waiting in the SDL queue, with the geometry it was
	// computed for.
	struct MouseMotion
	{
		MousePosition position;
		MousePosition delta;
		MouseGeometry geometry;

		// Re-map position and delta through the screenshot point if the
		// window or image was resized before the event was dispatched.
		MouseMotion Rebase(const MouseGeometry& current) const
		{
			if (geometry.SameSize(current))
				return { position, delta, current };
			auto target = current.ScreenToWindow(geometry.WindowToScreen(position));
			auto previous = current.ScreenToWindow(geometry.WindowToScreen({ position.x - delta.x, position.y - delta.y }));
			return { target, { target.x - previous.x, target.y - previous.y }, current };
		}
	};

	class MouseState
	{
	public:
		void Reset() { *this = MouseState{}; }

		bool SetGeometry(MouseGeometry geometry)
		{
			if (!geometry.Valid())
				return false;
			MousePosition previous = m_geometry.Valid() ? ScreenPosition() : MousePosition{};
			// Avoid resampling/rounding when nothing was resized.
			bool resized = !m_geometry.SameSize(geometry);
			m_geometry = geometry;
			if (resized)
				m_position = geometry.ScreenToWindow(previous);
			return true;
		}

		void Seed(MousePosition point)
		{
			if (m_geometry.Valid())
				m_position = m_geometry.ClampWindow(point);
		}

		// x/y in screenshot pixels; relative adds them to the current position.
		template<class Push>
		bool Move(int x, int y, bool relative, Push push)
		{
			if (!m_geometry.Valid())
				return false;
			MousePosition previous = ScreenPosition();
			auto screen = m_geometry.ClampScreen(int64_t(x) + (relative ? previous.x : 0),
				int64_t(y) + (relative ? previous.y : 0));
			return MoveWindow(m_geometry.ScreenToWindow(screen), push);
		}

		// push(target, delta) queues the motion event; the position only
		// advances once it was accepted.
		template<class Push>
		bool MoveWindow(MousePosition target, Push push)
		{
			if (!m_geometry.Valid())
				return false;
			target = m_geometry.ClampWindow(target);
			MousePosition delta{ target.x - m_position.x, target.y - m_position.y };
			if (!push(target, delta))
				return false;
			m_position = target;
			m_virtual = true;
			return true;
		}

		// A dispatched motion event. Allowed physical motion takes the cursor
		// over; physical motion is rejected while it is blocked.
		bool ObserveMotion(MousePosition point, bool injected, bool blockPhysical)
		{
			if (!injected && blockPhysical)
				return false;
			Seed(point);
			m_virtual = injected;
			return true;
		}

		void KeepVirtualPosition() { m_virtual = true; }
		bool HasVirtualPosition() const { return m_virtual; }
		MousePosition ScreenPosition() const { return m_geometry.Valid() ? m_geometry.WindowToScreen(m_position) : MousePosition{}; }
		MousePosition WindowPosition() const { return m_position; }
		const MouseGeometry& Geometry() const { return m_geometry; }

	private:
		MouseGeometry m_geometry;
		MousePosition m_position;
		bool m_virtual = false;
	};

	// Effective engine activation: the real window focus, or forced on by
	// the focus lock.
	class FocusState
	{
	public:
		bool Active() const { return m_locked || m_actualActive; }
		bool Locked() const { return m_locked; }
		bool ActualActive() const { return m_actualActive; }
		// Both setters return whether the effective state changed.
		bool SetLocked(bool locked)
		{
			bool previous = Active();
			m_locked = locked;
			return previous != Active();
		}
		bool SetActualActive(bool active)
		{
			bool previous = Active();
			m_actualActive = active;
			return previous != Active();
		}

	private:
		bool m_actualActive = false;
		bool m_locked = false;
	};
}
