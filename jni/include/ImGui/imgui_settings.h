#include "imgui.h"

namespace c {

	inline ImVec4 accent = ImColor(1.0f, 0.5f, 0.0f, 1.0f);

	namespace bg
	{

		inline ImVec4 background = ImColor(0, 0, 0, 200);
		inline ImVec2 size = ImVec2(960, 610);
		inline float rounding = 9.f;

	}

    namespace widget
    {

        inline ImVec4 background = ImColor(23, 25, 24, 255);
        inline ImVec4 outlinecolor = ImColor(28, 30, 29, 255);
        inline float outline = 2.f;
        inline ImVec2 size = ImVec2(310, 50);
        inline float rounding = 4.f;

	}

	namespace child
	{

		inline ImVec4 background = ImColor(25, 25, 25, 145);
        inline ImVec4 outline = ImColor(50, 50, 50, 255);
		inline float rounding = 10.f;

	}

	namespace checkbox
	{
		inline ImVec4 background = ImColor(45, 45, 48, 255);
        inline ImVec4 background_off = ImColor(15, 17, 16, 255);
        inline ImVec4 background_on = ImColor(0, 0, 0, 255);
        inline ImVec4 circle_inactive = ImColor(54, 58, 65, 255);
		inline float rounding = 50.f;
	}

	namespace slider
	{
		inline ImVec4 background = ImColor(45, 45, 48, 255);
        inline ImVec4 circle_inactive = ImColor(54, 58, 65, 255);
		inline float rounding = 2.f;
	}

	namespace tabs
	{
		inline ImVec4 background = ImColor(16, 17, 19, 255);

		inline ImVec4 outline = ImColor(22, 24, 26, 255);
		inline float rounding = 6.f;
	}

	namespace combo
	{
		inline ImVec4 background = ImColor(50, 50, 50, 200);
		inline ImVec4 outline = ImColor(50, 50, 50, 255);
		inline ImVec4 icon = ImColor(26, 27, 31, 255);

		inline float rounding = 2.f;
	}

	namespace picker
	{
		inline ImVec4 background = ImColor(16, 17, 19, 255);
		inline ImVec4 outline = ImColor(22, 24, 26, 255);

		inline float rounding = 6.f;
	}

	namespace button
	{
		inline ImVec4 background_active = ImColor(26, 27, 29, 255);
		inline ImVec4 background_inactive = ImColor(16, 17, 19, 255);

		inline ImVec4 outline = ImColor(22, 24, 26, 255);

		inline float rounding = 2.f;
	}

	namespace keybind
	{
		inline ImVec4 background_active = ImColor(26, 27, 29, 255);
		inline ImVec4 background_inactive = ImColor(16, 17, 19, 255);

		inline ImVec4 outline = ImColor(22, 24, 26, 255);

		inline float rounding = 2.f;
	}

	namespace input
	{
		inline ImVec4 background_active = ImColor(26, 27, 29, 255);
		inline ImVec4 background_inactive = ImColor(16, 17, 19, 255);

		inline ImVec4 outline = ImColor(22, 24, 26, 255);
		inline ImVec4 icon = ImColor(26, 27, 31, 255);

		inline float rounding = 2.f;
	}

	namespace scroll
	{
		inline float rounding = 30.f;
	}

	namespace text
	{
		inline ImVec4 text_active = ImColor(255, 255, 255, 255);
		inline ImVec4 text_hov = ImColor(255/2, 255/2, 255/2, 255);
		inline ImVec4 text = ImColor(255/2, 255/2, 255/2, 255);

		inline float glow_size = 16.f;
	}
}
