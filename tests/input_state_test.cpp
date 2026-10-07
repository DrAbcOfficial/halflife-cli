#include "input/input_state.h"

#include <climits>
#include <cstdio>
#include <stdexcept>

using namespace InputState;

namespace
{
	void Check(bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	void Position(MousePosition expected, MousePosition actual, const char* message)
	{
		Check(expected.x == actual.x && expected.y == actual.y, message);
	}

	void TestMotion()
	{
		MouseState mouse;
		bool pushed = false;
		auto push = [&](MousePosition, MousePosition) { pushed = true; return true; };
		Check(!mouse.Move(10, 20, false, push) && !pushed, "no window must reject motion");
		Check(mouse.SetGeometry({ 1280, 720, 1280, 720 }), "valid geometry");
		mouse.Seed({ 10, 20 });
		Check(mouse.Move(640, 360, false, [&](MousePosition target, MousePosition delta) {
			Position({ 640, 360 }, target, "absolute event position");
			Position({ 630, 340 }, delta, "absolute event delta");
			return true;
		}), "absolute motion");
		Check(mouse.HasVirtualPosition(), "successful push owns the cursor");
		Check(mouse.Move(-20, 10, true, push), "relative motion");
		Position({ 620, 370 }, mouse.ScreenPosition(), "relative result");
		Check(!mouse.Move(100, 200, false, [](MousePosition, MousePosition) { return false; }), "rejected push");
		Position({ 620, 370 }, mouse.ScreenPosition(), "rejected push must not advance");
		Check(mouse.Move(INT_MAX, INT_MIN, true, push), "large relative delta");
		Position({ 1279, 0 }, mouse.ScreenPosition(), "overflow-safe bounds");
		Check(!mouse.ObserveMotion({ 5, 6 }, false, true), "queued physical motion must not reach VGUI after blocking");
		Position({ 1279, 0 }, mouse.ScreenPosition(), "blocked physical motion must not take over");
		mouse.ObserveMotion({ 40, 50 }, false, false);
		Position({ 40, 50 }, mouse.ScreenPosition(), "physical takeover");
		Check(!mouse.HasVirtualPosition(), "physical takeover releases virtual ownership");
		mouse.ObserveMotion({ 1100, 600 }, true, false);
		Check(mouse.SetGeometry({ 800, 450, 800, 450 }), "resize geometry");
		Position({ 799, 449 }, mouse.ScreenPosition(), "resize clamps previous image position");
		mouse.Reset();
		Check(!mouse.HasVirtualPosition(), "reset clears virtual cursor");
		Position({ 0, 0 }, mouse.ScreenPosition(), "reset has no stale position");
		Check(!mouse.SetGeometry({ 0, 450, 800, 450 }), "zero-size window is invalid");
	}

	void TestCoordinates()
	{
		MouseGeometry geometry{ 800, 450, 1600, 900 };
		Position({ 800, 450 }, geometry.WindowToScreen({ 400, 225 }), "window to screenshot");
		Position({ 400, 225 }, geometry.ScreenToWindow({ 800, 450 }), "screenshot to window");
		Position({ 799, 449 }, geometry.ScreenToWindow({ 1599, 899 }), "last pixel stays inside window");
		Position({ 0, 0 }, geometry.ScreenToWindow({ -5, -5 }), "negative screenshot point clamps");
		MouseState mouse;
		mouse.SetGeometry(geometry);
		mouse.Seed({ 400, 225 });
		Check(mouse.Move(-400, -250, true, [](MousePosition, MousePosition) { return true; }), "scaled relative move");
		Position({ 400, 200 }, mouse.ScreenPosition(), "relative units are screenshot pixels");
	}

	void TestFocus()
	{
		FocusState focus;
		Check(!focus.Active(), "initial inactive");
		Check(focus.SetLocked(true) && focus.Active(), "lock activates an unfocused game");
		Check(!focus.SetActualActive(false), "focus loss under lock must not reactivate");
		Check(!focus.SetActualActive(true), "focus gain under lock must not clear held input");
		Check(!focus.SetLocked(false) && focus.Active(), "unlock retains actual focus");
		Check(focus.SetActualActive(false) && !focus.Active(), "unlocked focus loss");
		Check(focus.SetLocked(true), "relock inactive game");
		Check(focus.Locked() && !focus.ActualActive(), "lock alone keeps the engine active");
		Check(focus.SetLocked(false) && !focus.Active(), "unlock restores actual inactive state");
	}

	void TestQueuedMotionResize()
	{
		MouseMotion queued{ { 700, 500 }, { 600, 300 }, { 800, 600, 800, 600 } };
		auto scaled = queued.Rebase({ 400, 300, 800, 600 });
		Position({ 350, 250 }, scaled.position, "queued motion preserves screenshot point after window scaling");
		Position({ 300, 150 }, scaled.delta, "queued delta uses new window scale");
		auto resized = queued.Rebase({ 400, 300, 400, 300 });
		Position({ 399, 299 }, resized.position, "queued point clamps to resized image");
		Position({ 299, 99 }, resized.delta, "queued delta reflects clamping");
		MouseState mouse;
		mouse.SetGeometry(resized.geometry);
		Check(mouse.ObserveMotion(resized.position, true, true), "accept resized injection");
		Position(resized.position, mouse.WindowPosition(), "queries use the accepted event point");
		// Unchanged geometry must not introduce a second integer-rounding pass.
		MouseMotion fractional{ { 1, 1 }, { 1, 1 }, { 1000, 700, 1280, 900 } };
		Position({ 1, 1 }, fractional.Rebase(fractional.geometry).position, "unchanged geometry keeps exact SDL pixel");
		mouse.SetGeometry(fractional.geometry);
		Check(mouse.MoveWindow({ 1, 1 }, [](MousePosition, MousePosition) { return true; }), "window-coordinate warp");
		Position({ 1, 1 }, mouse.WindowPosition(), "warp avoids screenshot integer-rounding drift");
	}
}

int main()
{
	try
	{
		TestMotion();
		TestCoordinates();
		TestFocus();
		TestQueuedMotionResize();
		std::puts("input state tests passed");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
