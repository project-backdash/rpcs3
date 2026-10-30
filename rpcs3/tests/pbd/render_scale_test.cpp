#include "pbd_test.h"

#include "Emu/RSX/Utils/pbd_render_scale.h"

using pbd::test::check;

PBD_TEST(PbdRenderScale, Checks)
{
	using rsx::pbd::render_ratio;
	for (const auto size : {720u, 1080u, 1440u, 2160u})
	{
		const auto percentage = static_cast<std::uint16_t>(size == 720 ? 67 : size == 1080 ? 100 :
																		  size == 1440     ? 133 :
																							 200);
		const auto label = size == 720 ? "1280x720" : size == 1080 ? "1920x1080" :
		                                          size == 1440     ? "2560x1440" :
		                                                             "3840x2160";
		const auto ratio = render_ratio(percentage, label);
		const auto width = 1920u * ratio.numerator / ratio.denominator;
		const auto height = 1080u * ratio.numerator / ratio.denominator;
		check(width == size * 16 / 9 && height == size);
		check(width * ratio.denominator / ratio.numerator == 1920);
		check(height * ratio.denominator / ratio.numerator == 1080);
	}
	for (const auto percentage : {25u, 67u, 100u, 133u, 150u, 200u, 300u})
	{
		const auto ratio = render_ratio(static_cast<std::uint16_t>(percentage), "");
		check(ratio.numerator == percentage && ratio.denominator == 100);
	}
	check(render_ratio(67, "2560x1440").numerator == 67);
	check(render_ratio(100, "1280x720").numerator == 100);
}
