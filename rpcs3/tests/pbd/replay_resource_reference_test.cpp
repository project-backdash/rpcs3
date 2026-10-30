#include "Emu/PBD/pbd_replay_resource_reference.h"
#include "Emu/PBD/pbd_replay_result_reference.h"

#include "pbd_test.h"

using namespace pbd::replay_resource;

PBD_TEST(ReplayResource, Relocation)
{
	const catalog source_samples{0x216cace0, 1190, 0x216cb62c};
	const auto left = identify(section_kind::samples16, source_samples, 31, 0x216cacf8);
	pbd::test::check(left && left->index == 12 && left->byte_offset == 0 && left->bank_style == 31);
	pbd::test::check(resolve(*left, source_samples, 31) == 0x216cacf8u);
	const catalog relocated_samples{0x32000000, 1190, 0x3200094c};
	pbd::test::check(resolve(*left, relocated_samples, 31) == 0x32000018u);

	const catalog source_records{0x2163d47c, 223, 0x21641a2c};
	const auto right = identify(section_kind::records80, source_records, 16, 0x2163f6ec);
	pbd::test::check(right && right->index == 110 && right->byte_offset == 16);
	const catalog relocated_records{0x33000000, 223, 0x330045b0};
	pbd::test::check(resolve(*right, relocated_records, 16) == 0x33002270u);
	pbd::test::check(!resolve(*right, relocated_records, 31));

	auto final_sample = *left;
	final_sample.index = 1189;
	pbd::test::check(resolve(final_sample, source_samples, 31) == source_samples.next - 2);
	final_sample.index = 1190;
	pbd::test::check(!resolve(final_sample, source_samples, 31));
	pbd::test::check(!identify(section_kind::samples16, source_samples, 31, source_samples.next));
	pbd::test::check(!identify(section_kind::samples16, source_samples, 31, source_samples.start + 1));

	auto invalid = *right;
	invalid.byte_offset = 80;
	pbd::test::check(!resolve(invalid, source_records, 16));
	invalid.byte_offset = 17;
	pbd::test::check(!resolve(invalid, source_records, 16));
	invalid = *right;
	invalid.kind = static_cast<section_kind>(99);
	pbd::test::check(!resolve(invalid, source_records, 16));
	pbd::test::check(!valid_catalog(section_kind::samples16, {0xfffffff0, 16, 0x10}));
	pbd::test::check(!valid_catalog(section_kind::samples16, {0x1000, 0x100001, 0x201002}));
	pbd::test::check(!valid_catalog(section_kind::records80, {0x1000, 2, 0x109f}));
	pbd::test::check(!valid_catalog(section_kind::samples16, {0x1001, 2, 0x1005}));

	pbd::test::check(resolve({}, {}, 99) == 0u);
	pbd::test::check(!resolve({section_kind::none, 1, 0, 0}, {}, 0));
	pbd::test::check(!resolve({section_kind::none, 0, 2, 0}, {}, 0));
	pbd::test::check(!resolve({section_kind::none, 0, 0, 16}, {}, 16));
	namespace rr = pbd::replay_result;
	const pbd::replay_carry::reaction_result_ref group{1, 21, 31}, entry{2, 28, 16};
	const catalog groups{0x216c92a0, 410, 0x216ca5d8};
	pbd::test::check(rr::resolve_word(group, groups, 31) == 0x216c93a4u);
	pbd::test::check(rr::resolve_word(entry, source_records, 16) == 0x2163dd4cu);
	pbd::test::check(rr::resolve_word(group, {0x32000000, 410, 0x32001338}, 31) == 0x32000104u);
	pbd::test::check(rr::resolve_word(entry, relocated_records, 16) == 0x330008d0u);
	pbd::test::check(!rr::resolve_word(group, groups, 16));
	pbd::test::check(!rr::resolve_word({1, 410, 31}, groups, 31));
	pbd::test::check(!rr::resolve_word({2, 223, 16}, source_records, 16));
	pbd::test::check(!rr::valid_catalog(1, {0xfffffff0, 2, 8}));
	pbd::test::check(!rr::valid_catalog(1, {0x1002, 2, 0x101a}));
	pbd::test::check(!rr::valid_catalog(99, groups));
	pbd::test::check(rr::resolve_word({}, {}, 99) == 0u);
	pbd::test::check(!rr::resolve_word({0, 1, 0}, {}, 0));
	pbd::test::check(!rr::resolve_word({0, 0, 31}, {}, 31));
	const pbd::replay_carry::reaction_result_ref prefix{3, 0xbbe8, 16};
	pbd::test::check(rr::resolve_prefix(prefix, 0x2162df00, source_records, 16) == 0x21639d5cu);
	pbd::test::check(rr::resolve_prefix(prefix, 0x33000000, {0x3300f57c, 223, 0x33013b2c}, 16) == 0x3300be5cu);
	pbd::test::check(!rr::resolve_prefix(prefix, 0x2162df00, source_records, 23));
	pbd::test::check(!rr::resolve_prefix({3, 0xbbe9, 16}, 0x2162df00, source_records, 16));
	pbd::test::check(!rr::resolve_prefix({3, 0xf308, 16}, 0x2162df00, source_records, 16));
	pbd::test::check(rr::resolve_prefix({3, 0xf304, 16}, 0x2162df00, source_records, 16) == source_records.start - 4);
	pbd::test::check(!rr::resolve_prefix({3, 0xfffffffc, 16}, 0x2162df00, source_records, 16));
	pbd::test::check(!rr::resolve_prefix(prefix, 0xfffffff0, source_records, 16));
	pbd::test::check(!rr::resolve_prefix(group, 0x2162df00, source_records, 16));
}
